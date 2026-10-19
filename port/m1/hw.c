/* M1/M2 hardware layer: everything translated code reaches outside main RAM.
 *
 * - VRAM (area 1, 8 MB) and AICA wave RAM (0x00800000, 2 MB) are plain arrays.
 * - Every other address is a register: writes are stored, reads return the last value written
 *   (0 if never written), except the registers modelled below.
 * - Clock: there is no CPU timing, so a virtual cycle counter advances by CYC_CALL per
 *   dispatcher call and CYC_HW per hardware access. The video beam (SPG_STATUS, VBlank
 *   interrupts) and the TMU counters derive from it: 200 MHz, 60 frames, 525 lines.
 * - Holly interrupts: ISTNRM/ISTEXT (005F6900/04) hold pending events and are cleared by
 *   writing 1s, as on hardware; IML6/4/2 (005F6930/20/10 and the EXT ones) route them to
 *   SH-4 IRL levels. hw_pending_intevt() tells the dispatcher which interrupt to take.
 * - DMA: a ch2 DMA (SB_C2DST) or PVR DMA (SB_PDST) runs at once when started and raises
 *   its end-of-DMA interrupt.
 * - Store queues: writes to 0xE0000000-0xE3FFFFFF fill SQ0/SQ1; `pref` flushes them to RAM,
 *   VRAM or the TA (ta.c builds display lists from the TA data).
 * - VRAM is kept in the 64-bit (texture) layout. The CPU's 32-bit path (0x05xxxxxx) and the
 *   32-bit TA texture path interleave the two 4 MB banks every 32 bits (as Flycast pvr_map32).
 * - A register read again and again with no other hardware write in between is a wait the
 *   model does not satisfy: after POLL_LIMIT reads the run stops and reports it.
 * - m1_hw.txt: the first LOG_LIMIT accesses (VRAM excluded) in order, then a per-register summary.
 */
#include "m1.h"
#include "ta.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>	/* timeBeginPeriod */
#endif

#define ARAM_SIZE (2u << 20)
#define POLL_LIMIT 2000000
#define LOG_LIMIT 20000
#define NREG 4096
#define CYC_FRAME 3333333u	/* 200 MHz / 60 Hz */
#define CYC_CALL 100
#define CYC_HW 200
#define CYC_SQ 2

static u8 vram[VRAM_SIZE], sq[64];
extern u8 aica_ram[];	/* sound RAM, owned by aica.c */
#define aram aica_ram
u32 aica_sh4_read(u32 p, int size);
void aica_sh4_write(u32 p, u32 v, int size);
void aica_tick(u32 n);
static u64 ta_bytes, accesses, cycles, renders;
u64 m1_cycles(void) { return cycles; }
static FILE *log_file;

struct reg { u32 addr, value, reads, writes, order; int used; };
static struct reg regs[NREG];
static u32 nregs;

static u32 poll_addr, poll_count;
static u32 frames, last_line;
static int vsync_seen;
u64 hw_dma_count[3];

static void hw_presets(void);

/* BIOS ROM area (0x00000000-0x001FFFFF): only the system font is provided, at 0x00100020,
   where the font syscall points (288 12x24, 7078 24x24 and 129 32x32 glyphs). The font is
   Flycast's replacement BIOS font, loaded at run time (M1_FONT, default ~/m1_build/biosfont.bin,
   on Windows %LOCALAPPDATA%/sc-m1/biosfont.bin,
   unpacked by m1/build.sh from the local Flycast checkout); it is not part of this project. */
#define BIOS_SIZE (2u << 20)
static u8 bios[BIOS_SIZE];
static void load_bios_font(void)
{
	char def[512];
	const char *path = getenv("M1_FONT");
	if (!path)
	{
#ifdef _WIN32
		snprintf(def, sizeof(def), "%s/sc-m1/biosfont.bin", getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : ".");
#else
		snprintf(def, sizeof(def), "%s/m1_build/biosfont.bin", getenv("HOME") ? getenv("HOME") : ".");
#endif
		path = def;
	}
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		printf("no BIOS font (%s): system text will be blank\n", path);
		return;
	}
	const size_t n = fread(bios + 0x100020, 1, BIOS_SIZE - 0x100020, f);
	fclose(f);
	printf("BIOS font: %zu bytes from %s\n", n, path);
}

