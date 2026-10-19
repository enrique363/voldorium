# Phase 3: 16:9 widescreen patch list

Names below are applied in Ghidra (`sc/names/40-widescreen.tsv`). Raw scan data:
`sc/facts/screen_consts.txt` (32-bit float/int literal pools), `sc/facts/scan2d.tsv`
(decompiled C, includes 16-bit `mov.w` immediates).

## Approach: anamorphic 640x480 virtual screen

Keep the game's 640x480 coordinate space everywhere and let the output stretch it to 16:9
(the port's renderer, or Flycast's "stretch" aspect for testing). Then:

- 3D gets correct proportions by narrowing the projection X scale to 0.75 (Hor+, same
  vertical FOV, more visible horizontally).
- **Rule: no textured or text 2D is ever stretched** (HUD, menus, fonts and artwork would look
  distorted). Only untextured solid-colour fills (fades, flashes, plain bars) may span 16:9.
- HUD and in-battle text: compressed about an anchor, `x' = a + (x - a) * 0.75` with a = 0
  (left), 320 (centre) or 640 (right), which restores their original proportions.
- Menus, title, character select, gallery and other screens: pillarboxed, `x' = 80 + x * 0.75`
  (a centred 4:3 area, undistorted).
- Tiled or scrolling textured layers (menu wallpaper, stage smoke/weather): drawn undistorted
  (compressed by 0.75) with their coverage extended so they fill the width.

This avoids touching the hardware clip window, render targets and tile setup, which all
stay 640x480. A "true wide" mode (virtual width 853.33) is listed at the end; it is more
work for no visible gain at the same output resolution.

## What the projection does (findings)

- `view_set_camera` (0x8C03E9C0) multiplies the X row of the view matrix by
  `g_aspect_scale_x` (0x8C266C28) and the Y row by `g_aspect_scale_y` every frame.
- The projectors (`project_point`/`project_points`/`project_points_rev` 0x8C0C08F2..,
  the model renderers called from `render_scene`) then compute `sx = x/z + 320`,
  `sy = y/z + 240` with focal length and aspect already folded into the matrix.
- Particle and effect sizing (`xform_project_clip`, `xform_project_simple`,
  0x8C0AECE0..0x8C0B5740) reads `g_proj_focal` and both aspect scales directly, so effects
  follow the same change.
- **There are no software X clip bounds in the 3D path.** The projectors only flag points
  behind the camera (z <= 0). Horizontal clipping is done by the PowerVR against the pixel
  clip window set through `kmSetPixelClipping` (0x8C223FA2). No object-level horizontal
  frustum culling was found; confirm at runtime by watching the screen edges for pop-in.
  **Runtime result (port, 2026-10-16):** there is a screen-space mesh cull. The stage mesh
  projector 0x8C03A9CC counts vertices with |x - 320| < 320, and its callers skip a mesh with a
  count of 0. Behind-camera vertices come out as (X * 1e30, Y * 1e30, 1/w = 1e30). In Hor+ the
  count dropped large near-camera meshes at the screen sides. The port counts any vertex in front
  of the camera in wide mode (see `sc/port/README.md`, Widescreen).

## Patch list

### P1. 3D aspect (required, 1 value)

| Where | Now | 16:9 |
| --- | --- | --- |
| `g_aspect_scale_x` 0x8C266C28, written once in `game_init` at 0x8C031586 (VGA), 0x8C031598 (NTSC), 0x8C0315D0 (PAL) | 1.0 (`fldi1 fr15`) | 0.75 (`0x3F400000`) |

Leave `g_aspect_scale_y` (1.0 VGA / 0.92 NTSC / 1.1 PAL) and `g_proj_focal` unchanged.
Emulator test now: 32-bit RAM write `0x0C266C28 = 0x3F400000` after boot (the game never
rewrites it), with 16:9 stretch output. Port: assign 0.75 in `game_init`.

### P2. Hardware clip window (no change in anamorphic mode)

