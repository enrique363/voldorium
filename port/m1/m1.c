/* M1: first native executable. Boots 1ST_READ.BIN through the translated code (build/sweep)
 * with a minimal hardware layer and HLE BIOS, and stops at the first wait for the hardware
 * (a hardware register polled in a tight loop) or when the watchdog fires.
 *
 * usage: m1 <sc dir> [seconds]
 *   <sc dir>/extracted/1ST_READ.BIN, <sc dir>/extracted/IP.BIN, <sc dir>/disc/gdi/soulcalibur03.bin
 * Writes a hardware access summary and the first accesses in order to m1_hw.txt.
 */
#include "sh4rt.h"
#include "m1.h"
#include "ta.h"
#include <signal.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

struct sh4_fn { u32 addr; void (*fn)(SH4 *); };
extern const struct sh4_fn sh4_fns[];
extern const int sh4_nfns;

u8 *sh4_ram;
SH4 *m1_cpu;

/* ---- shadow call stack (diagnostics only) ---------------------------------------------- */
#define MAX_DEPTH 512
static u32 call_stack[MAX_DEPTH];
static int depth;
static u64 calls, irqs_taken;

/* frame-loop functions counted for the stop report; kmRender's first call is a milestone */
static const struct { u32 addr; const char *name; } watch[] = {
	{ 0x8c031800, "game_update" }, { 0x8c03e9c0, "view_set_camera" }, { 0x8c03ed00, "render_scene" },
	{ 0x8c224be0, "kmEndScene" }, { 0x8c0aa4c0, "hud_battle_draw" }, { 0x8c068240, "text_emit_glyphs" }, { 0x8c2246a0, "kmRender" }, { 0x8c063040, "frame_end_input_audio" },
};
static u64 watch_count[sizeof(watch) / sizeof(watch[0])];
static void watch_report(FILE *f)
{
	for (unsigned i = 0; i < sizeof(watch) / sizeof(watch[0]); i++)
		fprintf(f, "  %-22s %08x called %llu times\n", watch[i].name, watch[i].addr, (unsigned long long)watch_count[i]);
}
static u32 recent[64][4];	/* last calls: target, PR, r4, r15 */

u64 m1_calls(void) { return calls; }
int m1_verbose = 1;
u64 cyc_src[4];	/* virtual cycles by source: calls, hardware accesses, loops, other (M1_PERF) */
u32 m1_current_function(void) { return depth > 0 && depth <= MAX_DEPTH ? call_stack[depth - 1] : 0; }

void m1_print_stack(FILE *f)
{
	fprintf(f, "call stack (innermost last):\n");
	for (int i = depth > 24 ? depth - 24 : 0; i < depth; i++)
		fprintf(f, "  %2d %08x\n", i, call_stack[i]);
}

void m1_stop(const char *why)
{
	fflush(stdout);
	fprintf(stderr, "\n=== M1 stop: %s\n", why);
	fprintf(stderr, "%llu calls through the dispatcher\n", (unsigned long long)calls);
	fprintf(stderr, "%llu interrupts taken\n", (unsigned long long)irqs_taken);
	thread_report(stderr);
	state_report(stderr);
	{
		void aica_report(FILE *f);
		aica_report(stderr);
	}
	{
		void netplay_report(FILE *f);
		netplay_report(stderr);
	}
	maple_report(stderr);
	{
		extern u64 tag_writes, tag_writes_2d;
		fprintf(stderr, "vertex-buffer writes tagged: %llu, with a 2D class: %llu\n", (unsigned long long)tag_writes,
				(unsigned long long)tag_writes_2d);
		extern u64 tag_page_2d[256];
		for (int i = 0; i < 256; i++)
			if (tag_page_2d[i] > 1000)
				fprintf(stderr, "  2D writes to 8c%02x0000: %llu\n", i, (unsigned long long)tag_page_2d[i]);
	}
	if (m1_cpu)
	{
		fprintf(stderr, "registers:");
		for (int i = 0; i < 16; i++)
			fprintf(stderr, "%s r%d=%08x", i % 8 ? "" : "\n ", i, m1_cpu->r[i]);
		fprintf(stderr, "\n pr=%08x sr=%08x\n", m1_cpu->pr, sh4_get_sr(m1_cpu));
	}
	watch_report(stderr);
	m1_print_stack(stderr);
	if (getenv("M1_DUMP"))
	{
		FILE *d = fopen("m1_ram.bin", "wb");
		fwrite(sh4_ram, 1, 16 << 20, d);
		fclose(d);
		extern u8 *hw_vram(void);
		d = fopen("m1_vram.bin", "wb");
		fwrite(hw_vram(), 1, 8 << 20, d);
		fclose(d);
	}
	fprintf(stderr, "last calls (oldest first): target / pr\n");
	for (u64 i = calls > 64 ? calls - 64 : 0; i < calls; i++)
		fprintf(stderr, "  %08x / %08x\n", recent[i % 64][0], recent[i % 64][1]);
	hw_report();
#ifdef _WIN32
	/* player form: tell the player why the game stopped, unless they quit themselves */
	if (!m1_verbose && strcmp(why, "window closed") && strcmp(why, "quit (Esc)"))
		MessageBoxA(0, why, "Voldorium", MB_ICONINFORMATION);
#endif
	exit(0);
}

