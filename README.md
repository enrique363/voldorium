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

You need Windows 10 or 11, your own disc dump of SoulCalibur (USA, T-1401N, V1.000) as a
`.gdi` file with its track files, about 1 GB of free disk space and an internet connection for
the first build. Nothing else has to be installed.

### The easy way

1. Download this repository: the green **Code** button on GitHub, then **Download ZIP**, and
   unzip it anywhere (or clone it with git).
2. Double-click **`Build Voldorium.bat`** in the unzipped folder. You can also drop your `.gdi`
   file onto it.
3. Choose your `.gdi` file when it asks.
4. Wait. The first build downloads its tools (about 220 MB, once) and then takes from about a
   minute to ten, depending on the PC. When it is done, the **`Voldorium`** folder opens:
   start `Voldorium.exe`.

What the builder does, so you know what runs on your PC:

| Step | What happens |
| --- | --- |
| Tools | Downloads into `toolchain/` here, at fixed versions, each checked against its SHA-256: the [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) compiler, [SDL2](https://github.com/libsdl-org/SDL), and two files from [Flycast](https://github.com/flyinghead/flycast) (the SH-4 sine table header and its replacement BIOS font). A portable Python too, if you have no Python 3.10 or newer. Nothing is installed on the system; delete `toolchain/` to remove it all. |
| 1. Extract | Reads the boot files from your dump into `extracted/` and checks they are the supported release. |
| 2. Translate | Turns the game code into C (`port/build/sweep/`). |
| 3. GGPO | Builds the rollback netcode library. |
| 4. Compile | Builds `Voldorium.exe` (objects in `%LOCALAPPDATA%\sc-m1`). |
| 5. Player folder | Puts the game, `SDL2.dll`, the font, `voldorium.ini` and `README.txt` in `Voldorium/`. |

Run it again after updating the repository: finished steps are skipped and your settings and
memory card in `Voldorium/` are kept. Builds are reproducible: everyone who builds the same
version gets a byte-identical `Voldorium.exe` (pinned tools, no time stamps or folder names in
it), which is what the online lobby needs to match players.

### Troubleshooting

| Message | What to do |
| --- | --- |
| `This dump is not SoulCalibur USA (T-1401N, V1.000)` | Only that release is supported. Check the `.gdi` is the USA disc and that its track files are complete. |
| `no high-density data track` | Choose the `.gdi` file itself, with all its `.bin` / `.raw` track files in the same folder. |
| `could not download ...` | Check the internet connection (or a firewall or proxy) and run the builder again. |
| `does not match its pinned SHA-256` | The download was damaged or changed. Delete the `toolchain` folder and run the builder again; if it keeps happening, open an issue. |
| Windows SmartScreen warns about the `.bat` | It is a plain text script: right-click it, **Edit**, to read it first. Then **More info**, **Run anyway**. |
| Antivirus removes `Voldorium.exe` | A freshly compiled, unsigned program can trip heuristics. Allow it, or exclude the folder. |
| Online Versus does not find a friend | Both of you must build the same version of this repository. |

### By hand (developers)

`tools/build.py` does every step and can be run directly: `python -I tools/build.py --gdi
path\to\soulcalibur.gdi [--download] [--player-dir DIR]` (`--help` lists the options). It finds
its tools in this order: the variables `LLVM_MINGW`, `SDL2_DIR` and `FLYCAST` (the environment,
or a `local.env` file at the repository root, which is not committed), an `engines/` folder next
to the repository, then `toolchain/`. The Git Bash scripts `port/m1/build_ggpo.sh`,
`build_win.sh` and `make_player.sh` run its single steps; `port/sh4c/sweep.py` is the
translator and `port/tests/run_all.sh` the test suite.

Everything you build stays on your machine. Do not upload or share the `Voldorium` folder,
`port/build/` or the executables: they contain code translated from the game.

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
- Flycast: GPL-2.0. Its sine table header (the SH-4 FSCA results, measured on the hardware)
  and its BIOS font are downloaded by the builder from Flycast's repository at a pinned commit,
  or taken from your own checkout, at build time; they are not stored here. The patches in
  `flycast-trace/` and `port/m0/flycast-hooks/` apply to a Flycast checkout and are offered
  under the GPL-2.0.