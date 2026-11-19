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
#include <pthread.h>
#include <sys/stat.h>
#define Sleep(ms) usleep((ms) * 1000)
#define ExitProcess(code) _exit(code)
#endif

/* the translated functions, from the game module (module.c) */
static const struct sh4_fn *sh4_fns;
static int sh4_nfns;

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
	if (!m1_verbose && strcmp(why, "window closed") && strncmp(why, "quit (", 6))	/* quit (Esc), quit (EXIT GAME) */
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

/* a translated function by its Dreamcast address (threads.c), 0: none */
void (*m1_translated(u32 a))(SH4 *) { return find(a); }

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

/* a tight loop's check-in (sh4c.py loop_jump): what a call does first, so a loop that waits
   for an interrupt handler or another thread to set a flag gets there */
void sh4_loop_poll(SH4 *c)
{
	hw_tick(0);	/* drains sh4_loop_cycles */
	if (!in_irq && !(c->sr_ctl & 0x10000000u))
	{
		const u32 intevt = hw_pending_intevt((c->sr_ctl >> 4) & 15);
		if (intevt)
			take_interrupt(c, intevt);
	}
}

/* Voldo's original 2P costume (issue #22). The US release swaps it for a new one: the human loader
   (0x8c02e37a: r4 player, r5 character file 1-21, r6 costume) turns Voldo's (file 5) costume 1 into
   costume 3, so HUMAN.DAT entry 43 loads instead of 41, which is still on the disc. Holding Y and
   confirming with X on the select screen picks the original: costume 1 (2P) with entry 41. Y with A
   stays the third costume, and the plain 2P costume stays the US one.
   - pick (0x8c0e39a8, one side's confirm; r0 1 or 2 when a character was chosen): the game made
     it costume 2 (Y held), or kept the preview when the third costume is locked; for Voldo with X
     it becomes 1 and the select slot is marked
   - copy to the player (0x8c0e34ae: r4 select slot, r5 player) carries the mark to the player
   - character select starts (0x8c033b00): marks cleared, so CPU players never have one
   - loading: entry 43 asked of the disc loader (0x8c0ac666, r4 3 = HUMAN.DAT, r5 entry) becomes 41;
     the loader's cache key (player + 0x5d10) gets bit 30, so the two 2P costumes never stand in
     for each other.
   Inferno's boss form (issue #23): the final Arcade battle gives the CPU Inferno (file 19) costume 2,
   the flaming one, which the select screen never offers (Y with A gives his 2P costume). Holding Y
   and confirming with A on Inferno marks the slot, and the copy to the player makes it costume 2.
   The fight setup (0x8c039d60) moves any fight with a boss Inferno to Chaos; for a picked one the
   stage it had is put back.
   Everything follows the game's own inputs, so both sides of a netplay match agree. */
#define SEL_STATE_PTR 0x8c25bc70u	/* the select slot being updated (0x9c bytes from 0x8c25bb38) */
#define SEL_STATES 0x8c25bb38u
#define PLAYERS 0x8c24a958u		/* two players, 0x86dc bytes apart */
#define VOLDO_SELECT_ID 5		/* select slot + 0x14 (player + 0x5c30) */
#define VOLDO_FILE 5			/* loader r5 */
#define PAD_HOLD_Y 0x0100u		/* select slot's pad record: +0 held, +2 pressed (game bits) */
#define PAD_PRESS_X 0x0200u
#define PAD_PRESS_A 0x0040u
#define INFERNO_SELECT_ID 19
#define INFERNO_BOSS_COSTUME 2
#define STAGE 0x8c25b8bcu			/* the fight's stage */
#define CHAOS_STAGE 14
static u8 orig_slot[2], orig_player[2], orig_loading, boss_slot[2], boss_player[2];