static struct reg *reg(u32 a)
{
	static int init;
	if (!init)
	{
		init = 1;
		hw_presets();
	}
	u32 h = (a * 2654435761u) % NREG;
	while (regs[h].used && regs[h].addr != a)
		h = (h + 1) % NREG;
	if (!regs[h].used)
	{
		if (nregs >= NREG * 3 / 4) m1_stop("hardware register table full");
		regs[h].used = 1;
		regs[h].addr = a;
		regs[h].order = nregs++;
	}
	return &regs[h];
}
static u32 regv(u32 a) { return reg(a)->value; }
u32 hw_reg(u32 a) { return regv(a); }
void hw_set(u32 a, u32 v) { reg(a)->value = v; }
void maple_dma(void);

/* read-only identification registers, as on a retail console */
static void hw_presets(void)
{
	reg(0x005f8000)->value = 0x17fd11db;	/* PowerVR2 ID: Kamui parks the CPU in a loop if wrong */
	reg(0x005f8004)->value = 0x00000011;	/* PowerVR2 revision */
	reg(0x005f811c)->value = 0x000000ff;	/* PT_ALPHA_REF reset value */
	/* system bus reset values (Flycast sb_Reset). SB_SBREV matters: Kamui picks its video
	   setup mode from it (8C379AD4), which decides the whole VRAM layout */
	reg(0x005f689c)->value = 0x0000000b;	/* SB_SBREV */
	reg(0x005f7880)->value = 0x00000012;	/* SB_G2ID */
	reg(0x005f74b0)->value = 0x00000001;	/* SB_G1SYSM */
	reg(0x005f6880)->value = 0x00000008;	/* SB_TFREM */
	reg(0x005f7c80)->value = 0x00007f00;	/* SB_PDAPRO */
	reg(0x005f74b8)->value = 0x00007f00;	/* SB_GDAPRO */
	load_bios_font();
}

static void logf_access(const char *kind, u32 a, u32 v, int size)
{
	if (!log_file && m1_verbose)	/* development runs only */
		log_file = fopen("m1_hw.txt", "w");
	if (accesses++ < LOG_LIMIT && log_file)
		fprintf(log_file, "%s %08x %08x /%d\n", kind, a, v, size);
}

/* canonical register address: P0-P3 mirrors folded to the 29-bit physical address, P4 kept */
static u32 phys(u32 a) { return a >= 0xe0000000u ? a : (a & 0x1fffffffu); }

static u32 rd_le(const u8 *p, int size)
{
	u32 v = 0;
	memcpy(&v, p, size);
	return v;
}

/* ---- VRAM ------------------------------------------------------------------------------ */
static u32 vram_gen[VRAM_SIZE >> 12], palette_gen = 1;

static u32 map32(u32 off)
{
	off &= VRAM_SIZE - 1;
	return (off & 3) | ((off & 0x3ffffcu) << 1) | ((off >> 22) & 1) * 4;
}
/* VRAM offset of an area-1 address: 0x04/0x06 = 64-bit path, 0x05/0x07 = 32-bit path */
static u32 vram_offset(u32 p) { return (p & 0x01000000u) ? map32(p) : (p & (VRAM_SIZE - 1)); }

static void vram_put(u32 off, const void *src, u32 n)
{
	for (u32 i = 0; i < n; i++)
	{
		const u32 o = (off + i) & (VRAM_SIZE - 1);
		vram[o] = ((const u8 *)src)[i];
		vram_gen[o >> 12]++;
	}
}
static void vram_put32(u32 p, const void *src, u32 n)	/* through an area-1 address, n <= 4 */
{
	for (u32 i = 0; i < n; i++)
	{
		const u32 o = vram_offset(p + i);
		vram[o] = ((const u8 *)src)[i];
		vram_gen[o >> 12]++;
	}
}

