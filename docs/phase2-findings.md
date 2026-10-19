# Phase 2: SDK identification, main loop, input, projection

Target: Soulcalibur (Dreamcast, USA T1401N V1.000), `1ST_READ.BIN` loaded at `0x8C010000`.
Ghidra project `sc/ghidra/SoulCalibur`. Names live in `sc/names/*.tsv` and are applied with
`bash sc/ghidra-run.sh ApplyNames.java <tsv>`; every name has a confidence note as a plate comment.

## Memory layout

| Range | Block | Contents |
| --- | --- | --- |
| `8C010000-8C1036DF` | text | Startup, game code, SDK helper sources compiled into the game (sbinit.c) |
| `8C1036E0-8C21AF1F` | data | Game constants and initialized data (strings, tables) |
| `8C21AF20-8C24561F` | libtext | Katana libraries: Shinobi (sy, pd 1.46, gd, bu), Kamui 2, sound (~755 functions) |
| `8C245620-...` | libdata | Library data, version banners, then BSS from `8C247480` |

Both code blocks are marked read-only so the decompiler resolves literal-pool calls.
Function count: 3,211 (609 recovered from pointer-table-only code by `RecoverFunctions.java`;
a few of those found by prologue scan may be false positives inside data).
Ninja's version string is present but unreferenced: the game appears to use its own 3D
pipeline on top of Kamui rather than Ninja's draw functions.

## Main loop (`main`, 0x8C031420)

```c
game_init();
while (true) {
    view_set_camera(&camera_matrix);  // 0x8C03E9C0
    if (game_update() < 0) break;     // 0x8C031800: 15 subsystem ticks
    render_scene(&scene);             // 0x8C03ED00
    kmSetBorderColor(0);
    screen_fade_draw();               // 0x8C02014C
    kmEndScene();                     // close TA lists
    kmRender();                       // wait for previous frame, start render, swap
    frame_end_input_audio();          // 0x8C063040: pads for next frame
    g_pad_any_held = p1 | p2;         // combined button words
}
sbExitSystem(); syBtExit();
```

Frame pacing comes from `kmRender` waiting on the previous render and VBlank, so there is
one game tick per displayed frame (60 Hz NTSC/VGA). `vsync_callback` (0x8C031740) only bumps
counters. For GGPO this is a good shape: `game_update` is the simulation step, and
`render_scene` through `kmRender` is presentation that a rollback re-simulation can skip.

## Input path

1. `pdExecPeripheralServer` (0x8C21F756), called from `frame_end_input_audio`, updates
   `PDS_PERIPHERAL[24]` at `0x8C33D2DC` (0x34 bytes each: id, support, on, off, press, release,
   r, l, x1, y1, x2, y2, name, extend, old, info).
2. `game_read_pads` (0x8C061A20) reads the records via pointers from `pdGetPeripheral`
   (cached by `pads_init`), detects the device type by name, converts analog triggers to
   buttons (thresholds 0x30, or 0x1C with an option bit), and writes a **16-byte game pad
   record per port at `g_game_pads` (0x8C24A4D4 + port*0x10)**.
3. `pads_copy_to_players` (0x8C062F26) copies those records into the two player structs
   (stride 0x86DC; P1 input words at 0x8C2521D0, P2 at 0x8C25A8AC).

GGPO hook: replace the output of step 2 (the two 16-byte records) with the synchronized
inputs. Everything downstream is then deterministic input.

## Projection and screen setup

`screen_proj_init` (0x8C041980) fills a struct at `0x8C266C10`:

| Address | Name | Value |
| --- | --- | --- |
| 8C266C10 | g_screen_width | 640.0 |
| 8C266C14 | g_screen_height | 480.0 |
| 8C266C18 | g_screen_center_x | 320.0 |
| 8C266C1C | g_screen_center_y | 240.0 |
| 8C266C20 | g_proj_focal | 320 / tan(27.5 deg) |
| 8C266C24 | g_proj_fov_deg | 55.0 |
| 8C266C28 | g_aspect_scale_x | 1.0 (set in game_init) |
| 8C266C2C | g_aspect_scale_y | 1.0 VGA, 0.92 NTSC, 1.1 PAL |

Users: `view_set_camera` multiplies the view matrix by the X/Y scales every frame, and
`xform_project_clip` (0x8C0B62C0) / `xform_project_simple` (0x8C0B16C6) use focal and scales
for perspective division.

Widescreen plan: set `g_aspect_scale_x = 0.75` for 16:9 (Hor+, same vertical FOV), widen the
X clip bounds in the projection functions, and fix hard-coded 640x480 2D like
`screen_fade_draw`. Display resolution is chosen by `sbInitSystem(mode, bpp, count)` through
`km_InitDisplayMode` (mode bit 0x10 = 640 wide, 0x20 = 480 high).

## Not done yet

- Most of the ~755 library functions and ~2,400 game functions are still unnamed.
- Whether the simulation reads anything besides pad records and RNG per frame (needed for
  GGPO determinism) is unverified.
- The X clip bounds inside the projection functions are not yet located.
- Names marked medium/low have not been confirmed against a running game.
