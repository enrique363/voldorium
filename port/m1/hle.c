/* M1 high-level BIOS: the syscall vectors at 8C0000B0-8C0000E0 point at stubs (8C001000..08)
 * that are answered here, synchronously. Behaviour follows the documented Dreamcast syscall
 * interface (as also implemented by Flycast's reios): r6/r7 select the function, r4/r5 carry
 * arguments, r0 returns. GD-ROM reads come straight from the disc's data track (raw 2352-byte
 * sectors; frame address = LBA + 150). Everything is logged to stdout.
 */
#include "m1.h"
#include <stdlib.h>
#include <string.h>

/* ---- disc ------------------------------------------------------------------------------- */
static FILE *track;
static u32 track_fad;	/* frame address of the track's first sector */

void gd_open(const char *path, u32 lba)
{
	track = fopen(path, "rb");
	if (!track) { perror(path); exit(2); }
	track_fad = lba + 150;
}

/* reads count 2048-byte data sectors from LBA lba of the open data track into dst (host memory);
   returns 0 if they are outside the track */
int gd_read_data(u32 lba, u32 count, u8 *dst)
{
	u8 raw[2352];
	for (u32 i = 0; i < count; i++, dst += 2048)
	{
		if (lba + i + 150 < track_fad || fseek(track, (long)(lba + i + 150 - track_fad) * 2352, SEEK_SET)
			|| fread(raw, 1, sizeof(raw), track) != sizeof(raw))
			return 0;
		memcpy(dst, raw + 16, 2048);
	}
	return 1;
}

static void read_sectors(u32 dest, u32 fad, u32 count)
{
	u8 raw[2352];
	for (u32 i = 0; i < count; i++, dest += 2048)
	{
		if (fad + i < track_fad) m1_stop("GD-ROM read before the data track");
		fseek(track, (long)(fad + i - track_fad) * 2352, SEEK_SET);
		if (fread(raw, 1, sizeof(raw), track) != sizeof(raw)) m1_stop("GD-ROM read past the end of the track");
		for (u32 k = 0; k < 2048; k++)
			wr8(dest + k, raw[16 + k]);	/* mode 1: 16-byte header, 2048 bytes of data */
		/* M1_GDSPEED=bytes/s: let the drive's transfer time pass on the virtual clock
		   (default 0: reads complete at once) */
		static u32 cyc_per_sector = ~0u;
		if (cyc_per_sector == ~0u)
		{
			const double speed = getenv("M1_GDSPEED") ? atof(getenv("M1_GDSPEED")) : 0;
			cyc_per_sector = speed > 0 ? (u32)(200e6 * 2048 / speed) : 0;
		}
		hw_tick(cyc_per_sector);
	}
}

/* ---- GD-ROM syscalls (vector 8C0000BC, r6 = 0) --------------------------------------------- */
enum { GDC_ERR = -1, GDC_OK, GDC_BUSY, GDC_COMPLETE, GDC_CONTINUE };
static struct
{
	int status;
	u32 cmd, params[4], result[4], req_id, last_id;
	u32 dma_callback, dma_callback_arg;
	int dma_ended;	/* a DMA read finished and its G1_DMA_END callback has not run yet */
} gd = { .last_id = ~0u };

static const char *gd_cmd_name(u32 cc)
{
	switch (cc)
	{
	case 0x10: return "PIOREAD";
	case 0x11: return "DMAREAD";
	case 0x12: return "GETTOC";
	case 0x13: return "GETTOC2";
	case 0x14: return "PLAY";
	case 0x15: return "PLAY2";
	case 0x16: return "PAUSE";
	case 0x17: return "RELEASE";
	case 0x18: return "INIT";
	case 0x1b: return "SEEK";
	case 0x1e: return "REQ_MODE";
	case 0x1f: return "SET_MODE";
	case 0x21: return "STOP";
	case 0x22: return "GETSCD";
	case 0x23: return "REQ_SES";
	case 0x24: return "REQ_STAT";
	case 0x28: return "GET_VERSION";
	default: return "?";
	}
}

