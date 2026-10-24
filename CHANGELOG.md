# Changelog

All builds need your own legal disc dump of SoulCalibur (USA, T-1401N, V1.000); see the README.

## Unreleased

Fixes from the first issue report (enrique363/voldorium#1).

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