u8 *hw_vram(void) { return vram; }
void hw_vram_touch(u32 off, u32 len)	/* VRAM changed outside vram_put (render to texture) */
{
	for (u32 pg = off >> 12; pg <= (off + len - 1) >> 12; pg++)
		vram_gen[pg & ((VRAM_SIZE >> 12) - 1)]++;
}
u32 hw_vram_gen(u32 page) { return vram_gen[page & ((VRAM_SIZE >> 12) - 1)]; }
u32 hw_palette_gen(void) { return palette_gen; }
u32 vram_rd32_map32(u32 off32)
{
	u32 v = 0;
	for (int i = 0; i < 4; i++)
		v |= (u32)vram[map32(off32 + i)] << (8 * i);
	return v;
}

/* ---- interrupts ------------------------------------------------------------------------- */
enum { ISTNRM = 0x005f6900, ISTEXT = 0x005f6904, ISTERR = 0x005f6908 };

void hw_raise(int bit)
{
	reg(ISTNRM)->value |= 1u << bit;
}

void hw_cancel(int bit)
{
	reg(ISTNRM)->value &= ~(1u << bit);
}

/* external interrupt lines (ISTEXT), level-triggered: bit 1 = AICA */
void hw_ext_irq(int bit, int on)
{
	if (on)
		reg(ISTEXT)->value |= 1u << bit;
	else
		reg(ISTEXT)->value &= ~(1u << bit);
}

/* INTEVT of the highest-priority pending Holly interrupt the CPU accepts, or 0 */
u32 hw_pending_intevt(u32 imask)
{
	const u32 nrm = regv(ISTNRM), ext = regv(ISTEXT);
	static const struct { u32 nrm, ext, level, intevt; } irl[] = {
		{ 0x005f6930, 0x005f6934, 6, 0x320 },
		{ 0x005f6920, 0x005f6924, 4, 0x360 },
		{ 0x005f6910, 0x005f6914, 2, 0x3a0 },
	};
	for (int i = 0; i < 3; i++)
		if (irl[i].level > imask && ((nrm & regv(irl[i].nrm)) || (ext & regv(irl[i].ext))))
			return irl[i].intevt;
	return 0;
}

/* ---- clock and video beam --------------------------------------------------------------- */
static u32 beam_line(void) { return (u32)((cycles % CYC_FRAME) * 525 / CYC_FRAME); }

/* Frame pacing: each V Blank-in waits for the next 1/60 s tick of the host clock, so the
   virtual 60 Hz is real time (M1_FAST=1 runs unpaced). When the host falls more than
   100 ms behind, the schedule restarts from now instead of rushing to catch up. */
#ifdef _WIN32
static void pace_frame(void)
{
	static int fast = -1;
	static LARGE_INTEGER freq, next;
	if (fast < 0)
	{
		fast = getenv("M1_FAST") != 0 || getenv("M1_NET") != 0;	/* netplay.c paces itself */
		QueryPerformanceFrequency(&freq);
		timeBeginPeriod(1);
	}
	if (fast)
		return;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (next.QuadPart == 0)
		next = now;
	next.QuadPart += freq.QuadPart / 60;
	const double behind = (double)(now.QuadPart - next.QuadPart) / (double)freq.QuadPart;
	if (behind > 0.1)
		next = now;
	else
		for (;;)
		{
			QueryPerformanceCounter(&now);
			const double left = (double)(next.QuadPart - now.QuadPart) / (double)freq.QuadPart;
			if (left <= 0)
				break;
			if (left > 0.002)
				Sleep((DWORD)(left * 1000) - 1);	/* coarse sleep, then spin the last ms */
		}
}
#else
static void pace_frame(void)
{
	static int fast = -1;
	static struct timespec next;
	if (fast < 0)
		fast = getenv("M1_FAST") != 0;
	if (fast)
		return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (next.tv_sec == 0)
		next = now;
	next.tv_nsec += 16666667;
	if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
	const double behind = (now.tv_sec - next.tv_sec) + (now.tv_nsec - next.tv_nsec) * 1e-9;
	if (behind > 0.1)
		next = now;
	else if (behind < 0)
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
}
#endif

