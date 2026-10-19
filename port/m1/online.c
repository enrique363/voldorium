/* Online Versus and Graphics from the game's own main menu (Windows).
 *
 * GRAPHICS (after OPTIONS) opens a submenu the same way: display (full screen or a window),
 * resolution and anti-aliasing; selecting a line cycles its value, applied at once and saved
 * (render.c video_*). The main menu's entries are rebuilt in scratch RAM (13 instead of 12);
 * ONLINE VERSUS and GRAPHICS share the INTERNET entry's id (help line switched by the cursor).
 *
 * The main menu has a hidden INTERNET entry (left over from the Dreamcast's online features).
 * It becomes ONLINE VERSUS, moved under VS BATTLE, and opens a submenu that the game's menu code
 * draws like any other: it borrows the Museum's menu slot while the cursor is on ONLINE VERSUS.
 * The submenu lists the players on the local network who are in the same submenu (UDP
 * broadcast), and handles challenge, accept and decline. A match restarts the game in a netplay
 * session (config.c --versus): both sides boot with the same scripted inputs into VS BATTLE
 * character select, then GGPO takes over (netplay.c).
 *
 * Menu data (1ST_READ.BIN):
 *   0x8c21a9e8  pointer to the menu cursor: +0 menu, +4 entry, +0x28 selection countdown
 *   0x8c21a848  menu descriptors, 0x14 bytes: entry count, parent menu, parent's entry, entries, title
 *   0x8c21a718  main menu entries, 16 bytes: s16 type (1 action, 2 submenu, 3 unused), s16 value,
 *               s16 id, s16 0, u32 colour, u32 label
 *   0x8c21a870  per entry id: s16 shown (1; 2 adds NEW), s16 selectable
 *   0x8c21a2f8  per entry id: 12 words, the help line in English, German, French, Spanish first
 * The menu function 0x8c0f3d46 returns the value of a chosen action entry once, then 0x12 while
 * its countdown runs and 0x13 to leave the mode; the online entries' values are taken here and
 * the countdown cleared, so the game never sees them.
 *
 * Strings and the submenu's entries live in BIOS work RAM that nothing uses under the HLE BIOS.
 *
 * Internet players come from the lobby server (lobby/sc_lobby.py; [net] lobby = host:port in
 * voldorium.ini, default below; M1_LOBBY_SERVER, "none" = off): the game says HELLO from its game
 * port every 2 s and gets the list of same-build players with their public addresses. Messages
 * to them go through the server (TO / FROM). After an ACCEPT the server sends both sides MATCH
 * (a token and the other's public address); both send SCPUNCH packets to each other for 2 s from
 * the game port, report PUNCHED 0/1, and the server answers GO DIRECT (the match connects
 * straight to the other's address) or GO RELAY (through the server's relay port, config.c
 * --relay). Players on the same network as well are taken from the broadcast list.
 */
#include "m1.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void online_menu(SH4 *c, void (*f)(SH4 *));

#define CURSOR_PTR 0x8c21a9e8u
#define MENUS 0x8c21a848u
#define MAIN_ITEMS 0x8c21a718u
#define AVAIL 0x8c21a870u
#define DESC 0x8c21a2f8u
#define SCRATCH 0x8c006000u

#define ID_INTERNET 20
#define ONLINE_INDEX 2		/* ONLINE VERSUS's place in the main menu (after VS BATTLE) */
#define MUSEUM_INDEX 7		/* MUSEUM's place after the move (was 6) */
#define GRAPHICS_INDEX 11	/* GRAPHICS: after OPTIONS (10); MISSION BATTLE moves to 12 */
#define MAIN_COUNT 13
#define ID_POOL 14			/* the Museum's entry ids 14-19 serve the lobby's lines */
#define POOL_N 6
#define ID_BACK 21			/* TO MAIN MENU */
/* values of the lobby's action entries (the game's own go up to 0x13) */
enum { V_PEER = 0x40, V_CANCEL = 0x48, V_ACCEPT, V_DECLINE, V_STATUS, V_G_DISPLAY = 0x50, V_G_RES, V_G_AA };	/* V_STATUS: a status line (selecting it does nothing; a disabled line would show "Not available in this version.") */

static void put32(u32 a, u32 v) { memcpy(&sh4_ram[a & 0xffffff], &v, 4); }
static void put16(u32 a, u16 v) { memcpy(&sh4_ram[a & 0xffffff], &v, 2); }
static u32 get32(u32 a) { u32 v; memcpy(&v, &sh4_ram[a & 0xffffff], 4); return v; }
static void put_str(u32 a, const char *s) { strcpy((char *)&sh4_ram[a & 0xffffff], s); }

/* scratch layout */
#define S_LABEL (SCRATCH + 0x000)		/* "ONLINE VERSUS" */
#define S_HELP (SCRATCH + 0x020)		/* main menu help line */
#define S_ITEMS (SCRATCH + 0x100)		/* lobby entries, 8 x 16 bytes */
#define S_LINES (SCRATCH + 0x200)		/* lobby labels, 8 x 32 bytes */
#define S_HELPS (SCRATCH + 0x400)		/* lobby help lines, 8 x 96 bytes */
#define S_MAIN (SCRATCH + 0x800)		/* the main menu's entries, 13 x 16 bytes */
#define S_GLABEL (SCRATCH + 0x900)	/* "GRAPHICS" */
#define S_GHELP (SCRATCH + 0x920)	/* its help line */

static void set_help(int id, u32 str)
{
	for (int k = 0; k < 4; k++)
		put32(DESC + (u32)id * 0x30 + (u32)k * 4, str);
}

