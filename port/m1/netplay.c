/* Online versus with GGPO (Windows; engines/ggpo, docs/phase4-ggpo-state.md).
 *
 * M1_NET=local_port,player,remote_ip:remote_port[,delay]
 *   player 1 or 2 (port A or B), delay = local input delay in fights, in frames (default 2).
 *   M1_NET_MENU_DELAY: input delay outside fights (default 8; lockstep there runs at full
 *   speed only while this covers the one-way latency).
 * Example on one machine: M1_NET=7000,1,127.0.0.1:7001 and M1_NET=7001,2,127.0.0.1:7000.
 *
 * The game owns the frame loop, while GGPO wants to load a state and re-simulate frames from
 * inside its callbacks. So GGPO runs on a driver fiber of its own: the hook at the top of
 * main's loop (state.c, the save point) switches to the driver, and the driver runs a frame by
 * switching back to the game until the next hook. Loading a state while the game waits at the
 * hook and then running frames is the rewind the SyncTest checks.
 *
 * Inputs: the button word of each player (Dreamcast bits, 1 = pressed). The Maple layer serves
 * controllers in ports A and B from the synchronised inputs (maple.c). Local input is the
 * window's keyboard / pad, plus M1_PADS, which counts netplay frames in this mode.
 *
 * Rollback only in fights: outside game mode 1 the SDK threads load data from disc, and a
 * rewind does not rewind their host stacks. There the prediction window is 1 frame
 * (ggpo_set_prediction_window, a small addition to the vendored GGPO), so every frame runs on
 * confirmed inputs (lockstep, hidden by the input delay); in fights it is 8.
 *
 * A match from the in-game lobby (online.c, config.c --versus, M1_NET_PRELUDE=1) first boots both
 * sides offline on the same scripted inputs into VS BATTLE character select, unpaced and not
 * drawn; boot is deterministic, so both arrive there in the same state, and GGPO starts there.
 *
 * Re-simulated frames are not drawn and not paced. Every 60 frames each side prints the state
 * checksum of a frame 10 frames back (past the prediction window, so final); comparing the
 * two logs shows whether the peers stayed in sync.
 */
#include "m1.h"
#include <stdlib.h>
#include <string.h>

int m1_net_active;	/* a netplay session runs: Maple serves both ports from net_pads */
int m1_resim;		/* the frame being run is a GGPO re-simulation: do not draw or pace */
u16 net_pads[2];

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdbool.h>
#include "ggponet.h"

u32 state_size(void);
void state_save(const SH4 *c, u8 *buf);
void state_load(SH4 *c, const u8 *buf);
u64 state_checksum(const u8 *buf);
void *m1_fiber_current(void);
u32 maple_script_buttons(u64 frame);
u32 video_buttons(void);
void video_title(const char *status);

static GGPOSession *gs;
static GGPOPlayerHandle local_h, remote_h;
static char peer_name[96];

void online_return(const char *why);

static void *game_fiber, *driver_fiber;
static int prelude = -1;
static int running, sleep_frames, delay = 2, menu_delay = 8, in_fight = -1;
static u64 netframe, resims, rollbacks;

/* window title: connection state, then ping, frames ahead and rollbacks once a second */
static void show_status(void)
{
	static DWORD last;
	const DWORD now = GetTickCount();
	if (now - last < 1000)
		return;
	last = now;
	char t[160];
	if (!running)
		snprintf(t, sizeof(t), "waiting for %s", peer_name);
	else
	{
		GGPONetworkStats st = { 0 };
		ggpo_get_network_stats(gs, remote_h, &st);
		snprintf(t, sizeof(t), "online, ping %d ms, %llu rollbacks", st.network.ping, (unsigned long long)rollbacks);
	}
	video_title(t);
}

/* state buffers: GGPO keeps up to 10 saved frames, so a fixed pool avoids 8 MB mallocs */
#define POOL 16
static u8 *pool[POOL];
static int pool_used[POOL];
static u8 *pool_get(void)
{
	for (int i = 0; i < POOL; i++)
		if (!pool_used[i])
		{
			if (!pool[i] && !(pool[i] = malloc(state_size())))
				break;
			pool_used[i] = 1;
			return pool[i];
		}
	m1_stop("net: state buffer pool exhausted");
	return 0;
}
static void pool_put(void *b)
{
	for (int i = 0; i < POOL; i++)
		if (pool[i] == b)
			pool_used[i] = 0;
}