void online_menu(SH4 *c, void (*f)(SH4 *));

void m1_fatal(const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", msg);
	fflush(stdout);
	fflush(stderr);
#ifdef _WIN32
	MessageBoxA(0, msg, "Voldorium", MB_ICONERROR);
#endif
	exit(2);
}

static void (*find(u32 a))(SH4 *)
{
	int lo = 0, hi = sh4_nfns - 1;
	while (lo <= hi)
	{
		int m = (lo + hi) / 2;
		if (sh4_fns[m].addr == a) return sh4_fns[m].fn;
		if (sh4_fns[m].addr < a) lo = m + 1; else hi = m - 1;
	}
	return 0;
}

/* ---- interrupts: the game's own handlers, called at dispatcher calls ---------------------
 * On hardware, an interrupt enters at VBR+0x600, where the game's stub stores INTEVT at
 * 8C2416F4 and jumps to 8C241420: that routine saves the whole context, sets IMASK=15,
 * FPSCR=00040001, and calls handler = *(VBR + 0x200 + (INTEVT - 0x200) / 8) with r4 = INTEVT,
 * then restores the context and returns with rte. The context save/restore and rte cannot be
 * translated (SSR/SPC), so this does the same from C at a safe point: before a call. */
static int in_irq;
/* per-thread: threads.c saves it with each context and restores it on a switch */
int m1_get_irq(void) { return in_irq; }
void m1_set_irq(int v) { in_irq = v; }


static void take_interrupt(SH4 *c, u32 intevt)
{
	const u32 handler = rd32(c->vbr + 0x200 + ((intevt - 0x200) >> 3));
	if (handler == 0 || handler == 0x8c241700u)	/* 8C241700: the plain return path */
		return;
	SH4 saved = *c;
	in_irq = 1;
	irqs_taken++;
	wr32(0x8c2416f4, intevt);
	c->sr_ctl = (c->sr_ctl & ~0x10000000u) | 0xf0u;	/* BL off, IMASK 15 */
	sh4_set_fpscr(c, 0x00040001);
	c->fpul = 0;
	c->r[4] = intevt;
	c->pr = 0;
	sh4_call(c, handler);
	*c = saved;
	sh4_set_host_fpmode(c->fpscr);
	in_irq = 0;
}

/* ---- 2D classes for widescreen (sc/docs/phase3-widescreen.md) ----------------------------
 * 1 battle HUD (hud_battle_draw), 2 2D primitives and the text engine, 3 screens that write
 * the TA directly (character select), 4 tiled scrolling layers (menu wallpaper). */
/* Widescreen: the stage mesh projector 0x8C03A9CC returns how many projected vertices fall
   within 320 of the screen centre, and its callers skip the mesh when none does. In Hor+
   that drops large near-camera meshes that cover the screen with all vertices off it, so in
   wide mode a mesh with any vertex in front of the camera counts as visible. Output vertices
   are 16 bytes (x, y, 1/w, attribute); vertices behind the camera carry 1/w equal to the
   projector's sentinel, the third float of its constant pool at 0x8C03AA30. */
static void wide_mesh_visible(SH4 *c, u32 out, u32 n)
{
	if ((s32)n <= 0)
		return;
	const u32 sentinel = rd32(0x8c03aa38u);
	u32 front = 0;
	for (u32 i = 0; i < n; i++)
		front += rd32(out + 16 * i + 8) != sentinel;
	c->r[0] = front;
}

u8 m1_2d_class;
u16 m1_2d_group;