| Function | Call | Note |
| --- | --- | --- |
| `clip_window_reset` 0x8C01C92A | `kmSetPixelClipping(0,0,640,480)` | full screen |
| `clip_window_init_mode` 0x8C031E08 | `(0,0,640,480)` | full screen |
| `clip_window_set_mode` 0x8C01CC60 | 640x480 or smaller | mode dependent |
| `clip_window_wipe_update` 0x8C01C9C0 | animated rect clamped to 0..640/0..480 | iris/wipe transition |
| 0x8C037884, 0x8C039EE0 | `(0,0,0)` | disables output during transitions |

### P3. Full-screen / full-width 2D

Rule (project decision, 2026-10-15): HUD, menus, fonts and any textured artwork are never stretched.

#### P3a. Untextured solid-colour fills: may span the full 16:9 width

| Function | What |
| --- | --- |
| `screen_fade_draw` 0x8C02014C | `draw_rect_2d({0,0,640,480})`, flat fade to/from colour |
| `draw_caption_bar` 0x8C01E440 | flat bar `{0,402,640,34}` behind subtitles (its text is P4, centre) |
| `ranking_draw_row_bar` 0x8C0C9E20 | row bars `{x,y,640,64}`, only if untextured; otherwise P5 with its screen |

#### P3b. Tiled / scrolling textured layers: undistorted, coverage extended

Draw with x compressed by 0.75 (so texels keep their shape) and widen the drawn span so the
layer still reaches both 16:9 edges. These layers repeat, so extra width is just more tiles.

| Function | What |
| --- | --- |
| `menu_bg_scroll_draw` 0x8C06E54E | tiled scrolling menu wallpaper, spans -192..832 and wraps at 1024, used by 18 menu screens |
| `stage_fx_smoke_dispatch` 0x8C0CF2C0 and 0x8C0DE720..0x8C0DEF60 | smoke overlays (2560-wide strips) |
| 0x8C0DAD20, 0x8C0DBFC0, 0x8C0DE000 | stage screen-space effects (weather/grass) |
| `stage_fx_set_scroll` 0x8C0D9340, 0x8C0D9420, 0x8C0D9560, 0x8C0D966E | `640 - x` offsets for those effects (scale with the extended span) |

#### P3c. Full-screen textured pictures: pillarbox (P5)

0x8C053238, 0x8C059538, 0x8C059602, 0x8C059720, 0x8C0FCE60 build a `{0,0,640,480}` rect on the
stack in museum/menu code (which draw call consumes it is unverified: 0x8C0FE6FC is
`__sdivsi3`, not a drawer). Any full-screen picture, logo or gallery image goes to P5.

Default for anything unclassified: pillarbox (P5), never stretch.

### P4. HUD and in-game text: compress about an anchor

Hook the 2D primitives once, and pick the anchor per call site (by caller address in the
port, or by x range: x < 213 left, x > 427 right, else centre):

| Primitive | Callers | Role |
| --- | --- | --- |
| `draw_rect_2d` 0x8C0631D0 | 18 | flat quads |
| `draw_quad_2d_gouraud` 0x8C0632BE | 29 | gradient quads (gauges) |
| `draw_sprite_2d` 0x8C0633A8, `draw_sprite_2d_ex` 0x8C0634F8, `draw_sprite_2d_full` 0x8C0637B0 | 1-5 | textured quads |
| `draw_poly_2d_list` 0x8C063ABA | 10 | 2D polygons |
| `text_emit_glyphs` 0x8C068240 (via `text_format_draw` 0x8C0652C0) | all text | glyph quads |
| `text_printf_centered` 0x8C0681A0 | 76 | x is the centre: anchor 320 keeps it centred |

Calls that pass a full-width rect (x=0, w=640) must bypass the compression (P3).
In-battle banners in this group: "FIGHT!!", "BATTLE %d", "FINAL BATTLE" (0x8C024512),
"NEW CHALLENGER!!" (0x8C0260F2, slides in from x=640: anchor right), "CONTINUE?"
(0x8C027AE0), pause and controller-removed text (0x8C031FA0), result banners
(0x8C05E060, 0x8C05DDE0), "BURST" gauge text (0x8C09F0A0).

#### Battle HUD: exact call sites (confirmed by runtime trace)

