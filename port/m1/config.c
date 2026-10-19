/* Startup: settings file, command line, and the game files from the disc dump.
 *
 * voldorium.ini (next to the executable) holds the player's settings:
 *   [game]  disc = path of the .gdi        font = BIOS font file (optional)
 *           vmu = memory card file (default vmu_a1.bin next to the executable; none in netplay)
 *   [video] fullscreen = 0/1 (player form default 1)  resolution = native or WxH (16:9; the
 *           picture is drawn at that size)  window = 1280x720 (window size when native)
 *           msaa = 0/2/4/8 (anti-aliasing), vsync = 1/0, text = smooth/sharp
 *           backgrounds = stretch / blur / replace (4:3 menu and loading pictures in 16:9)
 *   [audio] enabled = 0/1  volume = 0-200 (percent)
 *   [keys]  up down left right a b x y start = SDL key names (e.g. Up, Z, Return)
 *   [net]   port = 7000  peer = address:port  player = 1/2  delay = 2  menu_delay = 8  name = lobby name
 *   [debug] artdump = folder: while playing, save the 4:3 artwork that leaves bars in 16:9
 *           (m1/artdump.c; the folder gets index.html)
 * Each setting becomes the M1_* variable the runtime already reads, unless that variable is
 * set (so development runs keep full control).
 *
 * Command line:
 *   m1 <sc folder> [seconds]           development form: <sc>/disc/gdi/soulcalibur.gdi,
 *                                      <sc>/ggpo/savestate_ranges.tsv, watchdog (default 20 s)
 *   m1 [--disc file.gdi] [--host port] [--join address:port [--port p]] [--player 1|2]
 *      [--delay n] [--seconds n]       player form (no arguments: the game; Windows asks once for
 *                                      the disc dump with a file picker)
 *
 * The game files come straight from the disc dump: the .gdi names the tracks, IP.BIN is the
 * first 16 sectors of the data track (LBA 45000) and 1ST_READ.BIN is found through its ISO 9660
 * directory, so no extracted files are needed.
 */
#include "m1.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void gd_open(const char *path, u32 lba);
int gd_read_data(u32 lba, u32 count, u8 *dst);

/* ---- settings file ---------------------------------------------------------------------- */
#define MAX_KEYS 64
static struct { char section[16], key[24], value[512]; } ini[MAX_KEYS];
static int nini;
static char ini_path[1024], exe_dir[1024];

static void find_exe_dir(void)
{
#ifdef _WIN32
	GetModuleFileNameA(0, exe_dir, sizeof(exe_dir));
#else
	snprintf(exe_dir, sizeof(exe_dir), "./x");
#endif
	char *slash = strrchr(exe_dir, '\\');
	char *slash2 = strrchr(exe_dir, '/');
	if (slash2 > slash)
		slash = slash2;
	if (slash)
		*slash = 0;
	else
		strcpy(exe_dir, ".");
}

const char *config_get(const char *section, const char *key)
{
	for (int i = 0; i < nini; i++)
		if (!strcmp(ini[i].section, section) && !strcmp(ini[i].key, key))
			return ini[i].value;
	return 0;
}

void config_set(const char *section, const char *key, const char *value)
{
	int i = 0;
	while (i < nini && !(!strcmp(ini[i].section, section) && !strcmp(ini[i].key, key)))
		i++;
	if (i == nini)
	{
		if (nini == MAX_KEYS)
			return;
		nini++;
		snprintf(ini[i].section, sizeof(ini[i].section), "%s", section);
		snprintf(ini[i].key, sizeof(ini[i].key), "%s", key);
	}
	snprintf(ini[i].value, sizeof(ini[i].value), "%s", value);
}

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
	size_t b = 0;
	while (s[b] == ' ' || s[b] == '\t')
		b++;
	memmove(s, s + b, strlen(s + b) + 1);
}

