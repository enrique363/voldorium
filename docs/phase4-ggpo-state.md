# Phase 4: GGPO rollback state and determinism

Evidence: `sc/traces/mem_writes.csv`, `mem_reads.csv`, `mem_hwreads.csv`, `mem_rng.csv`,
captured by the Flycast memory hook (`sc/flycast-trace/apply_memtrace_hook.py`) over 600
frames of an active fight (GAME MODE, battle state 1, battle-state frames 120-719).
Tools: `sc/tools/memregions.py`. Save-state table: `sc/ggpo/savestate_ranges.tsv`.

Caveat: one capture of one fight. Rarely used paths (throws, ring-outs, round transitions,
other stages and characters, Team/Survival modes) may write more. The checksum test at
the end is what proves the set complete.

## 1. Frame structure (where GGPO plugs in)

`main` runs one iteration per displayed frame:

```
view_set_camera -> game_update (15 subsystems; battle_frame via the mode table,
                   hud_battle_draw, effects) -> render_scene -> screen_fade_draw
-> kmEndScene -> kmRender (wait + start render) -> frame_end_input_audio (pads)
```

Drawing is interleaved with simulation: `game_update` itself builds HUD, text and effect
polygons, and effect code consumes the game RNG in the same pass. So a rollback
re-simulation must **run the whole loop body**, including list building, and only
suppress `kmRender` / TA DMA and audio output. Splitting "logic" from "draw" is not
needed and would change RNG consumption.

- **GGPO advance_frame** = one main-loop iteration with inputs injected.
- **Save/load point** = top of the loop body. At that point the stack holds no game
  locals and the TA buffers are about to be rebuilt.
- **Input injection** = after `game_read_pads` (0x8C061A20) writes the 16-byte
  `g_game_pads` record per port (0x8C24A4D4 + port*0x10): overwrite records 0 and 1 with
  the synchronized inputs before `pads_copy_to_players`. Pack the 16 bytes to
  about 6 bytes per player for the GGPO input (button word, two analog trigger bytes,
  stick bytes).

## 2. What a fight frame writes

107 regions, 688,602 bytes written. Classified:

| Class | Regions | Size | Decision |
| --- | --- | --- | --- |
| Fighters + battle globals | 0x8C24A7C0-0x8C25BAA0 (both player structs, `g_battle_state`, life, rounds) | ~45 KB span | **save** |
| Animation / skeleton / motion | 0x8C25F120-0x8C26D1A0, heap 0x8C8FBFC0, 0x8CB35CC0, 0x8CB5C0A0, 0x8CD91560 | ~50 KB | **save** |
| Camera | 0x8C3366C0-0x8C336820, 0x8C335140 | <1 KB | **save** |
| CPU AI | .data 0x8C184D80, 0x8C185020, 0x8C1A6B00 | ~2 KB | **save** (in .data) |
| RNG states | LFSR 0x8C159060, rand 0x8C21AECC | 8 B | **save** (in .data) |
| Particles / effects | heap 0x8C8BC440-0x8C8C6440, .data 0x8C15EF40 | ~40 KB | **save** (they consume RNG) |
| HUD state | 0x8C28DC00-0x8C28F360 | ~6 KB | **save** (uses rand) |
| Round-start buffers | heap 0x8C789C40 (599 KB, written frames 1-8, never read), 0x8C81FC20 | 600 KB | **save** (insurance) |
| TA vertex buffers | heap 0x8CD9E480-0x8CE7F560 | ~190 KB | skip (rebuilt every frame) |
| Projected particle vertices | heap 0x8C689C40-0x8C692040 | ~34 KB | skip (render scratch) |
| Libraries | pd/Maple 0x8C33D2C0, Kamui 0x8C379C80, sound driver 0x8C37B1C0, DMA, GD-ROM | | skip (not simulation) |
| Stack | 0x8C00C000-0x8C00F400 | | skip (empty at the save point) |

### Save state = 2.8 MB

`sc/ggpo/savestate_ranges.tsv` lists 10 include ranges (2,801,568 bytes) and 7 exclude
ranges; every line written during the capture falls in exactly one. Simplest form:
whole game `.data` (1.1 MB) + game BSS 0x8C247480-0x8C33C840 (1 MB) + six heap ranges.
At ~3 MB a memcpy is ~0.3 ms, so saving every frame with an 8-frame rollback ring is
cheap. Further trimming (only the written .data lines) is possible later but not needed.

