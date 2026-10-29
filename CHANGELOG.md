# Changelog

All builds need your own legal disc dump of SoulCalibur (USA, T-1401N, V1.000); see the README.

## 1.1.2 (2026-10-29)

### Added
- **A native Linux version** (`Voldorium-1.1.2-linux.tar.gz`), made the same way as the
  Windows one: the download holds no game code, and the first start builds the game module
  `sc_game.so` from your own disc, after saying what it downloads and asking. It uses the Zig
  compiler (pinned, about 50 MB, SHA-256 checked) and Python 3 from the system, asks for the
  disc dump with the desktop's file picker (zenity or kdialog) and shows the build in a terminal
  window. One build runs on any 64-bit Linux with glibc 2.28 or newer (2018 on), SDL2 and
  OpenGL.
- **Online Versus between Linux and Windows**: both meet in the same lobby (local network or
  internet, direct or through the relay) and play in step with rollback. In tests a Windows and a
  Linux copy played lobby matches and two-peer sessions with about 200 rollbacks each, and every
  state checksum both reported was identical.
- Tests: `port/tests/crossplay.sh` (a Windows and a Linux peer) and `port/tests/lobby_cross.sh`
  (a lobby match between them through a local lobby server); the netplay, lobby and SyncTest
  scripts also run on Linux.

### Changed
- GGPO (`third_party/ggpo`) builds on Linux: its Linux platform layer did not compile upstream.
  Its packets are byte for byte the same on both systems.
- Linux builds: the game runs on a 64 MB stack as on Windows, the time limit and the hang report
  of development runs work there too, and a copy started from the desktop logs to `m1_log.txt`
  next to the executable.

### Fixed
- Online matches no longer freeze when a fight ends. Leaving a fight raises the input delay
  (2 frames in fights, 8 elsewhere), and GGPO sent the other player only the new frame, not the
  frames the longer delay inserts; it then stopped on an internal check (on Windows behind an
  unseen message box) and the other player saw the connection drop. GGPO now sends those
  frames too (`third_party/ggpo`, `udp_proto.cpp`).

## 1.1.0 (2026-10-27)