static void vblank_in(void)
{
	hw_raise(3);
	m1_check_watchdog();
	{
		/* M1_ARAMDUMP=frame: write sound RAM to m1_aram_<frame>.bin at that video frame */
		static long at = -2;
		if (at == -2)
			at = getenv("M1_ARAMDUMP") ? atol(getenv("M1_ARAMDUMP")) : -1;
		if ((long)frames == at)
		{
			char name[64];
			snprintf(name, sizeof(name), "m1_aram_%ld.bin", at);
			FILE *f = fopen(name, "wb");
			if (f)
				fwrite(aram, 1, ARAM_SIZE, f), fclose(f);
		}
	}
	video_poll();
	wr32(0x8c266c28, 0x3f400000);	/* P1: g_aspect_scale_x = 0.75 (Hor+, 16:9); set once by game_init, kept here */
	if ((regv(0x005f6c14) & 1) && regv(0x005f6c10) == 1)	/* maple DMA triggered by V Blank */
		maple_dma();
	static u32 mode = ~0u;
	const u32 m = rd32(0x8c24a584);
	if (m != mode)
	{
		printf("frame %u: game mode %u -> %u\n", frames, mode, m);
		mode = m;
	}
	pace_frame();
}

static void hw_tick_step(u32 n);
u32 sh4_loop_cycles;
/* advance the virtual clock by n cycles plus the loop cycles the translated code accumulated,
   in steps short enough not to skip a V Blank */
void hw_tick(u32 n)
{
	static int loops = -1;	/* M1_NOLOOPTIME=1: ignore loop cycles (for comparison) */
	if (loops < 0)
		loops = !getenv("M1_NOLOOPTIME");
	extern u64 cyc_src[4];
	cyc_src[n == 100 ? 0 : (n == CYC_HW || n == CYC_SQ) ? 1 : 3] += n;
	cyc_src[2] += loops ? sh4_loop_cycles : 0;
	u64 total = (u64)n + (loops ? sh4_loop_cycles : 0);
	sh4_loop_cycles = 0;
	while (total)
	{
		const u32 step = total > 500000 ? 500000u : (u32)total;
		hw_tick_step(step);
		total -= step;
	}
}

static void hw_tick_step(u32 n)
{
	const u32 before = beam_line();
	const u64 frame_before = cycles / CYC_FRAME;
	cycles += n;
	aica_tick(n);
	const u32 line = beam_line();
	if (cycles / CYC_FRAME != frame_before)
	{
		frames++;
		hw_raise(4);	/* V Blank-out: a new frame starts */
	}
	if ((before < 480 && line >= 480) || (cycles / CYC_FRAME != frame_before && line >= 480))
		vblank_in();
	last_line = line;
}

static u32 spg_status(void)
{
	if (!vsync_seen)
	{
		vsync_seen = 1;
		printf("M1 milestone: first VBlank wait (SPG_STATUS polled)\n");
		m1_print_stack(stdout);
		if (getenv("M1_STOP_AT_VSYNC"))
			m1_stop("first VBlank wait reached");
	}
	const u32 line = beam_line();
	return line | (line >= 480 ? 0x2000u : 0);
}

/* ---- DMA ---------------------------------------------------------------------------------- */
/* M1_VRAMLOG=1: one line per VRAM write burst (path, address, length) to vram_log.txt */
static void vram_log(const char *path, u32 addr, u32 len)
{
	static FILE *f;
	static int on = -1;
	static u32 from, to;
	static u64 n;
	if (on < 0)
	{
		const char *e = getenv("M1_VRAMLOG");	/* "from-to" video frames */
		on = e != 0;
		if (e && sscanf(e, "%u-%u", &from, &to) != 2)
			from = 0, to = ~0u;
	}
	if (!on || frames < from || frames > to || n > 200000)
		return;
	if (!f)
		f = fopen("vram_log.txt", "w");
	/* consecutive writes on the same path are merged into one run */
	static const char *rpath;
	static u32 rstart, rlen, rframe, rfunc;
	extern u32 m1_current_function(void);
	if (path == rpath && addr == rstart + rlen)
	{
		rlen += len;
		return;
	}
	if (rpath)
	{
		fprintf(f, "%u %s %08x %u %08x\n", rframe, rpath, rstart, rlen, rfunc);
		fflush(f);
		n++;
	}
	rpath = path;
	rstart = addr;
	rlen = len;
	rframe = frames;
	rfunc = m1_current_function();
}

