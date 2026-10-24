# SC port: static recompilation (SH-4 -> C)

Plan: `sc/docs/port-scoping.md`. This folder holds the translator, runtime and validation.

## M0 result (2026-10-15): translator validated on three functions

| Function | Address | Recorded calls | Result |
| --- | --- | --- | --- |
| `rng_next15` (LFSR + call to `sh4_ReadTimer`) | 0x8C0639C8 | 300 | 300/300 exact |
| `__sdivsi3` (`div0s` / 32 x `div1` + `rotcl` / `addc`) | 0x8C0FE6FC | 300 | 300/300 exact |
| `memset` (byte loop, delayed branches) | 0x8C0FED6C | 83 | 83/83 exact |

"Exact" = every integer register, T/S/Q/M, PR, MACH/MACL, GBR, FPUL, the stack and the
recorded memory ranges match the emulator's exit state, and the number of timer reads matches.

Mutation check (the harness must be able to fail):

| Injected bug | Result |
| --- | --- |
| `div1` T flag inverted (runtime) | 300 `__sdivsi3` failures: caught |
| `rng_next15` output mask 0x7FFF -> 0xFFFF | 169 failures (all calls with bit 15 set): caught |
| `rng_next15` arithmetic -> logical shift | 0 failures: harmless, the LFSR never has bit 31 set (no recorded state did), so both shifts agree on every reachable state |

## M0 FPU result (2026-10-15): floating point validated on 14 matrix/projection functions

The probe now records FR/XF banks, FPSCR and FPUL too; `m0/capture_oracle_fpu.sh` captured 200 calls
each of 14 functions (2,800 calls, autoplay battle, FPSCR = 0x00040001: round to zero, denormals
flushed). `m0/gen_harness_fpu.py` builds `m0/tests_fpu.c`: RAM = `1ST_READ.BIN` image + recorded
ranges (the code reads constants and its literal pools at run time), host MXCSR set from FPSCR,
calls between translated functions go through the dispatcher (`view_set_camera` calls two of them).

Result: **2,800 / 2,800 exact** (every FR and XF register bit for bit, FPSCR, FPUL, integer state,
memory ranges), at both `-O2` and `-O3`. Integer suite still 683/683.

Covered: `fmov.s` (all addressing modes, SZ=0), `fadd/fsub/fmul/fdiv`, `fcmp/gt,eq`, `fmac`,
`ftrv`, `fipr`, `fsca`, `fsrra`, `fsqrt`, `float`, `ftrc`, `fldi0/1`, `flds/fsts`, `fneg/fabs`,
`frchg` (XMTRX loads), `lds/sts FPUL/FPSCR`. No oracle coverage yet: `project_points_rev`
(0x8C0C09AE, never called in the run), `fschg` 64-bit pair moves, PR=1 double arithmetic.

Bug found by the oracle: `project_points` failed all 200 calls at `-O2` (projected Y overwritten)
but passed once debug prints were added. Cause: the runtime accessed float registers through a
`u32 *` cast, which breaks strict aliasing, so gcc reordered an `fmov.s` store. Fixed with
memcpy-based `frbits/set_frbits`; generated code and runtime must never type-pun through pointers.

Mutation check (runtime variants, FPU suite):

| Injected bug | Result |
| --- | --- |
| round to nearest instead of FPSCR.RM = zero | 1,413 failures in 8 functions: caught |
| `frchg` does not swap the banks | 1,000 failures (every XMTRX loader): caught |
| `ftrv` accumulated in float instead of double (Flycast) | 730 failures in 5 functions: caught |
| `fmac` as separate multiply + add (not fused) | 200 failures (`project_points`): caught |
| flush-to-zero (FPSCR.DN) off | 0 failures: no recorded call produced a denormal; kept on to match the emulator |

Reproduce (WSL, from `sc/port`; `fsca-table.h` comes from Flycast, GPL, not copied here):

```
python -I sh4c/sh4c.py sh4c/m0_fpu.c 8c063a00 8c03dfa0 8c03c73e 8c03c6ae 8c03c6d6 8c03c7c0 8c03c886 8c03dc38 8c0c08f2 8c0c0942 8c0c09ae 8c03e9c0 8c060f54 8c060f9c 8c060da0
python -I m0/gen_harness_fpu.py m0/oracle_fpu.jsonl m0/tests_fpu.c <same entries>
gcc -O2 -ffp-contract=off -w -I sh4c -I ~/sc-flycast-m0/core/hw/sh4 -o /tmp/m0_fpu sh4c/m0_fpu.c sh4c/fsca.c m0/tests_fpu.c -lm
/tmp/m0_fpu ../extracted/1ST_READ.BIN
```

`-ffp-contract=off` is required: contracted multiply-adds would differ from the SH-4 bit results.

## M0 sweep (2026-10-15): every function translated, compiled and linked

`sh4c/sweep.py build/sweep 8` translates all 3,251 functions of `functions.json` (plus 5
secondary entry points), writes them to `build/sweep/all_0..7.c` with a sorted dispatch table
(`dispatch.c`), and writes `build/sweep/sweep_report.json`. Counts below are for instructions
reachable from each entry, so literal pools that happen to decode as code do not count.

| | |
| --- | --- |
| Reachable instructions | 421,626 |
| Generated C | 441,095 lines, compiles with gcc `-O1` in about 15 s (8 files in parallel), 0 errors, 0 warnings, links |
| Functions with no unimplemented reachable instruction | **3,247 / 3,251** |
| Oracle regression after all translator changes | integer 683/683, FPU 2,800/2,800 |

Still unimplemented (all in library code, none on a game path seen so far):

| What | Where | Plan |
| --- | --- | --- |
| `stc.l ssr/spc`, `ldc.l ssr/spc` | 8C241420, 8C24156C, 8C241700 (exception/interrupt entry and exit in libtext) | HLE: interrupts are not emulated |
| opcode 0x003B (debug break) | 8C3923D8, a one-instruction stub | no-op or HLE |

Translator changes the sweep forced:

- **Body recovery by control flow.** Ghidra's ranges miss case bodies reached only through
  jump tables and a few tails (30 functions). `recover()` takes Ghidra's ranges plus
  everything reachable from the entry, stopping at other function entries.
- **`braf` jump tables** (`mova` / `mov.w @(r0,Ri)` / `braf`): 39 resolved to a C `switch` of
  gotos, entry count from the `cmp/hs` bound. The tables sit in data outside Ghidra's ranges.
- **`jmp @Rn`**: 500 sites tail-call a function entry (runtime dispatch), 568 are long branches
  inside the function (literal target, now a guarded `switch` + goto), 3 go through pointer
  tables (resolved the same way), 45 jump through a register value only known at run time,
  including the BIOS syscall vectors at 0x8C0000B0-BC, which become HLE. Static targets are
  only guesses: the emitted `switch` compares them with the real register, so a wrong guess
  falls back to the dispatcher.
- **Function entries not at the lowest body address** (16) start with a `goto`.
- **Fallthrough into the next function** (7 cases) becomes a tail call.
- **`bsr` into the middle of a function** (5 targets) gets a secondary entry point.
- New opcodes: `mac.l`, `mac.w`, `tas.b`, `movca.l`, `ocbi/ocbp/ocbwb` (no-op), `addv`, `subv`,
  `and/or/xor/tst.b @(r0,gbr)`, `stc/ldc(.l) sr`, `gbr`, `vbr`. SR keeps its control bits
  in `sr_ctl`; register bank switching is not modelled.
- Code outside the read-only blocks (6 small routines near the end of the image, referenced
  from library code) is translated from Ghidra's ranges, with literal loads at run time.

Known limits of the sweep: the generated code has not run as a whole program yet. The dispatch
table only covers entries known statically; a `jmp`/`jsr` to an address outside it stops in
`sh4_call` with the address, which is how the remaining dynamic targets will be found in M1.

## Broad oracle (2026-10-15): 600 functions, every call checked against the whole program

The probe now logs each recorded call's complete memory footprint, so a call can be replayed
in isolation through the whole translated program (callees included, via the dispatch table):

- `m0/capture_oracle_broad.sh` probes every function entry (3,250) during an autoplay fight,
  up to 10 calls each (`SC_PROBE_FILE`, `SC_PROBE_ACCESS`, `SC_PROBE_ARM`). Per call it records
  the entry/exit state (now including SR and the store-queue buffers), the first value read
  from every RAM byte, the final value of every RAM byte written, and the ordered list of
  hardware, store-queue and `pref` flush accesses.
- `m0/oracle_pack.py` packs it; `m0/replay.c` (`m0/build_replay.sh`) links every translated
  function and replays each call. RAM starts as 0xA5 poison plus only the bytes the emulator
  read; hardware reads return the recorded values; writes, flushes and their order must match;
  on exit every register and **all 16 MB of RAM** must equal the emulator's result.

Result (one 10-minute autoplay run; 5,739 replayable calls of 600 functions):

| | Calls | Functions |
| --- | --- | --- |
| Exact | **5,577** | 577 always exact |
| Calls into the BIOS syscall stub (0x8C001006) | 127 | HLE in the port |
| Thread context restore (0x8C102E10, reloads r15/PR) | 35 | HLE in the port |
| Other mismatches | **0** | |

So every call that stays inside game and library code matches the emulator bit for bit.
181 further calls were not replayable and were dropped by the probe: an interrupt taken during
the call (128), more than 64 KB or 4,096 hardware accesses (49), the SyncTest hook editing
RAM (4).

Mutation check on this suite (baseline 162 HLE failures):

| Injected bug | Failing calls |
| --- | --- |
| round to nearest instead of FPSCR.RM | 1,637 |
| `rd16` returns one byte | 890 |
| `fmac` unfused | 616 |
| `div1` T inverted | 424 |
| flush-to-zero off | 164 (2 more than baseline: now caught) |

What the broad run fixed or found:

- **Functions only reached through pointers.** Ghidra missed small functions that are only
  called through tables; the replay showed them as "call to an unknown address".
  `sh4c/extra_entries.txt` adds them: 19 found by the replay, plus 189 found by scanning the
  image for pointers to code right after an `rts`. The scan found the last chained misses before
  the oracle did.
- **Probe artefacts, not translator bugs.** Some Ghidra entries are also loop labels reached by
  plain branches; frames now open only on `jsr/bsr/bsrf` or a tail `jmp/braf`. Store-queue
  flushes write RAM behind the memory handlers; they are now recorded, and the SQ buffer
  contents at entry too, because some routines flush a partly written buffer.