void config_set(const char *section, const char *key, const char *value);
void config_save(void);

/* fast loading: M1_GDINSTANT (from [game] loading in voldorium.ini), switched in the GRAPHICS menu */
static int fast_loading = -1;
int loading_fast(void)
{
	if (fast_loading < 0)
		fast_loading = getenv("M1_GDINSTANT") && atoi(getenv("M1_GDINSTANT"));
	return fast_loading;
}
void loading_next(void)
{
	fast_loading = !loading_fast();
	config_set("game", "loading", fast_loading ? "fast" : "original");
	config_save();
#ifdef _WIN32
	_putenv_s("M1_GDINSTANT", fast_loading ? "1" : "0");	/* a restart into an online match inherits it */
#else
	setenv("M1_GDINSTANT", fast_loading ? "1" : "0", 1);
#endif
}

static void gd_execute(void)
{
	const u32 *p = gd.params;
	switch (gd.cmd)
	{
	case 0x10:	/* PIOREAD: fad, sectors, buffer, 0 */
	case 0x11:	/* DMAREAD: same, ends with the GD-DMA interrupt */
	{
		/* A DMA read takes the drive's time, in emulated cycles, as in Flycast's HLE BIOS: 5
		   sectors per 1,000,000 cycles (5 ms, about 2 MB/s), the last few at the G1 bus rate.
		   With instant reads the VS screen before a fight was cut short: the stage's textures
		   replaced its pictures while it was still shown (garbage frames) and its side pan never
		   played. Fights keep instant reads: the game streams from the disc during them, and
		   rollback (netplay) does not rewind the drive. PIO reads are instant there too.
		   Fast loading ([game] loading = fast, M1_GDINSTANT=1, GRAPHICS menu): instant reads
		   everywhere, except in online matches, where both sides must take the same time. */
		static u64 ready;
		extern int m1_net_active;
		if (gd.cmd == 0x11 && (!loading_fast() || m1_net_active || getenv("M1_NET")) && rd32(0x8c24a584) != 1)
		{
			extern u64 m1_cycles(void);
			if (!ready)
			{
				u64 t = 0;
				for (u32 left = p[1]; left; left -= left < 5 ? left : 5)
					t += left * 2048 > 10240 ? 1000000 : left * 2048 * 2;
				ready = m1_cycles() + t;
				return;	/* busy until then (EXEC_SERVER calls this again) */
			}
			if (m1_cycles() < ready)
				return;
		}
		ready = 0;
		if (m1_verbose)
			printf("GD %s fad %u count %u -> %08x\n", gd_cmd_name(gd.cmd), p[0] & 0xffffff, p[1], p[2]);
		read_sectors(p[2], p[0] & 0xffffff, p[1]);
		gd.result[2] = p[1] * 2048;
		if (gd.cmd == 0x11)
		{
			/* the transfer ends with the GD-DMA interrupt (Holly bit 14); the game's handler
			   then calls G1_DMA_END, which runs the callback it registered */
			gd.dma_ended = 1;
			hw_raise(14);
		}
		break;
	}
	case 0x18:	/* INIT */
	case 0x16:	/* PAUSE */
	case 0x21:	/* STOP */
	case 0x17:	/* RELEASE */
	case 0x1b:	/* SEEK */
		printf("GD %s\n", gd_cmd_name(gd.cmd));
		break;
	default:
		printf("GD command %02x (%s) not modelled, reported complete\n", gd.cmd, gd_cmd_name(gd.cmd));
		break;
	}
	gd.status = GDC_COMPLETE;
}

