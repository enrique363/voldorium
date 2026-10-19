/* Maple bus: a standard controller in port A (and port B in netplay), and a VMU (memory card)
 * in port A's first expansion slot when M1_VMU names its file.
 *
 * A maple DMA (SB_MDST, 005F6C18) walks the descriptor list at SB_MDSTAR (005F6C04):
 *   word 0: bit 31 last entry, bits 16-17 port, bits 8-10 pattern (0 = send frame),
 *           bits 0-7 frame length in words minus 1
 *   word 1: address the reply frame is written to
 *   frame:  header (command | recipient << 8 | sender << 16 | data words << 24), data
 * A device replies with a frame whose header swaps sender and recipient; an absent device
 * (or an empty expansion slot) replies 0xFFFFFFFF. When the list is done SB_MDST returns to 0
 * and the maple end-of-DMA interrupt (ISTNRM bit 12) is raised. Reply layouts follow the
 * Dreamcast controller as Flycast models it (maple_devs.cpp).
 *
 * VMU (M1_VMU=file, 128 KB; the player form sets vmu_a1.bin next to the executable, see
 * config.c; netplay and development runs have none, so both sides and the test scripts see the
 * same machine): storage, LCD and clock functions answered as Flycast's VMU does. A missing
 * file becomes a blank formatted card (VMU file system: root block 255, FAT 254, directory
 * 253-241, 200 user blocks); block writes go to the file at once. The main unit's replies say
 * the slot is occupied (sender address bit 0).
 *
 * Pad input: M1_PADS="frame:buttons[,frame:buttons...]" holds a button set from that maple
 * frame on (frames count controller condition requests); buttons are letters
 * a b x y s(tart) u d l r (D-pad), or "-" for none. Example: M1_PADS="600:s,610:-,900:a,905:-".
 */
#include "m1.h"
#include "ta.h"
#include <stdlib.h>
#include <string.h>

u32 hw_reg(u32 addr);
void hw_set(u32 addr, u32 value);

enum { MFID_INPUT = 0x01000000, MFID_STORAGE = 0x02000000, MFID_LCD = 0x04000000, MFID_CLOCK = 0x08000000 };
enum { MDC_DEVICE_REQUEST = 1, MDC_ALL_STATUS = 2, MDC_RESET = 3, MDC_KILL = 4, MDC_GET_CONDITION = 9,
	   MDC_GET_MEDIA_INFO = 10, MDC_BLOCK_READ = 11, MDC_BLOCK_WRITE = 12, MDC_GET_LAST_ERROR = 13,
	   MDC_SET_CONDITION = 14 };
enum { MDRS_STATUS = 5, MDRS_STATUS_ALL = 6, MDRS_REPLY = 7, MDRS_DATA = 8, MDRE_UNKNOWN_FUNCTION = 0xfe,
	   MDRE_UNKNOWN = 0xfd, MDRE_AGAIN = 0xfc, MDRE_FILE = 0xfb };

/* Dreamcast button bits (active low in the condition reply) */
static u32 button_bit(char ch)
{
	switch (ch)
	{
	case 'b': return 0x0002;
	case 'a': return 0x0004;
	case 's': return 0x0008;
	case 'u': return 0x0010;
	case 'd': return 0x0020;
	case 'l': return 0x0040;
	case 'r': return 0x0080;
	case 'y': return 0x0200;
	case 'x': return 0x0400;
	default: return 0;
	}
}

static struct { u32 frame, buttons; } script[256];
static int nscript;
static u64 conditions, dmas;

static void load_script(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	const char *s = getenv("M1_PADS");
	while (s && *s && nscript < 256)
	{
		char *end;
		u32 frame = strtoul(s, &end, 10);
		if (*end != ':')
			break;
		u32 b = 0;
		for (s = end + 1; *s && *s != ','; s++)
			b |= button_bit(*s);
		script[nscript].frame = frame;
		script[nscript++].buttons = b;
		if (*s == ',')
			s++;
	}
}