Not in RAM, and not needed: CPU registers (save point is a loop boundary), VRAM (textures
are uploaded at load time; nothing in the fight uploads simulation data), sound chip.

## 3. Non-determinism to remove

### 3.1 SH4 timer feeding the RNG (required fix)

`TCNT1` (0xFFD80024) was read 139,625 times in 600 frames, all through `sh4_ReadTimer`
(0x8C220060):

| Reader | Reads | Effect |
| --- | --- | --- |
| `rng_next_float` 0x8C063A00 | 110,706 | steps LFSR (timer & 3) extra times |
| `rng_next15` 0x8C0639C8 | 19,048 | same |
| 0x8C063A7C (third RNG entry) | 4,800 | same pattern |
| 0x8C0608C0 (in `battle_frame`) | 2,261 | direct timer use |
| 0x8C06C0E0 (effects) | 1,197 | direct timer use |
| `stage_wind_update` 0x8C05FDC0 | (via RNG + direct) | stage sway |

Fix: replace `sh4_ReadTimer` with a deterministic counter stored inside the saved state
(for example a word appended to the save buffer, advanced per call). Any deterministic
sequence works; the game only uses it as entropy.

### 3.2 RNG consumers

LFSR: 129,754 calls from 41 functions, ~75% from the particle/"Anime" effect system
(0x8C069DA0..0x8C06CABC), the rest from CPU AI (`cpu_ai_think`, 0x8C097EE0..0x8C09EA9E),
stage wind and 0x8C0608C0. `rand()` (C LCG at 0x8C21AECC, seeded with 0 by `srand`
0x8C0FEF7C in `game_init`): 42,272 calls from 27 functions, mostly effects, plus HUD (8),
0x8C0A6400/0x8C0A6B74/0x8C0A7F00 and the camera code 0x8C0C1D40.

With both states saved and the whole loop body re-run on rollback (section 1), all of these
stay in sync. No stream splitting is needed.

### 3.3 Other hardware reads (no simulation effect expected; verify)

| Source | Reads | Notes |
| --- | --- | --- |
| HOLLY G2 DMA / FIFO status 0x5F7818..78, 0x5F688C | ~290,000 | sound library polling loops (0x8C102764, 0x8C1022E8) |
| HOLLY interrupt status 0x5F6900/08/20, Maple 0x5F6C18, PVR 0x5F80DC/810C/8138/8144 | ~37,000 | Kamui / pd libraries |
| SH4 INTEVT 0xFF000028, DMAC 0xFFA0002C, TMU0 0xFFD8000C | ~14,000 | interrupt trampoline, DMA wait, Kamui render timing |
| AICA RAM / registers | 35,000 | all through 0x8C0FF350 (G2 bus read). **Open:** confirm the game does not branch on sound status (voice finished, etc.) inside the simulation |
| Address 0 (BIOS ROM) | 1,200 | null-pointer read in the model renderer 0x8C03F700 <- 0x8C03FA8E; value 0x0000085B under Flycast's HLE BIOS. A port must return the same bytes the real BIOS has at address 0 to match original behaviour |

### 3.4 Audio during rollback

Sound effects and voices are triggered from simulation code through the sound library.
During re-simulated frames the port must drop those commands, and after the rollback
reconcile only what should still be playing (the standard GGPO "mute during resim").
The sound driver state (0x8C37B1C0..) stays out of the save state.

## 4. SyncTest results (2026-10-15)

Implementation: `sc/flycast-trace/apply_synctest_hook.py` (hook in `Sh4Interpreter::ReadNexOp`).
Per fight frame: snapshot include ranges + CPU registers + fake-timer counter at the main-loop
top 0x8C03144E, run, checksum, restore, replay the recorded `PDS_PERIPHERAL` input at
`game_read_pads`, run again, compare. `sh4_ReadTimer` is replaced by a saved counter.
`SC_AUTOPLAY=1` drives the menus into Arcade and plays pseudo-random moves, so the test
runs unattended (no display needed). Diagnostics: per-frame vsync counts, full snapshot
dumps (`sync_<n>_{snap,end1,end2}.bin`, `tools/diff_sync_dump.py`) and a whole-RAM leak
report (`sync_<n>_leaks.txt`: memory run 1 changed that the restore did not cover).

