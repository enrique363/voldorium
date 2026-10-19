# Voldorium

A native Windows port of the Dreamcast fighting game **SoulCalibur** (USA, T-1401N, V1.000),
made by static recompilation: the game's SH-4 machine code is translated into C and compiled
together with a small native runtime that stands in for the Dreamcast hardware. It adds:

- 16:9 widescreen (3D scene widened; HUD, menus and text kept at their true proportions)
- any resolution, full screen at the desktop's native resolution, multisample anti-aliasing
- **Online Versus** in the game's own main menu: a worldwide lobby with matchmaking, ping,
  accept/decline, NAT hole punching with a relay fallback, and GGPO rollback netcode
- a built-in virtual memory card, an in-game GRAPHICS menu, optional 16:9 replacement artwork

What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## You need your own copy of the game

> **This repository contains no game code, no game data and no executable.**
> Voldorium only works with a disc dump (`.gdi`) that **you made yourself from a SoulCalibur
> disc you own**. Nothing here can play anything without it, and none of it may be used with
> copies you do not legally own. Do not ask for, link to or share game files or built
> executables here: a built `Voldorium.exe` contains code translated from the game, so it is
> as much the game's copyright as the disc and must not be distributed either.

Only the USA release (T-1401N, V1.000) is supported: the translation tables are made for it, and
`tools/extract_boot.py` checks the SHA-1 of the files it extracts.

This is an independent, non-commercial preservation and interoperability project. It is not
affiliated with, endorsed or sponsored by Bandai Namco Entertainment or Sega. SoulCalibur is a
trademark of Bandai Namco Entertainment Inc.; Dreamcast is a trademark of Sega Corporation.

## What is in the repository

| Path | What |
| --- | --- |
| `port/sh4c/` | The translator: SH-4 to C (`sh4c.py`), the batch driver (`sweep.py`), the runtime header |
| `port/functions.json` | Where the game's functions start and end (addresses only, no code) |
| `port/m1/` | The native runtime: HLE BIOS, memory map, maple (pads, memory card), Tile Accelerator to OpenGL renderer, AICA sound, GGPO netplay, online lobby, settings |
| `port/lobby/` | The matchmaking and relay server (Python, standard library only) |
| `port/tests/` | Lobby, netplay, artwork and translator tests |
| `port/m0/` | Translator validation against the Flycast emulator (oracle capture scripts) |
| `port/art/` | Icon source |
| `tools/` | Analysis helpers; `extract_boot.py` extracts the boot files from your dump |
| `names/`, `ghidra-scripts/` | Function and variable names, Ghidra scripts used for the analysis |
| `ggpo/savestate_ranges.tsv` | Which memory ranges make up a rollback save state |
| `flycast-trace/` | Patches to a Flycast checkout used to record traces for validation |
| `third_party/ggpo/` | GGPO (MIT), with small changes for this port |
| `docs/` | Design notes and findings |

Not in the repository, by design (see `.gitignore`): `disc/` (your dump), `extracted/`, anything
generated from them (`port/build/`), Ghidra projects, traces, and `port/art16x9/refs.txt`.

## Building

Windows 10 or 11, with Git for Windows (Git Bash) and Python 3.10 or newer.

Tools, placed by default in an `engines/` folder **next to** this repository (or point to them
with the variables shown, for example in a `local.env` file at the repository root, which is
not committed):

| Tool | Default location | Variable |
| --- | --- | --- |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (x86_64, ucrt) | `engines/llvm-mingw-20260922-ucrt-x86_64` | `LLVM_MINGW` |
| [SDL2](https://github.com/libsdl-org/SDL/releases) 2.32 mingw development files | `engines/SDL2-2.32.10/x86_64-w64-mingw32` | `SDL2_DIR` |
| [Flycast](https://github.com/flyinghead/flycast) source checkout (for its FSCA table header and its replacement BIOS font) | `engines/flycast` | `FLYCAST` |

```
workspace/
  engines/          llvm-mingw, SDL2, flycast
  voldorium/        this repository
    disc/gdi/       your dump: soulcalibur.gdi and its track files (ignored by git)
```

Steps, in Git Bash from the repository root:

1. Extract the boot files from your dump (writes `extracted/`, checks the SHA-1):

   ```bash
   python -I tools/extract_boot.py disc/gdi/soulcalibur.gdi
   ```

2. Translate the game code into C (writes `port/build/sweep/`, a few minutes):

   ```bash
   cd port && python -I sh4c/sweep.py build/sweep 8
   ```

3. Build GGPO, then the game (output in `%LOCALAPPDATA%/sc-m1`):

   ```bash
   bash m1/build_ggpo.sh && bash m1/build_win.sh
   ```

4. Assemble the player folder `port/build/player` (executable, SDL2.dll, settings, README.txt):

   ```bash
   bash m1/make_player.sh
   ```

Everything you build stays on your machine. Do not upload or share `port/build/` or the
executables.

## Playing

Run `Voldorium.exe` from the player folder. The first time, it asks for your disc dump (the
`.gdi` file) and remembers it in `voldorium.ini`. It starts full screen at your desktop's
resolution; the main menu's **GRAPHICS** entry changes resolution, full screen and
anti-aliasing. Controllers and keyboard go through SDL2.

**Online Versus** is on the main menu. It lists players around the world (and on your local
network) who are searching with the same build. Pick one to challenge; the other player sees
your ping and accepts or declines. Players are shown as OPPONENT unless they set a nickname
(`[net] name = ...` in `voldorium.ini`); computer names are never sent. Both players must have
built from the same sources: the lobby only matches identical builds.

Settings (`voldorium.ini`): `[video] fullscreen, resolution, window, msaa, backgrounds`
(`stretch`, `blur` or `replace`), `[net] lobby, name`, `[game] disc, vmu`. More detail is in
`port/README.md` and in the README.txt of the player folder.

## Hosting a lobby server

The game uses a public lobby by default. To run your own, see `port/lobby/README.md`
(`sc_lobby.py`, UDP 7100 lobby and 7101 relay, with a systemd unit) and set
`[net] lobby = host:7100` on every player's copy.

## Credits

- [enrique363](https://github.com/enrique363): project lead, direction, testing on real
  hardware and networks.
- [Claude](https://claude.com/claude-code) (Anthropic), through Claude Code: the translator,
  the runtime, the online lobby and the tools, co-author of every commit.

## Licenses

The project's own code is provided for personal, non-commercial use with a legally owned copy
of the game. Third-party parts keep their own licenses:

- GGPO: MIT (`third_party/ggpo/LICENSE`)
- SDL2: zlib (linked at build time, shipped as `SDL2.dll`)
- Flycast: GPL-2.0. Its sine table header and BIOS font are taken from your own checkout at
  build time and are not stored here; the patches in `flycast-trace/` and `port/m0/flycast-hooks/`
  apply to a Flycast checkout and are offered under the GPL-2.0.
