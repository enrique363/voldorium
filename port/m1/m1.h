/* M1 executable: shared declarations. */
#pragma once
#include "sh4rt.h"
#include <stdio.h>

#define VOLDORIUM_VERSION "1.0.9"	/* shown in the window title and on the main menu (render.c); also voldorium.rc */

extern SH4 *m1_cpu;
void m1_stop(const char *why);
void m1_fatal(const char *fmt, ...);	/* startup error: log it, show it (Windows), exit */
void m1_print_stack(FILE *f);

/* hw.c: hardware layer (register file, VRAM/AICA RAM arrays, store queues, access log) */
void hw_report(void);
void hw_tick(u32 cycles);				/* advance the virtual clock */
u32 hw_pending_intevt(u32 imask);		/* INTEVT of an interrupt to take now, or 0 */
void hw_raise(int istnrm_bit);
void hw_cancel(int istnrm_bit);
double video_pace_hz(void);	/* frame clock rate: 60, 62 when a ~60 Hz VSync display paces (render.c) */
const u8 *hw_font_12x24(void);	/* the BIOS font's 12x24 glyphs, or 0 without the font */

/* hle.c: BIOS syscalls and other high-level replacements; returns 1 when it answered the call */
int hle_call(SH4 *c, u32 addr);
void gd_open(const char *track_path, u32 track_lba);

/* threads.c: SDK thread context save/restore on host coroutines */
int thread_hle(SH4 *c, u32 addr);
void thread_report(FILE *f);

/* maple.c: controller in port A */
void maple_report(FILE *f);

/* widescreen 2D classes (m1.c); M1_2D_* = 0 means 3D or unknown */
enum { M1_2D_HUD = 1, M1_2D_PRIM = 2, M1_2D_SCREEN = 3, M1_2D_TILED = 4 };
void m1_check_watchdog(void);
extern int m1_verbose;	/* per-DMA / per-read log lines (development runs; M1_VERBOSE=1 in the player form) */
void state_init(const char *ranges_path);	/* the rollback range table (savestate_ranges.tsv) */
int state_timer(SH4 *c, u32 addr);
void state_report(FILE *f);
extern u8 m1_2d_class;
extern u16 m1_2d_group;	/* new id on each entry to a known 2D function (one string, one HUD part) */