static void gd_syscall(SH4 *c)
{
	u32 *r = c->r;
	switch (r[7])
	{
	case 0:	/* REQ_CMD: r4 command, r5 parameter block -> request id */
		if (gd.status != GDC_OK) { r[0] = 0; break; }
		for (int i = 0; i < 4; i++)
			gd.params[i] = r[5] ? rd32(r[5] + 4 * i) : 0;
		memset(gd.result, 0, sizeof(gd.result));
		gd.cmd = r[4];
		gd.status = GDC_BUSY;
		if (++gd.req_id == 0) gd.req_id = 1;
		gd.last_id = r[0] = gd.req_id;
		break;
	case 1:	/* GET_CMD_STAT: r4 request id, r5 -> 4 words of status */
		for (int i = 0; i < 4; i++)
			wr32(r[5] + 4 * i, gd.result[i]);
		if (gd.status == GDC_OK || gd.status == GDC_BUSY)
			r[0] = (u32)gd.status;
		else if (r[4] != gd.last_id)
			r[0] = GDC_OK;
		else
		{
			r[0] = (u32)gd.status;
			gd.status = GDC_OK;
			gd.last_id = ~0u;
		}
		break;
	case 2:	/* EXEC_SERVER */
		if (gd.status == GDC_BUSY)
			gd_execute();
		break;
	case 3:	/* INIT_SYSTEM */
		printf("GD INIT_SYSTEM\n");
		memset(&gd, 0, sizeof(gd));
		gd.last_id = ~0u;
		break;
	case 4:	/* GET_DRV_STAT: r4 -> drive status, disc type */
		wr32(r[4], 1);			/* GD_STAT_PAUSE */
		wr32(r[4] + 4, 0x80);	/* GD-ROM */
		r[0] = GDC_OK;
		break;
	case 5:	/* G1_DMA_END: r4 callback, r5 argument */
		gd.dma_callback = r[4];
		gd.dma_callback_arg = r[5];
		r[0] = GDC_OK;
		hw_cancel(14);	/* the BIOS acknowledges the GD-DMA interrupt here */
		if (gd.dma_callback && gd.dma_ended)
		{
			/* as the BIOS does: continue in the callback with r4 = its argument; it returns
			   to whoever made the syscall */
			gd.dma_ended = 0;
			r[4] = gd.dma_callback_arg;
			sh4_call(c, gd.dma_callback);
		}
		break;
	case 8:	/* READ_ABORT */
		r[0] = GDC_OK;
		gd.status = GDC_OK;
		break;
	case 9:	/* RESET */
		gd.status = GDC_OK;
		gd.last_id = ~0u;
		break;
	case 10:	/* CHANGE_DATA_TYPE */
		printf("GD CHANGE_DATA_TYPE %u %u %u %u\n", rd32(r[4]), rd32(r[4] + 4), rd32(r[4] + 8), rd32(r[4] + 12));
		r[0] = GDC_OK;
		break;
	default:
	{
		char b[64];
		snprintf(b, sizeof(b), "GD-ROM syscall %u not modelled", r[7]);
		m1_stop(b);
	}
	}
}

/* ---- other vectors ---------------------------------------------------------------------- */
/* Flash ROM (128 KB): the console's settings (region, broadcast standard, language...) that
   the game reads at start-up. With M1_FLASH=<image> (e.g. ~/m1_build/dc_flash.bin, a copy of
   Flycast's, made by m1/build.sh) the syscalls are served from it; writes stay in memory.
   Partitions as Flycast's DCFlashChip. */
static u8 flash[128 * 1024];
static int flash_loaded = -1;

static void flash_load(void)
{
	if (flash_loaded >= 0)
		return;
	char def[512];
	/* opt-in: with Flycast's flash settings the game takes a start-up path M1 does not model
	   yet (it reads unmodelled areas at 0x007Dxxxx); without flash it uses its defaults */
	const char *path = getenv("M1_FLASH");
	(void)def;
	FILE *f = path ? fopen(path, "rb") : 0;
	flash_loaded = f && fread(flash, 1, sizeof(flash), f) == sizeof(flash);
	if (f)
		fclose(f);
	printf("flash ROM: %s\n", flash_loaded ? path : "none (settings reads fail)");
}

