# Changelog

All builds need your own legal disc dump of SoulCalibur (USA, T-1401N, V1.000); see the README.

## Unreleased

### Added
- **One-click builder**: double-click `Build Voldorium.bat` (or drop your `.gdi` onto it). It
  fetches pinned, SHA-256-checked tools into `toolchain/` (nothing is installed), runs every
  step and opens the finished `Voldorium` folder. No Git Bash, Python install or Flycast checkout
  needed any more; the two Flycast files the build uses are downloaded at a pinned commit.
- `tools/build.py`: every build step in one script (the Git Bash scripts now call it).
- README: step by step build instructions and a Troubleshooting table.

### Changed
- The online build key ignores line endings, so every checkout of the same sources matches.

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