static u8 class_of(u32 a)
{
	switch (a)
	{
	case 0x8c0aa4c0: return M1_2D_HUD;		/* hud_battle_draw */
	case 0x8c0631d0: case 0x8c0632be: case 0x8c0633a8: case 0x8c0634f8: case 0x8c0637b0:
	case 0x8c063aba:						/* draw_rect_2d .. draw_poly_2d_list */
	case 0x8c068240: case 0x8c0681a0: case 0x8c0652c0:	/* text_emit_glyphs, text_printf_centered, text_format_draw */
	case 0x8c02014c: case 0x8c01e440:		/* screen_fade_draw, draw_caption_bar */
		return M1_2D_PRIM;
	case 0x8c06e54e: return M1_2D_TILED;	/* menu_bg_scroll_draw */
	}
	if (a >= 0x8c0e7940 && a < 0x8c0eb340)	/* character select */
		return M1_2D_SCREEN;
	return 0;
}

void sh4_call(SH4 *c, u32 a)
{
	hw_tick(100);
	if (!in_irq && !(c->sr_ctl & 0x10000000u))
	{
		const u32 intevt = hw_pending_intevt((c->sr_ctl >> 4) & 15);
		if (intevt)
			take_interrupt(c, intevt);
	}
	static u32 last_vbr = 0x8c00f400;
	if (c->vbr != last_vbr)
	{
		printf("VBR %08x -> %08x (call %08x, pr %08x)%c", last_vbr, c->vbr, a, c->pr, 10);
		last_vbr = c->vbr;
	}
	recent[calls % 64][0] = a; recent[calls % 64][2] = c->r[4]; recent[calls % 64][3] = c->r[15];
	recent[calls % 64][1] = c->pr;
	calls++;
	if (((a >> 26) & 7) == 3 && a < 0xe0000000u)
		a = (a & 0x1fffffffu) | 0x80000000u;	/* RAM mirrors (P0/P2 uncached) run the P1 code */
	if (state_timer(c, a))
		return;	/* fake timer (state.c) */
	{
		/* M1_TRACECALL=addr: print each call of that function (r4-r7, PR, shadow stack top) */
		static long tc = -2;
		if (tc == -2)
			tc = getenv("M1_TRACECALL") ? (long)strtoul(getenv("M1_TRACECALL"), 0, 16) : -1;
		static long tr4 = -2;	/* M1_TRACER4=v: only calls with r4 == v */
		if (tr4 == -2)
			tr4 = getenv("M1_TRACER4") ? (long)strtoul(getenv("M1_TRACER4"), 0, 16) : -1;
		if ((long)a == tc && (tr4 >= 0 ? (long)c->r[4] == tr4 : c->r[6] > 0x100))
			printf("call %08x r4 %08x r5 %08x r6 %08x r7 %08x pr %08x caller %08x\n", a, c->r[4], c->r[5], c->r[6],
				   c->r[7], c->pr, depth ? call_stack[(depth - 1) % MAX_DEPTH] : 0);
	}
	if (hle_call(c, a))
		return;
	void (*f)(SH4 *) = find(a);
	if (!f)
	{
		char b[96];
		snprintf(b, sizeof(b), "call/jump to %08x: no translated function (pr %08x)", a, c->pr);
		m1_stop(b);
	}
	for (unsigned i = 0; i < sizeof(watch) / sizeof(watch[0]); i++)
		if (a == watch[i].addr && watch_count[i]++ == 0 && a == 0x8c2246a0u)
		{
			printf("M2 milestone: first kmRender (end of the first frame of main's loop)\n");
			m1_print_stack(stdout);
			if (getenv("M1_STOP_AT_KMRENDER"))
				m1_stop("first kmRender reached");
		}
	if (depth < MAX_DEPTH) call_stack[depth] = a;
	depth++;
	{
		/* M1_SNAP=addr: on that function's first call, save registers and the library globals
		   (8C379000-8C37AFFF, 8C246000-8C247FFF) on entry and exit to snap_in/out.bin */
		static long snap = -2;
		static int done;
		if (snap == -2)
			snap = getenv("M1_SNAP") ? (long)strtoul(getenv("M1_SNAP"), 0, 16) : -1;
		if ((long)a == snap && !done)
		{
			done = 1;
			FILE *s = fopen("snap_in.bin", "wb");
			fwrite(c->r, 4, 16, s);
			fwrite(sh4_ram + 0x379000, 1, 0x2000, s);
			fwrite(sh4_ram + 0x246000, 1, 0x2000, s);
			fclose(s);
			f(c);
			s = fopen("snap_out.bin", "wb");
			fwrite(c->r, 4, 16, s);
			fwrite(sh4_ram + 0x379000, 1, 0x2000, s);
			fwrite(sh4_ram + 0x246000, 1, 0x2000, s);
			fclose(s);
			printf("snapshot of %08x saved\n", a);
			depth--;
			return;
		}
	}
	/* 2D class of what is being drawn (for widescreen): set by the outermost known 2D function
	   on the call stack, restored on return. Each known 2D function also opens a new group
	   (the innermost one wins), so a text string can be placed as a whole */
	const u8 saved_class = m1_2d_class;
	const u16 saved_group = m1_2d_group;
	const u8 k = class_of(a);
	if (k)
		m1_2d_group++;
	if (!m1_2d_class)
		m1_2d_class = k;
	const u32 out = c->r[4], n = c->r[6];
	if (a == 0x8c0f3d46u)
		online_menu(c, f);	/* main menu: the Online Versus entry and lobby (online.c) */
	else
		f(c);
	if (a == 0x8c03a9ccu && c->r[0] == 0)
		wide_mesh_visible(c, out, n);
	m1_2d_class = saved_class;
	m1_2d_group = saved_group;
	depth--;
}
void sh4_jump(SH4 *c, u32 a) { sh4_call(c, a); }

