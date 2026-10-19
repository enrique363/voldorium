/* Rollback state: save, load, checksum, and a native SyncTest (docs/phase4-ggpo-state.md).
 *
 * Save point: the top of main's loop (0x8C03144E, an sh4_hook from sh4c/hooks.txt). There the
 * host stack is the same every frame, so loading a state there and carrying on re-runs the
 * frame. The state is:
 *   - the `include` and `restore` ranges of sc/ggpo/savestate_ranges.tsv (game RAM; the
 *     checksum covers `include` only),
 *   - the CPU registers,
 *   - the fake timer: sh4_ReadTimer (0x8C220060) and game-side TCNT0 reads (0x8C21FDBE from
 *     effect code) feed the RNGs, so the dispatcher answers them from a counter kept here,
 *   - the runtime's virtual clock and hardware register values (hw.c), so interrupts land at
 *     the same points when a frame is re-run, the Maple condition count that drives the
 *     M1_PADS script (maple.c), and the sound chip (aica.c: sound RAM, registers, channels,
 *     DSP, ARM). Not the GD-ROM request in progress: the music streams from disc on an SDK
 *     thread whose host stack a rewind does not restore, so its request must not rewind either
 *     (the table keeps the stream buffers out for the same reason).
 * M1_STATE=full saves all of RAM except the table's excluded buffers instead of its
 * include / restore lines (menus, title and attract mode need it).
 * Not in the state, as in the emulator SyncTest: VRAM, sound, the GD-ROM / Maple / streaming
 * buffers (excluded ranges) and the SDK threads' own stacks.
 *
 * M1_SYNCTEST=1: in every fight frame (game mode 1; M1_SYNCTEST=all: every frame) save,
 * run the frame, checksum, load, run it again, compare; mismatches list the first differing
 * addresses. M1_REALTIMER=1: no fake timer (control run: should mismatch).
 */
#include "m1.h"
#include <stdlib.h>
#include <string.h>

u32 hw_state_size(void);
void hw_state_save(void *p);
void hw_state_load(const void *p);
u64 maple_state_get(void);
u32 hle_state_size(void);
void hle_state_save(void *p);
void hle_state_load(const void *p);
u32 aica_state_size(void);
void aica_state_save(u8 *p);
void aica_state_load(const u8 *p);
void maple_state_set(u64 v);

typedef struct
{
	u32 lo, hi;
	int checked;	/* include (1) or restore (0) */
} Range;
static Range ranges[512];
static int nranges;
static u32 ram_bytes;
static u32 timer_counter;
static int fake_timer = 1;

/* Ranges come from a class per RAM byte (0 not saved, 1 include, 2 restore), built from the
   table: by default only its include / restore lines; with M1_STATE=full all of RAM except its
   exclude lines (menus and attract mode use RAM the fight table leaves out) */
void state_init(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
	{
		printf("state: no %s (save / load disabled)\n", path);
		return;
	}
	const int full = getenv("M1_STATE") && !strcmp(getenv("M1_STATE"), "full");
	static u8 cls[1 << 24];
	memset(cls, full ? 1 : 0, sizeof(cls));
	char line[2048];
	for (int pass = 0; pass < 2; pass++)	/* include and exclude lines first, restore lines win */
	{
		rewind(f);
		while (fgets(line, sizeof(line), f))
		{
			unsigned lo, hi;
			char kind[32];
			if (line[0] == '#' || sscanf(line, "%x %x %31s", &lo, &hi, kind) != 3 || hi <= lo)
				continue;
			const int k = !strcmp(kind, "include") ? 1 : !strcmp(kind, "restore") ? 2 : 0;
			if ((k == 2) != (pass == 1))
				continue;
			memset(cls + (lo & 0xffffff), k, hi - lo);
		}
	}
	fclose(f);
	if (getenv("M1_STATE_LIBS"))	/* experiment: also restore the library area the table excludes */
		memset(cls + 0x33c840, 2, 0x391560 - 0x33c840);
	for (u32 a = 0; a < sizeof(cls);)
	{
		u32 b = a;
		while (b < sizeof(cls) && cls[b] == cls[a])
			b++;
		if (cls[a])
		{
			if (nranges == 512)
				m1_stop("state: too many ranges");
			ranges[nranges].lo = 0x8c000000u + a;
			ranges[nranges].hi = 0x8c000000u + b;
			ranges[nranges].checked = cls[a] == 1;
			ram_bytes += b - a;
			nranges++;
		}
		a = b;
	}
	fake_timer = !(getenv("M1_REALTIMER") && atoi(getenv("M1_REALTIMER")));
	printf("state: %s, %d ranges, %u bytes of RAM, fake timer %s\n", full ? "all RAM but excluded buffers" : "fight table",
		   nranges, ram_bytes, fake_timer ? "on" : "off");
}

/* sh4_ReadTimer and game-side TCNT0 reads (m1.c): returns 1 and sets r0 when answered */
int state_timer(SH4 *c, u32 a)
{
	if (!fake_timer)
		return 0;
	if (a != 0x8c220060u && !(a == 0x8c21fdbeu && c->pr >= 0x8c068000u && c->pr < 0x8c0a0000u))
		return 0;
	c->r[0] = timer_counter++ * 0x9E3779B1u;
	return 1;
}

u32 state_size(void) { return ram_bytes + (u32)sizeof(SH4) + 4 + 8 + aica_state_size() + hw_state_size(); }

void state_save(const SH4 *c, u8 *buf)
{
	for (int i = 0; i < nranges; i++)
	{
		memcpy(buf, sh4_ram + (ranges[i].lo & 0xffffff), ranges[i].hi - ranges[i].lo);
		buf += ranges[i].hi - ranges[i].lo;
	}
	memcpy(buf, c, sizeof(SH4));
	buf += sizeof(SH4);
	memcpy(buf, &timer_counter, 4);
	buf += 4;
	const u64 m = maple_state_get();
	memcpy(buf, &m, 8);
	buf += 8;
	aica_state_save(buf);
	buf += aica_state_size();
	hw_state_save(buf);
}