static int patched;
static u32 museum_desc[5];	/* the Museum's descriptor (menu 1), restored when the cursor leaves ONLINE VERSUS */
static u32 museum_avail[POOL_N], museum_help[POOL_N];
enum { SLOT_MUSEUM, SLOT_LOBBY, SLOT_GRAPHICS };
static int slot_kind;	/* what menu 1 shows now */
#define slot_is_lobby (slot_kind == SLOT_LOBBY)

static void patch_main(void)
{
	patched = 1;
	put_str(S_LABEL, "ONLINE VERSUS");
	put_str(S_HELP, "Fight players around the world.");
	put_str(S_GLABEL, "GRAPHICS");
	put_str(S_GHELP, "Resolution, full screen and anti-aliasing.");
	set_help(ID_INTERNET, S_HELP);
	/* the main menu rebuilt in scratch RAM: entry 10 (INTERNET) becomes ONLINE VERSUS at index 2,
	   a copy of it GRAPHICS at index 11, both submenu entries like MUSEUM */
	static const int order[MAIN_COUNT] = { 0, 1, 10, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	for (int i = 0; i < MAIN_COUNT; i++)
		memcpy(&sh4_ram[(S_MAIN + (u32)i * 16) & 0xffffff], &sh4_ram[(MAIN_ITEMS + (u32)order[i] * 16) & 0xffffff], 16);
	const u32 museum_colour = get32(MAIN_ITEMS + 6 * 16 + 8);
	static const int ours[2] = { ONLINE_INDEX, GRAPHICS_INDEX };
	for (int k = 0; k < 2; k++)
	{
		const u32 it = S_MAIN + (u32)ours[k] * 16;
		put16(it, 2);	/* submenu */
		put16(it + 2, 1);	/* menu 1 (the borrowed Museum slot) */
		put32(it + 8, museum_colour);
		put32(it + 12, k ? S_GLABEL : S_LABEL);
	}
	put32(MENUS, MAIN_COUNT);
	put32(MENUS + 12, S_MAIN);
	for (int k = 0; k < 5; k++)
		museum_desc[k] = get32(MENUS + 0x14 + (u32)k * 4);
	museum_desc[2] = MUSEUM_INDEX;	/* back from the Museum lands on MUSEUM */
	put32(MENUS + 0x14 + 8, MUSEUM_INDEX);
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>

const char *config_get(const char *section, const char *key);
const char *config_disc_path(void);
const char *config_ranges_path(void);
extern int m1_net_active;

#define DISCOVERY_PORT 7100
#define GAME_PORT 7000		/* first of 20 tried; the chosen one carries the lobby's unicast and then GGPO */
#ifndef SC_BUILD_ID
#define SC_BUILD_ID "dev"
#endif

typedef struct
{
	u32 id;
	char name[16];
	struct sockaddr_in addr;	/* its game port (internet players: the public address) */
	DWORD seen;
	u8 net;						/* from the lobby server: messages go through it */
	int ping;					/* ms, -1 unknown: measured (same network) or estimated through the
								   server (internet: our time to it plus theirs) */
	int server_rtt;				/* internet: its time to the server, as the server reported it */
} Peer;
#define MAX_PEERS 6
static Peer peers[MAX_PEERS];
static int npeers;

enum { L_SEARCH, L_CHALLENGING, L_CHALLENGED, L_PUNCHING, L_STARTING };
static int lstate;
static Peer other;			/* the player being challenged or challenging us */
static char note[96];		/* a one-off help line (declined, busy) */
static DWORD note_until, sent_at;

static SOCKET disc_sock = INVALID_SOCKET, game_sock = INVALID_SOCKET;
static u16 game_port;
static u32 my_id;
static char my_name[16];
static int lobby_open;
static unsigned lobby_sig;

#define LOBBY_SERVER "147.15.149.200:7100"	/* sc-lobby on Oracle Cloud (Montreal) */
static struct sockaddr_in server_addr;
static int server_on;
static DWORD server_sent;
/* the internet match being set up: token, the other's address to punch, timings */
static char match_token[20];
static struct sockaddr_in punch_addr, punch_from;
static DWORD punch_start, punch_sent, punch_reported, punch_wait;
static int punched, my_player;
static int my_server_rtt = -1;	/* ms from HELLO to the server's LIST */

/* the menu font has capitals, digits and a little punctuation */
static void clean_name(char *dst, const char *src, size_t n)
{
	size_t j = 0;
	for (; *src && j + 1 < n; src++)
	{
		char ch = *src;
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 32);
		if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-')
			dst[j++] = ch;
	}
	dst[j] = 0;
	if (!j)
		snprintf(dst, n, "PLAYER");
}

static int sock_init(void)
{
	static int wsa;
	if (!wsa)
	{
		WSADATA d;
		if (WSAStartup(MAKEWORD(2, 2), &d))
			return 0;
		wsa = 1;
	}
	if (disc_sock == INVALID_SOCKET)
	{
		disc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		BOOL on = TRUE;
		setsockopt(disc_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
		setsockopt(disc_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
		struct sockaddr_in a = { 0 };
		a.sin_family = AF_INET;
		a.sin_port = htons(DISCOVERY_PORT);
		if (bind(disc_sock, (struct sockaddr *)&a, sizeof(a)))
			printf("online: discovery port %d: error %d\n", DISCOVERY_PORT, WSAGetLastError());
		u_long nb = 1;
		ioctlsocket(disc_sock, FIONBIO, &nb);
	}
	if (game_sock == INVALID_SOCKET)
	{
		/* the game port: first free one from 7000 (two copies on one PC get different ones) */
		for (u16 p = GAME_PORT; p < GAME_PORT + 20 && game_sock == INVALID_SOCKET; p++)
		{
			SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			struct sockaddr_in a = { 0 };
			a.sin_family = AF_INET;
			a.sin_port = htons(p);
			if (!bind(s, (struct sockaddr *)&a, sizeof(a)))
			{
				game_sock = s;
				game_port = p;
				BOOL on = TRUE;
				setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
				u_long nb = 1;
				ioctlsocket(s, FIONBIO, &nb);
			}
			else
				closesocket(s);
		}
	}
	if (!my_id)
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		my_id = (u32)t.QuadPart ^ (GetCurrentProcessId() << 16) ^ 0x5c5c0001u;
		/* never the computer's name (it often holds the owner's real name): a generic PLAYER,
		   shown to others as OPPONENT, unless the player sets a nickname ([net] name) */
		const char *n = config_get("net", "name");
		clean_name(my_name, n && *n ? n : "PLAYER", 13);
	}
	if (!server_on)
	{
		/* the lobby server: M1_LOBBY_SERVER, else [net] lobby, else the built-in one */
		const char *srv = getenv("M1_LOBBY_SERVER");
		if (!srv || !*srv)
			srv = config_get("net", "lobby");
		if (!srv || !*srv)
			srv = LOBBY_SERVER;
		char host[128] = "";
		unsigned port = 7100;
		if (strcmp(srv, "none") && sscanf(srv, "%127[^:]:%u", host, &port) >= 1)
		{
			struct addrinfo hints = { 0 }, *res = 0;
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			if (!getaddrinfo(host, 0, &hints, &res) && res)
			{
				server_addr = *(struct sockaddr_in *)res->ai_addr;
				server_addr.sin_port = htons((u16)port);
				server_on = 1;
				freeaddrinfo(res);
			}
			else
				printf("online: lobby server %s not found\n", srv);
		}
	}
	return disc_sock != INVALID_SOCKET && game_sock != INVALID_SOCKET;
}

static void send_to(const struct sockaddr_in *to, const char *fmt, ...)
{
	char m[160];
	int n = snprintf(m, sizeof(m), "SCPORT1 %s %08x ", SC_BUILD_ID, my_id);
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(m + n, sizeof(m) - (size_t)n, fmt, ap);
	va_end(ap);
	sendto(game_sock, m, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* to the lobby server: "SCL1 <build> <id> <text>" */
static void to_server(const char *fmt, ...)
{
	if (!server_on)
		return;
	char m[200];
	int n = snprintf(m, sizeof(m), "SCL1 %s %08x ", SC_BUILD_ID, my_id);
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(m + n, sizeof(m) - (size_t)n, fmt, ap);
	va_end(ap);
	sendto(game_sock, m, n, 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));
}

/* a lobby message for another player: straight to it on the network, through the server on the
   internet */
static void say(const Peer *p, const char *verb, const char *arg)
{
	if (p->net)
		to_server("TO %08x %s%s%s", p->id, verb, arg[0] ? " " : "", arg);
	else
		send_to(&p->addr, "%s%s%s", verb, arg[0] ? " " : "", arg);
}

static void broadcast(const char *fmt, const char *arg)
{
	struct sockaddr_in b = { 0 };
	b.sin_family = AF_INET;
	b.sin_port = htons(DISCOVERY_PORT);
	b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	send_to(&b, fmt, arg);
}

static Peer *find_peer(u32 id)
{
	for (int i = 0; i < npeers; i++)
		if (peers[i].id == id)
			return &peers[i];
	return 0;
}

static void set_note(const char *s)
{
	snprintf(note, sizeof(note), "%s", s);
	note_until = GetTickCount() + 4000;
}

static void start_match(int player, const char *relay);

/* how a player is shown: a nickname as set, else OPPONENT (numbered when several) in menu lines
   and "your opponent" in help lines */
static int generic(const char *name) { return !name[0] || !strcmp(name, "PLAYER"); }
static const char *label_of(const char *name, int index)
{
	static char buf[2][24];
	static int k;
	char *b = buf[k ^= 1];
	if (!generic(name))
		snprintf(b, sizeof(buf[0]), "%s", name);
	else
	{
		int n = 0;
		for (int i = 0; i < npeers; i++)
			n += generic(peers[i].name);
		if (n > 1 && index >= 0)
			snprintf(b, sizeof(buf[0]), "OPPONENT %d", index + 1);
		else
			snprintf(b, sizeof(buf[0]), "OPPONENT");
	}
	return b;
}
static const char *who(const char *name) { return generic(name) ? "your opponent" : name; }
static const char *Who(const char *name) { return generic(name) ? "Your opponent" : name; }


static void handle(u32 id, const char *verb, const char *arg, const struct sockaddr_in *from, int net);
static void on_server(char *m);

static void on_message(char *m, const struct sockaddr_in *from)
{
	char build[40], verb[16], arg[40] = "";
	unsigned id;
	static int debug = -1;
	if (debug < 0)
		debug = getenv("M1_LOBBY_DEBUG") != 0;
	if (debug)
		printf("online: from %s:%u: %s\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port), m);
	if (server_on && from->sin_addr.s_addr == server_addr.sin_addr.s_addr && from->sin_port == server_addr.sin_port)
	{
		on_server(m);
		return;
	}
	if (!strncmp(m, "SCPUNCH ", 8))
	{
		/* the other player's punch packet: the way between us is open */
		if (lstate == L_PUNCHING && match_token[0] && !strcmp(m + 8, match_token) && !punched)
		{
			punched = 1;
			punch_from = *from;
			printf("online: punched through to %s:%u\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port));
		}
		return;
	}
	if (sscanf(m, "SCPORT1 %39s %x %15s %39[^\n]", build, &id, verb, arg) < 3 || id == my_id)
		return;
	if (strcmp(build, SC_BUILD_ID))
		return;	/* another build: never listed, never matched */
	handle(id, verb, arg, from, 0);
}

/* a lobby message from another player (net: relayed by the server) */
static void handle(u32 id, const char *verb, const char *arg, const struct sockaddr_in *from, int net)
{
	Peer *p = find_peer(id);
	if (!strcmp(verb, "HELLO"))
	{
		/* the sender's port is its game port (HELLO goes out from that socket) */
		if (!p && npeers < MAX_PEERS)
		{
			p = &peers[npeers++];
			memset(p, 0, sizeof(*p));
			p->id = id;
			p->ping = -1;
		}
		if (p)
		{
			if (p->net)
				p->ping = -1;	/* was an internet entry: measure the direct way now */
			clean_name(p->name, arg, sizeof(p->name) - 3);
			p->addr = *from;
			p->seen = GetTickCount();
			p->net = 0;	/* on the same network: talk directly */
		}
	}
	else if (!strcmp(verb, "PING") && !net)
		send_to(from, "PONG %s", arg);	/* same network: the round trip is the ping */
	else if (!strcmp(verb, "PONG") && !net && p)
	{
		const int ms = (int)(GetTickCount() - (DWORD)strtoul(arg, 0, 10));
		if (ms >= 0 && ms < 5000)
			p->ping = p->ping < 0 ? ms : (p->ping * 3 + ms) / 4;
	}
	else if (!strcmp(verb, "BYE"))
	{
		if (p)
			*p = peers[--npeers];
		if (lstate == L_CHALLENGED && other.id == id)
			lstate = L_SEARCH;
	}
	else if (!strcmp(verb, "CHAL"))
	{
		if (lstate == L_CHALLENGED && other.id == id)
			return;	/* the same challenge again (it is resent every second): still waiting for our answer */
		if (lstate != L_SEARCH)
		{
			Peer busy = { 0 };
			busy.id = id, busy.addr = *from, busy.net = (u8)net;
			say(&busy, "BUSY", "");
			return;
		}
		memset(&other, 0, sizeof(other));
		other.id = id;
		other.addr = *from;
		other.net = (u8)net;
		other.ping = p ? p->ping : -1;
		clean_name(other.name, arg, sizeof(other.name) - 3);
		lstate = L_CHALLENGED;
	}
	else if (!strcmp(verb, "CANCEL") && lstate == L_CHALLENGED && other.id == id)
	{
		lstate = L_SEARCH;
		char t[96];
		snprintf(t, sizeof(t), "%s withdrew the challenge.", Who(other.name));
		set_note(t);
	}
	else if (!strcmp(verb, "ACCEPT") && lstate == L_CHALLENGING && other.id == id)
	{
		my_player = 2;	/* the challenger is player 2: the challenged player saw us as player 2 */
		if (!net)
			start_match(2, 0);
		else
		{
			lstate = L_PUNCHING;	/* the server's MATCH follows */
			punch_wait = GetTickCount();
			match_token[0] = 0;
		}
	}
	else if ((!strcmp(verb, "DECLINE") || !strcmp(verb, "BUSY")) && lstate == L_CHALLENGING && other.id == id)
	{
		lstate = L_SEARCH;
		char t[96];
		snprintf(t, sizeof(t), "%s %s.", Who(other.name), verb[0] == 'D' ? "declined" : "is busy");
		set_note(t);
	}
}

static int parse_addr(const char *s, struct sockaddr_in *a)
{
	unsigned b[4], port;
	if (sscanf(s, "%u.%u.%u.%u:%u", &b[0], &b[1], &b[2], &b[3], &port) != 5 || b[0] > 255 || b[1] > 255 || b[2] > 255
		|| b[3] > 255 || !port || port > 65535)
		return 0;
	memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = htonl(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
	a->sin_port = htons((u16)port);
	return 1;
}

/* from the lobby server: the player list, relayed messages, match set-up */
static void on_server(char *m)
{
	char w[4][48] = { "" };
	int n = sscanf(m, "SCL1 %47s %47s %47s %47s", w[0], w[1], w[2], w[3]);
	if (n < 1)
		return;
	if (!strcmp(w[0], "LIST"))
	{
		const DWORD now = GetTickCount();
		if (now - server_sent < 2000)
		{
			const int ms = (int)(now - server_sent);
			my_server_rtt = my_server_rtt < 0 ? ms : (my_server_rtt * 3 + ms) / 4;
		}
		for (char *it = strtok(m + 10, " "); it; it = strtok(0, " "))
		{
			unsigned id;
			char name[16], addr[32];
			int rtt = -1;
			struct sockaddr_in a;
			if (sscanf(it, "%x,%15[^,],%31[^,],%d", &id, name, addr, &rtt) < 3 || !parse_addr(addr, &a) || id == my_id)
				continue;
			Peer *p = find_peer(id);
			if (p && !p->net)
				continue;	/* also on this network: the direct way wins */
			if (!p && npeers < MAX_PEERS)
			{
				p = &peers[npeers++];
				memset(p, 0, sizeof(*p));
				p->id = id;
			}
			if (!p)
				continue;
			for (char *c = name; *c; c++)
				if (*c == '_')
					*c = ' ';
			clean_name(p->name, name, sizeof(p->name) - 3);
			p->addr = a;
			p->net = 1;
			p->seen = now;
			p->server_rtt = rtt;
			p->ping = rtt >= 0 && my_server_rtt >= 0 ? rtt + my_server_rtt : -1;	/* through the server */
		}
	}
	else if (!strcmp(w[0], "FROM") && n >= 4)
	{
		unsigned id;
		struct sockaddr_in a;
		char arg[40] = "";
		if (sscanf(w[1], "%x", &id) != 1 || !parse_addr(w[2], &a))
			return;
		const char *rest = strstr(m, w[3]);
		if (rest && sscanf(rest, "%*s %39[^\n]", arg) < 1)
			arg[0] = 0;
		handle(id, w[3], arg, &a, 1);
	}
	else if (!strcmp(w[0], "MATCH") && n >= 3 && (lstate == L_PUNCHING || lstate == L_CHALLENGING))
	{
		struct sockaddr_in a;
		if (strlen(w[1]) != 16 || !parse_addr(w[2], &a))
			return;
		snprintf(match_token, sizeof(match_token), "%s", w[1]);
		punch_addr = a;
		punched = 0;
		punch_start = GetTickCount();
		punch_sent = punch_reported = 0;
		if (lstate == L_CHALLENGING)
			my_player = 2;
		lstate = L_PUNCHING;
		printf("online: match %s, punching to %s\n", match_token, w[2]);
	}
	else if (!strcmp(w[0], "GO") && n >= 4 && lstate == L_PUNCHING && !strcmp(w[1], match_token))
	{
		if (!strcmp(w[2], "DIRECT"))
		{
			struct sockaddr_in a;
			if (!parse_addr(w[3], &a))
				return;
			other.addr = punched ? punch_from : a;
			start_match(my_player, 0);
		}
		else if (!strcmp(w[2], "RELAY"))
		{
			unsigned port;
			if (sscanf(w[3], "%u", &port) != 1 || !port || port > 65535)
				return;
			other.addr = server_addr;
			other.addr.sin_port = htons((u16)port);
			start_match(my_player, match_token);
		}
	}
}

static void poll(void)
{
	SOCKET ss[2] = { disc_sock, game_sock };
	for (int k = 0; k < 2; k++)
		for (;;)
		{
			char m[256];
			struct sockaddr_in from;
			int fl = sizeof(from);
			const int n = recvfrom(ss[k], m, sizeof(m) - 1, 0, (struct sockaddr *)&from, &fl);
			if (n <= 0)
				break;
			m[n] = 0;
			on_message(m, &from);
		}
	const DWORD now = GetTickCount();
	for (int i = 0; i < npeers;)
		if (now - peers[i].seen > (peers[i].net ? 6000u : 3500u))
			peers[i] = peers[--npeers];
		else
			i++;
	if (now - sent_at >= 1000)
	{
		sent_at = now;
		static int lan = -1;	/* M1_LOBBY_LAN=0 (tests): no network discovery, the server only */
		if (lan < 0)
			lan = !getenv("M1_LOBBY_LAN") || atoi(getenv("M1_LOBBY_LAN"));
		if (lan)
			broadcast("HELLO %s", my_name);
		char tick[16];
		snprintf(tick, sizeof(tick), "%lu", (unsigned long)now);
		for (int i = 0; i < npeers; i++)
			if (!peers[i].net)
				send_to(&peers[i].addr, "PING %s", tick);
		if (lstate == L_CHALLENGING)
			say(&other, "CHAL", my_name);	/* repeated: UDP may drop one */
	}
	if (server_on && now - server_sent >= 2000)
	{
		server_sent = now;
		char nm[16];
		snprintf(nm, sizeof(nm), "%s", my_name);
		to_server("HELLO %d %s", my_server_rtt < 0 ? 0 : my_server_rtt, nm);
	}
	if (lstate == L_PUNCHING)
	{
		if (!match_token[0])
		{
			if (now - punch_wait > 6000)	/* no MATCH from the server */
			{
				lstate = L_SEARCH;
				set_note("Could not reach the lobby server for the match.");
			}
		}
		else
		{
			if (now - punch_sent >= 100 && now - punch_start < 2500)
			{
				punch_sent = now;
				char pkt[40];
				const int n = snprintf(pkt, sizeof(pkt), "SCPUNCH %s", match_token);
				sendto(game_sock, pkt, n, 0, (const struct sockaddr *)&punch_addr, sizeof(punch_addr));
			}
			if (now - punch_start >= 2000 && now - punch_reported >= 500)
			{
				punch_reported = now;
				to_server("PUNCHED %s %d", match_token, punched && !getenv("M1_LOBBY_FORCE_RELAY"));	/* tests: relay */
			}
			if (now - punch_start > 12000)
			{
				lstate = L_SEARCH;
				char t[96];
				snprintf(t, sizeof(t), "Could not connect to %s.", who(other.name));
				set_note(t);
			}
		}
	}
}

/* starts this program again on the same disc with extra arguments; the log continues in the
   same output (inherited handles). Tests: M1_CHILD_SECONDS gives the new process a time limit,
   M1_CHILD_PADS replaces its M1_PADS script. */
static int relaunch(const char *extra)
{
	char exe[MAX_PATH], cmd[1400];
	GetModuleFileNameA(0, exe, sizeof(exe));
	int n = snprintf(cmd, sizeof(cmd), "\"%s\" --disc \"%s\" --ranges \"%s\" %s", exe, config_disc_path(),
					 config_ranges_path(), extra);
	if (getenv("M1_CHILD_SECONDS"))
		snprintf(cmd + n, sizeof(cmd) - (size_t)n, " --seconds %s", getenv("M1_CHILD_SECONDS"));
	if (getenv("M1_CHILD_PADS"))
		_putenv_s("M1_PADS", getenv("M1_CHILD_PADS"));
	fflush(stdout);
	fflush(stderr);
	STARTUPINFOA si = { sizeof(si) };
	HANDLE out = (HANDLE)_get_osfhandle(_fileno(stdout)), err = (HANDLE)_get_osfhandle(_fileno(stderr));
	const BOOL inherit = out != INVALID_HANDLE_VALUE && err != INVALID_HANDLE_VALUE;
	if (inherit)
	{
		SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		si.hStdOutput = out;
		si.hStdError = err;
	}
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(exe, cmd, 0, 0, inherit, 0, 0, 0, &si, &pi))
		return 0;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 1;
}

/* restarts this program into the session: both sides boot into VS BATTLE (config.c --versus);
   relay: the match goes through the lobby server's relay with that token */
static void start_match(int player, const char *relay)
{
	lstate = L_STARTING;
	char ip[64];
	inet_ntop(AF_INET, &other.addr.sin_addr, ip, sizeof(ip));
	const unsigned rport = ntohs(other.addr.sin_port);
	printf("online: match with %s (%s:%u), player %d\n", other.name, ip, rport, player);
	fflush(stdout);
	closesocket(game_sock);	/* GGPO binds the game port in the new process */
	closesocket(disc_sock);
	char args[512];
	int n = snprintf(args, sizeof(args), "--join %s:%u --port %u --player %d --versus --opponent \"%s\"", ip, rport,
					 game_port, player, who(other.name));
	if (relay)
		snprintf(args + n, sizeof(args) - (size_t)n, " --relay %s,%08x", relay, my_id);
	if (!relaunch(args))
		m1_fatal("Could not start the match (error %lu).", GetLastError());
	exit(0);
}

/* the end of a lobby match (netplay.c, render.c): restarts the game without the session; the new
   process boots straight to the main menu with the cursor on ONLINE VERSUS and shows why there
   (why = "": the player left with Esc). No message box: under a full screen window it would be
   hidden and the game would seem frozen. */
void online_return(const char *why)
{
	printf("online: back to the main menu: %s\n", why[0] ? why : "left the match");
	fflush(stdout);
	/* the session's settings are inherited by the new process through the environment */
	static const char *const session[] = { "M1_NET", "M1_NET_PRELUDE", "M1_NET_RELAY", "M1_NET_OPPONENT",
										   "M1_NET_MENU_DELAY", "M1_PADS", "M1_CHILD_PADS", "M1_LOBBY_TEST" };
	for (unsigned i = 0; i < sizeof(session) / sizeof(session[0]); i++)
		_putenv_s(session[i], "");
	char arg[160], *o = arg;
	o += sprintf(o, "--back \"");
	for (const char *s = why[0] ? why : "-"; *s && o < arg + sizeof(arg) - 2; s++)
		if (*s != '"')
			*o++ = *s;
	strcpy(o, "\"");
	if (!relaunch(arg))
		m1_fatal("Could not restart the game (error %lu).", GetLastError());
	exit(0);
}

/* after a match (config.c --back, M1_BACK): the cursor on ONLINE VERSUS, the reason as its help
   line until the cursor moves */
static void back_notice(u32 cur)
{
	static int done, shown;
	if (!done && get32(cur) == 0)
	{
		done = 1;
		const char *why = getenv("M1_BACK");
		if (!why)
			return;
		put32(cur + 4, ONLINE_INDEX);
		if (strcmp(why, "-"))
		{
			put_str(S_HELP, why);
			shown = 1;
		}
	}
	else if (shown && (get32(cur) != 0 || get32(cur + 4) != ONLINE_INDEX))
	{
		shown = 0;
		put_str(S_HELP, "Fight players around the world.");
	}
}

/* one lobby line: label, help line, value (type 2: the menu to return to) */
static int nlines;
static void line(const char *label, const char *help, int value, int type)
{
	const int k = nlines++;
	const u32 it = S_ITEMS + (u32)k * 16, lab = S_LINES + (u32)k * 32, hl = S_HELPS + (u32)k * 96;
	snprintf((char *)&sh4_ram[lab & 0xffffff], 32, "%s", label);
	snprintf((char *)&sh4_ram[hl & 0xffffff], 96, "%s", help);
	const int id = type == 2 ? ID_BACK : ID_POOL + k;
	memset(&sh4_ram[it & 0xffffff], 0, 16);
	put16(it, (u16)type);
	put16(it + 2, (u16)value);
	put16(it + 4, (u16)id);
	put32(it + 8, 0xffffffffu);
	put32(it + 12, lab);
	if (type != 2)
	{
		put16(AVAIL + (u32)id * 4, 1);
		put16(AVAIL + (u32)id * 4 + 2, 1);
		set_help(id, hl);
	}
}

static void build_lines(u32 cur)
{
	nlines = 0;
	char t[96], l[32];
	const int noted = note[0] && GetTickCount() < note_until;
	switch (lstate)
	{
	case L_SEARCH:
		if (!npeers)
			line("SEARCHING...", noted ? note : "Looking for players around the world.", V_STATUS, 1);
		for (int i = 0; i < npeers; i++)
		{
			if (peers[i].ping >= 0)
			{
				snprintf(l, sizeof(l), "%s  %d MS", label_of(peers[i].name, i), peers[i].ping);
				snprintf(t, sizeof(t), "Challenge %s. Ping %s%d ms.", generic(peers[i].name) ? "this opponent" : peers[i].name,
						 peers[i].net ? "about " : "", peers[i].ping);
			}
			else
			{
				snprintf(l, sizeof(l), "%s", label_of(peers[i].name, i));
				snprintf(t, sizeof(t), "Challenge %s.", generic(peers[i].name) ? "this opponent" : peers[i].name);
			}
			line(l, noted ? note : t, V_PEER + i, 1);
		}
		line("TO MAIN MENU", "", 0, 2);
		break;
	case L_CHALLENGING:
		snprintf(l, sizeof(l), "WAITING...");
		snprintf(t, sizeof(t), "Waiting for %s. You will be player 2.", who(other.name));	/* one line: the help bar
																									   centres a two-line text badly */
		line(l, t, V_STATUS, 1);
		line("CANCEL", "Withdraw the challenge.", V_CANCEL, 1);
		break;
	case L_CHALLENGED:
	{
		/* the opponent as player 2, its ping, then the choice (the cursor starts on ACCEPT) */
		Peer *p = find_peer(other.id);
		const int ping = p && p->ping >= 0 ? p->ping : other.ping;
		char ping_txt[24] = "";
		if (ping >= 0)
			snprintf(ping_txt, sizeof(ping_txt), " Ping %s%d ms.", other.net ? "about " : "", ping);
		if (generic(other.name))
			snprintf(t, sizeof(t), "Player 2 challenges you!%s", ping_txt);
		else
			snprintf(t, sizeof(t), "%s challenges you!%s", other.name, ping_txt);	/* fits one line with a 12-letter nickname */
		if (ping >= 0)
			snprintf(l, sizeof(l), "P2 %s  %d MS", label_of(other.name, -1), ping);
		else
			snprintf(l, sizeof(l), "P2 %s", label_of(other.name, -1));
		line(l, t, V_STATUS, 1);	/* right above ACCEPT in the menu wheel */
		line("ACCEPT", t, V_ACCEPT, 1);
		line("DECLINE", t, V_DECLINE, 1);
		break;
	}
	case L_PUNCHING:
		snprintf(t, sizeof(t), "Connecting to %s over the internet...", who(other.name));
		line("CONNECTING...", t, V_STATUS, 1);
		break;
	default:
		line("STARTING MATCH", "Connecting...", V_STATUS, 1);
		break;
	}
	for (int k = nlines; k < POOL_N; k++)
		put16(AVAIL + (u32)(ID_POOL + k) * 4, 0);	/* unused pool ids hidden */
	put32(MENUS + 0x14, (u32)nlines);
	/* a new list starts at its first line; the same list keeps the cursor */
	unsigned sig = (unsigned)lstate * 977u + (unsigned)nlines;
	for (int i = 0; i < npeers && lstate == L_SEARCH; i++)
		sig = sig * 31u + peers[i].id;
	if (get32(cur) == 1 && (sig != lobby_sig || get32(cur + 4) >= (u32)nlines))
		put32(cur + 4, lstate == L_CHALLENGED ? 1 : 0);	/* a challenge: straight onto ACCEPT */
	lobby_sig = sig;
}

/* the GRAPHICS submenu: one line per setting, its current value as the label */
void video_resolution_label(char *buf, size_t n);
int video_is_fullscreen(void);
int video_msaa(void);
void video_toggle_fullscreen(void);
void video_next_resolution(void);
void video_next_msaa(void);

static void build_graphics(u32 cur)
{
	nlines = 0;
	char l[32];
	line(video_is_fullscreen() ? "FULL SCREEN" : "WINDOW", "Display: full screen or a window. Select to switch.", V_G_DISPLAY, 1);
	video_resolution_label(l, sizeof(l));
	line(l, "Resolution the game draws at. Select to change.", V_G_RES, 1);
	const int aa = video_msaa();
	snprintf(l, sizeof(l), "ANTI-ALIASING %s", aa == 2 ? "2X" : aa == 4 ? "4X" : aa == 8 ? "8X" : "OFF");
	line(l, "Smooths jagged edges. Select to change it.", V_G_AA, 1);
	line("TO MAIN MENU", "", 0, 2);
	for (int k = nlines; k < POOL_N; k++)
		put16(AVAIL + (u32)(ID_POOL + k) * 4, 0);
	put32(MENUS + 0x14, (u32)nlines);
	if (get32(cur) == 1 && get32(cur + 4) >= (u32)nlines)
		put32(cur + 4, 0);
}

static void graphics_choose(int v)
{
	if (v == V_G_DISPLAY)
		video_toggle_fullscreen();
	else if (v == V_G_RES)
		video_next_resolution();
	else if (v == V_G_AA)
		video_next_msaa();
}

static void lobby_choose(int v)
{
	if (v >= V_PEER && v < V_PEER + npeers && lstate == L_SEARCH)
	{
		other = peers[v - V_PEER];
		lstate = L_CHALLENGING;
		sent_at = 0;	/* send the challenge now */
	}
	else if (v == V_CANCEL && lstate == L_CHALLENGING)
	{
		say(&other, "CANCEL", "");
		lstate = L_SEARCH;
	}
	else if (v == V_ACCEPT && lstate == L_CHALLENGED)
	{
		my_player = 1;	/* the challenger, shown as player 2, plays player 2 */
		if (!other.net)
		{
			for (int i = 0; i < 3; i++)
				say(&other, "ACCEPT", "");
			start_match(1, 0);
		}
		else
		{
			say(&other, "ACCEPT", "");	/* the server answers both of us with MATCH */
			lstate = L_PUNCHING;
			punch_wait = GetTickCount();
			match_token[0] = 0;
		}
	}
	else if (v == V_DECLINE && lstate == L_CHALLENGED)
	{
		say(&other, "DECLINE", "");
		lstate = L_SEARCH;
	}
}

static void lobby_close(void)
{
	lobby_open = 0;
	if (lstate == L_CHALLENGING || lstate == L_CHALLENGED)
		say(&other, lstate == L_CHALLENGING ? "CANCEL" : "DECLINE", "");
	broadcast("BYE %s", my_name);
	to_server("BYE");
	lstate = L_SEARCH;
	npeers = 0;
}

/* the borrowed menu slot: the lobby while the cursor is on ONLINE VERSUS, the graphics
   settings on GRAPHICS, else the Museum */
static void use_slot(int kind)
{
	if (kind == slot_kind)
		return;
	if (slot_kind == SLOT_MUSEUM)
		for (int k = 0; k < POOL_N; k++)
		{
			museum_avail[k] = get32(AVAIL + (u32)(ID_POOL + k) * 4);
			museum_help[k] = get32(DESC + (u32)(ID_POOL + k) * 0x30);
		}
	slot_kind = kind;
	if (kind != SLOT_MUSEUM)
	{
		const int lobby = kind == SLOT_LOBBY;
		put32(MENUS + 0x14 + 4, 0);	/* parent: main menu */
		put32(MENUS + 0x14 + 8, lobby ? ONLINE_INDEX : GRAPHICS_INDEX);
		put32(MENUS + 0x14 + 12, S_ITEMS);
		put32(MENUS + 0x14 + 16, lobby ? S_LABEL : S_GLABEL);
		if (lobby)
		{
			lstate = L_SEARCH;
			build_lines(get32(CURSOR_PTR));
		}
		else
			build_graphics(get32(CURSOR_PTR));
	}
	else
	{
		for (int k = 0; k < 5; k++)
			put32(MENUS + 0x14 + (u32)k * 4, museum_desc[k]);
		for (int k = 0; k < POOL_N; k++)
		{
			put32(AVAIL + (u32)(ID_POOL + k) * 4, museum_avail[k]);
			set_help(ID_POOL + k, museum_help[k]);
		}
	}
}

void online_menu(SH4 *c, void (*f)(SH4 *))
{
	if (!patched)
		patch_main();
	/* in a session the entry is hidden (both sides do the same, so the state stays equal) */
	const int allowed = !m1_net_active;
	put16(AVAIL + ID_INTERNET * 4, (u16)allowed);
	put16(AVAIL + ID_INTERNET * 4 + 2, (u16)allowed);
	const u32 cur = get32(CURSOR_PTR);
	/* M1_LOBBY_TEST=open|challenge|accept (tests): opens the lobby a second into the main menu;
	   challenge picks the first player listed, accept takes the first challenge */
	static const char *test = (const char *)-1;
	static int test_calls;
	if (test == (const char *)-1)
		test = getenv("M1_LOBBY_TEST");
	if (test && strstr(test, "graphics") && allowed)
	{
		/* M1_LOBBY_TEST=graphics: the cursor onto GRAPHICS a second into the main menu, the
		   submenu two seconds later */
		if (get32(cur) == 0 && ++test_calls == 60)
			put32(cur + 4, GRAPHICS_INDEX);
		else if (get32(cur) == 0 && test_calls == 180)
		{
			use_slot(SLOT_GRAPHICS);
			put32(cur, 1);
			put32(cur + 4, 0);
		}
		/* M1_LOBBY_TEST=graphics,cycle: then changes the resolution and anti-aliasing once */
		static int cycle_calls;
		if (strstr(test, "cycle") && get32(cur) == 1 && ++cycle_calls == 120)
		{
			graphics_choose(V_G_RES);
			graphics_choose(V_G_AA);
		}
	}
	else if (test && allowed && get32(cur) == 0 && ++test_calls == 60)
	{
		printf("online: lobby test (%s)\n", test);
		put32(cur + 4, ONLINE_INDEX);
		use_slot(SLOT_LOBBY);
		put32(cur, 1);
		put32(cur + 4, 0);
	}
	/* M1_MENU_PICK=menu:entry (tests, artdump runs): chooses that entry of the main menu (0) or
	   the Museum (1) a second into the main menu, as if confirmed with the pad */
	static const char *pick = (const char *)-1;
	static int pick_calls;
	if (pick == (const char *)-1)
		pick = getenv("M1_MENU_PICK");
	if (pick && get32(cur) == 0 && ++pick_calls == 60)
	{
		unsigned m = 0, e = 0;
		sscanf(pick, "%u:%u", &m, &e);
		printf("online: menu pick %u:%u\n", m, e);
		put32(cur, m);
		put32(cur + 4, e);
		put32(cur + 0x28, 1);	/* the menu's "entry chosen" step: it returns the entry's value next */
	}
	if (test && lobby_open)
	{
		if (strstr(test, "challenge") && lstate == L_SEARCH && npeers)
			lobby_choose(V_PEER);
		if (strstr(test, "accept") && lstate == L_CHALLENGED)
			lobby_choose(V_ACCEPT);
	}
	if (allowed && slot_kind != SLOT_MUSEUM && get32(cur) == 1)
		put32(0x8c25dcdcu, 0);	/* the main menu's idle timer (mode 11, 0x8c033a86: back to the title after
								   3600 frames without input): waiting in the lobby is not idling */
	if (allowed)
	{
		back_notice(cur);
		if (get32(cur) == 0)
		{
			const u32 e = get32(cur + 4);
			use_slot(e == ONLINE_INDEX ? SLOT_LOBBY : e == GRAPHICS_INDEX ? SLOT_GRAPHICS : SLOT_MUSEUM);
			set_help(ID_INTERNET, e == GRAPHICS_INDEX ? S_GHELP : S_HELP);	/* the two share an id */
		}
		const int open = get32(cur) == 1 && slot_is_lobby;
		if (open && !lobby_open)
		{
			lobby_open = sock_init();
			lstate = L_SEARCH;
			sent_at = 0;
			if (!lobby_open)
				set_note("The network could not be opened.");
			else
				printf("online: searching as %s on game port %u\n", my_name, game_port);
		}
		else if (!open && lobby_open)
			lobby_close();
		if (lobby_open)
			poll();
		if (slot_is_lobby)
			build_lines(cur);
		else if (slot_kind == SLOT_GRAPHICS)
			build_graphics(cur);
	}
	f(c);
	const u32 v = c->r[0];
	if (slot_is_lobby && v >= V_PEER && v <= V_STATUS)
	{
		put32(cur + 0x28, 0);	/* cancel the menu's leave-the-mode countdown */
		c->r[0] = 0x12;
		lobby_choose((int)v);
	}
	else if (slot_kind == SLOT_GRAPHICS && v >= V_G_DISPLAY && v <= V_G_AA)
	{
		put32(cur + 0x28, 0);
		c->r[0] = 0x12;
		graphics_choose((int)v);
	}
}
#else
void online_menu(SH4 *c, void (*f)(SH4 *))
{
	(void)patch_main;
	f(c);	/* Linux build: the entry stays hidden */
}
void online_return(const char *why) { m1_stop(why); }
#endif