/* the checksum of every 60th frame (frame % 60 == 50), kept until it is final */
static u64 sums[16];
static int sum_frame[16];

static void run_frame(void) { SwitchToFiber(game_fiber); }	/* back at the next loop top */

static bool __cdecl cb_begin(const char *game) { (void)game; return true; }
static bool __cdecl cb_save(unsigned char **buffer, int *len, int *checksum, int frame)
{
	u8 *b = pool_get();
	state_save(m1_cpu, b);
	*buffer = b;
	*len = (int)state_size();
	*checksum = 0;
	if (frame % 60 == 50)
	{
		sums[(frame / 60) & 15] = state_checksum(b);
		sum_frame[(frame / 60) & 15] = frame;
	}
	return true;
}
static bool __cdecl cb_load(unsigned char *buffer, int len)
{
	(void)len;
	state_load(m1_cpu, buffer);
	rollbacks++;
	return true;
}
static bool __cdecl cb_log(char *filename, unsigned char *buffer, int len)
{
	(void)filename; (void)buffer; (void)len;
	return true;
}
static void __cdecl cb_free(void *buffer) { pool_put(buffer); }
static bool __cdecl cb_advance(int flags)
{
	(void)flags;
	int disconnected;
	ggpo_synchronize_input(gs, net_pads, sizeof(net_pads), &disconnected);
	m1_resim = 1;
	run_frame();
	m1_resim = 0;
	resims++;
	ggpo_advance_frame(gs);
	return true;
}
static bool __cdecl cb_event(GGPOEvent *e)
{
	switch (e->code)
	{
	case GGPO_EVENTCODE_CONNECTED_TO_PEER: printf("net: connected to peer\n"); break;
	case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER: break;
	case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER: printf("net: synchronized with peer\n"); break;
	case GGPO_EVENTCODE_RUNNING: printf("net: running\n"); running = 1; break;
	case GGPO_EVENTCODE_TIMESYNC: sleep_frames = e->u.timesync.frames_ahead; break;
	case GGPO_EVENTCODE_CONNECTION_INTERRUPTED: printf("net: connection interrupted\n"); break;
	case GGPO_EVENTCODE_CONNECTION_RESUMED: printf("net: connection resumed\n"); break;
	case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
		if (prelude)
			online_return("The connection to your opponent was lost.");
		m1_stop("net: peer disconnected");
		break;
	default: break;
	}
	fflush(stdout);
	return true;
}

/* 60 Hz on the wall clock, giving GGPO the network while waiting */
static void wait_next_frame(void)
{
	static LARGE_INTEGER freq, next;
	if (!freq.QuadPart)
	{
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&next);
	}
	next.QuadPart += (LONGLONG)((double)freq.QuadPart / video_pace_hz());	/* 60, or 62 when VSync paces (render.c) */
	for (;;)
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const double left = (double)(next.QuadPart - now.QuadPart) / (double)freq.QuadPart;
		if (left < -0.1)
			next = now;	/* far behind: restart the schedule */
		if (left <= 0)
			break;
		ggpo_idle(gs, left > 0.002 ? (int)(left * 1000) - 1 : 0);
	}
}

static void CALLBACK driver(void *arg)
{
	(void)arg;
	for (;;)
	{
		m1_check_watchdog();	/* the game's V Blank check does not run while this waits */
		show_status();
		if (!running)
		{
			static DWORD since;
			if (!since)
				since = GetTickCount();
			if (prelude && GetTickCount() - since > 20000)	/* a lobby match that never connects */
			{
				online_return("Could not connect. Check your firewall.");
			}
			ggpo_idle(gs, 5);
			continue;
		}
		if (sleep_frames > 0)	/* GGPO timesync: this side is ahead, skip a frame */
		{
			sleep_frames--;
			wait_next_frame();
			continue;
		}
		const int fight = rd32(0x8c24a584) == 1;
		if (fight != in_fight)
		{
			/* fights: rollback with a short input delay; elsewhere lockstep, with a delay
			   long enough to cover the network (GGPO repeats or drops local inputs to
			   follow a delay change) */
			in_fight = fight;
			ggpo_set_prediction_window(gs, fight ? 8 : 1);
			ggpo_set_frame_delay(gs, local_h, fight ? delay : menu_delay);
		}
		u16 local = (u16)(maple_script_buttons(netframe) | video_buttons());
		if (!GGPO_SUCCEEDED(ggpo_add_local_input(gs, local_h, &local, sizeof(local))))
		{
			ggpo_idle(gs, 1);	/* prediction barrier: wait for the remote inputs */
			continue;
		}
		int disconnected;
		if (GGPO_SUCCEEDED(ggpo_synchronize_input(gs, net_pads, sizeof(net_pads), &disconnected)))
		{
			run_frame();
			netframe++;
			ggpo_advance_frame(gs);
			if (netframe % 60 == 0 && netframe >= 120)
			{
				const int f = (int)netframe - 70, k = (f / 60) & 15;	/* f % 60 == 50 */
				if (sum_frame[k] == f)
					printf("net: frame %d checksum %016llx (mode %u, %llu re-simulated frames, %llu rollbacks)\n", f,
						   (unsigned long long)sums[k], rd32(0x8c24a584), (unsigned long long)resims,
						   (unsigned long long)rollbacks);
				fflush(stdout);
			}
		}
		wait_next_frame();
	}
}