Source: `sc/traces/hud_trace.csv`, captured with the store-queue hook in
`sc/flycast-trace/` during fight frames 120-122; decoded with `sc/tools/hud_sprites.py`.
Names are in `sc/names/70-hud.tsv`.

Call chain: `game_update` -> `hud_update_dispatch` 0x8C01D300 -> `hud_battle_draw`
0x8C0AA4C0, which loops over both players and then draws the timer. Every element picks
its position from the player's side flag `player+0x5C0C` (0 = left, 1 = right), so the
anchor follows the side, not P1/P2.

| Element | Function | Side 0 (left) | Side 1 (right) | Anchor |
| --- | --- | --- | --- | --- |
| Life gauge | `hud_draw_life_gauge` 0x8C0AAC80 | x 19.8..271.9 (table 0x8C1F5AC4) | x 368.1..620.2 (table +0x60) | left / right |
| Gauge layer 2, damage trail | `hud_draw_life_gauge_trail` 0x8C0ABA80 | same span, runtime corners | same | left / right |
| Character name plate | `hud_draw_name_plate` 0x8C0AB500 | x = 26 | x = 614 - width | left / right |
| Name / win-count text | `hud_draw_player_text` 0x8C0AB6BC | from x = 16 (24 px steps) | right-aligned to x = 624 | left / right |
| Round-win markers 20x20 | `hud_draw_round_markers` 0x8C0AAD54 | x = 256 - n*20 | x = 364 + n*20 | **centre** (they hug the timer) |
| Team-battle markers | `hud_draw_team_markers` 0x8C0AB164 | x = 252 - k*26 | x = 364 + k*26 | centre |
| Timer (2 digits, 38 px) | `text_printf_centered` from 0x8C0AA4C0 | x = 320 + shake | | centre |

Vertical positions use `g_hud_shake_y` 0x8C24A918 plus 18 (timer), 32.5 table (gauges,
drawn at +8), 54 (plates), 58 (markers); no vertical change is needed.

Port implementation: set an "x anchor" before each element call in `hud_battle_draw`
(side 0 -> 0, side 1 -> 640, markers and timer -> 320) and apply
`x' = a + (x - a) * 0.75` to every vertex those calls emit. The gauge's static table can
instead be pre-transformed once, but layer 2 builds corners at runtime from
`g_hud_player_state` 0x8C28DE28 (+0xA94 per side), so the per-call transform is the
complete fix.

Correction: phase 3 listed 0x8C0AA4C0 under "options" (P5). Its menu strings come from
a caller, the options screen 0x8C0BA380, which reuses it to preview the HUD. It is the
battle HUD.

### P5. Menus, galleries, FMV: pillarbox the whole screen

Simplest rule: while not in battle, apply `x' = 80 + x * 0.75` to all 2D (a centred 4:3
region) and keep P3 fills full-width. Screens in this group (strings in brackets):

- Title / logo 0x8C08A7B8 ("SOULCALIBUR", "THE LEGEND WILL NEVER DIE"), copyright 0x8C0F41E0
- Mode select and "PRESS START BUTTON" 0x8C05CDC0, 0x8C05AF60
- Character select 0x8C0E7940..0x8C0EB340 ("SELECT YOUR CHARACTER", weapon names), these
  write to the TA FIFO directly, not through the primitives
- Options and button config 0x8C0BB640, 0x8C0BD640, 0x8C0BA380, 0x8C0FCFA0 (0x8C0BA380
  embeds a battle-HUD preview through `hud_battle_draw`; use the P4 HUD rule there)
- Museum / collection 0x8C055E20, 0x8C056DC0, 0x8C0577A0, 0x8C057DC0, gallery pictures via
  0x8C06D62A