static void config_load(void)
{
	find_exe_dir();
	snprintf(ini_path, sizeof(ini_path), "%s/voldorium.ini", exe_dir);
	FILE *f = fopen(ini_path, "r");
	if (!f)
	{
		/* settings from before the rename (sc-port.ini) carry over */
		char old[1100];
		snprintf(old, sizeof(old), "%s/sc-port.ini", exe_dir);
		if (!rename(old, ini_path))
			f = fopen(ini_path, "r");
	}
	if (!f)
		return;
	char line[1024], section[16] = "";
	while (fgets(line, sizeof(line), f))
	{
		trim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#')
			continue;
		if (line[0] == '[')
		{
			char *e = strchr(line, ']');
			if (e)
				*e = 0;
			snprintf(section, sizeof(section), "%s", line + 1);
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char k[64], v[512];
		snprintf(k, sizeof(k), "%s", line);
		snprintf(v, sizeof(v), "%s", eq + 1);
		trim(k);
		trim(v);
		config_set(section, k, v);
	}
	fclose(f);
}

void config_save(void)
{
	if (!ini_path[0])
		return;
	FILE *f = fopen(ini_path, "w");
	if (!f)
		return;
	fprintf(f, "; Voldorium settings (see README.txt)\n");
	static const char *order[] = { "game", "video", "audio", "keys", "net" };
	for (unsigned s = 0; s < sizeof(order) / sizeof(order[0]); s++)
	{
		int any = 0;
		for (int i = 0; i < nini; i++)
			if (!strcmp(ini[i].section, order[s]))
			{
				if (!any)
					fprintf(f, "\n[%s]\n", order[s]);
				any = 1;
				fprintf(f, "%s = %s\n", ini[i].key, ini[i].value);
			}
	}
	fclose(f);
}

static void env_default(const char *name, const char *value)
{
	if (!value || getenv(name))
		return;
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 0);
#endif
}