- WSLg sometimes starts the emulator without running it; the capture script retries until
  the heartbeat appears. Probing all 3,250 entries needed a per-instruction bitmap test instead
  of a `std::map` lookup (20x slowdown otherwise).

Reproduce (WSL, from `sc/port`):

```
bash m0/capture_oracle_broad.sh 600 10          # ~10 min, writes /tmp/oracle_broad.jsonl
python3 -I sh4c/sweep.py build/sweep 8
bash m0/build_replay.sh
python3 -I m0/oracle_pack.py /tmp/oracle_broad.jsonl /tmp/oracle_broad.bin
/tmp/replay /tmp/oracle_broad.bin build/replay_broad.tsv
```

`REPLAY_ONLY=<fn>` and `REPLAY_TRACE=1` replay one function and print its call chain.

## M1 (2026-10-15): first native executable boots to the first VBlank wait and beyond

`m1/` builds `/tmp/m1`: every translated function (`build/sweep`) linked with a small runtime.
It loads `1ST_READ.BIN` and runs `start` (0x8C010000) natively, without an emulator.

| Piece | File | What |
| --- | --- | --- |
| Loader | `m1/m1.c` | RAM as Flycast's HLE BIOS leaves it (0x8C000000-0x8C00FFFF = 0xFF, syscall vectors 8C0000B0-E0 -> stubs at 8C001000-08, IP.BIN at 8C008000), `1ST_READ.BIN` at 8C010000, boot registers (r15 8D000000, SR 400000F1, FPSCR 00040001, VBR/GBR 8C000000), dispatcher with RAM mirrors folded (code also runs from the uncached 0xAC mirror), shadow call stack, last-calls ring, watchdog |
| Hardware | `m1/hw.c` | VRAM and AICA RAM arrays; every other address is a register file (writes stored, reads return them); PowerVR ID/revision preset; SPG_STATUS as a scanline counter (one line per read, 525 lines, vsync in lines 480-524); TMU counters count down; store queues and `pref` (RAM, VRAM, TA byte count); a register polled 200,000 times with no hardware write in between stops the run; `build/m1/m1_hw.txt` logs the first 20,000 accesses and a per-register summary |
| BIOS | `m1/hle.c` | GD-ROM syscalls answered synchronously (REQ_CMD, GET_CMD_STAT, EXEC_SERVER, INIT_SYSTEM, GET_DRV_STAT, G1_DMA_END, READ_ABORT, RESET, CHANGE_DATA_TYPE; PIO/DMA reads served from `disc/gdi/soulcalibur03.bin`, FAD = LBA + 150), system info, font and flash stubs (flash reads fail, the game falls back to defaults) |

Run (WSL, from `sc/port`): `bash m1/build.sh && cd build/m1 && /tmp/m1 ../.. 20`
(`M1_STOP_AT_VSYNC=1` stops at the milestone).

### What happens

1. `start` -> `main` (8C031420) -> `game_init` (8C031540) -> `sbInitSystem` (8C0C3940):
   system info and flash syscalls, then `km_InitDisplayMode` (8C221816).
2. **First VBlank wait: 8C2216A8 polls SPG_STATUS (005F810C)** until the scanline field wraps
   to 0 (start of frame). This is the M1 milestone; it is reached with the translated code
   only, about 40 dispatcher calls after boot.
3. With SPG_STATUS modelled, the run continues: Kamui programs the PowerVR (more than 600
   registers; it parks the CPU in a deliberate `bra .` unless the PowerVR ID/revision read back
   as 17FD11DB/11, so those are preset), GD-ROM init and the first DMA reads of the file system
   (FAD 45166-45172), the flash settings lookups, then `text_init` (8C064560).
4. It stops in Kamui (8C2380A0 -> 8C238018): a loop waiting for a request slot in the table at
   8C34E8B0 to be released. On hardware an interrupt handler releases it (most likely a
   DMA-end interrupt for the font texture upload); M1 has no interrupts, so this is where it
   waits now.

Two run-time facts the static work could not show:

- Cache-control code runs from the uncached mirror (0xAC2330BC); the dispatcher folds
  0x0C/0x8C/0xAC to the same function.
- `km_InitDisplayMode` ends with a tail jump through a hook pointer (8C246B60) whose default
  is 0x8C000000, the BIOS RAM area. In the emulator that area is 0xFF and execution slides into
  the syscall stub, which returns; M1 treats a call to 0x8C000000 as a return.

### Next (M2)

1. Interrupts: run the game's own interrupt dispatch at the points where hardware would raise
   them (VBlank in/out from the SPG model, end of GD/TA/texture DMA, TMU), instead of
   emulating the exception entry. First find the dispatcher behind VBR+0x600 and the request
   slot release that `text_init` waits for.
2. Carry on to `main`'s frame loop: `kmRender`'s wait is the next VBlank wait that matters.
3. Then the TA stream renderer (store-queue writes to the TA are already counted).

## M2 (2026-10-15): interrupts, threads and DMA; main's frame loop runs

M2 extends the M1 executable (same `m1/` folder, same build) until the game runs `main`'s
frame loop continuously: 62,196 frames in a 60-second run (about 1,000 frames per second; no
frame pacing yet), every frame calling `game_update`, `render_scene`, `kmEndScene`, `kmRender`
and `frame_end_input_audio` once and starting one render.

| Piece | Where | What |
| --- | --- | --- |
| Interrupts | `m1/m1.c` `take_interrupt` | The game's VBR (8C00F400, set by the IP.BIN bootstrap) holds stubs copied from 8C2413B8; VBR+0x600 stores INTEVT at 8C2416F4 and jumps to 8C241420, which saves the context and calls `*(VBR + 0x200 + (INTEVT - 0x200) / 8)` with r4 = INTEVT. M2 does that call from C before any dispatcher call when a Holly interrupt is pending, routed by IML6/4/2 (IRL 6 -> INTEVT 0x320 -> 8C23BD10, IRL 4 -> 0x360 -> 8C22E2AA) and allowed by SR (BL, IMASK), with IMASK=15 and FPSCR=00040001 inside and the whole CPU state restored after |
| Clock | `m1/hw.c` | A virtual cycle counter (100 per dispatcher call, 200 per hardware access) drives the beam: SPG_STATUS, V Blank-in/out interrupts (bits 3/4) at 60 Hz, TMU counters |
| Holly interrupt registers | `m1/hw.c` | ISTNRM/ISTEXT/ISTERR: set by events, cleared by writing 1s; ISTNRM bits 30/31 summarize EXT/ERR |
| DMA | `m1/hw.c` | ch2 (SB_C2DST: TA polygon/texture paths to VRAM, bit 19), PVR DMA (bit 11), G2 ch0-3 (AICA sound RAM, bits 15-18): each runs at once when started and raises its end interrupt |
| GD-ROM | `m1/hle.c` | DMA reads raise the GD-DMA interrupt (bit 14); `G1_DMA_END` acknowledges it and runs the registered callback, as the BIOS does |
| Tile accelerator | `m1/hw.c` | Store-queue and ch2 data sent to the TA is parsed into 32/64-byte parameters (end of list, global parameters, vertices; sizes from the global parameter). Each end of list raises the list's end-of-transfer interrupt (bits 7-10, 21); STARTRENDER raises render-done (bits 0-2) |
| SDK threads | `m1/threads.c` | The thread library's context save/restore (8C102E84 / 8C102E10) is `setjmp`/`longjmp`; each context gets a host coroutine (`ucontext`), a never-saved context starts a new host stack at its PR. Not exercised yet in the runs so far (contexts are saved, none restored) |

How each wait was found and what unblocked it:

1. `text_init` waited on a Kamui request slot: the font texture goes to VRAM by ch2 DMA; its
   end-of-DMA interrupt (bit 19, level 6) runs Kamui's handler, which releases the slot.
2. `game_update` (mode setup) waited on a file-system event at 8C37A60C. Probing every library
   function in the emulator showed the writer: 8C244140, called from the level-4 handler's list
   for **AICA DMA end** (bit 15). The game moves sound data to AICA RAM by G2 DMA; modelling the
   G2 channels unblocked it.
3. `kmRender` waits each frame for the end-of-list interrupts of the lists it sent to the TA;
   with the TA parser they arrive, and the loop runs on.
4. The GD-DMA interrupt first caused an interrupt storm: the BIOS, not the game, acknowledges it
   inside `G1_DMA_END`.

