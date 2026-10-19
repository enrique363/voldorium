/* TA display lists (ta.c) and the renderer that draws them (render.c, tex.c, video.c). */
#pragma once
#include "sh4rt.h"

#define TA_MAXV (1 << 20)
#define TA_MAXP (1 << 18)
#define TA_MAXMV (1 << 16)	/* opaque modifier volume triangles per frame */

typedef struct { float x, y, z, u, v; u32 col, ofs; } TaVert;	/* z = 1/w; colours ARGB */
typedef struct
{
	u32 isp, tsp, tcw;		/* PowerVR ISP/TSP instruction and texture control words */
	u8 list, sprite;		/* list type 0 opaque, 2 translucent, 4 punch-through */
	u32 first, count;		/* triangle strip: vertices [first, first + count) */
	u8 clip;				/* user tile clip: 0 none, 2 draw inside the rectangle, 3 outside */
	u8 via_sq;				/* global parameter arrived by store queue (immediate 2D primitives) */
	u32 caller;				/* innermost game function when it arrived (store queue only) */
	u8 cls;					/* 2D class of the code that drew it (M1_2D_*), 0 = 3D */
	u16 grp;				/* 2D group (m1_2d_group) of the code that drew it */
	u8 shadow;				/* PCW bit 7: affected by modifier volumes (cheap shadow) */
	u16 clip_rect[4];		/* x0, y0, x1, y1 in pixels (from 32-pixel tiles) */
} TaPoly;
typedef struct
{
	u32 key;				/* TA_ISP_BASE of the frame */
	int used;
	u64 serial;
	TaVert *v;
	TaPoly *p;
	u32 nv, np;
	float *mv;				/* opaque modifier volume triangles: 9 floats (x, y, 1/w x 3) each */
	u8 *mvmode;				/* per triangle: volume instruction (bits 0-2), volume last (bit 3), new parameter (bit 7) */
	u32 nmv;
} TaCtx;

void ta_input(const u8 *data, u32 len);
extern int ta_via_sq;		/* set by hw.c around store-queue TA input */
extern u8 ta_cur_class;		/* 2D class of the parameter being input (hw.c sets it) */
extern u16 ta_cur_group;	/* and its 2D group */
u8 ta_class_at(u32 ram_addr);	/* 2D class recorded for a vertex-buffer slot in RAM */
u16 ta_group_at(u32 ram_addr);	/* 2D group recorded for it */
void ta_list_init(void);
void ta_list_cont(void);
TaCtx *ta_render_ctx(void);
extern u64 ta_params, ta_vertices, ta_lists_ended;

/* VRAM (hw.c): 8 MB in the 64-bit (texture) layout; page generations count writes per 4 KB */
#define VRAM_SIZE (8u << 20)
u8 *hw_vram(void);
u32 hw_vram_gen(u32 page);
u32 hw_palette_gen(void);
u32 vram_rd32_map32(u32 offset32);	/* a 32-bit path offset, as the CPU sees 0x05xxxxxx */

/* render.c / video.c */
int video_enabled(void);
void video_poll(void);
u32 video_buttons(void);
void render_frame(TaCtx *c);