static void start(void)
{
	const char *e = getenv("M1_NET");
	unsigned lport = 0, rport = 0, player = 0;
	char host[64] = { 0 };
	if (sscanf(e, "%u,%u,%63[^:]:%u,%d", &lport, &player, host, &rport, &delay) < 4 || (player != 1 && player != 2))
		m1_stop("net: M1_NET=local_port,player(1|2),remote_ip:remote_port[,delay]");
	if (delay < 1)
		delay = 1;
	if (getenv("M1_NET_MENU_DELAY"))
		menu_delay = atoi(getenv("M1_NET_MENU_DELAY"));
	if (menu_delay < 1)
		menu_delay = 1;	/* lockstep needs at least one frame of input delay */
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa))	/* GGPO expects the application to start Winsock */
		m1_stop("net: WSAStartup failed");
	if (getenv("M1_NET_RELAY"))
	{
		/* through the lobby server's relay (online.c): register this game port with the match's
		   token first; the relay then forwards GGPO's packets to the other player */
		char token[24] = "", id[12] = "";
		if (sscanf(getenv("M1_NET_RELAY"), "%23[0-9a-f],%11[0-9a-f]", token, id) == 2)
		{
			SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			struct sockaddr_in me = { 0 }, relay = { 0 };
			me.sin_family = AF_INET;
			me.sin_port = htons((u_short)lport);
			relay.sin_family = AF_INET;
			relay.sin_port = htons((u_short)rport);
			inet_pton(AF_INET, host, &relay.sin_addr);
			if (s != INVALID_SOCKET && !bind(s, (struct sockaddr *)&me, sizeof(me)))
			{
				char reg[48];
				const int n = snprintf(reg, sizeof(reg), "SCR1 %s %s", token, id);
				for (int i = 0; i < 5; i++)
				{
					sendto(s, reg, n, 0, (struct sockaddr *)&relay, sizeof(relay));
					Sleep(40);
				}
				printf("net: registered with the relay %s:%u\n", host, rport);
			}
			if (s != INVALID_SOCKET)
				closesocket(s);
		}
	}
	GGPOSessionCallbacks cb = { 0 };
	cb.begin_game = cb_begin;
	cb.save_game_state = cb_save;
	cb.load_game_state = cb_load;
	cb.log_game_state = cb_log;
	cb.free_buffer = cb_free;
	cb.advance_frame = cb_advance;
	cb.on_event = cb_event;
	if (!GGPO_SUCCEEDED(ggpo_start_session(&gs, &cb, "soulcalibur", 2, sizeof(u16), (unsigned short)lport)))
		m1_stop("net: ggpo_start_session failed");
	ggpo_set_disconnect_timeout(gs, 5000);
	ggpo_set_disconnect_notify_start(gs, 1000);
	GGPOPlayer local = { sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, (int)player };
	GGPOPlayer remote = { sizeof(GGPOPlayer), GGPO_PLAYERTYPE_REMOTE, (int)(3 - player) };
	strncpy(remote.u.remote.ip_address, host, sizeof(remote.u.remote.ip_address) - 1);
	remote.u.remote.port = (unsigned short)rport;
	if (getenv("M1_NET_OPPONENT"))
		snprintf(peer_name, sizeof(peer_name), "%s", getenv("M1_NET_OPPONENT"));
	else
		snprintf(peer_name, sizeof(peer_name), "%s:%u", host, rport);
	if (!GGPO_SUCCEEDED(ggpo_add_player(gs, &local, &local_h)) || !GGPO_SUCCEEDED(ggpo_add_player(gs, &remote, &remote_h)))
		m1_stop("net: ggpo_add_player failed");
	ggpo_set_frame_delay(gs, local_h, menu_delay);	/* the session starts outside a fight */
	m1_net_active = 1;
	printf("net: player %u on port %u, peer %s:%u, input delay %d in fights, %d elsewhere\n", player, lport, host, rport,
		   delay, menu_delay);
	fflush(stdout);
	driver_fiber = CreateFiberEx(64 << 10, 4u << 20, 0, driver, 0);
	if (!driver_fiber)
		m1_stop("net: CreateFiberEx failed");
}