| Run | Table | Result | Finding |
| --- | --- | --- | --- |
| user fight, 600 frames | v1 (2.8 MB) | 0/600 OK | ~200 lines/frame in per-fighter matrix data |
| autoplay, 20 frames | v1 | 0/20 OK | vsync counts identical in both runs: not interlace/timing |
| autoplay, 6 frames + leak report | v1 | 0/6 OK | leaks only next to the per-player heap motion buffers (sized per character) |
| autoplay, 60 frames | **v2** (heap windows widened, 3.1 MB) | **35/60 OK** | remaining diffs: a few lines per frame |
| autoplay, 60 frames | v2 + render buffers | 14/60 OK | vertex buffers move between Kamui ring slots; not state |
| autoplay, 60 frames | v2 + Kamui frame state | 0/60 OK | Kamui block holds render timing stats; not state |

`savestate_ranges.tsv` is v2. Remaining divergences in v2 (23 of 25 with identical vblank counts,
so they are real, not emulator timing):

- `0x8C27D920` (scratch matrix written by 0x8C03DFA0 / 0x8C03C73E), every other frame.
- `0x8C24A91C/0x8C24A920` (computed in `hud_battle_draw` from the camera), and the vsync
  words 0x8C24A900-0x8C24A908 on the 2 frames where the vblank count differed (emulator only).
- `0x8C28DDA0..0x8C28DE00` HUD animation state (0x8C0A8D40).
- Three real divergence events (frames 3, 4, 51): blocks at 0x8C281C00 / 0x8C289C00 and, in
  frame 51, the `rand()` state, a player struct line and battle globals. Some code made a
  different decision in the rerun from an input that is not restored.

Next: dump snapshots for those specific frames (raise `SC_SYNC_DUMP`, or dump on a chosen test
number), find the first instruction whose result differs (watch the diverging address with
the memtrace writer PCs), and identify which unrestored input it reads. Candidates: sound
status via `g2_bus_read` 0x8C0FF350, Maple/pd library state other than PDS_PERIPHERAL, the
interrupt-driven `vsync_callback` work (it also calls 0x8C223C76, 0x8C0A76BC, 0x8C21CD10),
and address-0 BIOS reads.

### 4.1 Resolution (same day): 300/300 fight frames pass

A keyed event diff was added to the SyncTest (`SC_MEMTRACE` wrappers feed an event hook;
per frame it records every write into the save state and every read outside it, keyed by
PC and address, and on mismatch writes `sync_<n>_events.txt` sorted by first divergence).
Each remaining divergence then pointed at its cause:

| Symptom | Cause | Fix in `savestate_ranges.tsv` |
| --- | --- | --- |
| frames 12, 21: game code (0x8C0ADxxx, 0x8C0C246C) reads differing lists/counters in heap 0x8C689C40.. | that heap area is an **effect object pool**, not render scratch; 0x8C250260 points into it | include 0x8C689C00-0x8C693000 |
| frames 1-4: 16 KB blocks at 0x8C27DC00..0x8C289C00 and bytes in 0x8C28DD50.. | **streaming audio** (FUN_8C0A8D40): GD-ROM DMA disc buffers + player structs + request table, advanced by DMA/interrupt timing | exclude 0x8C27DC00-0x8C28DE28 and 0x8C28FD60-0x8C2907A0 |
| frames 9, 39: 0x8C31CA14 / 0x8C31FA14 | **Maple DMA buffers** that `sbInitSystem` passes to `pdInitPeripheral` (0x8C3109A4..) | exclude 0x8C3109A4-0x8C328A00, and GD-ROM work buffers 0x8C328A00-0x8C32A000 |
| frame 60: fade alpha differs | game code reads `vsync_callback` counters 0x8C24A904/908 (0x8C0C9450, 0x8C0AE0C4) **and** uninitialised stack slots (0x8C0FEA7E, 0x8C0499EC) | `restore` (not checksummed) for 0x8C24A900-0x8C24A920 and the stack 0x8C00C000-0x8C00F400 |

New decision type `restore`: saved and restored, but excluded from the checksum, because the
emulator's interrupts also write there at timing-dependent moments.