/* settings -> M1_* variables (only those not set already) */
static void apply_settings(void)
{
	const char *v;
	env_default("M1_FONT", config_get("game", "font"));
	if ((v = config_get("video", "fullscreen")) && atoi(v))
		env_default("M1_FULLSCREEN", "1");
	env_default("M1_WINDOW", config_get("video", "window"));
	env_default("M1_RESOLUTION", config_get("video", "resolution"));
	env_default("M1_BGMODE", config_get("video", "backgrounds"));
	env_default("M1_ARTDUMP", config_get("debug", "artdump"));
	{
		char art[1100];	/* 16:9 replacement pictures (artswap.c) */
		snprintf(art, sizeof(art), "%s/art16x9", exe_dir);
		env_default("M1_ART16X9", art);
	}
	if ((v = config_get("video", "msaa")) && atoi(v))
		env_default("M1_MSAA", v);
	env_default("M1_VSYNC", config_get("video", "vsync"));	/* on unless 0 (render.c) */
	if ((v = config_get("video", "text")) && !strcmp(v, "sharp"))
		env_default("M1_SMOOTH2D", "0");	/* smooth (default): upscaled 2D text and HUD, render.c */
	if ((v = config_get("audio", "enabled")) && !atoi(v))
		env_default("M1_NOSOUND", "1");
	env_default("M1_VOLUME", config_get("audio", "volume"));
	/* keys: "up=Up,down=Down,..." for render.c */
	char keys[512] = "";
	static const char *names[] = { "up", "down", "left", "right", "a", "b", "x", "y", "start", "l", "r" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if ((v = config_get("keys", names[i])))
		{
			size_t n = strlen(keys);
			snprintf(keys + n, sizeof(keys) - n, "%s%s=%s", n ? "," : "", names[i], v);
		}
	if (keys[0])
		env_default("M1_KEYS", keys);
}

/* ---- disc ------------------------------------------------------------------------------- */
static char gdi_path[1024], ranges_path[1024];

/* opens the data track named by the .gdi (the track at LBA 45000 or above) */
static int open_gdi(const char *gdi)
{
	FILE *f = fopen(gdi, "r");
	if (!f)
		return 0;
	char line[1024], dir[1024];
	snprintf(dir, sizeof(dir), "%s", gdi);
	char *slash = strrchr(dir, '/'), *slash2 = strrchr(dir, '\\');
	if (slash2 > slash)
		slash = slash2;
	if (slash)
		slash[1] = 0;
	else
		dir[0] = 0;
	int ok = 0;
	while (fgets(line, sizeof(line), f))
	{
		unsigned trackno, lba, type, size;
		char name[512];
		if (sscanf(line, "%u %u %u %u %511s", &trackno, &lba, &type, &size, name) == 5 && lba >= 45000 && type == 4 && size == 2352)
		{
			char path[1600];
			snprintf(path, sizeof(path), "%s%s", dir, name);
			gd_open(path, lba);
			ok = 1;
			break;
		}
	}
	fclose(f);
	return ok;
}

/* IP.BIN (16 sectors at LBA 45000) and 1ST_READ.BIN (ISO 9660) into SH-4 RAM */
static long load_boot_files(void)
{
	static u8 buf[16 * 2048];
	if (!gd_read_data(45000, 16, buf))
		m1_stop("disc: cannot read IP.BIN");
	memcpy(sh4_ram + 0x8000, buf, sizeof(buf));
	u8 pvd[2048];
	if (!gd_read_data(45000 + 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5))
		m1_stop("disc: no ISO 9660 volume on the data track");
	const u32 root = pvd[156 + 2] | pvd[156 + 3] << 8 | pvd[156 + 4] << 16 | (u32)pvd[156 + 5] << 24;
	const u32 root_len = pvd[156 + 10] | pvd[156 + 11] << 8 | pvd[156 + 12] << 16 | (u32)pvd[156 + 13] << 24;
	for (u32 s = 0; s < (root_len + 2047) / 2048; s++)
	{
		u8 dir[2048];
		if (!gd_read_data(root + s, 1, dir))
			break;
		for (u32 o = 0; o < 2048 && dir[o];)
		{
			const u8 *r = dir + o;
			const u32 len = r[0], nlen = r[32];
			if (nlen >= 12 && !memcmp(r + 33, "1ST_READ.BIN", 12))
			{
				const u32 ext = r[2] | r[3] << 8 | r[4] << 16 | (u32)r[5] << 24;
				const u32 size = r[10] | r[11] << 8 | r[12] << 16 | (u32)r[13] << 24;
				if (size > (16u << 20) - 0x10000)
					m1_stop("disc: 1ST_READ.BIN too large");
				u8 *dst = malloc(((size + 2047) / 2048) * 2048);
				if (!dst || !gd_read_data(ext, (size + 2047) / 2048, dst))
					m1_stop("disc: cannot read 1ST_READ.BIN");
				memcpy(sh4_ram + 0x10000, dst, size);
				free(dst);
				return (long)size;
			}
			o += len;
		}
	}
	m1_stop("disc: 1ST_READ.BIN not found");
	return 0;
}

/* ---- startup ---------------------------------------------------------------------------- */
#ifdef _WIN32
#include <commdlg.h>
/* the first start (or a disc dump that moved): Windows' file picker for the .gdi, remembered in
   voldorium.ini. Returns 0 when cancelled. */
static int pick_disc(void)
{
	char file[1024] = "";
	OPENFILENAMEA of = { sizeof(of) };
	of.lpstrFilter = "Dreamcast disc dump (*.gdi)\0*.gdi\0All files\0*.*\0";
	of.lpstrFile = file;
	of.nMaxFile = sizeof(file);
	of.lpstrTitle = "Choose your game disc dump (the .gdi file)";
	of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileNameA(&of))
		return 0;
	config_set("game", "disc", file);
	config_save();
	return 1;
}
#endif

static int file_exists(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (f)
		fclose(f);
	return f != 0;
}

const char *config_disc_path(void) { return gdi_path; }
const char *config_ranges_path(void) { return ranges_path; }

/* Decides how to start; loads the boot files. Returns the watchdog limit in seconds (0 = none)
   and the path of the rollback range table through *ranges. */