/* the scripted boot of a lobby match: Start through the title and notices, DOWN to VS BATTLE in
   the main menu (mode 11), Start; done once character select (mode 15) runs. Inputs depend only
   on the loop count since each mode began, so both sides run the same frames. Returns 1 while
   it runs. */
static int prelude_step(void)
{
	static u64 hooks, since;
	static u32 last = ~0u;
	const u32 mode = rd32(0x8c24a584);
	if (mode != last)
	{
		printf("net: boot, mode %u at loop %llu\n", mode, (unsigned long long)hooks);
		last = mode;
		since = hooks;
	}
	const u64 rel = hooks - since;
	u16 b = 0;
	if (mode == 15)
	{
		if (rel >= 2)
		{
			printf("net: boot done at loop %llu\n", (unsigned long long)hooks);
			fflush(stdout);
			return 0;
		}
	}
	else if (mode == 11)
	{
		if (rel >= 60 && rel < 64)
			b = 0x0020;	/* DOWN: VS BATTLE */
		else if (rel >= 100 && rel < 104)
			b = 0x0008;	/* Start */
		else if (rel > 900)
			online_return("The match could not be started.");
	}
	else if (rel >= 20 && rel % 40 < 4)
		b = 0x0008;	/* Start */
	if (hooks > 20000)
		online_return("The match could not be started.");
	net_pads[0] = b;
	net_pads[1] = 0;
	hooks++;
	return 1;
}

/* the boot after a lobby match (online.c online_return, M1_BACK): Start through the title and
   notices, unpaced and not drawn, until the main menu (mode 11) runs; then the player has it.
   Returns 1 while it runs. */
static int back_step(void)
{
	static u64 hooks, since;
	static u32 last = ~0u;
	const u32 mode = rd32(0x8c24a584);
	if (mode != last)
	{
		last = mode;
		since = hooks;
	}
	const u64 rel = hooks - since;
	if ((mode == 11 && rel >= 2) || hooks > 20000)
	{
		printf("online: main menu at loop %llu\n", (unsigned long long)hooks);
		fflush(stdout);
		return 0;
	}
	net_pads[0] = mode != 11 && rel >= 20 && rel % 40 < 4 ? 0x0008 : 0;	/* Start */
	net_pads[1] = 0;
	hooks++;
	return 1;
}

void netplay_hook(SH4 *c)
{
	(void)c;
	static int on = -1, back;
	if (on < 0)
	{
		back = getenv("M1_NET") == 0 && getenv("M1_BACK") != 0;
		if (back)
		{
			m1_net_active = 1;	/* the pads from net_pads */
			m1_resim = 1;		/* not drawn, not paced, silent */
		}
		on = getenv("M1_NET") != 0;
		prelude = on && getenv("M1_NET_PRELUDE") != 0;
		if (prelude)
		{
			m1_net_active = 1;	/* both ports from net_pads, and the lobby entry hidden */
			m1_resim = 1;		/* not drawn, not paced, silent */
			video_title("starting the match");
		}
	}
	if (back && !back_step())
	{
		back = 0;
		m1_net_active = 0;
		m1_resim = 0;
	}
	if (!on)
		return;
	static int booting = 1;
	if (prelude && booting)
	{
		if (prelude_step())
			return;
		booting = 0;
		m1_resim = 0;
	}
	game_fiber = m1_fiber_current();
	if (!driver_fiber)
		start();
	SwitchToFiber(driver_fiber);	/* back here when the driver runs the next frame */
}

void netplay_report(FILE *f)
{
	if (gs)
		fprintf(f, "net: %llu frames, %llu rollbacks, %llu re-simulated frames\n", (unsigned long long)netframe,
				(unsigned long long)rollbacks, (unsigned long long)resims);
}
#else
void netplay_hook(SH4 *c) { (void)c; }
void netplay_report(FILE *f) { (void)f; }
#endif