Where it stands: the game sits in `mode_autoload_mode` (game mode 31, VMU save auto-load),
rendering every frame. It waits for the memory card over the maple bus, which is not modelled;
the vsync counter (8C24A904, bumped by the game's VBlank callback) follows the frame count, so
the VBlank interrupt path works.

### Next (M3)

1. Maple bus: answer the maple DMA (bit 12) with a controller in port A and no VMU (or an empty
   one), so the game leaves the autoload mode, and feed pad input from the host.
2. Frame pacing to 60 Hz on the host clock instead of the virtual one.
3. The TA renderer: the parser already splits the stream into lists, global parameters and
   vertices; draw them with a host graphics API, with textures from the VRAM array.

## M3 (2026-10-16): maple bus, pads, threads, 60 Hz; the game plays into a fight

With a controller on the maple bus the game leaves the VMU auto-load, and scripted pad presses
take it through the title and menus into a battle (game mode 1), natively, in about
12,600 video frames. Paced, it runs at 59.3 frames per second (20-second run).

| Piece | Where | What |
| --- | --- | --- |
| Maple bus | `m1/maple.c` | Maple DMA (SB_MDST, or at V Blank when SB_MDTSEL = 1) walks the descriptor list and answers each frame: a standard controller in port A (Device Request -> 28-word device status, Get Condition -> buttons and analog bytes, Reset/Kill -> reply); empty expansion slots (no VMU) and ports B-D answer 0xFFFFFFFF. Then SB_MDST = 0 and the maple end-of-DMA interrupt (bit 12) |
| Pad input | `m1/maple.c` | `M1_PADS="frame:buttons,..."`: button sets (a b x y s u d l r, `-` = none) from a given controller poll on. Live host input comes with the renderer's window |
| SDK threads | `m1/threads.c`, `sh4c/sh4c.py` | The library's switch routines call save and later restore another thread in the same function, so a context resumes mid-function. The translator now emits a host `setjmp` in the caller's frame at every call to the save routine (`SETJMP_FUNCS`), keyed by the buffer; restore runs the translated register reload and `longjmp`s there (`_FORTIFY_SOURCE` off for this). A buffer never saved is a new thread: its own 1 MB host stack, starting at the buffer's PR, continuing through PR whenever its code returns. 8 threads, about 460,000 switches in a 30-second run; switches from inside interrupt handlers work |
| Pacing | `m1/hw.c` | Each V Blank-in waits for the next 1/60 s of the host clock (`M1_FAST=1` runs unpaced); more than 100 ms behind restarts the schedule |
| Mode log | `m1/hw.c` | Game mode changes (8C24A584) are printed with the video frame number |

Game modes seen in the run (scripted Start/A every 300 frames from frame 600):
0 -> 4 -> 5 -> 30 -> 31 (auto-load) -> 70 -> 71 (title) -> 10 -> 11 -> 14 -> 15 (game start and
menus) -> 16 -> 17 (data read) -> 0 -> **1 (fight)**.

Found on the way:

- **Threads start mid-table.** The thread entry points are a table of 4-byte stubs
  (`bra common; mov #k,r4`, 8C22EC16-8C22EC2E) reached through a computed address; they are in
  `sh4c/extra_entries.txt` now, with two more pointer-only functions found by the run
  (8C0BA098, 8C0D85BA).
- **Context switches from interrupts.** The maple interrupt wakes a thread from inside the
  level-4 handler; the interrupt state is saved with each context so a thread started there
  runs with interrupts enabled.

Regression after the translator change: broad oracle unchanged (all calls exact except the
162 HLE boundaries), M0 suites 683/683 and 2,800/2,800.

Run (WSL, from `sc/port`): `bash m1/build.sh && cd build/m1 && M1_PADS="600:s,606:-,900:a,906:-,1200:s,1206:-,1500:a,1506:-,1800:s,1806:-,2100:a,2106:-,2400:s,2406:-,2700:a,2706:-" M1_FAST=1 /tmp/m1 ../.. 30`

### Next (M4)

1. TA renderer: draw the parsed lists (opaque, punch-through, translucent with sorting,
   modifier volumes) with a host graphics API, textures decoded from the VRAM array, in a
   window that also takes live pad input.
2. Sound: the AICA (ARM7 driver + DSP) receives its data in the AICA RAM array but nothing
   plays it yet.
3. Widescreen and resolution per `sc/docs/phase3-widescreen.md`, then GGPO per
   `sc/docs/phase4-ggpo-state.md`.

## M4 (2026-10-16): the game draws natively in a window

The executable now opens an SDL2 window and draws every frame the game renders with OpenGL 3.3:
boot messages, title, menus, character select, intros and fights with their HUD
(`build/m4_vmu.png`, `m4_title.png`, `m4_select.png`, `m4_fight_intro.png`, `m4_fight.png`,
`m4_fight2.png`).

| Piece | File | What |
| --- | --- | --- |
| Display lists | `m1/ta.c` | The TA stream (store queues, ch2 DMA) parsed into polygons: global parameters with their ISP/TSP/TCW words, triangle strips in all vertex formats 0-14 (packed, float, intensity colour with face colours; 16-bit UVs; volume 0 of two-volume polygons), sprites as quads. One context per TA_ISP_BASE; STARTRENDER draws the context whose key is PARAM_BASE. Modifier volumes are parsed, not drawn |
| Textures | `m1/tex.c` | Decoded from VRAM: twiddled and scan-order (stride), VQ, mipmaps, ARGB1555/RGB565/ARGB4444/YUV422, 4- and 8-bit palettes; cached, re-decoded when a VRAM page or the palette changes |
| Drawing | `m1/render.c` | Screen-space rasterization like the PowerVR (1/w and attribute/w interpolated linearly, divided per pixel, depth from 1/w), PowerVR depth modes and culling modes mapped to GL, opaque / punch-through (PT_ALPHA_REF) / translucent passes with the PowerVR blend factors, texture instructions, offset colour, UV clamp/flip, filtering, background plane colour. Render-to-texture (FB_W_SOF1 bit 24) draws offscreen and writes the result into VRAM in the framebuffer pack format |
| Window and input | `m1/render.c` | 4:3 picture in a resizable window; keyboard (arrows, Z/X/A/S = A/B/X/Y, Enter = Start) and SDL game controllers feed the maple controller live, together with `M1_PADS` scripts; `M1_SHOTS` saves frames, `M1_DUMPLIST` dumps a frame's display list and VRAM, `M1_HEADLESS=1` runs without a window |
| VRAM layout | `m1/hw.c` | VRAM kept in the 64-bit (texture) layout; the CPU's 32-bit path (0x05xxxxxx) and 32-bit TA texture writes interleave the two 4 MB banks as on hardware (Flycast `pvr_map32`); texture-path DMA follows SB_LMMODE0/1 |
| Hardware details found on the way | `m1/hw.c`, `m1/hle.c` | PT_ALPHA_REF resets to 0xFF (else every transparent texel of punch-through text shows); video cable from PDTRA bits 8-9 (`M1_CABLE`, default 3 = composite, as Flycast); BIOS ROM area with the system font at 0x00100020 (Flycast's replacement font, loaded at run time from the local Flycast checkout, not part of the project); flash syscalls from an image when `M1_FLASH` is given |

Speed: in the menus about 50-55 rendered frames per second unpaced with the window (D3D12
through WSLg on the RTX 4060), so a little under 60 Hz there; fight speed has not been measured.
The renderer draws strip by strip with a state change each, which is the obvious place to gain.

### Text and HUD: three bugs, found by comparing with the emulator

Text and parts of the HUD first sampled the wrong VRAM. Every texture M1 uploaded was
byte-identical to the emulator's (oracle Flycast with `SC_VRAM_DUMP_AT`), but placed elsewhere:
the first texture went to VRAM 0 instead of 0x35F580 (`SC_LOG_DMA`), framebuffers to 0x125000
instead of 0x200000.

1. **SB_SBREV.** Snapshots of Kamui's globals around `km_InitDisplayMode` (`M1_SNAP` in M1, a
   `SC_PROBE_MEM` capture in the emulator) had identical inputs and exactly one difference on
   exit: 8C379AD4, which Kamui sets from the system bus revision register 005F689C. M1 returned
   0 (unwritten register); the hardware returns 0x0B. That one value selects Kamui's video setup
   and with it the whole VRAM layout. M1 now presets the system bus reset values (as Flycast's
   `sb_Reset`); the layout then matches the emulator exactly.
2. **ISP texture bits come from the PCW.** The TA replaces bits 25-22 of the ISP word (texture,
   offset, gouraud, 16-bit UV) with the polygon's PCW bits 3-0; the game sends ISP words with
   them clear, so portraits, icons and many HUD parts were drawn untextured.
3. **User tile clip and translucent sorting.** Menu text is clipped to its panel with TA user
   clip rectangles (PCW bits 17-16), now applied as scissors, and sits behind the panel in
   submission order: the PowerVR sorts translucent polygons by depth (unless ISP_FEED_CFG
   presort is set), now done per strip.

Other gaps: modifier volumes (shadows), fog, per-pixel (rather than per-strip) translucency
sorting, "outside" user clips and the YUV converter path (0x10800000) are not done; sound is
silent. With Flycast's flash settings (`M1_FLASH`) the start-up takes a path M1 does not
model yet, so flash stays off by default.

Run (WSL, from `sc/port`):

```
bash m1/build.sh
cd build/m1 && GALLIUM_DRIVER=d3d12 SDL_VIDEODRIVER=x11 ~/m1_build/m1 ../.. 600
```

## Widescreen and higher resolution (2026-10-16)

**16:9 only since 2026-10-17**: the 4:3 mode, `M1_WIDE`, the `[video] widescreen` setting and
the launcher's checkbox are gone; everything below always applies. Before that, `M1_WIDE=1`
showed the game in 16:9 (window 1280x720 by default, any size with
`M1_WINDOW=WxH`; the picture is letterboxed to 16:9 inside it), following
`sc/docs/phase3-widescreen.md` and the rule that textured 2D is never stretched
(`build/w_vmu.png`, `w_title.png`, `w_select.png`, `w_fight.png`, `w_fight2.png`).

- **3D: Hor+.** At every VBlank `g_aspect_scale_x` (0x8C266C28) is set to 0.75, so the game
  projects a wider view into its 640x480 virtual screen, which is shown across 16:9.
- **2D is recognised by the code that drew it.** The dispatcher sets a class while a known 2D
  function runs (HUD `hud_battle_draw`; text and primitives; the screen code at
  0x8C0E7940-0x8C0EB340; the tiled menu wallpaper `menu_bg_scroll_draw`). Store-queue TA
  input takes the current class. Kamui first builds its vertex buffers in RAM, with store-queue
  flushes as well as plain writes, and sends them by DMA later, so with `-DSH4_TAG_WRITES` every
  write to the buffer window (0x0CD00000-0x0CEFFFFF, `M1_TAGWIN=lo,len`) tags its 32-byte slot,
  and the DMA hands the tag to the TA parser.
- **Rules** (flat polygons only; a polygon with differing depths is 3D):
  - menus and screens: compressed by 0.75 about the centre, so they are pillarboxed;
  - fight HUD: compressed about 0, 320 or 640 by position (centre x below 280, between, above
    360);
  - text: anchored by its whole run of glyphs (same draw call, same line, no wide gap), so a
    string is never split between anchors. One call draws "STAGE 1", the elapsed time and
    "INSERT COIN", which split into three runs;
  - tiled wallpaper: compressed like other 2D, then drawn twice more, one 1024-pixel wrap
    period to either side, so it fills the width undistorted;
  - only untextured full-width fills stretch.
- **Meshes at the screen sides.** The stage mesh projector 0x8C03A9CC returns how many
  vertices fall within 320 of the screen centre, and its nine callers skip the mesh when none
  does. In Hor+ this dropped big near-camera meshes that cover the corners with every vertex off
  screen (black wedges). In wide mode the dispatcher wraps it: if the count is 0, any vertex in
  front of the camera makes the mesh visible. The projector itself runs unchanged.

Three renderer fixes came out of this, and they apply in 4:3 too. The phase 3 doc had found no
horizontal culling; the runtime showed the projector's count and these sentinel vertices.

1. **Screen-space rasterization.** The projector writes vertices behind the camera as
   (X * 1e30, Y * 1e30, 1/w = 1e30). The PowerVR rasterizes in screen space and draws these out
   to the screen edge; GL's homogeneous clipping with w = 1/z put them near the screen centre or
   dropped them. Vertices now go to GL with w = 1; 1/w and attribute/w are interpolated linearly
   and divided per pixel, and the depth is written per pixel from 1/w.
2. **Non-finite vertices.** A polygon wholly behind the camera can overflow to inf. The PowerVR
   cannot set it up and drops it; the shader now discards every triangle with such a vertex
   (one of them covered the whole screen otherwise).
3. **Culling.** ISP culling modes 2 and 3 (bits 28-27) are applied as GL face culling. The game
   relies on them for polygons that straddle the camera plane. The battle timer text, "Select
   Your Character" and its countdown, which used to be lost to w-clipping, now draw in 4:3 as
   well (`build/m4_fight3.png`).

`wide` changes what the game draws, and so the timing, so after the menus a wide run takes a
different but deterministic path from a 4:3 run (another opponent for the same pad script).

Not done: MSAA. `M1_MSAA=4` gives black frames under WSLg/D3D12, so it is off unless asked
for; higher resolution comes from the window size. Culling mode 1 (cull if small) is not modelled.
The rules have been checked on the VMU message, title menu, character select and one stage
(Voldo's); other stages, the gallery and the mission screens have not been looked at.

Regression after these changes: broad oracle 5,577 exact (the same 162 HLE boundaries),
integer 683/683, FPU 2,800/2,800.

Run: `cd build/m1 && M1_WIDE=1 GALLIUM_DRIVER=d3d12 SDL_VIDEODRIVER=x11 ~/m1_build/m1 ../.. 600`.

## Native Windows build, speed and rollback state (2026-10-16)

### Windows executable

`bash m1/build_win.sh` (Git Bash, from `sc/port`) builds `%LOCALAPPDATA%/sc-m1/m1.exe` with the
project-local toolchain in `engines/`: llvm-mingw 20260922 (clang 23, UCRT) and SDL2 2.32.10
(MinGW development package; `SDL2.dll` is copied next to the exe). The BIOS font is unpacked
there from the local Flycast checkout in WSL (`\\wsl.localhost\Ubuntu\...`), as on Linux.
Run: `cd build/m1 && "$LOCALAPPDATA/sc-m1/m1.exe" C:/path/to/sc 600`.

Porting the runtime:

| Piece | Linux | Windows |
| --- | --- | --- |
| SDK thread contexts | glibc `setjmp` / `longjmp` across stacks | `m1/sjlj_win64.S`: the CRT `longjmp` unwinds through SEH and clang's `__builtin_setjmp` loses locals on Win64 (tested), so a small assembly pair saves the callee-saved registers (rbx, rbp, rdi, rsi, r12-r15, xmm6-15, MXCSR, x87 CW) and jumps without unwinding |
| New SDK threads | `ucontext` stacks | fibers (8 MB). The stack limits live in the TEB, so a restore whose context was saved on another fiber switches to that fiber, which then makes the jump itself |
| OpenGL 3.3 | linked directly | 26 functions loaded with `SDL_GL_GetProcAddress` (opengl32 exports GL 1.1) |
| Watchdog, pacing | `alarm`, `clock_nanosleep` | timer thread + flag checked at V Blank; `QueryPerformanceCounter` + `Sleep` with 1 ms timer resolution |

The translator emits `sh4_setjmp` for context saves (glibc `setjmp` on Linux, the assembly
version on Windows).

Result: frames 2000 (character select) and 4900 (fight) match the WSL build to ±1 in a few
hundred pixels. The only real difference is one 3-pixel row at the clipped bottom edge of the
scrolling title-menu text, where the native NVIDIA driver and WSL's D3D12 translation round an
edge on a pixel boundary differently. The game-mode sequence is identical, and runs are
deterministic. MSAA (`M1_MSAA=4`) works on the native driver, so the WSLg black-frame problem
is gone.

### Speed

`M1_PERF=1` reports every 300 frames how wall time splits between the game (everything between
two renders) and drawing, plus texture decodes per frame. Native, unpaced (`M1_FAST=1`), RTX 4060:

| Screen | Game | Draw |
| --- | --- | --- |
| Menus, title | 0.9-1.3 ms | 0.03-0.16 ms |
| Character select | 1.9 ms | 0.2 ms (was 25 ms) |
| Fight (about 3,450 polygons) | 3.2 ms | 0.4 ms |

A fight frame takes about 3.6 ms of the 16.7 ms budget, so a GGPO rollback can re-simulate
several frames per displayed frame. Batching the renderer is not needed. Character select was
slow because the game rewrites palette entries every frame, and the texture cache kept one
generation number for the whole palette RAM, so every paletted texture (ten, five of them
512x512) was decoded again each frame. Each paletted texture is now checked against a hash of
the palette bank it actually uses. A changed VRAM page is confirmed against a hash of the
texture's bytes as well. `M1_TEXHASH=0` turns both checks off (output is identical either
way), and `M1_TEXLOG=n` lists the textures decoded in render n.

### Rollback state and native SyncTest

`m1/state.c` implements the state of `sc/docs/phase4-ggpo-state.md`:

- **Save point:** the top of main's loop, 0x8C03144E. The translator emits `sh4_hook(c, addr)`
  at the addresses in `sh4c/hooks.txt`. There the host call stack is the same every frame, so
  loading a state and carrying on re-runs the frame.
- **State (7.9 MB):** the `include` and `restore` ranges of `sc/ggpo/savestate_ranges.tsv`, the
  CPU registers, the fake-timer counter, and the runtime's virtual clock, hardware register
  values and store queue (`hw_state_save` / `hw_state_load`), so interrupts land at the same
  points when a frame is re-run.
- **Fake timer:** the dispatcher answers `sh4_ReadTimer` (0x8C220060) and the game-side TCNT0
  read 0x8C21FDBE from effect code (`pr` in 0x8C068000-0x8C0A0000) with `n++ * 0x9E3779B1`, as
  the emulator SyncTest did. It is on by default; `M1_REALTIMER=1` turns it off. It changes the
  RNG, and with it the scripted run's opponent.
- **Boot stack fix:** M1 started the game with reios' r15 = 0x8D000000. The IP.BIN bootstrap
  (which M1 skips) moves it to 0x8C00F400, where every emulator trace has it and where the
  table's stack `restore` range is. M1 now starts there too.

`M1_SYNCTEST=1` (fights; `=all` for every frame) saves at the loop top, runs the frame,
checksums, loads, runs it again and compares. Mismatches list the first differing words.

| Run | Result |
| --- | --- |
| Windows, scripted Arcade fight, fake timer | **5,415/5,415** frames identical |
| Linux (WSL), same, headless | **4,620/4,620** |
| Windows, `M1_REALTIMER=1` (control) | 2,483/2,484: test 176 diverges in the LFSR 0x8C159060, effect state and CPU AI |

The control mismatches less often than in the emulator (0/120 there), because the native timer
comes from the virtual clock, which the state restores. It still shows that the test detects a
desync and that the fake timer is needed.

Regression after these changes: broad oracle 5,577 exact (the same 162 HLE boundaries),
integer 683/683, FPU 2,800/2,800.

`M1_STATE=full` saves all of RAM except the table's `exclude` buffers, for testing outside
fights. The state now also holds the GD-ROM request in progress (`hle.c`) and the Maple
condition count that drives `M1_PADS`. `M1_SYNCTEST=all` with full RAM gives 588/588 until
the first rewind across a disc load hangs the game. A rewind restores RAM and the runtime but
not the SDK threads' host stacks, and during loads those threads are working. This is the
emulator doc's "rollback never spans a load", here as a hard rule: rollback only in fights.

## Online versus: GGPO (2026-10-16)

GGPO (`engines/ggpo`, pond3r/ggpo at 7ddadef, MIT) is built as a static library by
`bash m1/build_ggpo.sh` and linked into the Windows exe (C++ runtime static). There are two local
changes, both marked "sc port": one MSVC-only class qualification clang rejects
(`network/udp_proto.h`), and `ggpo_set_prediction_window(session, frames)`.

```
M1_NET=local_port,player,remote_ip:remote_port[,fight_delay]   M1_NET_MENU_DELAY=n (default 8)
```

`m1/netplay.c`:

- **Driver fiber.** GGPO's rollback callback (`advance_frame`) must simulate a frame
  synchronously, but the game owns its loop. So GGPO runs on a fiber of its own. The loop-top
  hook switches to the driver, and the driver runs a frame by switching back to the game until
  the next hook. `load_game_state` loads the state while the game waits at the hook, which is
  the rewind the SyncTest checks. Re-simulated frames are neither drawn nor paced; the driver
  paces to 60 Hz and runs `ggpo_idle` while it waits.
- **Rollback in fights, lockstep elsewhere.** In game mode 1 the prediction window is 8 frames
  and the input delay is `fight_delay` (default 2). Outside fights the window is 1 and the delay
  is `M1_NET_MENU_DELAY`, so every frame runs on confirmed inputs. Menus, character select and
  loading therefore never roll back, and run at full speed while the menu delay covers the
  one-way latency. GGPO's input queue repeats or drops local inputs when the delay changes.
- **Inputs.** Each player's button word (2 bytes). Maple serves controllers in ports A and B
  from the synchronised inputs. Local input is the window's keyboard / pad, plus `M1_PADS`
  counted in netplay frames.
- **Desync check.** Every 60 frames each side prints the state checksum of a frame 10 frames
  back, which is past the prediction window and therefore final. The two logs must match.

Tests (two instances on one machine over UDP on localhost; `ggpo.network.delay` is GGPO's own
send-latency simulator, in ms):

| Run | Frames | Rollbacks (P1 / P2) | Checksums |
| --- | --- | --- | --- |
| no added latency, P1 scripted into an Arcade fight | 6,470 | 0 / 0 | identical |
| 50 ms, P1 mashing in the fight | 9,013 | 0 / 6 | 149/149 identical |
| 100 ms, both players mashing through the fight | 10,126 | 210 / 210 (423 / 630 frames re-simulated) | **167/167 identical** |
| 100 ms, windowed and wide, same scripts | 5,911 | 138 / 130 | display frame 5200 byte-identical on both peers (`build/net_fight.png`) |

Play on one machine (each in its own folder; keyboard: arrows, Z/X/A/S, Enter):

```
cd build/net1 && M1_NET=7000,1,127.0.0.1:7001 "$LOCALAPPDATA/sc-m1/m1.exe" C:/path/to/sc 3600
cd build/net2 && M1_NET=7001,2,127.0.0.1:7000 "$LOCALAPPDATA/sc-m1/m1.exe" C:/path/to/sc 3600
```

Over the internet, use the peer's address and an open UDP port on both sides, and set
`M1_NET_MENU_DELAY` to at least the one-way latency in frames.

Not done yet: sound, which will need its commands dropped during re-simulated frames; a
spectator mode; and a session that starts in a fight or reconnects. Both peers must start from
boot together, and a disconnect stops the game.

## Sound: the AICA (2026-10-16)

The game uploads Sega's standard Katana sound driver ("1998,(C)SEGA ENTERPRISES 1999.03.18:
DIGITALMEDIA", Manatee) into sound RAM and talks to it through shared memory. The port runs
that driver unchanged on an emulated sound chip.

| Piece | File | What |
| --- | --- | --- |
| ARM7 core | `m1/arm7.c` | ARMv4 ARM-state interpreter (no Thumb, no coprocessor): all data processing and shifter forms, MRS/MSR, multiplies, word/byte/halfword/signed loads and stores, LDM/STM with user-bank and SPSR forms, SWP, B/BL/BX, SWI, undefined trap, banked modes, FIQ. 512 instructions per output sample (22.5792 MHz / 44,100). An idle skip ends a sample early when the ARM returns to an instruction with the same registers and has changed no memory (the driver's command-polling loop), so it costs 0.9 ms per frame instead of 3.4 |
| Chip | `m1/aica.c` | Sound RAM (2 MB), registers, real-time clock (from the virtual clock), timers A-C, the ARM interrupt controller (level L latched until the driver acknowledges with M, as on the chip), SH-4 interrupts (ISTEXT bit 1) |
| Channels | `m1/aica.c` | 64 channels: PCM16, PCM8, Yamaha ADPCM with loop state, forward loops, LP flag, pitch (OCT/FNS), amplitude envelope (exponential attack from 0x280, decays, release, key scaling), filter envelope and resonant low-pass (Neill Corlett's description), TL, DISDL/DIPAN, IMXL/ISEL to the DSP; MSLC readback of SGC/EG/CA. Entering release clears KYONB, as on the chip |
| DSP | `m1/aica.c` | The 128-step effects processor: TEMP/MEMS/MIXS, multiply-accumulate with shifter and saturation, ring buffer in sound RAM with the 16-bit float format, EFREG outputs mixed with EFSDL/EFPAN (the game's "e-reverb" program) |
| Output | `m1/aica.c` | SDL audio, 44,100 Hz stereo; `M1_WAV=file` writes a WAV as well; `M1_NOSOUND` / headless: none. Re-simulated netplay frames run the chip but are not heard |

Levels and behaviour were checked against Flycast (oracle hook `SC_WAV`, new
`m0/flycast-hooks/apply_wav_hook.py`, and `SC_ARAM_DUMP` for sound RAM). The driver's bank
table at 0x14000 matches Flycast's (sequence bank 0x10AB00, instruction bank 0x10AB40, DSP and
output banks). Loudness: Flycast's fights measure RMS 4,500-5,900 with rare clipping, the port
5,300-7,100 (different fights and characters); menus 3,700-4,700 against 3,300-6,500.
Listening excerpts: `build/sound_menus.wav`, `build/sound_fight.wav`.

Four bugs along the way, each found by comparing with the emulator:

1. **Busy-wait loops took no virtual time.** After starting the ARM, the sound library waits
   with a 2-million-iteration loop (about 30 ms on hardware) and then writes the driver's bank
   table. The virtual clock only advanced at calls and hardware accesses, so the game wrote the
   table while the driver was still initialising, and the driver's init then zeroed the music
   bank entries: every fight note played the driver's built-in square wave. The translator now
   charges each tight loop iteration (backward branch, at most 32 instructions, no call) its
   length to the virtual clock (`sh4_loop_cycles`, drained at the next call or access; 828
   loops). `M1_NOLOOPTIME=1` turns it off.
2. **Store-queue writes cost like bus accesses.** Each 32-bit write into the SH-4's on-chip
   store queue was charged 200 cycles. Kamui fills its vertex buffers that way, about 128,000
   writes per fight frame, so a fight frame took about 9 V Blanks of virtual time: the game
   ran at a fraction of its speed in virtual time all along (unnoticed in unpaced runs; the
   pacing would have shown about 7 frames per second in fights). They now cost 2 cycles: fights
   run at 1.00 V Blank per frame, character select at its real length.
3. **A finished sound kept KYONB set.** KYONEX applies every channel's KYONB, so each key-on
   restarted all finished one-shots; fights were twice as loud as Flycast and clipped 7% of
   the time.
4. **Channel level.** A full-level channel contributes its whole sample (Flycast shifts the
   sample up 4 bits before its `>> 19`), the DSP input is the sample << 4, and DIPAN bit 4
   attenuates the right side.

Rollback: the sound chip (sound RAM, registers, channels, DSP, ARM) is part of the native state
(now 10.1 MB). The GD-ROM request state is not: the music streams from disc on an SDK thread
whose host stack a rewind cannot restore. With sound running, a SyncTest leak report
(`M1_SYNCLEAK=1`: RAM changed by the first run and not restored) found two regions the table
missed, now in `savestate_ranges.tsv` v5: the heap below the arena (effect pools move there,
0x8C8200C0-0x8C8B0000, include) and the SH-4 sound library's globals (0x8C246B00-0x8C246C00
around the Kamui block, restore).

| Run | Result |
| --- | --- |
| SyncTest, fights, Windows | **4,219/4,219** |
| SyncTest, fights, Linux | **2,488/2,488** |
| Netplay, two peers, 100 ms simulated latency, both mashing | 10,129 frames, 203 / 210 rollbacks, **167/167 checksums identical** |
| Speed, fight frame (Windows, unpaced) | 4.4 ms (0.9 ms sound chip) |

Diagnostics added: `M1_AICAKEYLOG=from-to` (key-ons with their registers), `M1_AICADUMP=sample`
(playing channels), `M1_AICASTAT` (loudness per channel and DSP), `M1_ARMPROF` (ARM time by
address), `M1_ARMRATE`, `M1_NOIDLE`, `M1_NODSP`, `M1_NOARM`, `M1_CMDLOG` (driver command
traffic), `M1_ARAMDUMP=frame`, `M1_GDSPEED=bytes/s` (disc transfer time, default instant).

Not done: LFOs (vibrato, tremolo), AICA-internal DMA, MIDI. Timing changes from fixes 1-2 shift
when scripted pad presses land, so screenshots and frame numbers in the earlier sections no
longer match current runs (the scripted run now reaches the fight at frame 4,979).

## Player build (2026-10-16)

`bash m1/build_win.sh` now also links `Voldorium.exe` (the same program as a GUI executable:
no console; its log goes to `m1_log.txt` next to it). `bash m1/make_player.sh` assembles
`build/player/`: `Voldorium.exe`, `m1.exe`, `SDL2.dll`, `savestate_ranges.tsv`, the BIOS font
when this machine has it, a starting `voldorium.ini` and `README.txt` (player instructions,
online checklist). No game data goes in.

| Piece | File | What |
| --- | --- | --- |
| Settings | `m1/config.c` | `voldorium.ini` next to the executable: `[game]` disc (.gdi) and font, `[video]` fullscreen / window / msaa (16:9 always), `[audio]` enabled / volume, `[keys]` (SDL key names), `[net]` port / peer / player / delay / menu_delay. Each fills the `M1_*` variable the runtime reads unless it is already set, so development runs keep control |
| Disc | `m1/config.c`, `m1/hle.c` | The game files come from the disc dump: the `.gdi` names the data track, IP.BIN is its first 16 sectors and 1ST_READ.BIN is found through ISO 9660 (identical to the extracted files). No `extracted/` folder is needed |
| Command line | `m1/config.c` | `m1 <sc folder> [seconds]` as before; `m1 [--disc file.gdi] [--host port] [--join address:port] [--port p] [--player 1\|2] [--delay n] [--seconds n]`; no arguments: the game, after a one-time file picker for the disc dump when `[game] disc` is missing or gone (2026-10-18; the launcher window, `m1/launcher.c`, was retired: its settings live in the game's menus, its source kept in `m1/retired/`, not built) |
| Window and input | `m1/render.c` | Alt+Enter or F11 toggles fullscreen (remembered), Esc quits, keys from `[keys]`, controller left stick as D-pad, hot-plugged controllers |
| Netplay status | `m1/netplay.c` | Window title: "waiting for the opponent at ..." while connecting, then ping and rollback count |
| Audio | `m1/aica.c` | A 46 ms queue cushion (playback pauses to refill if it ever runs dry; 2 underruns in 120 s paced, from 120), at most 0.1 s latency, `[audio] volume` |
| Log | `m1/m1.c` | In the player form the per-DMA / per-read lines are off (`M1_VERBOSE=1` turns them on); an unexpected stop (an error) shows a message box; the end of an online match does not (back to the main menu, the reason in its help line) |

Checked: the player form boots from the disc dump with the ini's settings; two copies started
8 s apart with `--join` connect and keep identical checksums; the launcher renders as intended
(`M1_LAUNCHER_SHOT`); paced play holds 60 frames per second with sound. Not checked: a session
between two machines over the internet (see `build/player/README.txt` for the checklist).

## Shadows, fog, intro backdrops, LFOs (2026-10-16)

- **Shadows** (`m1/ta.c`, `m1/render.c`): the TA now keeps the opaque list's modifier volume
  triangles (with each global parameter's volume instruction, ISP 31-29, and "last" flag, PCW
  bit 6) and each polygon's shadow bit (PCW bit 7). The game uses the cheap-shadow mode
  (FPU_SHAD_SCALE 0x160: darken by 0x60/256). After the opaque and punch-through lists:
  shadow-flagged pixels are marked in stencil bit 7; each volume's triangles OR (open volume)
  or XOR (closed) a parity bit where they are in front of or at the scene's depth; a volume's
  end (mode 1 inside, 2 outside) resolves its footprint into bit 1; a full-screen quad darkens
  pixels with both bits. The game's shadow triangles lie exactly at the floor's depth, so the
  test is "in front or equal" (`GL_LEQUAL`; a strict test, as in Flycast's renderer, found
  nothing). `M1_NOSHADOW`, `M1_SHADOWDEBUG=1..5` (flagged pixels, inside pixels, volume
  triangles in colour, stencil readback) help debugging.
- **Fog**: 98% of fight polygons use per-vertex fog (TSP 23-22 = 1): the colour moves towards
  FOG_COL_VERT by the offset colour's alpha, in the fragment shader.
- **Intro backdrops** (widescreen): the intro's skies are flat polygons at 1/w 0.001 drawn by
  code not classed as 2D, and were pillarboxed. A far (1/w < 0.01) flat class-0 polygon that
  covers the whole width is now drawn as part of the 3D scene (full 16:9); a centred picture
  at that depth (the framed wall scene) and menu pieces stay undistorted.
- **LFOs** (`m1/aica.c`): per-channel vibrato (PLFOS) and tremolo (ALFOS) with saw, square,
  triangle and noise waveforms at the published LFOF frequencies; reset at key-on, held by
  LFORE.

Not checked against the no-stretch rule yet: the gallery, mission and option screens (the test
scripts do not reach them).

## Music drop-outs: ADPCM long streams (2026-10-21)

The intro's music (and stage music) cut out and jumped in level. Measured against the oracle
Flycast with its WAV hook (`SC_WAV`, `m0/flycast-hooks/apply_wav_hook.py`): ours swung between
-8 and -35 dB where Flycast stayed at -11 to -18. The music is two streamed channels (left,
right) in PCMS 3, "ADPCM long stream": the loop LSA-LEA is a ring buffer the sound driver keeps
refilling. At each lap `channel` restored the decoder state saved at LSA (right for PCMS 2, which
repeats the same data) and set `have` back, but in stream mode the state had not been saved, and
`fetch` only decodes forward from `have`: after the wrap it decoded nothing until the position
passed the old end again, holding one value for a lap. Now PCMS 3 decodes on from LSA without
restoring anything (as Flycast, `sgc_if.cpp`, `PCMS == ADPCM` only). Level now follows Flycast
within 1.0 dB. The DSP reverb adds about 2.7 dB on top and clips about 66 samples a second in
loud passages; Flycast's oracle runs with its DSP off (`aica.DSPEnabled` is false by default),
so its level cannot be checked against it.

Footage: `M1_RECORD=out.mp4` (render.c `video_record_vblank`): every V Blank, the last frame at
`M1_RESOLUTION` goes to ffmpeg (`M1_FFMPEG`) at 60 fps, black before the first frame, so it
starts with `M1_WAV`; `M1_RECORD_FROM=n` skips the first n V Blanks. A file that exists gets a
`-1`, `-2` suffix (a lobby match restarts the program), for the WAV too.

## Arcade freeze: wait loops and interrupts (2026-10-20)

Arcade mode froze after a few fights. A hang report (new: `m1.c` `hang_thread`, after 8 s
without a frame the log gets the call stack and the last dispatcher calls; `M1_HANG_EXIT=1`
exits for tests) showed `battle_state_ko` (0x8C024E00, the KO slow motion) spinning at
0x8C024F6A with no calls: it clears a flag at +576 of its state, calls 0x8C0AC666 to start a
job, and waits for the flag. Usually the job finishes inside the call; when it is completed by
an interrupt handler or another SDK thread, the wait never ended, because interrupts are only
taken at the top of `sh4_call`, and a tight loop without calls (sh4c `loop_jump`) only added to
`sh4_loop_cycles`. Tight loops now call `sh4_loop_poll` every 4096 instructions (run the clock,
take a pending interrupt, as a call does), so a wait behaves as on hardware: 896 loops in the
sweep. The M0 replay harness has an empty `sh4_loop_poll` (single calls, no interrupts).

`tests/arcade.sh` plays arcade start to finish (`M1_TEST_P2KO=1`: in fights player 1 keeps full
life and player 2 a sliver, so player 1 wins on time; writing 0 does not end a round, the KO
check runs on hits). Before the fix it froze in the 7th fight; after it, 9 fights through the
last stage, no hang.

## VSync (2026-10-19)

Swaps used to be immediate (`SDL_GL_SetSwapInterval(0)`; the 60 Hz host clock in `hw.c
pace_frame` / `netplay.c wait_next_frame` paced frames), so the picture tore. Now `[video]
vsync` (M1_VSYNC, on by default; GRAPHICS menu line VSYNC ON / OFF) asks for adaptive VSync
(interval -1, EXT_swap_control_tear), else plain VSync. The frame clocks run at
`video_pace_hz()`: 62 when VSync is on and the display is 58-62 Hz, so the display's refresh sets
the pace (two 60 Hz clocks would drift apart and drop a frame every few seconds), else 60 (a
120-180 Hz display: the swap waits at most one short refresh; G-Sync / FreeSync in full screen
show each frame when ready). The refresh rate is read again on a full screen / window switch.
Unpaced runs (M1_FAST) and headless runs never wait.

Measured on a 60 Hz display (`M1_FRAMETIME=1`: time between swaps every 300 swaps):

| Run | Mean | Frames over 25 ms |
| --- | --- | --- |
| Arcade, VSync off | 16.66 ms (15.8-17.6) | 0 outside loads |
| Arcade, adaptive VSync | 16.66 ms (16.4-16.9) | 0 outside loads |
| Netplay pair in windows, VSync off | 17.0 ms | 858 / 1298 of 9900 |
| Netplay pair, plain VSync | 17.9 ms (late re-simulated frames waited a refresh: 6% slow) | 220 / 259 |
| Netplay pair, adaptive VSync | 16.95 ms | 269 / 271 |

Both peers stayed in sync in every run (164/164 checksums). The slow netplay frames are
rollbacks of two copies sharing one PC with 100 ms simulated latency.

## Missing text, colours, smooth text (2026-10-19)

- **Missing text** (gallery descriptions, the VS screen's score and DRAW count, TEAM BATTLE's
  title, the gallery's "?" thumbnails): punch-through polygons with PT_ALPHA_REF 255 (only fully
  opaque texels pass). The shader compared floats, and a vertex alpha of 1.0 interpolated with
  perspective (`col * iw`, then `* w`) comes out 0.99999994 at some depths: the title at 1/w 2000
  passed, the text at 600 was dropped. The test now compares 8-bit values, as the PowerVR does
  (`floor(a * 255 + 0.5) < PT_ALPHA_REF`). Found with `M1_FLATPOLY=a:b` (draw polygons a..b
  untextured; `M1_FLATKEEPTEX=1` keeps the texture but skips the alpha test), `M1_TEXDUMP=tcw`
  (the texture as decoded, with its alpha), `M1_SKIPLIST=n`, `M1_TEXNOCACHE=tcw|1`, and the list
  dump's new clip, sprite corner and palette RAM output.
- **Colours**: vertex colours are ARGB words, but went to OpenGL as RGBA bytes, so red and blue
  were swapped in every vertex colour (the VS screen's blue VERSUS BATTLE bar was red, yellow
  gallery text cyan, the red NEW labels blue). The attributes now use the GL_BGRA layout.
- **Smooth text**: 2D polygons of the known 2D classes (HUD, primitives, screens) drawn above
  600 lines use an upscaled copy of their texture (`tex.c tex_get_hi`: 4x up to 256 texels, 2x
  up to 512): Catmull-Rom on premultiplied colour, and for 0/255-alpha textures the coverage
  steepened around one half, so glyphs keep their size and weight with smooth outlines; drawn
  with bilinear filtering, punch-through at alpha 128. A texture upscaled more than 6 times in
  300 renders (animations) stays plain. About 40 upscales in an arcade run, at most 16 ms each,
  once (tex.c is built at -O2). `[video] text = sharp` (M1_SMOOTH2D=0) turns it off.
- **Anti-aliasing**: checked outside online matches (a fight frame at 4x differs from no MSAA on
  13,500 edge pixels). A GRAPHICS change was only saved to the ini, so the restart into an online
  match inherited the old `M1_MSAA` / `M1_RESOLUTION` / `M1_FULLSCREEN`; the change now updates
  the environment too. MSAA smooths polygon edges, not the pixels inside textures (text).
- `build_win.sh` takes GGPO's headers from `third_party/ggpo` (it still pointed at `engines/`).
- **Triggers**: the controller reply always said L and R were released, so the gallery's
  picture viewer (L / R: previous / next picture; the D-pad pans) never changed pictures. The
  triggers now travel in two of the button word's absent bits (0x1000 L, 0x2000 R; maple.c sends
  them as 0 / 255 trigger bytes), so netplay's 16-bit inputs carry them; keyboard Q / W
  (`[keys] l`, `r`), a controller's triggers or shoulder buttons, `L` / `R` in M1_PADS scripts.

## Every mode reachable, second controller (2026-10-19)

- **Missing functions**: modes stopped with "no translated function" (one report: 0x8C05A826,
  called from `mode_kmaster_mode` through a table). Ghidra finds functions through direct calls;
  routines a mode keeps in a table of function pointers, or stores as a callback from a literal
  pool, were never translated. `tools/find_entries.py` finds them: runs of words pointing into the
  code blocks that hold at least one known entry, plus code addresses in the literal pools of
  known functions; each is kept when its code decodes cleanly (known, unprivileged instructions,
  ending in a return or jump), repeated until nothing new turns up. 150 entries (96 from tables,
  54 from literal pools) went into `sh4c/extra_entries.txt`; the sweep has no unresolved targets.
  `tests/modes.sh` picks every main menu entry and taps A into it: all 11 run 60 s without a stop.
- **Controls**: the game confirms with A everywhere except the main menu, where Start also works;
  pressing Enter (Start) on the Options screen does nothing, which looks like a dead screen. The
  player README now says so (Z or the controller's A confirms, X / B goes back).
- **Player 2**: port B had a controller only in netplay, so VS BATTLE showed "A Controller was
  removed". A second game controller is now player 2 (port B present while it is connected;
  `render.c` `pads[2]`, hot-plug by instance id); `M1_PORT_B=1` gives tests an idle player 2.
- **Scripted pads**: each M1_PADS step logs its poll, render count and mode (`pads: step ...`),
  so screenshots (render counts) can be matched to presses (poll counts).

## Voldorium: name, icon, lobby polish (2026-10-19)

- **Name**: the player build is **Voldorium** (no trademark in the product name): `Voldorium.exe`
  (GUI; `m1.exe` stays the console build), `voldorium.ini` (an old `sc-port.ini` is renamed on the
  first start and by `make_player.sh`), window title, message boxes, file properties
  (`m1/voldorium.rc`), disc picker and the player README. The game's own content (logos, names)
  comes from the player's disc and is unchanged. Development paths (`sc/port`, `M1_*`) stay.
- **Icon**: a stylized katar on a crimson badge, drawn by `art/make_icon.py` (supersampled,
  256 to 16 px, `m1/voldorium.ico`, preview `art/voldorium_256.png`), built into both executables
  with llvm-mingw's windres and used by the game window (`SDL_HINT_WINDOWS_INTRESOURCE_ICON`).
- **ONLINE VERSUS**: help lines say "around the world". Each listed player shows a ping:
  measured (PING / PONG every second) on the same network; estimated through the server on the
  internet (our time to the server, from HELLO to LIST, plus theirs, which HELLO now reports and
  LIST carries). A challenge shows the challenger as player 2 with name and ping on one line right
  above ACCEPT (the cursor starts there), then DECLINE; the help bar repeats it. The challenger
  now plays player 2 and the player who accepts player 1, so "player 2" is literally true.
- **Privacy**: the game never sends the computer's name (it often holds the owner's real name):
  players are "PLAYER" and shown to others as OPPONENT (numbered when several; "your opponent"
  in help lines), unless they set `[net] name`; the server logs ids and match tokens only, no
  names or addresses.
- **Idle fix**: the main menu returns to the title after 3600 frames (60 s) without input (mode
  11 handler 0x8c033a86, counter 0x8c25dcdc); the counter is held at 0 while ONLINE VERSUS or
  GRAPHICS is open, so waiting for an opponent no longer drops you to the attract demo.
- **Version** (1.0.1; now 1.0.9): `VOLDORIUM_VERSION` in `m1/m1.h` (and the numbers in `m1/voldorium.rc`).
  Shown in the window title ("Voldorium ver. 1.0.1") and, in game mode 11 (the main menu and its
  submenus), as "ver. 1.0.1" in the lower right corner: drawn by the port over the finished frame
  in the BIOS font's 12x24 glyphs (as the help line), white with a shadow (`render.c`
  `draw_version`); not drawn without the font.