static u32 pressed_now(void)
{
	u32 b = 0;
	for (int i = 0; i < nscript; i++)
		if (script[i].frame <= conditions)
			b = script[i].buttons;
	return b;
}

/* writes into a reply frame */
static u8 out[1024];
static u32 outlen;
static void w8(u8 v) { out[outlen++] = v; }
static void w16(u16 v) { memcpy(out + outlen, &v, 2); outlen += 2; }
static void w32(u32 v) { memcpy(out + outlen, &v, 4); outlen += 4; }
static void wstr(const char *s, u32 n)
{
	u32 len = (u32)strlen(s);
	for (u32 i = 0; i < n; i++)
		w8(i < len ? (u8)s[i] : ' ');
}

extern int m1_net_active;
extern u16 net_pads[2];

/* netplay.c: the script's buttons at a netplay frame (in M1_NET mode M1_PADS counts those) */
u32 maple_script_buttons(u64 frame)
{
	load_script();
	u32 b = 0;
	for (int i = 0; i < nscript; i++)
		if (script[i].frame <= frame)
			b = script[i].buttons;
	return b;
}

static u32 controller(u32 cmd, u32 port)
{
	switch (cmd)
	{
	case MDC_DEVICE_REQUEST:
	case MDC_ALL_STATUS:
		w32(MFID_INPUT);
		w32(0xfe060f00);	/* capabilities: A B X Y Start, D-pad, analog X/Y and triggers */
		w32(0);
		w32(0);
		w8(0xff);			/* area code: all regions */
		w8(0);				/* connector direction */
		wstr("Dreamcast Controller", 30);
		wstr("Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
		w16(0x01ae);		/* standby current */
		w16(0x01f4);		/* maximum current */
		return MDRS_STATUS;
	case MDC_GET_CONDITION:
	{
		/* script and live window input; in netplay the synchronised inputs of both players */
		u32 vmu_autostart(void);
		const u32 pressed = m1_net_active ? net_pads[port & 1] : pressed_now() | video_buttons() | vmu_autostart();
		if (port == 0)
			conditions++;
		w32(MFID_INPUT);
		w16((u16)(~pressed | 0xf901));	/* active low; C, D, Z and the second D-pad absent */
		w8(0);		/* right trigger */
		w8(0);		/* left trigger */
		w8(0x80);	/* analog X */
		w8(0x80);	/* analog Y */
		w8(0x80);
		w8(0x80);
		return MDRS_DATA;
	}
	case MDC_RESET:
	case MDC_KILL:
		return MDRS_REPLY;
	default:
		printf("maple: controller command %u not modelled\n", cmd);
		return MDRE_UNKNOWN;
	}
}

/* ---- VMU ---- */
static u8 vmu[128 << 10];
static FILE *vmu_file;
static int vmu_on = -1;

/* a blank formatted card (Marcus Comstedt's VMU file system description) */
static void vmu_format(void)
{
	memset(vmu, 0, sizeof(vmu));
	u8 *root = vmu + 255 * 512;
	memset(root, 0x55, 16);	/* formatted */
	static const u16 info[12] = { 0xff, 0, 0xff, 0xfe, 1, 0xfd, 13, 0, 200, 31, 0, 0 };
	/* total size, partition, root block, FAT block, FAT blocks, directory block, directory blocks,
	   icon, user blocks, extra blocks */
	memcpy(root + 0x40, info, sizeof(info));
	u16 *fat = (u16 *)(vmu + 254 * 512);
	for (int b = 0; b < 256; b++)
		fat[b] = 0xfffc;	/* free */
	fat[255] = fat[254] = 0xfffa;	/* root, FAT: single blocks */
	for (int b = 253; b > 241; b--)
		fat[b] = (u16)(b - 1);	/* directory: 253 down to 241 */
	fat[241] = 0xfffa;
}

static int vmu_present(void)
{
	if (vmu_on >= 0)
		return vmu_on;
	const char *path = getenv("M1_VMU");
	vmu_on = 0;
	if (!path || !*path || m1_net_active || getenv("M1_NET"))	/* netplay: no card (lobby restarts inherit M1_VMU) */
		return 0;
	vmu_file = fopen(path, "r+b");
	if (vmu_file && fread(vmu, 1, sizeof(vmu), vmu_file) == sizeof(vmu))
		printf("maple: VMU %s\n", path);
	else
	{
		if (vmu_file)
			fclose(vmu_file);
		vmu_format();
		vmu_file = fopen(path, "w+b");
		if (vmu_file)
		{
			fwrite(vmu, 1, sizeof(vmu), vmu_file);
			fflush(vmu_file);
		}
		printf("maple: VMU %s: new blank card%s\n", path, vmu_file ? "" : " (cannot be saved)");
	}
	vmu_on = 1;
	return 1;
}

/* the card holds the game's save (directory blocks 253-241, 32-byte entries: type, name at 4) */
static int vmu_has_save(void)
{
	for (int b = 253; b >= 241; b--)
		for (int e = 0; e < 16; e++)
		{
			const u8 *ent = vmu + b * 512 + e * 32;
			if (ent[0] && !memcmp(ent + 4, "SCALIBUR", 8))
				return 1;
		}
	return 0;
}

/* First run with a new card: the game's memory card check (game mode 31) asks to press Start to
   create its save; the port presses it (6 polls of every 60), so the screen only flashes by. */
u32 vmu_autostart(void)
{
	static u32 polls;
	if (!vmu_present() || rd32(0x8c24a584) != 31 || vmu_has_save())
	{
		polls = 0;
		return 0;
	}
	return ++polls % 60 >= 30 && polls % 60 < 36 ? 0x0008 : 0;
}

/* data words of the request (after the frame header) */
static const u8 *in_data;
static u32 in_len;
static u32 in32(u32 i) { u32 v = 0; if (i * 4 + 4 <= in_len) memcpy(&v, in_data + i * 4, 4); return v; }
static u32 swap32(u32 v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }

static u32 vmu_cmd(u32 cmd)
{
	switch (cmd)
	{
	case MDC_DEVICE_REQUEST:
	case MDC_ALL_STATUS:
		w32(MFID_STORAGE | MFID_LCD | MFID_CLOCK);
		w32(0x403f7e7e);	/* clock */
		w32(0x00100500);	/* LCD */
		w32(0x00410f00);	/* storage */
		w8(0xff);
		w8(0);
		wstr("Visual Memory", 30);
		wstr("Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
		w16(0x007c);
		w16(0x0082);
		if (cmd == MDC_ALL_STATUS)
		{
			wstr("Version 1.005,1999/04/15,315-6208-03,SEGA Visual Memory System BIOS Produced by ", 80);
			return MDRS_STATUS_ALL;
		}
		return MDRS_STATUS;
	case MDC_GET_MEDIA_INFO:
		if (in32(0) == MFID_STORAGE)
		{
			w32(MFID_STORAGE);
			for (int i = 0; i < 24; i++)
				w8(vmu[255 * 512 + 0x40 + i]);
			return MDRS_DATA;
		}
		if (in32(0) == MFID_LCD)
		{
			w32(MFID_LCD);
			w8(47), w8(31), w8(0x10), w8(2);	/* 48 x 32 dots, one colour */
			return MDRS_DATA;
		}
		return MDRE_UNKNOWN_FUNCTION;
	case MDC_BLOCK_READ:
		if (in32(0) == MFID_STORAGE)
		{
			const u32 loc = in32(1), block = swap32(loc) & 0xff;
			w32(MFID_STORAGE);
			w32(loc);
			for (int i = 0; i < 512; i++)
				w8(vmu[block * 512 + i]);
			return MDRS_DATA;
		}
		if (in32(0) == MFID_CLOCK)
		{
			w32(MFID_CLOCK);
			w16(1999), w8(9), w8(9), w8(0), w8(0), w8(0), w8(0);	/* a fixed date: runs stay repeatable */
			return MDRS_DATA;
		}
		return MDRE_UNKNOWN_FUNCTION;
	case MDC_BLOCK_WRITE:
		if (in32(0) == MFID_STORAGE)
		{
			const u32 loc = swap32(in32(1)), block = loc & 0xffff, phase = (loc >> 16) & 0xff;
			const u32 at = block * 512 + phase * 128, len = in_len > 8 ? in_len - 8 : 0;
			if (at + len > sizeof(vmu))
				return MDRE_FILE;
			memcpy(vmu + at, in_data + 8, len);
			if (vmu_file && (fseek(vmu_file, (long)at, SEEK_SET) || fwrite(vmu + at, 1, len, vmu_file) != len))
				return MDRE_FILE;
			return MDRS_REPLY;
		}
		if (in32(0) == MFID_LCD || in32(0) == MFID_CLOCK)
			return MDRS_REPLY;	/* the picture on the card's screen, the clock: not kept */
		return MDRE_UNKNOWN_FUNCTION;
	case MDC_GET_LAST_ERROR:	/* the end of a block write */
		if (vmu_file)
			fflush(vmu_file);
		return MDRS_REPLY;
	case MDC_SET_CONDITION:	/* the beeper */
	case MDC_RESET:
	case MDC_KILL:
		return MDRS_REPLY;
	default:
		printf("maple: VMU command %u not modelled\n", cmd);
		return MDRE_UNKNOWN;
	}
}

void maple_dma(void)
{
	load_script();
	u32 addr = hw_reg(0x005f6c04) & 0x1fffffe0u;
	dmas++;
	for (int n = 0; n < 64; n++)
	{
		const u32 h1 = rd32(addr), reply = rd32(addr + 4) & 0x1fffffe0u;
		const u32 pattern = (h1 >> 8) & 7, port = (h1 >> 16) & 3, words = (h1 & 0xff) + 1;
		if (pattern == 0)
		{
			const u32 frame = rd32(addr + 8);
			const u32 cmd = frame & 0xff, reci = (frame >> 8) & 0xff, send = (frame >> 16) & 0xff;
			const int main_unit = (reci & 0x1f) == 0;	/* bits 0-4 address the expansion slots */
			static u8 req[1024];
			in_len = (words - 1) * 4 < sizeof(req) ? (words - 1) * 4 : sizeof(req);
			for (u32 i = 0; i < in_len; i++)
				req[i] = rd8(addr + 12 + i);
			in_data = req;
			if (port == 0 && (reci & 0x1f) == 1 && vmu_present())	/* port A, slot 1: the VMU */
			{
				outlen = 0;
				const u32 resp = vmu_cmd(cmd);
				const u32 head = resp | send << 8 | reci << 16 | (outlen / 4) << 24;
				wr32(reply, head);
				for (u32 i = 0; i < outlen; i++)
					wr8(reply + 4 + i, out[i]);
			}
			else if ((port == 0 || (port == 1 && m1_net_active)) && main_unit)	/* port B: netplay */
			{
				outlen = 0;
				const u32 resp = controller(cmd, port);
				const u32 from = reci | (port == 0 && vmu_present() ? 1 : 0);	/* slot 1 occupied */
				const u32 head = resp | send << 8 | from << 16 | (outlen / 4) << 24;
				wr32(reply, head);
				for (u32 i = 0; i < outlen; i++)
					wr8(reply + 4 + i, out[i]);
			}
			else
				wr32(reply, 0xffffffffu);	/* no device */
			addr += (2 + words) * 4;
		}
		else
			addr += 4;	/* reset, NOP, light-gun patterns: one word */
		if (h1 >> 31)
			break;
	}
	hw_set(0x005f6c18, 0);
	hw_raise(12);
}

void maple_report(FILE *f)
{
	fprintf(f, "maple: %llu DMAs, %llu controller condition requests\n", (unsigned long long)dmas,
			(unsigned long long)conditions);
}

/* rollback state (m1/state.c): the condition count drives the M1_PADS script */
u64 maple_state_get(void) { return conditions; }
void maple_state_set(u64 v) { conditions = v; }