static void copy_to_pvr(u32 dest, u32 src, u32 len)
{
	vram_log("dma", dest, len);
	const u32 area = dest & 0xff000000u;
	if (area == 0x10000000u || area == 0x12000000u)	/* TA polygon path */
	{
		u8 unit[32];
		ta_bytes += len;
		for (u32 i = 0; i + 32 <= len; i += 32)
		{
			for (u32 k = 0; k < 32; k++)
				unit[k] = rd8(src + i + k);
			ta_cur_class = ta_class_at(src + i);
			ta_cur_group = ta_group_at(src + i);
			ta_input(unit, 32);
		}
	}
	else if (area == 0x11000000u || area == 0x13000000u)
	{
		/* texture paths: SB_LMMODE0/1 select the 64-bit (0) or 32-bit (1) layout */
		const int bus32 = regv(area == 0x11000000u ? 0x005f6884u : 0x005f6888u) & 1;
		for (u32 i = 0; i < len; i++)
		{
			const u8 b = rd8(src + i);
			const u32 o = (dest + i) & (VRAM_SIZE - 1);
			vram_put(bus32 ? map32(o) : o, &b, 1);
		}
	}
	else	/* area 1 directly */
		for (u32 i = 0; i < len; i++)
		{
			const u8 b = rd8(src + i);
			vram_put32(dest + i, &b, 1);
		}
}

static void ch2_dma(void)
{
	const u32 dest = regv(0x005f6800), len = regv(0x005f6804), src = regv(0xffa00020);
	if (m1_verbose)
		printf("DMA ch2 %08x -> %08x, %u bytes\n", src, dest, len);
	copy_to_pvr(dest, src, len);
	reg(0x005f6800)->value = dest + len;	/* SB_C2DSTAT advances, SB_C2DLEN and DMATCR2 reach 0 */
	reg(0x005f6804)->value = 0;
	reg(0x005f6808)->value = 0;
	reg(0xffa00028)->value = 0;
	reg(0xffa00020)->value = src + len;
	reg(0xffa0002c)->value |= 2;	/* CHCR2.TE */
	hw_dma_count[0]++;
	hw_raise(19);
}

static void pvr_dma(void)
{
	const u32 pvr = regv(0x005f7c00), sys = regv(0x005f7c04), len = regv(0x005f7c08), dir = regv(0x005f7c0c);
	if (m1_verbose)
		printf("DMA pvr %08x %s %08x, %u bytes\n", sys, dir ? "<-" : "->", pvr, len);
	if (!dir)
		copy_to_pvr(pvr, sys, len);
	else
		for (u32 i = 0; i < len; i++)
			wr8(sys + i, vram[(pvr + i) & (VRAM_SIZE - 1)]);
	reg(0x005f7c18)->value = 0;
	hw_dma_count[1]++;
	hw_raise(11);
}

/* G2 DMA channels (005F7800 + 0x20 * ch): ch0 = AICA (sound RAM), ch1/2 = external, ch3 = dev.
   Registers: +0 G2 address, +4 system memory address, +8 length, +C direction (0: to G2),
   +18 start. End-of-DMA interrupt: ISTNRM bit 15 + ch. */
static void g2_dma(u32 ch)
{
	const u32 base = 0x005f7800u + 0x20 * ch;
	const u32 g2 = regv(base) & 0x1fffffe0u, sys = regv(base + 4), len = regv(base + 8) & 0x7fffffe0u, dir = regv(base + 0xc);
	if (ch != 0 || len > 0x10000)
		if (m1_verbose)
			printf("DMA g2 ch%u %08x %s %08x, %u bytes\n", ch, sys, dir ? "<-" : "->", g2, len);
	for (u32 i = 0; i < len; i++)
	{
		const u32 a = g2 + i;
		if (a >= 0x00800000u && a < 0x00a00000u)
		{
			if (!dir)
				aram[a - 0x00800000u] = rd8(sys + i);
			else
				wr8(sys + i, aram[a - 0x00800000u]);
		}
	}
	if (getenv("M1_CMDLOG") && g2 < 0x00815000u && g2 + len > 0x00813000u)
		printf("cmdlog: frame %u sample %llu G2 DMA %08x -> %08x, %u bytes, first word %08x\n", frames,
			   (unsigned long long)(cycles * 441 / 2000000), sys, g2, len, rd32(sys));
	reg(base + 0x18)->value = 0;
	hw_dma_count[2]++;
	hw_raise(15 + (int)ch);
}