## Internet lobby (2026-10-18)

ONLINE VERSUS now also lists players on other networks, through a lobby server of our own
(`lobby/`, details and operations in `lobby/README.md`): Oracle Cloud Always Free, Montreal,
`147.15.149.200`, UDP 7100 (lobby) and 7101 (relay).

- **Lobby**: the game says HELLO from its game port every 2 s; the server answers with the
  same-build players and the public address it sees for each. Messages to internet players go
  through the server (TO / FROM); same-network players are still found by broadcast and talked to
  directly (a player found both ways counts as same-network).
- **Connecting**: after ACCEPT the server sends both a match token and the other's public address;
  both send punch packets to each other from the game port for 2 s and report; if both got
  through the match connects directly (the restarted game binds the same port, so the router's
  opening is reused), otherwise both go through the relay (the match registers its game port with
  the token, `--relay token,id`, and GGPO talks to the relay, which forwards between the two
  registered addresses only).
- **Protection** (server): printable-ASCII, size and format checks on every datagram, a token
  bucket per source address, caps on players / matches / relays, relay rate and size limits and
  idle expiry; only the address that registered an id may speak for it. It runs as an
  unprivileged user under systemd confinement. The game holds only the address, nothing that
  controls the Oracle account.

Checked: `tests/lobby_server_test.py` (fake players: list, challenge, match, relay, a stranger
dropped); `tests/lobby_inet.sh direct` and `relay` (two game copies through a local server, no
network discovery): both paths connect and keep 46/46 checksums identical.