void state_load(SH4 *c, const u8 *buf)
{
	for (int i = 0; i < nranges; i++)
	{
		memcpy(sh4_ram + (ranges[i].lo & 0xffffff), buf, ranges[i].hi - ranges[i].lo);
		buf += ranges[i].hi - ranges[i].lo;
	}
	memcpy(c, buf, sizeof(SH4));
	buf += sizeof(SH4);
	memcpy(&timer_counter, buf, 4);
	buf += 4;
	u64 m;
	memcpy(&m, buf, 8);
	maple_state_set(m);
	buf += 8;
	aica_state_load(buf);
	buf += aica_state_size();
	hw_state_load(buf);
	sh4_set_host_fpmode(c->fpscr);
}

/* FNV-1a over the include ranges, the registers and the timer counter */
u64 state_checksum(const u8 *buf)
{
	u64 h = 0xcbf29ce484222325ull;
	for (int i = 0; i < nranges; i++)
	{
		const u32 n = ranges[i].hi - ranges[i].lo;
		if (ranges[i].checked)
			for (u32 k = 0; k < n; k++)
				h = (h ^ buf[k]) * 0x100000001b3ull;
		buf += n;
	}
	for (u32 k = 0; k < sizeof(SH4) + 4; k++)
		h = (h ^ buf[k]) * 0x100000001b3ull;
	return h;
}

/* first differing checked bytes between two saved states, by Dreamcast address */
static void report_diff(FILE *f, const u8 *a, const u8 *b)
{
	int shown = 0;
	u32 off = 0;
	for (int i = 0; i < nranges && shown < 16; i++)
	{
		const u32 n = ranges[i].hi - ranges[i].lo;
		if (ranges[i].checked)
			for (u32 k = 0; k < n && shown < 16; k += 4)
				if (memcmp(a + off + k, b + off + k, 4))
				{
					u32 x, y;
					memcpy(&x, a + off + k, 4);
					memcpy(&y, b + off + k, 4);
					fprintf(f, "  %08x: %08x vs %08x\n", ranges[i].lo + k, x, y);
					shown++;
				}
		off += n;
	}
	if (memcmp(a + off, b + off, sizeof(SH4) + 4))
		fprintf(f, "  CPU registers or timer counter differ\n");
}

static u64 tests, passed;

void state_report(FILE *f)
{
	if (tests)
		fprintf(f, "SyncTest: %llu/%llu frames identical on re-run\n", (unsigned long long)passed, (unsigned long long)tests);
}

static void loop_top(SH4 *c)
{
	static int mode = -1;	/* 0 off, 1 fights, 2 every frame */
	if (mode < 0)
	{
		printf("state: main loop top reached, r15 = %08x\n", c->r[15]);
		const char *e = getenv("M1_SYNCTEST");
		mode = !e || !nranges ? 0 : !strcmp(e, "all") ? 2 : atoi(e) ? 1 : 0;
	}
	if (!mode)
		return;
	static u8 *start, *end1, *end2;
	static int phase;	/* 0: ready, 1: first run in progress, 2: re-run in progress */
	static u64 sum1;
	if (!start)
	{
		start = malloc(state_size());
		end1 = malloc(state_size());
		end2 = malloc(state_size());
		if (!start || !end1 || !end2)
			m1_stop("SyncTest: out of memory");
	}
	static u8 *leak_start;	/* M1_SYNCLEAK=1: all of RAM at the frame start (first test only) */
	if (phase == 1)
	{
		state_save(c, end1);
		sum1 = state_checksum(end1);
		state_load(c, start);	/* rewind: the frame runs again from here */
		if (leak_start)
		{
			/* RAM the run changed that the rewind did not restore, merged to 256-byte blocks */
			printf("SyncTest leak report (RAM changed by the first run and not restored):\n");
			u32 from = ~0u;
			for (u32 b = 0; b <= (16u << 20) / 256; b++)
			{
				const int d = b < (16u << 20) / 256 && memcmp(leak_start + b * 256, sh4_ram + b * 256, 256);
				if (d && from == ~0u)
					from = b;
				if (!d && from != ~0u)
				{
					printf("  %08x-%08x\n", 0x8c000000u + from * 256, 0x8c000000u + b * 256);
					from = ~0u;
				}
			}
			free(leak_start);
			leak_start = 0;
		}
		phase = 2;
		return;
	}
	if (phase == 2)
	{
		state_save(c, end2);
		const u64 sum2 = state_checksum(end2);
		tests++;
		if (sum1 == sum2)
			passed++;
		else
		{
			printf("SyncTest mismatch in test %llu (game mode %u):\n", (unsigned long long)tests, rd32(0x8c24a584));
			report_diff(stdout, end1, end2);
		}
		if (tests % 300 == 0)
			printf("SyncTest: %llu/%llu frames identical\n", (unsigned long long)passed, (unsigned long long)tests);
		phase = 0;
	}
	if (mode == 2 || rd32(0x8c24a584) == 1)
	{
		state_save(c, start);
		phase = 1;
		static int leak_done;
		if (getenv("M1_SYNCLEAK") && !leak_done && (leak_start = malloc(16u << 20)))
		{
			memcpy(leak_start, sh4_ram, 16u << 20);
			leak_done = 1;
		}
	}
}

void netplay_hook(SH4 *c);
void sh4_hook(SH4 *c, u32 addr)
{
	if (addr == 0x8c03144eu)
	{
		loop_top(c);
		netplay_hook(c);
	}
}