static void system_syscall(SH4 *c)
{
	u32 *r = c->r;
	switch (r[7])
	{
	case 0:	/* SYSINFO_INIT: 8C000068 = system id (flash 1A056, 8 bytes), properties (flash 1A000, 5), zeros */
		flash_load();
		for (u32 i = 0; i < 24; i++)
			wr8(0x8c000068 + i, 0);
		for (u32 i = 0; i < 8 && flash_loaded; i++)
			wr8(0x8c000068 + i, flash[0x1a056 + i]);
		for (u32 i = 0; i < 5 && flash_loaded; i++)
			wr8(0x8c000070 + i, flash[0x1a000 + i]);
		r[0] = 0;
		break;
	case 2:	/* SYSINFO_ICON */
		r[0] = r[4] > 9 ? ~0u : 704;
		break;
	case 3:	/* SYSINFO_ID */
		r[0] = 0x8c000068;
		break;
	default:
		r[0] = ~0u;
		break;
	}
	printf("BIOS system cmd %u -> %08x\n", r[7], r[0]);
}

static void flashrom_syscall(SH4 *c)
{
	static const u32 part[5][2] = { { 0x1a000, 0x2000 }, { 0x18000, 0x2000 }, { 0x1c000, 0x4000 },
									{ 0x10000, 0x8000 }, { 0x00000, 0x10000 } };
	u32 *r = c->r;
	flash_load();
	r[0] = ~0u;
	if (!flash_loaded)
		return;
	switch (r[7])
	{
	case 0:	/* INFO: r4 partition -> r5[0] offset, r5[1] size */
		if (r[4] < 5)
		{
			wr32(r[5], part[r[4]][0]);
			wr32(r[5] + 4, part[r[4]][1]);
			r[0] = 0;
		}
		break;
	case 1:	/* READ: r4 offset, r5 destination, r6 bytes */
		for (u32 i = 0; i < r[6]; i++)
			wr8(r[5] + i, flash[(r[4] + i) % sizeof(flash)]);
		r[0] = 0;
		break;
	case 2:	/* WRITE: r4 offset, r5 source, r6 bytes (flash can only clear bits) */
		for (u32 i = 0; i < r[6]; i++)
			flash[(r[4] + i) % sizeof(flash)] &= rd8(r[5] + i);
		r[0] = r[6];
		break;
	case 3:	/* DELETE: r4 partition start: erase to 0xFF */
		for (int k = 0; k < 5; k++)
			if (part[k][0] == r[4])
				memset(flash + part[k][0], 0xff, part[k][1]), r[0] = 0;
		break;
	}
}

int hle_call(SH4 *c, u32 a)
{
	switch (a)
	{
	case 0x8c001000: system_syscall(c); return 1;
	case 0x8c001002:	/* font ROM: no font image yet */
		printf("BIOS font cmd %u\n", c->r[1]);
		c->r[0] = c->r[1] == 0 ? 0xa0100020u : 0;
		return 1;
	case 0x8c001004: flashrom_syscall(c); return 1;
	case 0x8c001006:
		if (c->r[6] == 0)
			gd_syscall(c);
		else
		{
			printf("BIOS misc (r6 %08x) cmd %u\n", c->r[6], c->r[7]);
			c->r[0] = 0;
		}
		return 1;
	case 0x8c001008:
		printf("BIOS misc vector cmd %u\n", c->r[7]);
		c->r[0] = 0;
		return 1;
	case 0x8c000000:
		/* kmInit ends with a tail jump through a hook pointer (8C246B60) whose default is the
		   start of the BIOS RAM area. Under Flycast's HLE BIOS that area is 0xFF-filled and
		   execution slides into the syscall stub, which returns; nothing uses the result. */
		return 1;
	case 0x8c102e84:
	case 0x8c102e10:
		return thread_hle(c, a);
	}
	if (a < 0x8c010000u)
	{
		char b[64];
		snprintf(b, sizeof(b), "call into the BIOS area %08x", a);
		m1_stop(b);
	}
	return 0;
}

/* rollback state (m1/state.c): the GD-ROM request in progress */
u32 hle_state_size(void) { return sizeof(gd); }
void hle_state_save(void *p) { memcpy(p, &gd, sizeof(gd)); }
void hle_state_load(const void *p) { memcpy(&gd, p, sizeof(gd)); }