/* ---- accesses ------------------------------------------------------------------------- */
/* virtual cycles of an access: the store queues are on-chip (a couple of cycles); Holly, VRAM
   and G2 (sound) accesses go over the bus */
static u32 access_cycles(u32 p) { return p >= 0xe0000000u && p < 0xe4000000u ? CYC_SQ : CYC_HW; }

u32 sh4_hw_read(u32 a, int size)
{
	const u32 p = phys(a);
	hw_tick(access_cycles(p));
	u32 v;
	if (p >= 0xe0000000u && p < 0xe4000000u)
		v = rd_le(sq + (p & 0x3c), size);
	else if (((p >> 26) & 7) == 1)
	{
		u32 r = 0;
		for (int i = 0; i < size; i++)
			r |= (u32)vram[vram_offset(p + i)] << (8 * i);
		return r;
	}
	else if (p >= 0x00800000u && p < 0x00a00000u)
	{
		v = rd_le(aram + (p - 0x00800000u), size);
		/* M1_ARAMLOG=from-to (video frames): count SH-4 reads of sound RAM per function and address */
		static int on = -1;
		static u32 from, to;
		if (on < 0)
			on = getenv("M1_ARAMLOG") && sscanf(getenv("M1_ARAMLOG"), "%u-%u", &from, &to) == 2;
		if (on && frames >= from && frames < to)
		{
			u32 m1_current_function(void);
			static struct { u32 fn, addr; u64 n; } t[256];
			static int nt;
			const u32 fn = m1_current_function();
			int i = 0;
			while (i < nt && !(t[i].fn == fn && t[i].addr == p))
				i++;
			if (i == nt && nt < 256)
				t[nt].fn = fn, t[nt].addr = p, nt++;
			if (i < nt && ++t[i].n % 100000 == 0)
				printf("aramlog: frame %u fn %08x reads %08x x%llu (value %08x)\n", frames, fn, p, (unsigned long long)t[i].n, v);
			if (i < nt && t[i].n == 1)
				printf("aramlog: frame %u fn %08x first read of %08x (value %08x)\n", frames, fn, p, v);
		}
	}
	else if (p >= 0x00700000u && p < 0x00710008u)
		v = aica_sh4_read(p, size);
	else if (p < BIOS_SIZE)
	{
		reg(0x005f8000);	/* make sure the presets (and the font) are loaded */
		return rd_le(bios + p, size);
	}
	else
	{
		struct reg *r = reg(p);
		r->reads++;
		switch (p)
		{
		case 0xffd8000cu: case 0xffd80018u: case 0xffd80024u:	/* TCNT0..2 count down at Pck/4 */
			r->value = ~(u32)(cycles / 16);
			break;
		case 0xff800030u:	/* PDTRA: video cable in bits 8-9 (Kamui sizes its framebuffers by it) */
		{
			/* low bits as Flycast's bsc.cpp (from chankast), from what was written to PCTRA/PDTRA */
			static int cable = -1;
			if (cable < 0)
				cable = getenv("M1_CABLE") ? atoi(getenv("M1_CABLE")) & 3 : 3;	/* 3 = TV composite */
			const u32 pctra = regv(0xff80002cu) & 0xf, pdtra = r->value & 0xf;
			u32 v = (pctra == 0x8 || pctra == 0xb) ? 3 : 0;
			if (pctra == 0xb && pdtra == 2)
				v = 0;
			else if (pctra == 0xc && pdtra == 2)
				v = 3;
			r->value = (r->value & ~0x3ffu) | v | (u32)cable << 8;
			break;
		}
		case 0x005f810cu:
			r->value = spg_status();
			break;
		case ISTNRM:	/* bits 30/31 summarize the EXT and ERR registers */
			r->value = (r->value & 0x3fffffffu) | (regv(ISTEXT) ? 1u << 30 : 0) | (regv(ISTERR) ? 1u << 31 : 0);
			break;
		}
		v = r->value;
		if (p == poll_addr && p != 0x005f810cu && p != ISTNRM)
		{
			if (++poll_count == POLL_LIMIT)
			{
				char b[96];
				snprintf(b, sizeof(b), "waiting on hardware: register %08x read %u times (value %08x)", p, POLL_LIMIT, v);
				m1_stop(b);
			}
		}
		else
			poll_addr = p, poll_count = 1;
	}
	logf_access("R", p, v, size);
	return size == 4 ? v : v & ((1u << (8 * size)) - 1);
}

