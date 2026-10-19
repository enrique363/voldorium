# Port scoping: static recompilation of Soulcalibur (Dreamcast) to native Windows

Status: scoping only, no port code yet (2026-10-15). Data: `sc/facts/recomp_stats.txt`,
`sc/facts/lib_calls.tsv` (Ghidra script `RecompStats.java`), runtime code-page scan
(`SC_PCSCAN` in the traced Flycast).

## Recommendation

Hybrid static recompilation:

1. **Translate the game code** (`text`, 0x8C010000-0x8C1036E0) from SH-4 machine code into C,
   one C function per Ghidra function, against a 16 MB RAM array that keeps Dreamcast
   addresses. Compile with the port.
2. **Do not translate the Sega libraries** (`libtext`, 0x8C21AF20-0x8C245620). Replace the
   130 library functions the game calls with native implementations (high-level emulation):
   file system, input, memory, timers, graphics, sound.
3. **Render from the game's own polygon stream.** The game writes PowerVR TA parameter and
   vertex blocks through store queues (`pref`) into vertex buffers. The port parses that
   stream (format already decoded for the HUD trace) and draws it with a modern API. That is
   also where widescreen anchoring and higher resolution live.
4. **Replace hand-picked functions with hand-written C over time** (rendering hooks, the
   match loop, RNG/timer), as the widescreen and GGPO work needs.

## Why it is feasible

| Finding | Value | Consequence |
| --- | --- | --- |
| Game code size | 363,108 instructions (726 KB of the 997 KB block; the rest is literal pools and tables) | large but mechanical to translate |
| Instruction variety | 94 distinct SH-4 opcodes in game code | a manageable semantics table |
| Code outside 1ST_READ.BIN | none, besides BIOS boot, uncached P2 aliases of the same code and the library's interrupt trampoline | **no overlays**: one binary to translate |
| Calls into libraries | 400 call sites, 130 distinct functions | the high-level-emulation surface is bounded |
| Exceptions / MMU | no `trapa`, `ldtlb`, `sleep` in game code; `rte` only in libraries | no exception emulation needed in game code |
| Determinism | SyncTest 3,000/3,000 with the save state in `sc/ggpo/` | GGPO design already validated |
| Widescreen | validated in Flycast; gameplay identical at 4:3 and 16:9 | one value plus the 2D rules |

Library calls by module (module boundaries approximate):

| Module | Functions | Call sites |
| --- | --- | --- |
| system (malloc, timers, cache, interrupt chain) | 30 | 139 |
| Kamui 2 graphics | 29 | 108 |
| sound / audio streaming | 44 | 96 |
| GD-ROM file system | 16 | 32 |
| peripherals (pd) | 6 | 8 |
| others (sound driver interface, low-level interrupts) | 5 | 17 |

## Risks and how to handle them

| Risk | Detail | Handling |
| --- | --- | --- |
| Indirect control flow | 579 `jsr @rn`, 25 `jmp @rn`, 35 `braf` jump tables; game modes, battle states, stage/effect callbacks are dispatched through pointer tables (788 found) | resolve tables statically; route any unresolved target through a dispatcher that maps a Dreamcast address to the translated function, with a fallback (below) |
| Incomplete code discovery | 27% of `text` is not disassembled: mostly literal pools and tables, possibly some unreached code | merge the Ghidra function list with runtime PC coverage from the traced Flycast; the fallback catches misses |
| Fallback for missed code | an address with no translated function | embed an SH-4 interpreter as a safety net (note: Flycast's interpreter is GPL-2, so reusing it makes the port GPL) |
| FPU modes | 364 `frchg` (matrix bank swaps), 44 `fschg`, 42 FPSCR loads | track FPSCR.SZ/PR/FR per basic block in the translator; default SZ=0, PR=0 at function entry (SHC ABI) |
| Store queues | 358 `pref` in game code (polygon output) | translate `pref` to "flush 32 bytes to the TA stream or RAM", as Flycast's `sqWrite` does |
| Direct hardware access from game code | fog colour (PVR 0x80B0/0x80B4), PowerVR via SQ, BIOS GD-ROM syscall vector 0x8C0000BC, sound-chip reads through `g2_bus_read` | implement those few registers in the runtime; HLE the BIOS syscalls |
| Timing assumptions | `vsync_callback` counters read by game code; frame pacing through `kmRender`; uninitialised stack reads | call `vsync_callback` once per frame; keep a real stack region in the RAM array |
| Sound | the libraries drive the AICA sound chip and its ARM7 CPU running Sega's sound driver | biggest unknown: either emulate AICA + ARM7 (proven, heavier) or HLE the sound library API (lighter, needs reverse engineering of the driver protocol). Start with emulation |
| Legal | translated C is derived from the game binary | ship the translator and runtime; users run it on their own disc dump (the N64Recomp model). Never distribute game code or assets |

## Milestones

| # | Goal | Main work |
| --- | --- | --- |
| M0 | Translator skeleton | export functions/instructions from Ghidra; SH-4 to C for the 94 opcodes; FPSCR tracking; dispatcher for indirect calls; build a native executable that runs `main` until the first library call |
| M1 | Boot to the title screen | HLE for sy (malloc, timers), gd (read files from the disc image), pd (no input yet), Kamui init/scene/render; TA stream parser; OpenGL/Vulkan renderer for opaque + translucent lists |
| M2 | Menus and a fight with input | pd with real controllers; remaining Kamui (textures, render targets); punch-through/modifier volumes in the renderer |
| M3 | Audio | AICA + ARM7 emulation (or HLE), streaming audio |
| M4 | Widescreen and resolution | aspect 0.75; 2D rules (anchor/pillarbox/extend, never stretch); internal resolution scaling |
| M5 | Netplay | save state from `savestate_ranges.tsv`, timer and vsync fixes, input injection, GGPO |

The traced Flycast stays the reference throughout: same inputs (autoplay), compare per-frame
state fingerprints (`SC_STATELOG`) between the emulator and the port to find translation bugs.

## First concrete step for M0

Write a Ghidra script that exports, per function, its basic blocks with decoded operands
and resolved branch targets as JSON, then a small Python translator that emits C for a
handful of leaf functions (`rng_next15`, `memset`, `__sdivsi3`) and checks them against the
emulator. That validates the instruction semantics before scaling to all 3,200 functions.