Results with the final table (all unattended, `SC_AUTOPLAY`):

| Run | Result |
| --- | --- |
| fake timer, fight state, 300 frames | **300/300 OK** |
| real SH4 timer (control), 120 frames | 0/120 OK: RNG states, both players, stage wind diverge immediately |

The control run proves the test detects desyncs and that replacing `sh4_ReadTimer` is required.

Port requirements that follow from this:

1. Replace `sh4_ReadTimer` (0x8C220060) and the game-side TCNT0 read `0x8C21FDBE` (section 6.4) with a counter that is part of the saved state.
2. Run `vsync_callback` exactly once per simulated frame (its counters feed game code), and
   save its counters.
3. Save the stack region (or guarantee the same stack residue): some code reads stack slots
   it did not write this frame.
4. Keep audio streaming, Maple and GD-ROM buffers out of the state; they are reimplemented by
   the port and must not feed back into simulation.
5. Save state size with the final table: 3,090,204 bytes (`include` + `restore` ranges).

### 4.2 Long run across all battle states: 3,000/3,000 pass

`SC_SYNC_ANYSTATE=1` tests every frame in GAME MODE regardless of battle state. Two more
fixes came out of the first long attempt (1,561/1,585, then a crash):

| Symptom | Cause | Fix |
| --- | --- | --- |
| burst of mismatches from frame ~1,146 (pool head 0x8C689C44, lines at 0x8C692EF0..) followed by "SH4 exception when blocked" | effect pool larger than its window; a partial restore of a linked-list pool leaves dangling pointers | pool window widened to 0x8C689000-0x8C6A9000 |
| 6 isolated frames: 4 bytes at 0x8C266EC4 + n*0x90 | `memcpy` 0x8C0FEA78 copies a stack local with one uninitialised byte; in the emulator interrupt frames (0x8C241422) land there at timing-dependent moments | those 4 words marked `restore`; emulator-only (the port's interrupts do not share the game stack) |

Final run (`synctest_v11_all.csv`): **3,000 frames, 0 mismatches, no crash**, covering battle
states 0 (round intro, 202 frames), 1 (fight, 1,579), 4 (KO / round end, 320) and 0x16-0x18
(match end, results, continue flow, 899).

Remaining coverage gaps (closed by section 6): autoplay always picks the Arcade default character and stage, so
other characters, stages, weapons, VS / Team / Survival / Mission modes, throws and ring-outs
are not yet tested. The effect-pool and per-player heap windows are the ranges most likely to
need widening for heavier characters or effects.

## 5. Determinism test plan (original)

1. In the traced Flycast (interpreter), add a SyncTest mode: each frame, save the include
   ranges, run 1 frame, restore, run it again, and compare checksums of the include ranges.
   Any mismatch names the region and frame.
2. Run with `sh4_ReadTimer` replaced (return a counter) and without, to confirm the timer
   is the only entropy source.
3. Repeat across stages, characters, throws, ring-outs and round transitions to grow the
   capture coverage.

This is exactly what GGPO's own `ggpo_start_synctest` does, so passing it in the emulator
de-risks the native port's netplay before any network code exists.

## 6. Multi-combination SyncTest (2026-10-15)

Goal: cover more than "Arcade, default character, default stage" and make the save-state table hold for
them. Tooling: `SC_AUTOPLAY_SEED` in `sc/flycast-trace/apply_synctest_hook.py`, batch runner
`sc/flycast-trace/run_synctest_combos.sh <frames> <seeds...>`, table v4 (`sc/ggpo/savestate_ranges.tsv`).

### 6.1 Seeded autoplay

`SC_AUTOPLAY_SEED=n` derives a plan from `n` only. `autoplayPad()` plays it from the main-loop counter `loopCount`
(never the vsync counter, which varies with emulator workload), relative to the loop at which the current game mode
was entered, so every menu runs the same script on every run.

| Plan field | Meaning | Menu script |
| --- | --- | --- |
| menu (0-3) | main-menu cursor entry: 0 Arcade (game type 1), 1 VS Battle (3), 2 Survival (7), 3 Team Battle (2) | mode 11: DOWN x menu, then START taps (A does not confirm on this menu) |
| char moves (0-12) + directions | character-select cursor moves, one tap per 12 loops from loop 120 of mode 15 | R/D/L/U taps, then the costume button |
| costume button | A, B, X or Y | one tap |
| stage moves (0-8) | stage-select cursor moves, starting 120 loops after the costume tap | taps, then A every 30 loops |
| fight stream | `apRng` xor-ed with the seed (P2 has its own stream in VS) | random action every 8 loops |

Without the variable the old START/A script runs unchanged. `SC_AUTOPLAY_MENU / _CHAR / _STAGE / _COSTUME / _P2`
force single fields; `SC_AUTOPLAY_LOG=1` prints mode changes, the menu table, and for each fight the game type,
the stage descriptor (`*0x8c25dd3c`) and both characters (`0x8c250588` / `0x8c258c64`, costume word `+8`).

Facts found while building it:

- The main menu (`FUN_8c035860` / `FUN_8c0f3d46`, cursor state at `*0x8c21a9e8`) has 12 entries; entries 0-4 return
  game types 1, 3, 7, 2, 4 and are selectable with one pad. The names Arcade / VS Battle / Survival / Team Battle
  follow the order given in the task and the game types; they were not checked on screen.
- VS Battle needs a second pad (`g_pad_device_type` counts ports). The WSL Flycast config got `device2 = 0` under
  `[input]` (backup: `emu.cfg.bak_synctest`; the Fightcade install is untouched) and the autoplay writes the port 1
  record (`PDS_PERIPHERAL + 0x138`) with the same menu taps and its own fight stream.
- Stage-select cursor moves had no visible effect: the stage descriptor follows game type and round, not the taps
  (VS always `0x8c14cd34`, Arcade `0x8c14c554` / `0x8c14c674`, Survival `0x8c14c8b4`, later Survival rounds
  `0x8c14ce54`). Stage coverage therefore comes from game types and Survival rounds, not from the stage taps.
- Character-select taps register only after ~100 loops of mode 15 (earlier ones are ignored). The resulting pair of
  characters comes from the cursor path plus the game's own choices (P2 / CPU), so it is read back from memory.

### 6.2 Running it

```
wsl -d Ubuntu -- bash /mnt/c/path/to/sc/flycast-trace/run_synctest_combos.sh 1500 1 2 5 8
```

Environment per run: `SC_TRACE=1 SC_SYNCTEST=1 SC_AUTOPLAY=1 SC_AUTOPLAY_SEED=n SC_SYNC_ANYSTATE=1`,
`SC_SYNC_RANGES`, `SC_MEMTRACE=1 SC_MEM_FROM=999999` (event diff hooks on, trace off), `SC_SYNC_DUMP=8`,
`SC_STATELOG`, `GALLIUM_DRIVER=d3d12`, `SDL_VIDEODRIVER=x11`, plus `-config audio:backend=null -config config:rend.vsync=no`.
The null audio backend matters: with the WSLg PulseAudio sink the emulator thread deadlocked at boot (every thread in
a futex wait right after "REIOS: Booting up"), and without audio pacing 1,500 tested frames take about 4 minutes.
Do not `pkill flycast` in a shared WSL (other sessions run their own builds).

### 6.3 Combinations tested (final table v4)

Each run: `SC_SYNC_ANYSTATE=1`, 1,500 tested frames (3,000 simulated), no crash, 0 mismatches. "Plan" is
char moves / stage moves / costume button; "States" are the number of tested frames per battle state
(0 round intro, 1 fight, 4 KO, 22-24 match end / results / continue). Characters are `id/costume` read back at the
start of the first fight, `@` is the stage descriptor `0x8c14xxxx`. The per-frame state logs
(`SC_STATELOG`, `sc/traces/combo/s<seed>/statelog.csv`) differ in every run: the fingerprint of fight frame 60
(RNG states, both players, battle block, camera, globals) is unique for all 14 seeds.

| Seed | Mode | Plan | Characters (P1 v P2) and stage | Frames | Mismatches | States (tested frames) |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Arcade | 9 / 0 / A | 4/0 v 5/1 @c674 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 4 | Arcade | 0 / 1 / A | 12/0 v 4/1 @c554 | 1500 | 0 | 0:101, 1:864, 4:320, 22:182, 23:33 |
| 13 | Arcade | 12 / 2 / Y | 3/1 v 4/0 @c554 | 1500 | 0 | 0:101, 1:1052, 4:320, 22:27 |
| 49 | Arcade | 5 / 3 / X | 4/0 v 5/1 @c674 | 1500 | 0 | 0:101, 1:947, 4:320, 22:132 |
| 2 | VS Battle | 12 / 3 / Y | 12/1 v 13/0 @cd34 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 9 | VS Battle | 4 / 1 / B | 12/0 v 13/1 @cd34 | 1500 | 0 | 0:101, 1:872, 4:320, 22:122, 23:85 |
| 38 | VS Battle | 3 / 8 / Y | 5/1 v 11/0 @cd34 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 44 | VS Battle | 8 / 2 / Y | 4/1 v 17/0 @cd34 | 1500 | 0 | 0:101, 1:1045, 4:320, 22:34 |
| 5 | Survival | 12 / 5 / A | 17/0 v 4/1 @c8b4 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 34 | Survival | 6 / 5 / Y | 12/0 v 13/1 @c8b4 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 47 | Survival | 8 / 6 / Y | 12/0 v 11/0 @c8b4 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 8 | Team Battle | 8 / 8 / X | 4/0 v 5/1 @c674 | 1500 | 0 | 0:101, 1:918, 4:320, 22:122, 23:39 |
| 33 | Team Battle | 2 / 6 / A | 13/0 v 4/1 @c554 | 1500 | 0 | 0:101, 1:1079, 4:320 |
| 48 | Team Battle | 8 / 3 / A | 4/0 v 5/1 @c674 | 1500 | 0 | 0:101, 1:1079, 4:320 |

12 distinct (mode, characters, costume, stage) tuples across the 14 seeds: seeds 49 and 48 repeat the tuple of seeds 1 and 8
(the first Arcade / Team opponent pair) with a different plan and a different fight input stream, so their state logs still
differ. Four stage descriptors occur (`0x8c14c554`, `0x8c14c674`, `0x8c14c8b4`, `0x8c14cd34`).

Extended runs with the final table (4,500 frames each; `sc/traces/combo/e5_v5`, `e34_v5`; battle states 0, 1, 3, 4, 5,
22, 23, 24, including Survival round changes and the continue / ranking flow): seed 34 passes 4,500/4,500; seed 5
passes 4,499/4,500, the single mismatch being a loading transition frame (6.5).

### 6.4 Changes to the table and hook, with evidence

First results with the inherited table (v2, partly widened while debugging): Survival (seed 5) 60/60 mismatches in 60
frames; Team Battle (seed 33) and Survival (seed 34) failed every frame; VS seeds 38 and 44 failed once each.
Diagnostics: `sync_<n>_leaks.txt`, `sync_<n>_events.txt`, `tools/diff_sync_dump.py` (it now counts `restore` ranges
when walking the dump; an optional third argument selects the table the dump was made with).

| # | Symptom | Evidence | Change | Decision |
| --- | --- | --- | --- | --- |
| 1 | Survival: LFSR `0x8c159060` and effect floats diverge from frame 1 | `events`: read of TMU0 `TCNT0` (`0xffd8000c`) by `0x8c21fdc2`, then effect code `0x8c07befe` writes a different float to `0x8c8bab00` | hook: `0x8c21fdbe` (TCNT0 read, 11 callers) is answered by the saved fake-timer counter when the caller is game effect code (`pr` in `0x8c068000..0x8c0a0000`: `0x8c069030`, `0x8c07bb20`, `0x8c07ea60`, `0x8c07f2e0`, `0x8c086920`, `0x8c086cc0`, `0x8c095600`); streaming-audio and library callers keep the real timer | **fix non-determinism** (second entropy source next to `sh4_ReadTimer`) |
| 2 | Survival: read of `0x8c8b8608` (`0x8c07c428`) differs; leaks `0x8c8b8600..0x8c8baf80` | `leaks.txt`, `events`; 60/60 mismatches became 12/300, then 0/600 together with fix 1 | second particle pool below the particle state | **include** (inside the arena, row 5) |
| 3 | Survival p12 vs p13 (seed 34): every frame, 9 lines of skeleton matrices (`0x8c266a10`, `0x8c264f90`) | first read divergence `0x8cd851ec` by `0x8c0493f4` (motion blend); leaks `0x8cd82540..0x8cd853e0` | per-player motion / skeleton work buffers below `0x8cd91560` | **include** (arena) |
| 4 | VS seed 38 frame 73: effect list read at `0x8c8c6450` (`0x8c0712d2`) is `0xffff` vs `1`; leak `0x8c8cba40` | particle state continues past the old window end `0x8c8c6440` (to at least `0x8c8cba60`) | window widened, then arena | **include** (arena) |
| 5 | Team Battle seed 33 (p13): every frame; reads `0x8cb23474..` (`0x8c0493f4`), leaks `0x8cb20600..0x8cb235e0` | below the old P2 window `0x8cb30000`; block positions depend on character and mode, so window by window did not converge | one range for the whole arena | **include** |
| 6 | VS seed 44, battle state 0x16 frame 17: byte `0x8c2533fa` 80 -> 00 | writer `0x8c010fe4` is the struct copy `0x8c010fa0..0x8c011020`; its source is a stack local with uninitialised words and interrupt frames land there at timing-dependent moments (same class as the memcpy case in 4.2) | 64-byte destination `0x8c2533c8..0x8c253408` (player 2) and the identical player 1 block `0x8c24acec..0x8c24ad2c` | **restore** (player 1 by symmetry, not observed) |
| 7 | Survival, 4,500-frame runs with table v3 (`sc/traces/combo/x5_v3`, `x34_v3`): 4 mismatches at battle states 0x18 / 5 (seed 5), 1 at state 4 frame 1 (seed 34) | words `0x8c28f358..0x8c28f3ac`, `0x8c28f77c`, `0x8c28f7a4`: queue entries pointing into the stream buffers `0x8c27dc00..` and players `0x8c28ddf4`; first game-level read is the sound API (`FUN_8c0a7940` -> `0x8c0a9ab2` reads player state `0x8c28dd94`: 3 vs 5) | sound / stream request queue `0x8c28f344..0x8c28fd60` (the memtrace saw only the HUD byte `0x8c28f342` there), stream player structs `0x8c28dc00..0x8c28de28`, request table `0x8c28fd60..0x8c2907a0` | **restore** (was include / exclude): the game reads them, GD-ROM DMA advances them, the port reimplements audio. Seed 34 went from 1 mismatch to 0, seed 5 from 4 to 1 (6.5) |

The heap row that replaces the four heap windows of v2 (`0x8c8bc440..0x8c8c6440`, `0x8c8f0000..0x8c900000`,
`0x8cb30000..0x8cb70000`, `0x8cd91560..0x8cd915e0`):

```
0x8c8b0000  0x8cd9e480  include  heap arena: particle pools, per-player motion / skeleton / matrix work buffers
                                 (block addresses depend on character and mode; ends where the Kamui TA buffers start)
```

Table v4 size: 18 include ranges (7,875,664 bytes) + 11 restore ranges (19,252 bytes) = **7,894,916 bytes (7.5 MiB)**,
up from 3,090,204. A memcpy of that is about 1 ms, still cheap for an 8-frame ring (63 MB). Most of the arena is
immutable model data; a later `SC_MEMTRACE` pass over these combinations can shrink it to the blocks that are
written (not needed for correctness).

### 6.5 Open items and port requirements

- **Loading transition frames.** In the 4,500-frame run of seed 5, test 2742 (battle state 5, `state_frames` = 0xFFFFFFFF)
  is a ~100-vsync loading frame; run 2 took 99 vs 100 vsyncs and the pad bytes `0x8c24a4d8..0x8c24a52c` (device flags
  derived from Maple state) differ. This is emulator timing on a disc-load frame, not a rollback frame (rollback never
  spans a load). It is the only mismatch in any run with table v4 and it is not masked in the table.
- Port: the sound API branches on stream state (`FUN_8c0a7940`), so the port's audio layer must return a deterministic
  stream status per simulated frame, or the game's stream start / stop calls must be intercepted; the stream structs are
  `restore`, not simulation state.
- Port: `0x8c21fdbe` must be replaced like `sh4_ReadTimer` (requirement 1 in 4.1 covers both timer reads).
- Not tested: the other main-menu entries (Time Attack, Extra, Mission), costume variants beyond the button taps above,
  and Survival rounds beyond the third opponent. Throws and ring-outs are only covered if the random fight stream
  produced them.