void sh4_hw_write(u32 a, u32 v, int size)
{
	const u32 p = phys(a);
	hw_tick(access_cycles(p));
	poll_count = 0;
	if (p >= 0xe0000000u && p < 0xe4000000u)
	{
		memcpy(sq + (p & 0x3c), &v, size);
		return;
	}
	if (((p >> 26) & 7) == 1)
	{
		vram_log("cpu", p, size);
		vram_put32(p, &v, size);
		return;
	}
	if (p >= 0x005f9000u && p < 0x005fa000u)
		palette_gen++;
	logf_access("W", p, v, size);
	if (p >= 0x00800000u && p < 0x00a00000u)
	{
		memcpy(aram + (p - 0x00800000u), &v, size);
		if (((p >= 0x00813000u && p < 0x00815000u) || (frames >= 580 && frames <= 588 && getenv("M1_CMDALL"))) && getenv("M1_CMDLOG"))
		{
			u32 m1_current_function(void);
			printf("cmdlog: frame %u SH4 %08x = %08x /%d (fn %08x)\n", frames, p, v, size, m1_current_function());
		}
		return;
	}
	if (p >= 0x00700000u && p < 0x00710008u)
	{
		aica_sh4_write(p, v, size);
		return;
	}
	struct reg *r = reg(p);
	r->writes++;
	{
		/* M1_REGLOG=1: print changes of the framebuffer / TA base registers */
		static int on = -1;
		if (on < 0)
			on = getenv("M1_REGLOG") != 0;
		if (on && (p == 0x005f8050 || p == 0x005f8054 || p == 0x005f8060 || p == 0x005f8064 || p == 0x005f8124
				   || p == 0x005f8128 || p == 0x005f8020 || p == 0x005f8048 || p == 0x005f804c || p == 0x005f805c
				   || p == 0x005f8044) && r->value != v)
			printf("reg frame %u: %08x = %08x\n", frames, p, v);
	}
	if (p == ISTNRM || p == ISTEXT || p == ISTERR)
	{
		r->value &= ~v;	/* write 1 to clear */
		return;
	}
	if (size == 4)
		r->value = v;
	else
	{
		u32 m = ((1u << (8 * size)) - 1) << (8 * (p & 3 & (4 - size)));
		r->value = (r->value & ~m) | ((v << (8 * (p & 3 & (4 - size)))) & m);
	}
	if (p == 0x005f6808 && (v & 1))
		ch2_dma();
	if (p == 0x005f6c18 && (v & 1) && (regv(0x005f6c14) & 1))	/* SB_MDST with SB_MDEN */
		maple_dma();
	if (p == 0x005f8144 && (v & 0x80000000u))	/* TA_LIST_INIT */
		ta_list_init();
	if (p == 0x005f8160)	/* TA_LIST_CONT */
		ta_list_cont();
	if (p == 0x005f8014)	/* STARTRENDER: draw the frame; the render is reported done at once */
	{
		renders++;
		render_frame(ta_render_ctx());
		hw_raise(0);
		hw_raise(1);
		hw_raise(2);
	}
	if (p == 0x005f7c18 && (v & 1))
		pvr_dma();
	if (p >= 0x005f7818u && p <= 0x005f7878u && (p & 0x1f) == 0x18 && (v & 1))
		g2_dma((p - 0x005f7818u) >> 5);
}

