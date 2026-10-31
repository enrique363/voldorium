# Voldorium

[![Latest release](https://img.shields.io/github/v/release/enrique363/voldorium?label=latest%20release)](https://github.com/enrique363/voldorium/releases/latest)

A native Windows and Linux port of the Dreamcast fighting game **SoulCalibur** (USA, T-1401N, V1.000),
made by static recompilation: the game's SH-4 machine code is translated into C and compiled
together with a small native runtime that stands in for the Dreamcast hardware. It adds:

- 16:9 widescreen (3D scene widened; HUD, menus and text kept at their true proportions)
- any resolution, full screen at the desktop's native resolution, multisample anti-aliasing
- **Online Versus** in the game's own main menu: a worldwide lobby with matchmaking, ping,
  accept/decline, NAT hole punching with a relay fallback, and GGPO rollback netcode
- a built-in virtual memory card, an in-game GRAPHICS menu, optional 16:9 replacement artwork,
  and new high resolution art for the hit level labels in Practice (`port/m1/textures`, drawn by
  `tools/hitlabels.py`)

What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## You need your own copy of the game

> **This repository contains no game code, no game data and no executable.**
> Voldorium only works with a disc dump (`.gdi`) that **you made yourself from a SoulCalibur
> disc you own**. Nothing here can play anything without it, and none of it may be used with
> copies you do not legally own. Do not ask for, link to or share game files or a built game
> module here: `sc_game.dll` contains code translated from the game, so it is as much the
> game's copyright as the disc and must not be distributed either. `Voldorium.exe` holds none of
> it (the build checks): it loads `sc_game.dll`, which is built on each player's own PC from
> their own disc.

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

## Installing

You need Windows 10 or 11, your own disc dump of SoulCalibur (USA, T-1401N, V1.000) as a
`.gdi` file with its track files, about 1 GB of free disk space and an internet connection for
the first start. Nothing else has to be installed.

### Windows (download, from 1.1.0)

1. From [**Releases**](https://github.com/enrique363/voldorium/releases/latest), under
   **Assets**, download **`Voldorium-<version>-windows.zip`** (about 2 MB) and unzip it anywhere
   you can write to (not `Program Files`).
2. Start **`Voldorium.exe`** and choose your `.gdi` file when it asks.
3. The first start builds the game from your disc. It first says what it downloads and asks:
   the [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) compiler (about 190 MB), two small
   files from [Flycast](https://github.com/flyinghead/flycast) (the SH-4 sine table and its
   replacement BIOS font) and, only if you have no Python 3.10 or newer, a portable Python
   (about 11 MB). Each is a fixed version checked against its SHA-256, and all of it goes into
   the `builder` folder; nothing is installed on the system. A window then shows the three
   steps (extract the boot files from your dump, translate the game code to C, compile it into
   `sc_game.dll`), which take from about half a minute to a few minutes. The game starts when
   it is done; later starts go straight to the game.

The download holds no game code: `Voldorium.exe` is the runtime (renderer, sound, netplay,
lobby), and the game's code is made from your own disc on your PC. After you update to a new
version, the first start rebuilds `sc_game.dll` the same way (the tools are already there).
Everyone on the same version builds a byte-identical `sc_game.dll`, so Online Versus works
between any two players of that version.

To uninstall, delete the folder. Your settings and memory card are in it (`voldorium.ini`,
`vmu_a1.bin`): keep those two files to carry them over to a new version.

### Linux

You need a 64-bit Linux with glibc 2.28 or newer (any distribution from 2018 on), SDL2 and
OpenGL (desktop systems have them), Python 3, your own disc dump and an internet connection for
the first start.

1. From [**Releases**](https://github.com/enrique363/voldorium/releases/latest), download
   **`Voldorium-<version>-linux.tar.gz`** and unpack it anywhere you can write to
   (`tar xf Voldorium-<version>-linux.tar.gz`).
2. Start **`voldorium`** in that folder and choose your `.gdi` file when it asks (with
   `zenity` or `kdialog`; without either it says where to write the path in `voldorium.ini`).
3. The first start builds `sc_game.so` from your disc, as on Windows, after saying what it
   downloads and asking: the [Zig](https://ziglang.org) compiler (0.14.1, about 50 MB) and the
   two Flycast files, each checked against its SHA-256, into the `builder` folder. It shows the
   build in a terminal window (konsole, GNOME Terminal or xterm; otherwise it writes
   `builder/work/build.log`).

Online Versus matches Linux and Windows players of the same version: the lobby key and the game
are the same, and matches stay in step frame for frame.

## Building it yourself

The whole game (the executables as well as the game module) can also be built from the source.

### The easy way

1. Download the latest version from
   [**Releases**](https://github.com/enrique363/voldorium/releases/latest): under **Assets**,
   **Source code (zip)**. Unzip it anywhere. (The green **Code** button gives the newest work in
   progress instead; players who want to meet online should build the same release.)
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
| 4. Compile | Builds `Voldorium.exe` (the runtime: no game code) and the game module `sc_game.dll` (the translated game code, with Flycast's sine table); objects in `%LOCALAPPDATA%\sc-m1`. |
| 5. Player folder | Puts the game, `SDL2.dll`, the font, `voldorium.ini` and `README.txt` in `Voldorium/`. |

Run it again after updating the repository: finished steps are skipped and your settings and
memory card in `Voldorium/` are kept. Builds are reproducible: everyone who builds the same
version gets a byte-identical `Voldorium.exe` and `sc_game.dll` (pinned tools, no time stamps
or folder names in them), which is what the online lobby needs to match players.

### Troubleshooting

| Message | What to do |
| --- | --- |
| `This dump is not SoulCalibur USA (T-1401N, V1.000)` | Only that release is supported. Check the `.gdi` is the USA disc and that its track files are complete. |
| `no high-density data track` | Choose the `.gdi` file itself, with all its `.bin` / `.raw` track files in the same folder. |
| `unable to open output file ... No such file or directory` | You have an old version of the builder and the Microsoft Store version of Python, which hides what it writes in `AppData` from the compiler. Download the latest release: its builder skips that Python. |
| `could not download ...` | Check the internet connection (or a firewall or proxy) and run the builder again. |
| `does not match its pinned SHA-256` | The download was damaged or changed. Delete the `toolchain` folder and run the builder again; if it keeps happening, open an issue. |
| Windows SmartScreen warns about the `.bat` | It is a plain text script: right-click it, **Edit**, to read it first. Then **More info**, **Run anyway**. |
| Windows Defender quarantines `Voldorium.exe` as a Trojan ("executes commands from an attacker", often `Trojan:Win32/Wacatac` with `!ml`) | A known false positive. Each new version is an unsigned program Microsoft has not seen yet, and its first start runs a script that downloads the compiler and builds a DLL, which Defender's machine-learning check reads as a malware loader. In Windows Security > Virus & threat protection > **Protection history**, open the item and choose **Actions > Allow on device**. Only do this for a copy from this repository's Releases or one you built yourself. Reports to Microsoft ([submit as "Incorrectly detected"](https://www.microsoft.com/wdsi/filesubmission)) help clear it for everyone. |
| Another antivirus removes `Voldorium.exe` | The same false positive: allow it, or exclude the Voldorium folder. |
| Online Versus does not find a friend | Both of you must build the same release (the version is in the main menu corner). |

### By hand (developers)

`tools/build.py` does every step and can be run directly: `python -I tools/build.py --gdi
path\to\soulcalibur.gdi [--download] [--player-dir DIR]` (`--help` lists the options). It finds
its tools in this order: the variables `LLVM_MINGW`, `SDL2_DIR` and `FLYCAST` (the environment,
or a `local.env` file at the repository root, which is not committed), an `engines/` folder next
to the repository, then `toolchain/`. The Git Bash scripts `port/m1/build_ggpo.sh`,
`build_win.sh` and `make_player.sh` run its single steps; `port/sh4c/sweep.py` is the
translator and `port/tests/run_all.sh` the test suite.

The game's code and the runtime are separate: `sc_game.dll` (`port/sh4c/module.c` and the
translated code, compiled with `-DSH4_MODULE`) reaches the runtime only through a table of
about twenty functions and variables (`Sh4Host` in `port/sh4c/sh4rt.h`), and `Voldorium.exe`
loads it at start (`port/m1/module.c`). `--only module --gdi ... --module-dir DIR` builds the
module alone. `--only release --release-dir DIR` assembles a folder with the executables, no
game module and the builder (`builder/first_run.bat` and the translator): its first start
builds `sc_game.dll` from the player's disc, after asking. It refuses to finish if any file in
it holds a translated function or a piece of `1ST_READ.BIN`.

On Linux, `tools/build.py` uses [Zig](https://ziglang.org) 0.14.1 (`ZIG`, or `--download`)
for the game module (`--only module`, giving `sc_game.so`) and for the executable `voldorium`
(`--only host`, at the glibc 2.28 baseline; SDL2 and OpenGL development files from the system),
and `--only release` assembles the Linux release folder with `builder/first_run.sh`. For
development, `port/m1/build.sh` builds with the system's gcc into `~/m1_build`, and the test
scripts run there too (`tests/netplay.sh`, `lobby.sh`, `synctest.sh`). `tests/crossplay.sh` and
`tests/lobby_cross.sh` play a Windows copy against a Linux copy in WSL and compare their state
checksums. The lobby key is computed from the same sources on both systems, so a Windows and a
Linux build of the same version meet in the lobby.

Everything you build stays on your machine. Do not upload or share `sc_game.dll`, `sc_game.so`,
a `Voldorium` folder that holds one, or `port/build/`: they contain code translated from the
game.

## Playing

Run `Voldorium.exe` from the player folder. The first time, it asks for your disc dump (the
`.gdi` file) and remembers it in `voldorium.ini`. It starts full screen at your desktop's
resolution; the main menu's **GRAPHICS** entry changes resolution, full screen,
anti-aliasing, VSync and loading speed (original or fast). Controllers and keyboard go through SDL2.

**Online Versus** is on the main menu. It lists players around the world (and on your local
network) who are searching with the same build. Pick one to challenge; the other player sees
your ping and accepts or declines. Players are shown as OPPONENT unless they set a nickname
(`[net] name = ...` in `voldorium.ini`); computer names are never sent. Both players must have
built from the same sources: the lobby only matches identical builds.

Settings (`voldorium.ini`): `[video] fullscreen, resolution, window, msaa, backgrounds`
(`stretch`, `blur` or `replace`), `[net] lobby, name`, `[game] disc, vmu, loading` (`original` or `fast`). More detail is in
`port/README.md` and in the README.txt of the player folder.

## Steam Deck

Either version runs on the Deck; neither is tested on a real Deck yet, so reports are welcome.
Online Versus matches Deck, Linux and Windows players of the same version.

- **Linux (native):** copy the unpacked Linux download and your disc dump (the `.gdi` and its
  track files) to the Deck. In Desktop Mode, start `voldorium` once: it asks for the dump and
  builds the game (SteamOS's desktop includes Python and kdialog). Then in Steam choose **Games
  > Add a Non-Steam Game to My Library** and add `voldorium`.
- **Windows, through Proton:** copy the Windows `Voldorium` folder (after its first start) and
  your dump, add `Voldorium.exe` the same way, and in its **Properties > Compatibility** tick
  **Force the use of a specific Steam Play compatibility tool** and pick a recent Proton. Tested
  under Wine 10 (boot, menus, fights, and a lobby match against a Windows PC).

The Deck's controls work as a controller (Steam Input). Its 1280x800 screen shows the game in
16:9 with thin bars above and below: the game's picture is 16:9 on every screen, so that online
players see the same game. The defaults (full screen, VSync on) suit the Deck's 60 Hz screen.

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
- SDL2: zlib (linked at build time, shipped as `SDL2.dll`; on Linux the system's own)
- Zig (MIT): the Linux builder downloads it at first start; it is not shipped
- Flycast: GPL-2.0. Its sine table header (the SH-4 FSCA results, measured on the hardware)
  and its BIOS font are downloaded by the builder from Flycast's repository at a pinned commit,
  or taken from your own checkout, at build time; they are not stored here. The patches in
  `flycast-trace/` and `port/m0/flycast-hooks/` apply to a Flycast checkout and are offered
  under the GPL-2.0.