void sh4_unimpl(SH4 *c, u32 a, u32 op)
{
	(void)c;
	char b[96];
	snprintf(b, sizeof(b), "unimplemented instruction %04x at %08x", op, a);
	m1_stop(b);
}

/* watchdog: stop after the time limit (argv[2] seconds). Linux: SIGALRM on the game thread.
   Windows: a timer thread raises a flag that the next VBlank checks. */
static volatile int watchdog_fired;
void m1_check_watchdog(void)
{
	if (watchdog_fired)
		m1_stop("watchdog: time limit reached");
}
#ifdef _WIN32
static DWORD WINAPI watchdog_thread(void *arg)
{
	Sleep((DWORD)(size_t)arg * 1000);
	watchdog_fired = 1;
	return 0;
}
#else
static void on_alarm(int sig)
{
	(void)sig;
	m1_stop("watchdog: time limit reached");
}
#endif


int main(int argc, char **argv)
{
#ifdef _WIN32
	const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!out || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN)
	{
		/* the GUI build has no console: keep the log next to the executable */
		char log[1024];
		GetModuleFileNameA(0, log, sizeof(log) - 16);
		char *slash = strrchr(log, 92);	/* backslash */
		strcpy(slash ? slash + 1 : log, "m1_log.txt");
		if (!freopen(log, "w", stdout) || !freopen(log, "a", stderr))
			(void)0;
	}
#endif
	setvbuf(stdout, 0, getenv("M1_NET") ? _IONBF : _IOLBF, 0);	/* netplay: two instances, see progress as it happens */
	sh4_fsca_init();
	{
		extern void ta_tag_config(void);
		ta_tag_config();
	}
	sh4_ram = calloc(1, 16 << 20);

	/* RAM as the HLE BIOS leaves it (Flycast reios): system area filled with 0xFF, syscall
	   vectors pointing at stubs that hle_call() answers, IP.BIN at 8C008000 */
	memset(sh4_ram, 0xff, 0x10000);
	static const u32 vec[][2] = { { 0x8c0000b0, 0x8c001000 }, { 0x8c0000b4, 0x8c001002 }, { 0x8c0000b8, 0x8c001004 },
								  { 0x8c0000bc, 0x8c001006 }, { 0x8c0000e0, 0x8c001008 } };
	for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++)
		wr32(vec[i][0], vec[i][1]);
	/* settings, command line and the boot files from the disc dump (config.c) */
	int config_start(int argc, char **argv, const char **ranges);
	const char *ranges;
	const int limit = config_start(argc, argv, &ranges);
	state_init(ranges);
	{
		void aica_init(void);
		aica_init();
	}
	printf("M1: %d translated entry points\n", sh4_nfns);

	/* registers as reios_setup_state() */
	static SH4 c;
	m1_cpu = &c;
	c.r[15] = 0x8c00f400;	/* the IP.BIN bootstrap moves the stack here (reios starts at 8D000000; seen in the oracle) */
	c.gbr = 0x8c000000;
	c.vbr = 0x8c00f400;	/* set by the IP.BIN bootstrap (seen in the emulator), which M1 skips */
	c.pr = 0xac00043c;
	sh4_set_sr(&c, 0x400000f1);
	c.fpscr = 0x00040001;
	sh4_set_host_fpmode(c.fpscr);

	if (limit > 0)	/* development runs: stop after the time limit */
	{
#ifdef _WIN32
		CreateThread(0, 0, watchdog_thread, (void *)(size_t)limit, 0, 0);
#else
		signal(SIGALRM, on_alarm);
		alarm(limit);
#endif
	}
	sh4_call(&c, 0x8c010000);
	m1_stop("start returned");
	return 0;
}