static int costume_call(SH4 *c, u32 a, void (*f)(SH4 *))
{
	if (a == 0x8c0e39a8u)
	{
		const u32 st = rd32(SEL_STATE_PTR), pad = rd32(st);
		const u32 held = rd16(pad), pressed = rd16(pad + 2), slot = (st - SEL_STATES) / 0x9c;
		f(c);
		if (slot < 2 && (c->r[0] == 1 || c->r[0] == 2))
		{
			orig_slot[slot] = (s16)rd16(st + 0x14) == VOLDO_SELECT_ID && (held & PAD_HOLD_Y) && (pressed & PAD_PRESS_X);
			if (orig_slot[slot])
				wr16(st + 0x16, 1);
			boss_slot[slot] = (s16)rd16(st + 0x14) == INFERNO_SELECT_ID && (held & PAD_HOLD_Y) && (pressed & PAD_PRESS_A);
			if (getenv("M1_COSTUMELOG"))
				printf("costume pick: slot %u char %d costume %d held %04x pressed %04x original %u boss %u\n", slot,
					   (s16)rd16(st + 0x14), (s16)rd16(st + 0x16), held, pressed, orig_slot[slot], boss_slot[slot]);
		}
		return 1;
	}
	if (a == 0x8c0e34aeu)
	{
		const u32 slot = (c->r[4] - SEL_STATES) / 0x9c, player = (c->r[5] - PLAYERS) / 0x86dc;
		f(c);
		if (slot < 2 && player < 2)
		{
			orig_player[player] = orig_slot[slot];
			boss_player[player] = boss_slot[slot];
			if (boss_slot[slot])
				wr32(PLAYERS + player * 0x86dcu + 0x5c38, INFERNO_BOSS_COSTUME);
		}
		return 1;
	}
	if (a == 0x8c039d60u)
	{
		/* fight setup: a boss Inferno on either side makes the stage Chaos (14); only the game's
		   own boss (the CPU in Arcade's final battle) keeps that, a picked one leaves the stage */
		const u32 before = rd32(STAGE);
		f(c);
		int game_boss = 0;
		for (u32 p = 0; p < 2; p++)
		{
			const u32 pl = PLAYERS + p * 0x86dcu;
			game_boss |= rd32(pl + 0x5c30) == INFERNO_SELECT_ID && rd32(pl + 0x5c38) == INFERNO_BOSS_COSTUME && !boss_player[p];
		}
		if (rd32(STAGE) == CHAOS_STAGE && before != CHAOS_STAGE && !game_boss)
			wr32(STAGE, before);
		return 1;
	}
	if (a == 0x8c033b00u)
		memset(orig_slot, 0, 2), memset(orig_player, 0, 2), memset(boss_slot, 0, 2), memset(boss_player, 0, 2);
	else if (a == 0x8c0ac666u && orig_loading && c->r[4] == 3 && c->r[5] == (VOLDO_FILE - 1) * 10 + 3)
		c->r[5] = (VOLDO_FILE - 1) * 10 + 1, orig_loading = 0;
	else if (a == 0x8c02e37au && c->r[4] < 2)
	{
		const u32 key = PLAYERS + c->r[4] * 0x86dcu + 0x5d10;
		const int want = c->r[5] == VOLDO_FILE && c->r[6] == 1 && orig_player[c->r[4]];
		u32 k = rd32(key);
		if (k != ~0u && (k >> 30 & 1) != (u32)want)
			wr32(key, ~0u);	/* the other 2P costume (or another character) is loaded */
		else if (k != ~0u)
			wr32(key, k & ~0x40000000u);
		orig_loading = (u8)want;
		f(c);
		orig_loading = 0;
		k = rd32(key);
		if (want && k != ~0u)
			wr32(key, k | 0x40000000u);
		return 1;
	}
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
	else if (!costume_call(c, a, f))
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

/* watchdog: stop after the time limit (argv[2] seconds): a timer thread raises a flag that the
   next VBlank checks. */
static volatile int watchdog_fired;
void m1_check_watchdog(void)
{
	if (watchdog_fired)
		m1_stop("watchdog: time limit reached");
}
#ifdef _WIN32
#define THREAD_FN(name) static DWORD WINAPI name(void *arg)
#define THREAD_RET 0
static void start_thread(LPTHREAD_START_ROUTINE fn, void *arg) { CreateThread(0, 0, fn, arg, 0, 0); }
#else
#define THREAD_FN(name) static void *name(void *arg)
#define THREAD_RET 0
static void start_thread(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	if (!pthread_create(&t, 0, fn, arg))
		pthread_detach(t);
}
#endif
THREAD_FN(watchdog_thread)
{
	Sleep((unsigned)(size_t)arg * 1000);
	watchdog_fired = 1;
	return THREAD_RET;
}

/* Hang report: when no frame has been drawn for 8 s (loading screens take under 1 s), the log
   gets what the game is running: the call stack and the last calls through the dispatcher, read
   from this thread while the game is stuck. Once per hang; M1_HANG_EXIT=1 (tests) then exits. */
THREAD_FN(hang_thread)
{
	(void)arg;
	u64 render_count(void);
	u64 last_renders = render_count(), last_calls = calls;
	int still = 0, reported = 0;
	for (;;)
	{
		Sleep(1000);
		const u64 r = render_count();
		if (r != last_renders)
		{
			last_renders = r;
			still = reported = 0;
			last_calls = calls;
			continue;
		}
		if (++still < 8 || reported || r == 0)
			continue;
		reported = 1;
		const u64 c = calls;
		printf("\n=== hang: no frame for %d s (render %llu); %llu dispatcher calls since (%s)\n", still,
			   (unsigned long long)r, (unsigned long long)(c - last_calls), c != last_calls ? "busy in game code" : "no calls: stuck in one function");
		printf("current function %08x, depth %d\n", m1_current_function(), depth);
		m1_print_stack(stdout);
		printf("last calls (oldest first): target / pr / r4\n");
		for (u64 i = c > 64 ? c - 64 : 0; i < c; i++)
			printf("  %08x / %08x / %08x\n", recent[i % 64][0], recent[i % 64][1], recent[i % 64][2]);
		fflush(stdout);
		if (getenv("M1_HANG_EXIT"))
			ExitProcess(3);
	}
	return THREAD_RET;
}

/* One copy at a time (player form). A second start (a double click counted twice, say) would
   play the intro behind the running game and truncate its log, so it brings the running window
   to the front (Windows) and quits before anything else. The restarts into and out of a lobby
   match (--join, --back; online.c relaunch) start while the old copy is still exiting: they
   wait for it, up to 15 s, and then go on regardless (a match never fails over this). Off in
   the development form, with --seconds (tests) and with M1_MULTI=1. */
#ifdef _WIN32
static BOOL CALLBACK raise_running(HWND w, LPARAM arg)
{
	(void)arg;
	char t[64];
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	if (pid == GetCurrentProcessId() || !IsWindowVisible(w) || !GetWindowTextA(w, t, sizeof(t))
		|| strncmp(t, "Voldorium ver. ", 15))
		return TRUE;
	if (IsIconic(w))
		ShowWindow(w, SW_RESTORE);
	SetForegroundWindow(w);
	return FALSE;
}
#else
#include <fcntl.h>
#include <sys/file.h>
#endif
static void single_copy(int argc, char **argv)
{
	if ((argc >= 2 && argv[1][0] != '-') || getenv("M1_MULTI"))
		return;
	int wait = 0;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--seconds"))
			return;
		if (!strcmp(argv[i], "--join") || !strcmp(argv[i], "--host") || !strcmp(argv[i], "--back"))
			wait = 1;
	}
#ifdef _WIN32
	/* held until this process ends; then the next waiter gets it (WAIT_ABANDONED) */
	const HANDLE m = CreateMutexA(0, FALSE, "Local\\Voldorium.one_copy");
	if (!m)
		return;
	const DWORD r = WaitForSingleObject(m, wait ? 15000 : 0);
	if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED || wait)
		return;
	EnumWindows(raise_running, 0);
	ExitProcess(0);