int config_start(int argc, char **argv, const char **ranges)
{
	config_load();
	int seconds = 0, player = 0, delay = -1;
	char host_port[32] = "", join[256] = "", local_port[32] = "";
	if (argc >= 2 && argv[1][0] != '-')
	{
		/* development form */
		snprintf(gdi_path, sizeof(gdi_path), "%s/disc/gdi/soulcalibur.gdi", argv[1]);
		snprintf(ranges_path, sizeof(ranges_path), "%s/ggpo/savestate_ranges.tsv", argv[1]);
		seconds = argc > 2 ? atoi(argv[2]) : 20;
	}
	else
	{
#ifndef _WIN32
		if (argc < 2)
		{
			fprintf(stderr, "usage: m1 <sc folder> [seconds] | m1 --disc file.gdi [--host port | --join address:port]\n");
			exit(2);
		}
#endif
		for (int i = 1; i < argc; i++)
		{
			const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : 0;
			if (!strcmp(a, "--disc") && next)
				config_set("game", "disc", next), i++;
			else if (!strcmp(a, "--host") && next)
				snprintf(host_port, sizeof(host_port), "%s", next), i++;
			else if (!strcmp(a, "--join") && next)
				snprintf(join, sizeof(join), "%s", next), i++;
			else if (!strcmp(a, "--port") && next)
				snprintf(local_port, sizeof(local_port), "%s", next), i++;
			else if (!strcmp(a, "--player") && next)
				player = atoi(next), i++;
			else if (!strcmp(a, "--delay") && next)
				delay = atoi(next), i++;
			else if (!strcmp(a, "--seconds") && next)
				seconds = atoi(next), i++;
			else if (!strcmp(a, "--ranges") && next)	/* the rollback table elsewhere (lobby restarts of a dev build) */
				snprintf(ranges_path, sizeof(ranges_path), "%s", next), i++;
			else if (!strcmp(a, "--versus"))	/* a match from the Online Versus lobby (online.c) */
				env_default("M1_NET_PRELUDE", "1");
			else if (!strcmp(a, "--relay") && next)	/* token,id: the match goes through the lobby relay */
				env_default("M1_NET_RELAY", next), i++;
			else if (!strcmp(a, "--back") && next)	/* after a lobby match: straight to the main menu, why ("-": none) */
				env_default("M1_BACK", next), i++;
			else if (!strcmp(a, "--opponent") && next)
				env_default("M1_NET_OPPONENT", next), i++;
		}
		const char *disc = config_get("game", "disc");
#ifdef _WIN32
		if (!disc || !*disc || !file_exists(disc))
		{
			if (!pick_disc())
				exit(0);
			disc = config_get("game", "disc");
		}
#endif
		if (!disc)
			m1_fatal("No disc: pass --disc <file.gdi> or set [game] disc in %s", ini_path);
		snprintf(gdi_path, sizeof(gdi_path), "%s", disc);
		if (!ranges_path[0])
			snprintf(ranges_path, sizeof(ranges_path), "%s/savestate_ranges.tsv", exe_dir);
		if (!file_exists(ranges_path))
			m1_fatal("Missing %s.\nCopy the whole player folder, not only the executable.", ranges_path);
		/* netplay: --host port waits for the joiner's address too (GGPO is symmetric: each side
		   names the other), so hosting means player 1 with the peer given by --join or the ini */
		if (host_port[0] && !join[0] && config_get("net", "peer"))
			snprintf(join, sizeof(join), "%s", config_get("net", "peer"));
		if (join[0])
		{
			if (!player)
				player = host_port[0] ? 1 : 2;
			const char *port = host_port[0] ? host_port : local_port[0] ? local_port
				: config_get("net", "port") ? config_get("net", "port") : "7000";
			if (delay < 0)
				delay = config_get("net", "delay") ? atoi(config_get("net", "delay")) : 2;
			char net[512];
			snprintf(net, sizeof(net), "%s,%d,%s,%d", port, player, join, delay);
			env_default("M1_NET", net);
			env_default("M1_NET_MENU_DELAY", config_get("net", "menu_delay"));
		}
		if (!config_get("video", "fullscreen"))
			env_default("M1_FULLSCREEN", "1");	/* the player form starts full screen */
		if (!getenv("M1_NET"))
		{
			/* memory card in port A slot 1 (maple.c): player form outside netplay only, so both
			   sides of a match and the development runs see the same machine */
			char vmu[1100];
			const char *v = config_get("game", "vmu");
			snprintf(vmu, sizeof(vmu), "%s/vmu_a1.bin", exe_dir);
			env_default("M1_VMU", v && *v ? v : vmu);
		}
	}
	if (!seconds && !getenv("M1_VERBOSE"))
		m1_verbose = 0;	/* player form: a quiet log */
	apply_settings();
	if (!open_gdi(gdi_path))
		m1_fatal("Cannot read the disc dump:\n%s\n\nIts track files must be in the same folder as the .gdi file. Delete the disc line in voldorium.ini to choose it again.",
				 gdi_path);
	*ranges = ranges_path;
	printf("M1: disc %s, 1ST_READ.BIN %ld bytes\n", gdi_path, load_boot_files());
	return seconds;
}