### Changed
- **The game's code is now a module of its own, `sc_game.dll`, and `Voldorium.exe` holds none
  of it** (885 KB instead of 17.5 MB; the build checks for translated functions and pieces of
  the game's executable). The module talks to the runtime through a fixed table of about twenty
  functions and variables. Same game: screenshots from the menus into a fight are byte-identical
  to 1.0.10, SyncTest and netplay checksums match, and the game runs within a few percent of
  1.0.10's speed (unpaced; far inside a 60 fps frame). Builds of the module are reproducible: the
  same disc and version give the same file on every PC.
- The lobby's build key now also covers the translator, so players who meet online run the same
  game module as well as the same executable.

### Added
- **A ready-to-run Windows download** (`Voldorium-1.1.0-windows.zip`, about 2 MB): unzip it,
  start `Voldorium.exe`, choose your disc dump. It holds no game code.
- **First start builds the game from your disc.** A Voldorium folder without `sc_game.dll`
  (the download, or `tools/build.py --only release`) builds it at its first start, after saying
  what it downloads and asking: `builder/first_run.bat` extracts, translates and compiles, in
  about half a minute on a fast PC once the tools are there. A module from another version is
  rebuilt the same way. See the README's Installing section.

## 1.0.10 (2026-10-24)

### Fixed
- **Arcade: the VS screen before Inferno was garbled for a moment** (enrique363/voldorium#2).
  The VS screen's last picture is held while the next stage loads, but the hold only started
  after a megabyte or more of textures at once, and the stage before Inferno is smaller. It now
  starts as soon as the stage's textures overwrite the VS screen's own.

## 1.0.9 (2026-10-24)

### Fixed
- **GRAPHICS > LOADING sent the game into character select** and it could not be left
  (enrique363/voldorium#2): the menu did not recognise its new line and passed the choice on to
  the game's own menu. Selecting it now switches between ORIGINAL and FAST as intended.

## 1.0.8 (2026-10-24)

Fixes from the first issue reports (enrique363/voldorium#1, enrique363/voldorium#2) and more.

### Fixed
- **Practice: FREESTYLE SETTINGS > GUARD IMPACT stopped the game** ("call/jump to 8c032a36: no
  translated function"). The translator now also follows the calls made by functions found
  through pointer tables, which brought in the one function that was missing.
- **Practice menus in 16:9**: the pause menu, the command list and the attack data panel were
  pulled apart (some lines at the left, some in the middle, some at the right). In fights, a
  window's pieces now move together with the window.
- **Character select: dark and bright lines sweeping across the animated background.** The
  light streaks behind the portraits are 3D strips; a strip with a point behind the camera was
  mistaken for flat 2D artwork and narrowed for 16:9 while its neighbours were not, so cracks
  opened between them and showed what lies behind (a bright band, or black). Triangles that
  reach behind the camera are also left out now, as the Dreamcast does: some graphics cards drew
  them as black streaks.
- **Options menu: the scrolling logos flickered** at the sides and were cut in half: they now
  repeat across the whole 16:9 picture.
- **Mission Battle map: the markers flickered and jumped** between two positions (whenever the
  flying comet reached the edge of the screen). The map is now drawn undistorted with its
  markers, with a blurred copy of the map at the sides.
- **Character select (Practice, VS): the two big portraits were stretched** once both players
  had picked: they are no longer taken for a background picture.
- **Before each fight: a garbled VS screen, and the VS screen rushed** (enrique363/voldorium#2).
  Disc reads were instant, so the VS screen was cut short and its sideways sweep-in barely
  played, and the stage's textures replaced its pictures while it was still shown. Disc reads
  outside fights now take the Dreamcast drive's time (as in Flycast), so the VS screen lasts as
  long as on the console, and its last whole picture stays on screen while the fight loads.
  Fights keep instant reads (online play is not affected).
- **The intro ran too fast**: it streams from the disc, so with instant reads it outran the
  original. With the drive's timing it now lasts as long as in Flycast (9031 frames against
  about 9040).

### Added
- **Fast loading** (GRAPHICS menu: LOADING ORIGINAL / FAST; `[game] loading` in
  `voldorium.ini`): FAST makes every disc read instant, as in 1.0.7 (the VS screen is then cut
  short again, but no longer garbled). Online matches always load at the original speed, so both
  players stay in step.
- **High resolution hit level labels in Practice**: the HIGH / MID / S-MID / LOW and "!" labels
  shown at a hit were 64 x 64 pixel pictures drawn several times larger (blocky, and no
  smoothing could add the detail). They are redrawn at 4 times the size in the same style
  (`tools/hitlabels.py`), in every colour the game uses (the art follows the game's palette),
  with the same proportions as the originals. New: replacement art for any texture
  (`textures` folder, `port/m1/tex.c`).

### Changed
- **The memory card save screen** ("Saving SOULCALIBUR game data...", on the way back to the
  main menu) takes 0.3 s instead of 2.5 s: it runs 8 times as fast, showing every eighth
  frame. The game saves exactly as before.

## 1.0.7 (2026-10-21)

### Fixed
- **No version number and no system text** when the game was started from another folder (a
  shortcut or launcher whose "Start in" folder is not the Voldorium folder): the BIOS font
  (`biosfont.bin`) was looked for in that folder. It is now always looked for next to
  `Voldorium.exe`. The screen resolution was not the cause.

## 1.0.6 (2026-10-21)

### Fixed
- The version number in the main menu's corner was hard to see over the unlocked main menu's
  bright background: it now has a dark outline, readable over any background.

## 1.0.5 (2026-10-21)

### Fixed
- **Builder with the Microsoft Store Python** stopped with "unable to open output file ... No
  such file or directory": that Python keeps what it writes under `AppData` in a private copy the
  compiler cannot see. The builder skips it (it then uses its portable Python) and keeps its build
  files in `work\` inside the folder instead of `AppData`. The game itself is unchanged.

## 1.0.4 (2026-10-21)

### Fixed
- **Music dropped out and jumped in level** (the intro, stage music): streamed music plays from a
  ring buffer in the AICA's "ADPCM long stream" mode, and at each lap the decoder either restored
  an old state or stopped decoding until it caught up, holding one value for a whole lap. It now
  carries on through the loop, as the chip does. The intro's level now follows Flycast's within
  1 dB (it swung by up to 20 dB).

### Added
- Footage recording for videos: `M1_RECORD=file.mp4` (with `M1_FFMPEG` and `M1_RESOLUTION`)
  writes one frame per emulated refresh to ffmpeg, in step with `M1_WAV`; it runs at any speed.

## 1.0.3 (2026-10-20)

### Fixed
- **Arcade mode froze** after a few fights: the KO slow motion waits in a short loop for a flag
  that an interrupt sets, and the translated loop never let interrupts run (they were only taken
  at function calls). Tight loops now check in every 4096 instructions, as on the hardware; an
  arcade run now goes through every stage (`port/tests/arcade.sh`).

### Added
- A frozen game writes a hang report to `m1_log.txt` (what it was running) after 8 seconds
  without a frame, so a freeze can be found from the log.
- **One-click builder**: double-click `Build Voldorium.bat` (or drop your `.gdi` onto it). It
  fetches pinned, SHA-256-checked tools into `toolchain/` (nothing is installed), runs every
  step and opens the finished `Voldorium` folder. No Git Bash, Python install or Flycast checkout
  needed any more; the two Flycast files the build uses are downloaded at a pinned commit.
- `tools/build.py`: every build step in one script (the Git Bash scripts now call it).
- README: step by step build instructions and a Troubleshooting table.

### Changed
- The online build key ignores line endings, so every checkout of the same sources matches.
- Reproducible builds: no time stamps or folder names in the executables (they held the build
  folder's path, and so the user name), so the same version builds byte-identical files on
  every PC.

## 1.0.2 (2026-10-19)

### Added
- **VSync** (on by default; GRAPHICS menu and `[video] vsync`): no more screen tearing. Adaptive
  where the graphics driver supports it, so a late frame (a rollback in an online match) shows at
  once instead of slowing the game. On 60 Hz displays the display's refresh sets the pace (steady
  16.66 ms frames); on faster displays the game keeps 60 frames a second, and G-Sync / FreeSync
  work in full screen.
- The player README explains what VSync does and the recommended settings (on for every
  monitor; off only on a 60 Hz monitor without G-Sync / FreeSync, for the lowest input delay).

## 1.0.1 (2026-10-19)

### Fixed
- **Every main menu mode works.** Some modes stopped with a black screen and "no translated
  function": routines the game only reaches through tables of function pointers or stored
  callbacks had never been translated. A new scanner (`tools/find_entries.py`) found 150 of
  them, and a new test (`port/tests/modes.sh`) runs every main menu entry.
- **Missing text and pictures are back**: the Art Gallery's descriptions, item numbers, points
  and "?" thumbnails, the VS screen's score and DRAW count, the TEAM BATTLE title. The
  renderer's alpha test compared floating point values and dropped fully opaque text; it now
  compares 8-bit values as the Dreamcast does.
- **Colours**: red and blue were swapped in every vertex colour (the VERSUS BATTLE bar was red
  instead of blue, the NEW labels blue instead of red).
- **Online match ending**: when a match is cut off (lost connection, the opponent quits or
  closes the game, or Esc), both players go back to the main menu, with the cursor on ONLINE
  VERSUS and the reason in the help line. Before, both were stuck on a black screen.
- **VS BATTLE** no longer says "A Controller was removed": a second game controller is now
  player 2.
- **L / R triggers** reach the game (they were always released): the gallery's picture viewer
  changes pictures with them. Keyboard Q / W, a controller's triggers or shoulder buttons.
- Changes made in the GRAPHICS menu (resolution, full screen, anti-aliasing) now also apply to
  online matches.
- Building from the repository uses its own copy of GGPO (`third_party/ggpo`).

### Added
- **Smooth text**: menu and HUD text is drawn from upscaled copies of the game's font
  textures, so it keeps its size with smooth edges at high resolutions. `[video] text = sharp`
  in `voldorium.ini` restores the original pixels.
- **Version number** in the window title ("Voldorium ver. 1.0.1") and in the lower right corner
  of the main menu.
- `tools/vmu_import.py` copies a save in Nexus .dci format into the memory card image
  (`vmu_a1.bin`), replacing the game's old save.

### Changed
- The player README explains that the game confirms with A (Z on the keyboard) everywhere
  except the main menu, where Start also works.

## 1.0.0 (2026-10-19)

First version.

- Native Windows port by static recompilation of the game's SH-4 code to C, with a native
  runtime (BIOS, video, sound, controllers, memory card).
- 16:9 widescreen with undistorted HUD, menus and text; full screen at the desktop's
  resolution; a GRAPHICS menu for resolution, full screen and anti-aliasing.
- Online Versus in the main menu: a worldwide lobby with ping, accept / decline, NAT hole
  punching with a relay fallback, and GGPO rollback netcode.
- A built-in virtual memory card; optional 16:9 replacement artwork.