- Rankings and name entry 0x8C0FA230, 0x8C0F99E0, 0x8C0C9F80, 0x8C0CA540, 0x8C0CAB00, 0x8C0CB020
- Results 0x8C0F77C0 ("TEAM BATTLE RESULTS"), versus 0x8C0F4BE0
- Mission (Mission Battle) screens 0x8C050EA0, 0x8C050D1A
- Opening and endings: **there is no FMV.** The executable contains no Sofdec/CRI
  middleware. The "opening movie" is game mode 0x46/0x47 (`HOPENING INIT/MODE`, also
  `OPENINGWATCH` 0x2E/0x2F) rendered in real time from HOPEN.DAT; endings are `ENDING`
  0x24/0x25 and `ENDINGWATCH` 0x2A/0x2B (KENDING.DAT). The options even offer "RANDOM"
  and "CUSTOM" character movies. These get correct 16:9 from P1 alone; only their 2D
  overlays need the P5 rule.

Since the character-select screens bypass the primitives, the per-screen rule is easier
to apply in the port's TA/vertex submission layer (flag "2D mode" by caller) than in
game code.

### P6. Render targets and resolution (only for "true wide" or higher internal res)

- `tex_create_render_target` 0x8C06DD22 is called with 640x480 (0x8C054000, 0x8C054CA0)
  and 640x320 (0x8C050A82). In anamorphic mode these stay. For higher internal resolution
  the port should allocate them at the scaled size.
- `km_InitDisplayMode` 0x8C221816 derives 640x480 from mode bits; `screen_proj_init`
  0x8C041980 hard-codes width/height/centre. Leave both for anamorphic.

## True-wide alternative (not recommended now)

Virtual width 853.33: set `g_screen_width`, centre X 426.67, change the 320 in every
projector (about 30 functions with `sx = x/z + 320`), the clip window (P2), every
full-screen rect (P3, ~25 functions) and all menu layouts. Same visual result as the
anamorphic plan at much higher patch count.

## Side finding for GGPO

`rng_next15` 0x8C0639C8 / `rng_next_float` 0x8C063A00: the game RNG (LFSR at 0x8C159060)
is stepped `(SH4 timer & 3)` extra times per call (`sh4_ReadTimer` 0x8C220060). That makes
it non-deterministic across machines; the port must replace the timer read with a fixed
value (or a per-frame counter) for rollback to work.

More non-determinism found while hunting the HUD (all need fixing for rollback):

- `rand` 0x8C0FEEC4: C library LCG, state 0x8C21AECC (deterministic, but must be in the
  saved state).
- `stage_wind_update` 0x8C05FDC0 reads `sh4_ReadTimer` directly.
- `sun_flare_draw` 0x8C08D7E0 calls `rng_next_float` from the draw path, so drawing
  advances the game RNG; a rollback re-simulation that skips drawing would desync.

## Runtime validation (2026-10-15)

Traced Flycast with `SC_WIDESCREEN=1` (writes 0.75 to `g_aspect_scale_x` every frame) and
`SC_SHOTS` (screenshots at chosen fight frames), driven by the deterministic autoplay.
Comparisons: `sc/traces/shots/widescreen_{1..4}.png` (left 4:3, right 16:9 stretched to
853x480). Tool: `sc/tools/ws_compare.py`.

- **3D geometry is correct**: natural proportions after the 16:9 stretch, wider view (Hor+).
  At fight frame 150 the 4:3 frame cuts both fighters at the edges; 16:9 shows them whole.
- **No edge pop-in** seen: stage geometry fills the extra width in all four shots.
- **HUD stretches by 4/3** as predicted; the P4 anchors are needed.
- **Gameplay is unchanged by the aspect setting**: a per-frame state log (`SC_STATELOG`)
  over 2,044 frames shows identical player structs, battle state, camera and both RNG
  states between 4:3 and 16:9. Only the vsync counter differs (emulator vblank timing varies
  with render workload). So netplay peers can use different aspect settings.

Pitfall found on the way: autoplay originally chose buttons from the vsync counter, so a
different render workload changed its inputs and made the two fights diverge. It now uses the
main-loop iteration count. The same lesson applies to the port: anything keyed to vblanks
must be keyed to simulated frames instead.

## Next checks

1. Runtime test of P1 in Flycast (RAM write + 16:9 stretch): look for edge pop-in on wide stages.
2. Identify the HUD gauge/timer/round-marker call sites for P4 anchors.
3. Locate the FMV (Sofdec) draw path.