#else
	/* a lock file per user; O_CLOEXEC: a restarted copy must not inherit the old copy's lock */
	char path[512];
	const char *dir = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/voldorium-%u.lock", dir && *dir ? dir : "/tmp", (unsigned)getuid());
	const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	for (int tries = wait ? 150 : 1; tries > 0; tries--)
	{
		if (flock(fd, LOCK_EX | LOCK_NB) == 0)
			return;	/* fd stays open: the lock lasts until this process ends */
		if (tries > 1)
			usleep(100000);
	}
	if (wait)
		return;
	_exit(0);
#endif
}

static int m1_main(int argc, char **argv);
#ifdef _WIN32
int main(int argc, char **argv) { return m1_main(argc, argv); }	/* 64 MB stack: linked with --stack */
#else
/* the game runs on a 64 MB stack, as on Windows (the main thread's is 8 MB by default) */
static int main_argc;
static char **main_argv;
static void *main_thread(void *arg)
{
	(void)arg;
	exit(m1_main(main_argc, main_argv));
}
int main(int argc, char **argv)
{
	main_argc = argc;
	main_argv = argv;
	pthread_attr_t a;
	pthread_t t;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 64u << 20);
	if (pthread_create(&t, &a, main_thread, 0))
		return m1_main(argc, argv);
	pthread_join(t, 0);
	return 0;
}
#endif

static int m1_main(int argc, char **argv)
{
	single_copy(argc, argv);	/* before the log: a second copy must not truncate the running one's */
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
#else
	struct stat st;
	if (!isatty(1) && (fstat(1, &st) || !(S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode))))
	{
		/* started from the desktop: keep the log next to the executable */
		char log[1024];
		const ssize_t n = readlink("/proc/self/exe", log, sizeof(log) - 16);
		if (n > 0)
		{
			log[n] = 0;
			char *slash = strrchr(log, '/');
			strcpy(slash ? slash + 1 : log, "m1_log.txt");
			if (!freopen(log, "w", stdout) || !freopen(log, "a", stderr))
				(void)0;
		}
	}
#endif
	setvbuf(stdout, 0, getenv("M1_NET") ? _IONBF : _IOLBF, 0);	/* netplay: two instances, see progress as it happens */
	sh4_ram = calloc(1, 16 << 20);	/* before the module: it keeps this address */
	{
		extern void ta_tag_config(void);
		ta_tag_config();
	}

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
	{
		/* the game's code: the module built from the player's disc (module.c) */
		const char *module_path(void);
		const Sh4Module *module_load(const char **why);
		const char *why = "";
		const Sh4Module *m = module_load(&why);
		int module_build(const char *gdi);
		const char *config_disc_path(void);
		if (!m && module_build(config_disc_path()))	/* first start: build it from the disc */
			m = module_load(&why);
		if (!m)
		{
			char msg[1400];
			snprintf(msg, sizeof(msg), "The game module %s is %s.\nBuild it from your own disc dump with tools/build.py.",
					 module_path(), why);
			m1_stop(msg);
		}
		sh4_fns = m->fns;
		sh4_nfns = m->nfns;
		m->fsca_init();
	}
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

	start_thread(hang_thread, 0);	/* a frozen game says where in the log */
	if (limit > 0)	/* development runs: stop after the time limit */
		start_thread(watchdog_thread, (void *)(size_t)limit);
	sh4_call(&c, 0x8c010000);
	m1_stop("start returned");
	return 0;
}