/* pref: a store-queue address flushes 32 bytes to the external address selected by QACR0/1 */
void sh4_pref(SH4 *c, u32 a)
{
	(void)c;
	if (a < 0xe0000000u || a >= 0xe4000000u)
		return;
	const u32 qacr = regv((a & 0x20) ? 0xff00003cu : 0xff000038u);
	const u32 ext = (((qacr >> 2) & 7) << 26) | (a & 0x03ffffe0u);
	const u8 *src = sq + (a & 0x20);
	switch ((ext >> 26) & 7)
	{
	case 3:
		memcpy(sh4_ram + (ext & 0xffffe0u), src, 32);
		/* Kamui fills its vertex buffers this way: record who drew it, as wr32 does */
		if ((u32)((ext & 0xffffe0u) - sh4_tag_lo) < sh4_tag_len)
			sh4_tag_write(ext & 0xffffe0u);
		break;
	case 1: vram_log("sq-area1", ext, 32); for (u32 i = 0; i < 32; i += 4) vram_put32(ext + i, src + i, 4); break;
	case 4:
		if ((ext & 0x03000000u) == 0x01000000u)	/* TA texture path */
		{
			vram_log("sq-tex", ext, 32);
			vram_put(ext & (VRAM_SIZE - 1) & ~31u, src, 32);
		}
		else
		{
			ta_bytes += 32;
			ta_via_sq = 1;
			ta_cur_class = m1_2d_class;
			ta_cur_group = m1_2d_group;
			ta_input(src, 32);	/* tile accelerator polygon stream */
			ta_via_sq = 0;
		}
		break;
	default: logf_access("SQ?", ext, 0, 32); break;
	}
}

static int by_order(const void *x, const void *y)
{
	const struct reg *a = *(struct reg *const *)x, *b = *(struct reg *const *)y;
	return (int)a->order - (int)b->order;
}

void hw_report(void)
{
	if (!log_file && m1_verbose)
		log_file = fopen("m1_hw.txt", "w");
	struct reg *list[NREG];
	u32 n = 0;
	for (u32 i = 0; i < NREG; i++)
		if (regs[i].used) list[n++] = &regs[i];
	qsort(list, n, sizeof(list[0]), by_order);
	if (log_file)
	{
	fprintf(log_file, "\n# %llu hardware accesses, %u registers, %llu bytes sent to the TA, %u frames\n# addr reads writes last\n",
			(unsigned long long)accesses, n, (unsigned long long)ta_bytes, frames);
	for (u32 i = 0; i < n; i++)
		fprintf(log_file, "%08x %8u %8u %08x\n", list[i]->addr, list[i]->reads, list[i]->writes, list[i]->value);
	fclose(log_file);
	log_file = 0;
	}
	fprintf(stderr, "%llu hardware accesses to %u registers, %llu bytes to the TA, %u video frames, %llu ch2 / %llu PVR DMAs (details in m1_hw.txt)\n",
			(unsigned long long)accesses, n, (unsigned long long)ta_bytes, frames,
			(unsigned long long)hw_dma_count[0], (unsigned long long)hw_dma_count[1]);
	fprintf(stderr, "TA: %llu parameters, %llu vertices, %llu lists ended; %llu renders started%c",
			(unsigned long long)ta_params, (unsigned long long)ta_vertices, (unsigned long long)ta_lists_ended,
			(unsigned long long)renders, 10);
	fprintf(stderr, "%llu G2 (AICA) DMAs\n", (unsigned long long)hw_dma_count[2]);
}

/* ---- rollback state (m1/state.c): virtual clock, register values, store queue ------------- */
typedef struct
{
	u64 cycles;
	u32 frames, last_line, n, loop_cycles;
	u32 value[NREG];
	u8 sq[64];
} HwState;
u32 hw_state_size(void) { return sizeof(HwState); }
void hw_state_save(void *p)
{
	HwState *s = p;
	s->cycles = cycles;
	s->loop_cycles = sh4_loop_cycles;
	s->frames = frames;
	s->last_line = last_line;
	s->n = nregs;
	for (u32 i = 0; i < nregs; i++)
		s->value[i] = regs[i].value;
	memcpy(s->sq, sq, sizeof(sq));
}
void hw_state_load(const void *p)
{
	const HwState *s = p;
	cycles = s->cycles;
	sh4_loop_cycles = s->loop_cycles;
	frames = s->frames;
	last_line = s->last_line;
	for (u32 i = 0; i < s->n && i < nregs; i++)	/* registers first seen later keep their value */
		regs[i].value = s->value[i];
	memcpy(sq, s->sq, sizeof(sq));
}