## Graphics settings and the GRAPHICS menu (2026-10-18)

- **Frame buffer** (`m1/render.c`): every frame is drawn into a frame buffer of its own at the
  chosen resolution (always 16:9; "native" = the window's 16:9 area), with 0/2/4/8 MSAA samples
  (multisampled colour and depth/stencil renderbuffers, capped at `GL_MAX_SAMPLES`), resolved into
  a texture (the screenshots read there, at the drawing resolution), then scaled into the window's
  16:9 area. Resolution and anti-aliasing therefore change while the game runs; the window
  itself no longer has a multisampled pixel format.
- **Full screen**: the player form starts full screen (borderless, the monitor's own resolution)
  unless `[video] fullscreen = 0`; development runs keep a 1280x720 window. Going to a window
  sizes it to the chosen resolution (native: `[video] window`), shrunk to 90% of the screen's
  usable area if needed.
- **GRAPHICS** (`m1/online.c`): a main menu entry after OPTIONS (the main menu is rebuilt in scratch
  RAM with 13 entries; MISSION BATTLE moves to 12) opening a submenu in the Museum's borrowed slot,
  like ONLINE VERSUS. Lines: FULL SCREEN / WINDOW, the resolution (NATIVE W X H, or 1280 X 720 up
  to the monitor's size: 1600x900, 1920x1080, 2560x1440, 3200x1800, 3840x2160, 5120x2880,
  7680x4320), ANTI-ALIASING OFF/2X/4X/8X. Selecting a line cycles its value; it applies at once
  and is saved in `voldorium.ini` (`fullscreen`, `resolution`, `msaa`). ONLINE VERSUS and GRAPHICS
  share the old INTERNET entry's id; the help line follows the cursor. Both are hidden in
  netplay sessions. `M1_LOBBY_TEST=graphics[,cycle]` opens it (and cycles two settings) for tests.
- **Launcher**: the full screen box and window size field are gone (the game has them).

Checked: 1920x1080 with 4x MSAA in a 1280x720 window (fight edges smooth, screenshots at
1920x1080); the menu entry, the submenu, and cycling from the menu (native -> 1280x720, 4x ->
8x, saved to the ini).

## Memory card (VMU) (2026-10-18)

The player build has a VMU in controller port A's first slot (`m1/maple.c`), so the boot no
longer stops at the memory card warning and saves (options, unlocks, records) persist.

- **Device**: the main controller's replies set bit 0 of their sender address (slot 1 occupied);
  frames addressed to slot 1 get the VMU's answers as Flycast models them: device info
  (storage, LCD and clock functions), media info from the root block, block read, block write
  (128-byte phases, written to the file at once), write-complete, LCD and beeper accepted, clock
  read (a fixed date, so runs stay repeatable).
- **Card file**: `vmu_a1.bin` next to the executable (`[game] vmu = file` to move it). A missing
  file becomes a blank formatted card, built from the VMU file system layout (root block 255,
  FAT 254, directory 253-241 chained downward, 200 user blocks) rather than copied from Flycast.
- **First run**: on a card without the game's save (`SCALIBUR_MBU` in the directory) the game's
  card check (mode 31) asks for Start to create it; the port presses Start there, so the screen
  only flashes by once. Later boots load the save and go straight on (mode 31 lasts about 130
  frames, no input).
- **Who has a card**: the player form outside netplay only. Development runs (the tests'
  scripts) and every netplay session (including lobby restarts, which inherit `M1_VMU`) run
  without one, so both sides of a match start from the same machine whatever their saves hold.

## 4:3 artwork export (2026-10-17)

Backgrounds and other full-height pictures drawn by 2D code stay 4:3 in the middle of the
16:9 screen (never stretched), with bars beside them. `M1_ARTDUMP=folder` (or `[debug] artdump =
folder` in `voldorium.ini`, for collecting while playing) saves each such screen once
(`m1/artdump.c`): flat textured pieces outside fights that reach the left or right edge of the
640x480 screen mark the background; every piece at that depth or farther back is composed on
the CPU into one clean 640x480 picture (the main menu's is 80 tiles of 64x64; menus, logos and
text in front are left out), saved with the frame as shown. A picture is saved after it has
stayed the same for 10 frames (no transitions, loading noise or zooms), once per look per game
mode. Scrolling wallpapers are skipped: they already span 16:9.

`bash tests/artdump.sh` runs the arcade script and each main-menu mode (`M1_MENU_PICK=menu:entry`
chooses a main menu or Museum entry directly, `online.c`) and merges the results into
`build/art/index.html` and `build/art/list.csv` (game pictures: they stay on this machine).
First run: 12 screens (boot, opening logos, main menu, three loading screens, character
select portraits, art gallery menu, survival result, ranking). Not reached by the scripts: VS
and Team Battle (second controller), Mission Battle, gallery pictures, endings, profiles.
Found on the way: the scrolling wallpaper stops about 145 pixels short of the right edge.

**Background mode** (`[video] backgrounds` / `M1_BGMODE`, `render.c` `side_fill`): `stretch`
(the default since 2026-10-18) draws the background layer at full width,
stretched by 4/3, while menus, text and logos in front stay undistorted (the main menu's
SOULCALIBUR logo belongs to its background layer, so it stretches too); `replace` uses the 16:9
replacement pictures below, with the blur fill where there are none; `blur` only blurs.

**Blur fill** (`render.c` `side_fill`, `M1_BLURFILL=0` turns it off): on such a
screen the background layer (the same rule as the export) is drawn again, enlarged evenly by
4/3 to cover 16:9, into a 192x108 texture, blurred (two separable Gaussian passes, twice) and
laid under the frame at 60% brightness. The sides show a soft, dark continuation of the picture;
the picture itself stays undistorted in the middle. Fights (and the demo fights, modes 23 and
45) are never touched.

**Replacement pictures** (`m1/artswap.c`, `m1/png.c`): `art16x9/refs.txt` (in this folder,
copied into `build/player/art16x9/` by `make_player.sh`; `tests/art_refs.py` writes it from
`build/art_4x3`) lists each known background with a 32x24 RGB fingerprint. When the frame's
background layer changes (its textures, their contents, or places; the art gallery reuses the
main menu's tile addresses with new pictures), the runtime composes it on the CPU (as the
export does), takes its fingerprint and picks the closest reference under a mean difference of
12; if `<name>.png` is in the folder (`M1_ART16X9`, default `art16x9` next to the executable),
that picture is drawn over the whole 16:9 area (mipmapped, any size) and the layer's own pieces
are skipped; menus, logos and text in front are drawn as usual. `png.c` reads 8-bit PNGs with its
own inflate. `M1_ARTSWAP_LOG=1` prints each decision. Checked with a dummy 2560x1440 main menu
(original 3x in the middle, checkered sides): it lines up exactly, with the menu on top.
Upscaling tool for making the pictures: chaiNNer 0.25.1 portable in `ReverseEngineering/engines/chaiNNer`
(`chaiNNer.exe`; it downloads its own Python and PyTorch on first start). Model:
`ReverseEngineering/engines/chaiNNer-models/4x-FSDedither-Riven.pth` (ESRGAN 4x, GPL-3.0, from
OpenModelDB; trained on Riven's pre-rendered, dithered game screens).

## Online Versus lobby (2026-10-17)

Online play now starts from the game's own main menu, without the launcher's address and port
fields (`m1/online.c`).

- **Menu entry**: the main menu has a hidden INTERNET entry (type 3, id 20, help line "We
  cannot internet..."), left over from the Dreamcast's online features. It is moved under VS
  BATTLE, relabelled ONLINE VERSUS and turned into a submenu entry. The menu tables
  (`0x8c21a848` descriptors, `0x8c21a718` main entries, `0x8c21a870` per-id shown/selectable,
  `0x8c21a2f8` per-id help lines) are documented at the top of `online.c`.
- **Lobby**: a submenu drawn by the game's own menu code. It borrows the Museum's slot (menu 1)
  while the cursor is on ONLINE VERSUS, and the Museum's entry ids 14-19 for its lines; strings
  and entries live in BIOS work RAM (`0x8c006000`) that nothing uses under the HLE BIOS. Lines:
  SEARCHING... or the players found, then CHALLENGE / WAITING FOR ... / CANCEL, or ACCEPT /
  DECLINE for an incoming challenge; the help bar says what is happening. Status lines are
  selectable no-ops: a disabled line would show the game's "Not available in this version.".
  The menu function `0x8c0f3d46` is wrapped (`m1.c`): it returns our entries' values once,
  and the wrapper takes them and clears the menu's leave-the-mode countdown.
- **Discovery**: UDP broadcast on port 7100 once a second from each copy in the lobby (name =
  computer name, or `[net] name`), sent from its game port (first free of 7000-7019, so two
  copies on one PC work). Unicast CHAL / ACCEPT / DECLINE / CANCEL / BUSY go game port to game
  port. Only copies with the same build key are listed: a hash of the runtime sources and the
  state table (`SC_BUILD_ID`, `m1/build_win.sh`).
- **Match start**: both copies restart themselves (`--join --port --player --versus
  --opponent`, plus `--disc` and `--ranges`) on the game port the lobby used. `--versus` boots
  offline on scripted inputs (Start through the title, DOWN and Start in the main menu) into VS
  BATTLE character select, unpaced and not drawn (about 450 loops, a second or two); boot is
  deterministic, so both arrive in the same state, and GGPO starts there (`netplay.c`). The
  ONLINE VERSUS entry is hidden in a session (both sides alike). Widescreen changes the game's
  own projection value (`0x8C266C28`), so 16:9 and 4:3 copies simulate differently (a mixed
  pair desynced at the first checksum); with the port now 16:9 only, every copy agrees. A match that does not connect
  in 20 s, a lost connection (GGPO's 5 s disconnect timeout: the opponent quit, closed the game
  or dropped off the network), or Esc ends it, and both sides go back to the main menu:
  `online_return` restarts the program with `--back "<reason>"` and without the session's
  environment (`M1_NET*`; before this the new process inherited it, tried to rejoin the old
  match and sat on a black screen behind a hidden message box). `M1_BACK` boots unpaced and not
  drawn on Start presses to the main menu (about 320 loops: the first frame drawn is the menu),
  puts the cursor on ONLINE VERSUS and shows the reason as its help line until the cursor
  moves; no message box. Checked by `tests/lobby_drop.sh` (one side's match closes mid-fight,
  the other is back in the main menu).

Checked (`tests/lobby.sh`, two copies on this PC): both list each other, the challenge is
accepted, both boots end at the same loop (454), and a VS fight with mashed inputs keeps 83/83
checksums identical. `M1_LOBBY_DEBUG=1` logs every lobby message. Not yet: players on different networks (needs the lobby server and NAT
traversal), and a lobby between two PCs.

## Tests (`tests/`)

| Script | What | Expect |
| --- | --- | --- |
| `tests/run_all.sh` | build, then everything below (about 16 minutes) | |
| `tests/oracle.sh` | WSL: Linux build, broad oracle replay, M0 integer and FPU suites | 5,577 exact + 162 HLE boundaries; 683/683; 2,800/2,800 |
| `tests/synctest.sh [s]` | fight SyncTest | all frames identical |
| `tests/netplay.sh [s]` | two peers, 100 ms simulated latency, both pressing buttons | identical checksums, about 200 rollbacks each |
| `tests/screens.sh` | title, character select, fight (16:9) | `build/tests/screen_wide_*.png` |
| `tests/artdump.sh [s]` | export of the 4:3 artwork per screen | `build/art/index.html` |
| `tests/lobby.sh [s]` | two copies meet in the Online Versus lobby, challenge, accept, VS fight | same boot loop, identical checksums |
| `tests/lobby_inet.sh direct\|relay [s]` | the same through a local lobby server | identical checksums |
| `tests/lobby_drop.sh [s]` | one side's match closes mid-fight | the other is back in the main menu |
| `tests/netplay.sh` with `NET_WINDOWED=1` (`NET_VSYNC=0/1`) | the netplay match in windows, VSync on, frame times logged (M1_FRAMETIME) | identical checksums; frame times |
| `tests/arcade.sh [s]` | arcade mode start to finish (player 1 always wins), about 40 minutes, not in run_all | no freeze (hang report) |
| `tests/modes.sh [s] [entries]` | every main menu entry, A taps into the mode | no stop before the time limit; `build/tests/modes/<n>/shot_*.png` |

## Pieces

| Path | What |
| --- | --- |
| `functions.json` | Ghidra export (`ghidra-scripts/ExportFunctions.java`): 3,251 functions with body ranges |
| `sh4c/sh4c.py` | translator: decodes raw opcodes (about 150 patterns, integer ISA), one C function per Ghidra function, delay slots folded into branches, read-only literal pools folded to constants, calls through `sh4_call` |
| `sh4c/sh4rt.h` | runtime: `SH4` context, RAM accessors (area 3 -> 16 MB array, other areas -> `sh4_hw_read/write`), `div1` as in Flycast |
| `sh4c/m0_funcs.c` | generated C for the three functions |
| `m0/flycast-hooks/` | snapshot of the Flycast hook scripts plus `apply_probe_hook.py` (`SC_PROBE`: records entry/exit state of chosen functions); built into a separate copy `~/sc-flycast-m0` with `build_m0.sh`, so the shared traced build is untouched |
| `m0/capture_oracle.sh` | runs the oracle with autoplay and the fake timer, writes `m0/oracle_calls.jsonl` |
| `m0/gen_harness.py` | turns recorded calls into `m0/tests.c` (test vectors + harness) |
| `sh4c/m0_fpu.c`, `sh4c/fsca.c` | generated C for the FPU set; `fsca` table built at start from Flycast's `fsca-table.h` |
| `m0/capture_oracle_fpu.sh`, `m0/oracle_fpu.jsonl` | FPU oracle capture (14 functions x 200 calls) |
| `m0/gen_harness_fpu.py` | FPU harness generator (FR/XF/FPSCR, RAM image, call dispatcher) |
| `sh4c/sweep.py` | translates every function, writes the dispatch table and the coverage report (`build/sweep/`) |
| `sh4c/extra_entries.txt` | entry points Ghidra missed (reached only through pointers) |
| `m0/capture_oracle_broad.sh`, `m0/oracle_pack.py`, `m0/replay.c`, `m0/build_replay.sh` | broad oracle: capture, pack, whole-program replay |
| `m1/m1.c`, `m1/hw.c`, `m1/hle.c`, `m1/threads.c`, `m1/maple.c`, `m1/build.sh` | native executable: loader and interrupt delivery, hardware layer (clock, pacing, DMA, VRAM), HLE BIOS, SDK threads, maple bus and pads |
| `m1/ta.c`, `m1/tex.c`, `m1/render.c`, `m1/ta.h` | TA display lists, texture decoding, OpenGL renderer, window and live input |

Reproduce (WSL, from `sc/port`):

```
python -I sh4c/sh4c.py sh4c/m0_funcs.c 8c0639c8 8c0fed6c 8c0fe6fc
python -I m0/gen_harness.py m0/oracle_calls.jsonl m0/tests.c
gcc -O2 -w -I sh4c -o /tmp/m0_harness sh4c/m0_funcs.c m0/tests.c && /tmp/m0_harness
```

## Semantics notes

- Delayed branches: the condition (or target register) is read before the delay slot runs,
  as on hardware. A delay slot that is also a branch target is emitted again under its label.
- `rts` becomes a C `return`. Code that returns by rewriting PR to somewhere other than the
  caller would break this; none seen so far, the oracle checks will expose it.
- Literal loads from the read-only code blocks (text, libtext) are folded; anything else is
  loaded at run time.
- FPSCR is tracked at run time (`c->fpscr`), not per block: SZ/PR are tested inside each helper and
  `lds ...,FPSCR` / `frchg` swap banks and reset the host rounding mode. Cheap enough for now; static
  per-block specialisation can come later if profiling asks for it.
- Not translated: SSR/SPC/SGR/DBR and banked registers, `trapa`, `sleep`, `ldtlb`, `rte`.
  They compile to `sh4_unimpl` traps; the sweep found them only in library exception code.
- A tail branch, `jmp` or fallthrough to another function is `sh4_call(target); return;` (PR is
  unchanged, so the callee returns straight to our caller, as on hardware).

## Next

1. Internet matchmaking for the Online Versus lobby: a small lobby server (player list, NAT
   hole punching, relay fallback), then a test between two machines on different networks.
2. Widescreen check of the gallery, mission and option screens; translucent modifier volumes.
3. Listening tests of the sound against Flycast or hardware.
The broad oracle stays the regression suite for every translator change.
