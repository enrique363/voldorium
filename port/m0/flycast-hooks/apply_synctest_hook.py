"""Adds a GGPO-style SyncTest for Soulcalibur to the traced Flycast (interpreter only).

usage (inside WSL): python3 apply_synctest_hook.py ~/sc-flycast

Hook point: Sh4Interpreter::ReadNexOp (every instruction fetch). When SC_SYNCTEST is set:
  * sh4_ReadTimer (0x8c220060) is replaced by a deterministic counter: r0 = n++ * 0x9E3779B1,
    pc = pr. The counter is part of the saved state.
  * At the main-loop top (0x8c03144e), during a fight (mode 1, battle state 1):
      frame N start : save include ranges (sc/ggpo/savestate_ranges.tsv) + CPU registers + counter
      run 1         : at game_read_pads (0x8c061a20) record PDS_PERIPHERAL[24] (input)
      frame N+1 top : checksum include ranges (C1), restore the frame N snapshot, rerun
      run 2         : at game_read_pads replay the recorded PDS_PERIPHERAL (same input)
      frame N+1 top : checksum again (C2); C1 != C2 -> log differing 32-byte lines
    Library state (sound, Maple, Kamui, TA buffers) is NOT restored, exactly as in a port.
Environment:
  SC_SYNCTEST=1
  SC_SYNC_RANGES=path to savestate_ranges.tsv (required)
  SC_SYNC_LOG=path (default ~/synctest.csv)
  SC_SYNC_FRAMES=600 frames to test
  SC_SYNC_TIMER=0 keeps the real timer (to show it is an entropy source)
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).expanduser()

HOOK = r'''
// ---- SC SyncTest (Soulcalibur reverse-engineering project) ----
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "hw/sh4/sh4_mem.h"
#include "ui/gui.h"

#include <map>
#include <tuple>
namespace sctrace { extern void (*evHook)(int type, u32 addr, u32 value); }
namespace scsync
{
static const u32 LOOP_TOP = 0x8c03144e;
static const u32 READ_TIMER = 0x8c220060;
static const u32 READ_PADS = 0x8c061a20;
static const u32 PDS_ADDR = 0x8c33d2dc;
static const u32 PDS_SIZE = 0x4e0;

struct Range { u32 lo, hi; bool cmp; };	// cmp=false: restored but not checksummed
struct Regs
{
	u32 r[16], r_bank[8];
	float fr[16], xf[16];
	u64 mac;
	u32 gbr, pr, fpul;
	sr_t sr;
	fpscr_t fpscr;
};

static const bool enabled = getenv("SC_SYNCTEST") != nullptr || getenv("SC_WIDESCREEN") != nullptr
		|| getenv("SC_SHOTS") != nullptr || getenv("SC_RNGSPLIT") != nullptr || getenv("SC_STATELOG") != nullptr || getenv("SC_PCSCAN") != nullptr;
static const bool synctest = getenv("SC_SYNCTEST") != nullptr;

// SC_RNGSPLIT=1: RNG calls from cosmetic code get their own generator state, so presentation
// (effects, flare, HUD, cloth/wind sway) cannot shift the gameplay random stream.
static const bool rngSplit = getenv("SC_RNGSPLIT") != nullptr;
static u32 cosLfsr = 0x2545F491u, cosAlt = 1, cosRand = 1, cosTimer = 0;
static bool cosmeticCaller(u32 pr)
{
	return (pr >= 0x8c068000 && pr < 0x8c06d000)	// effects / Anime system
		|| (pr >= 0x8c05ec00 && pr < 0x8c061000)	// cloth / hair sway, stage wind
		|| (pr >= 0x8c08d7e0 && pr < 0x8c08dd00)	// sun flare
		|| (pr >= 0x8c0a6000 && pr < 0x8c0ac000);	// battle HUD
}
static u32 cosTimerRead() { return cosTimer++ * 0x9E3779B1u; }
static void cosLfsrSteps()
{
	int steps = (int)(cosTimerRead() & 3);
	do
	{
		bool bit = cosLfsr & 1;
		cosLfsr = (u32)((s32)cosLfsr >> 1);
		if (bit)
			cosLfsr ^= 0x48000000u;
	} while (--steps > 0);
}
// returns true when the call was answered here (pc set to the caller)
static bool cosmeticRng(Sh4Context *ctx)
{
	const u32 pc = ctx->pc;
	if (pc != 0x8c0639c8 && pc != 0x8c063a00 && pc != 0x8c063a7c && pc != 0x8c0feec4)
		return false;
	if (!cosmeticCaller(ctx->pr))
		return false;
	switch (pc)
	{
	case 0x8c0639c8:	// rng_next15
		cosLfsrSteps();
		ctx->r[0] = cosLfsr & 0x7fff;
		break;
	case 0x8c063a00:	// rng_next_float
		cosLfsrSteps();
		ctx->fr[0] = (float)(cosLfsr & 0xffff) * 1.5259021893143654e-05f;
		break;
	case 0x8c063a7c:	// rng_next_alt
	{
		cosAlt = cosAlt * 5 + 1;
		u32 v = (cosTimerRead() ^ cosAlt) & 0xffff;
		float div;
		u32 bits = *(u32 *)&mem_b[0x8c063c14 & RAM_MASK];
		memcpy(&div, &bits, 4);
		ctx->fr[0] = (float)v / div;
		break;
	}
	case 0x8c0feec4:	// rand
		cosRand = cosRand * 0x41c64e6du + 0x3039;
		ctx->r[0] = (cosRand >> 16) & 0x7fff;
		break;
	}
	ctx->pc = ctx->pr;
	return true;
}

// SC_WIDESCREEN=1: g_aspect_scale_x = 0.75 (Hor+ 16:9, anamorphic). SC_SHOTS="150,300": screenshots
// at those battle-state frames of the fight (GAME MODE, battle state 1).
// SC_STATELOG=path: per-frame fingerprint at the main-loop top (from boot), for diffing two runs.
static void stateLog()
{
	static FILE *f = getenv("SC_STATELOG") ? fopen(getenv("SC_STATELOG"), "w") : nullptr;
	static u32 n;
	if (f == nullptr)
		return;
	auto h = [](u32 lo, u32 hi) {
		u64 x = 1469598103934665603ull;
		for (u32 a = lo; a < hi; a++)
			x = (x ^ mem_b[a & RAM_MASK]) * 1099511628211ull;
		return x;
	};
	auto r32 = [](u32 a) { return *(u32 *)&mem_b[a & RAM_MASK]; };
	if (n == 0)
		fputs("n,mode,bstate,bframe,lfsr,lfsr2,rand,players,battle,camera,globals\n", f);
	fprintf(f, "%u,%u,%u,%u,%08x,%08x,%08x,%016llx,%016llx,%016llx,%016llx\n", n++, r32(0x8c24a584),
			r32(0x8c25b784), r32(0x8c25b788), r32(0x8c159060), r32(0x8c159064), r32(0x8c21aecc),
			(unsigned long long)h(0x8c24a958, 0x8c25b710), (unsigned long long)h(0x8c25b710, 0x8c25bb00),
			(unsigned long long)h(0x8c3366c0, 0x8c336820), (unsigned long long)h(0x8c24a4d4, 0x8c24a958));
	if (n % 300 == 0)
		fflush(f);
}

static void widescreenAndShots()
{
	stateLog();
	static const bool ws = getenv("SC_WIDESCREEN") != nullptr;
	if (ws)
		*(u32 *)&mem_b[0x8c266c28 & RAM_MASK] = 0x3f400000;
	static std::vector<u32> shots;
	static bool parsed;
	if (!parsed)
	{
		parsed = true;
		if (const char *e = getenv("SC_SHOTS"))
			for (const char *q = e; *q; )
			{
				char *end;
				shots.push_back(strtoul(q, &end, 0));
				q = *end ? end + 1 : end;
			}
	}
	if (shots.empty())
		return;
	u32 mode = *(u32 *)&mem_b[0x8c24a584 & RAM_MASK];
	u32 bstate = *(u32 *)&mem_b[0x8c25b784 & RAM_MASK];
	u32 sf = *(u32 *)&mem_b[0x8c25b788 & RAM_MASK];
	static u32 lastShot = ~0u;
	if (mode == 1 && bstate == 1 && sf != lastShot)
		for (u32 f : shots)
			if (f == sf)
			{
				lastShot = sf;
				fprintf(stderr, "SC shot: battle frame %u\n", sf);
				gui_takeScreenshot();
			}
}
static const bool fakeTimer = !(getenv("SC_SYNC_TIMER") && strcmp(getenv("SC_SYNC_TIMER"), "0") == 0);
static std::vector<Range> ranges;
static bool initDone;
static int state;			// 0 idle, 1 first run, 2 rerun
static u32 framesTested, framesBad, maxFrames = 600;
static std::vector<u8> snap, end1, end2, pds;
static Regs snapRegs;
static u32 timerCounter, snapTimerCounter;
static u32 vs1Start, vs1End, vs2Start, vs2End, dumped, maxDumps;
static std::string dumpDir;
static std::vector<u8> fullStart;	// whole RAM at run-1 start (leak detector)
static u32 leakDumps;

// ---- keyed event diff ----
struct EvKey
{
	u32 type, pc, addr;
	bool operator<(const EvKey &o) const { return std::tie(type, pc, addr) < std::tie(o.type, o.pc, o.addr); }
};
struct EvSeq
{
	u32 first = ~0u;	// global index of the first occurrence
	std::vector<u32> values;
};
static std::map<EvKey, EvSeq> evRun[2];
static int evActive = -1;	// -1 off, 0 run 1, 1 run 2
static u32 evIndex, evDumps;
static bool inInclude(u32 a)
{
	for (const Range &r : ranges)
		if (a >= r.lo && a < r.hi)
			return true;
	return false;
}
static void onEvent(int type, u32 addr, u32 value)
{
	if (evActive < 0)
		return;
	u32 a = addr;
	bool ramArea = ((addr >> 26) & 7) == 3 && (addr >> 29) != 7;
	if (ramArea)
		a = 0x8c000000 | (addr & RAM_MASK);
	bool inc = ramArea && inInclude(a);
	// writes into the save state, reads of anything outside it
	if ((type == 1 && !inc) || (type == 0 && inc))
		return;
	const Sh4Context &c = p_sh4rcb->cntx;
	EvSeq &s = evRun[evActive][{ (u32)type, c.pc - 2, a }];
	if (s.first == ~0u)
		s.first = evIndex;
	evIndex++;
	if (s.values.size() < 64)
		s.values.push_back(value);
}
static void startEvents(int run)
{
	evRun[run].clear();
	evIndex = 0;
	evActive = run;
	sctrace::evHook = &onEvent;
}
static void reportEvents(u32 test)
{
	struct Row { u32 first; EvKey k; const EvSeq *a, *b; };
	std::vector<Row> rows;
	static const EvSeq empty;
	for (const auto &kv : evRun[0])
	{
		auto it = evRun[1].find(kv.first);
		const EvSeq *b = it == evRun[1].end() ? &empty : &it->second;
		if (kv.second.values != b->values)
			rows.push_back({ std::min(kv.second.first, b->first), kv.first, &kv.second, b });
	}
	for (const auto &kv : evRun[1])
		if (evRun[0].find(kv.first) == evRun[0].end())
			rows.push_back({ kv.second.first, kv.first, &empty, &kv.second });
	std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.first < y.first; });
	FILE *f = fopen((dumpDir + "/sync_" + std::to_string(test) + "_events.txt").c_str(), "w");
	if (f == nullptr)
		return;
	fprintf(f, "# %zu differing keys; type R = read outside save state, W = write into save state\n", rows.size());
	fprintf(f, "# first type pc addr n1 n2 values1 | values2\n");
	for (size_t i = 0; i < rows.size() && i < 400; i++)
	{
		const Row &r = rows[i];
		fprintf(f, "%u %c %08x %08x %zu %zu ", r.first, r.k.type ? 'W' : 'R', r.k.pc, r.k.addr, r.a->values.size(), r.b->values.size());
		for (size_t j = 0; j < r.a->values.size() && j < 6; j++)
			fprintf(f, "%08x ", r.a->values[j]);
		fprintf(f, "| ");
		for (size_t j = 0; j < r.b->values.size() && j < 6; j++)
			fprintf(f, "%08x ", r.b->values[j]);
		fprintf(f, "\n");
	}
	fclose(f);
}

// After restoring the snapshot, everything still different from run-1 start was changed by
// run 1 and is NOT in the save state. Write those 32-byte lines as merged ranges.
static void reportLeaks(u32 test)
{
	const u32 size = RAM_MASK + 1;
	const u8 *now = &mem_b[0];
	FILE *f = fopen((dumpDir + "/sync_" + std::to_string(test) + "_leaks.txt").c_str(), "w");
	if (f == nullptr)
		return;
	u32 start = 0, bytes = 0;
	bool open = false;
	for (u32 o = 0; o <= size; o += 32)
	{
		bool diff = o < size && memcmp(now + o, &fullStart[o], 32) != 0;
		if (diff && !open)
		{
			start = o;
			bytes = 0;
			open = true;
		}
		if (diff)
			for (int k = 0; k < 32; k++)
				bytes += now[o + k] != fullStart[o + k];
		if (!diff && open)
		{
			fprintf(f, "%08x %08x %u\n", 0x8c000000 + start, 0x8c000000 + o, bytes);
			open = false;
		}
	}
	fclose(f);
}
static FILE *logFile;
static const bool autoplay = getenv("SC_AUTOPLAY") != nullptr;
static u32 apRng = 0x12345678u, apPrevOn;
static u32 loopCount;	// main-loop iterations since boot (deterministic per game frame)

// Shinobi PDD_DGT_* (positive logic): C 1, B 2, A 4, START 8, U 0x10, D 0x20, L 0x40, R 0x80, Y 0x200, X 0x400
static void autoplayPad()
{
	u32 vs = loopCount;	// was the vsync counter, which varies with emulated workload
	u32 mode = *(u32 *)&mem_b[0x8c24a584 & RAM_MASK];
	u32 on = 0;
	if (mode != 1)
	{
		// menus: tap START then A, 4 frames each, every 30 frames
		if ((vs % 30) < 4)
			on = ((vs / 30) & 1) ? 0x4 : 0x8;
	}
	else
	{
		// fight: new random action every 8 frames (no START, which pauses)
		if ((vs & 7) == 0)
		{
			apRng ^= apRng << 13; apRng ^= apRng >> 17; apRng ^= apRng << 5;
		}
		static const u16 acts[] = { 0x4, 0x2, 0x200, 0x400, 0x80, 0x40, 0x10, 0x20, 0x84, 0x42, 0x0, 0x6 };
		on = acts[apRng % (sizeof(acts) / sizeof(acts[0]))];
	}
	u32 *rec = (u32 *)&mem_b[PDS_ADDR & RAM_MASK];
	rec[2] = on;				// on
	rec[3] = ~on;				// off
	rec[4] = on & ~apPrevOn;	// press
	rec[5] = ~on & apPrevOn;	// release
	rec[11] = apPrevOn;			// old
	apPrevOn = on;
}

static u8 *ram(u32 addr) { return &mem_b[addr & RAM_MASK]; }
static u32 ram32(u32 addr) { return *(u32 *)ram(addr & ~3u); }

static void init()
{
	initDone = true;
	if (getenv("SC_SYNC_FRAMES"))
		maxFrames = strtoul(getenv("SC_SYNC_FRAMES"), nullptr, 0);
	if (getenv("SC_SYNC_DUMP"))
		maxDumps = strtoul(getenv("SC_SYNC_DUMP"), nullptr, 0);
	dumpDir = getenv("SC_SYNC_DUMPDIR") ? getenv("SC_SYNC_DUMPDIR") : "/tmp";
	const char *rp = getenv("SC_SYNC_RANGES");
	if (rp != nullptr)
		if (FILE *f = fopen(rp, "r"))
		{
			char line[512];
			while (fgets(line, sizeof(line), f))
			{
				if (line[0] == '#' || line[0] == '\n')
					continue;
				char a[32], b[32], d[32];
				if (sscanf(line, "%31s %31s %31s", a, b, d) == 3 && (strcmp(d, "include") == 0 || strcmp(d, "restore") == 0))
					ranges.push_back({ (u32)strtoul(a, nullptr, 0), (u32)strtoul(b, nullptr, 0), strcmp(d, "include") == 0 });
			}
			fclose(f);
		}
	const char *lp = getenv("SC_SYNC_LOG");
	std::string def = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/synctest.csv";
	logFile = fopen(lp ? lp : def.c_str(), "w");
	if (logFile)
		fputs("test,state_frames,battle_state,result,diff_lines,checksum1,checksum2,vsync_run1,vsync_run2,first_diffs\n", logFile);
	fprintf(stderr, "SC synctest: %zu include ranges, fake timer %s\n", ranges.size(), fakeTimer ? "on" : "off");
}

static void copyOut(std::vector<u8> &v)
{
	v.clear();
	for (const Range &r : ranges)
		v.insert(v.end(), ram(r.lo), ram(r.lo) + (r.hi - r.lo));
}

static void copyIn(const std::vector<u8> &v)
{
	size_t o = 0;
	for (const Range &r : ranges)
	{
		memcpy(ram(r.lo), &v[o], r.hi - r.lo);
		o += r.hi - r.lo;
	}
}

static u64 fnv(const std::vector<u8> &v)
{
	u64 h = 1469598103934665603ull;
	size_t o = 0;
	for (const Range &r : ranges)
	{
		size_t n = r.hi - r.lo;
		if (r.cmp)
			for (size_t i = o; i < o + n; i++)
				h = (h ^ v[i]) * 1099511628211ull;
		o += n;
	}
	return h;
}

static void saveRegs(const Sh4Context *c, Regs &g)
{
	memcpy(g.r, c->r, sizeof(g.r));
	memcpy(g.r_bank, c->r_bank, sizeof(g.r_bank));
	memcpy(g.fr, c->fr, sizeof(g.fr));
	memcpy(g.xf, c->xf, sizeof(g.xf));
	g.mac = c->mac.full;
	g.gbr = c->gbr; g.pr = c->pr; g.fpul = c->fpul;
	g.sr = c->sr; g.fpscr = c->fpscr;
}

static void loadRegs(Sh4Context *c, const Regs &g)
{
	memcpy(c->r, g.r, sizeof(g.r));
	memcpy(c->r_bank, g.r_bank, sizeof(g.r_bank));
	memcpy(c->fr, g.fr, sizeof(g.fr));
	memcpy(c->xf, g.xf, sizeof(g.xf));
	c->mac.full = g.mac;
	c->gbr = g.gbr; c->pr = g.pr; c->fpul = g.fpul;
	c->sr = g.sr; c->fpscr = g.fpscr;
	c->restoreHostRoundingMode();
}

static void compare(u32 stateFrames)
{
	u64 c1 = fnv(end1), c2 = fnv(end2);
	framesTested++;
	std::string details;
	u32 diffLines = 0;
	if (c1 != c2)
	{
		framesBad++;
		size_t o = 0;
		for (const Range &r : ranges)
		{
			if (!r.cmp)
			{
				o += r.hi - r.lo;
				continue;
			}
			for (u32 a = r.lo, n = 0; a < r.hi; a += n, o += n)
			{
				n = std::min<u32>(32, r.hi - a);
				if (memcmp(&end1[o], &end2[o], n) != 0)
				{
					if (diffLines++ < 24)
					{
						u32 k = 0;
						while (end1[o + k] == end2[o + k])
							k++;
						char buf[64];
						snprintf(buf, sizeof(buf), "%08x:%02x->%02x;", a + k, end1[o + k], end2[o + k]);
						details += buf;
					}
				}
			}
		}
	}
	if (logFile)
	{
		fprintf(logFile, "%u,%u,%u,%s,%u,%016llx,%016llx,%u-%u,%u-%u,%s\n", framesTested, stateFrames, ram32(0x8c25b784), c1 == c2 ? "OK" : "DIFF",
				diffLines, (unsigned long long)c1, (unsigned long long)c2, vs1Start, vs1End, vs2Start, vs2End, details.c_str());
		if (framesTested % 60 == 0)
			fflush(logFile);
	}
	if (c1 != c2 && dumped < maxDumps)
	{
		dumped++;
		const std::vector<u8> *bufs[3] = { &snap, &end1, &end2 };
		const char *names[3] = { "snap", "end1", "end2" };
		for (int i = 0; i < 3; i++)
			if (FILE *f = fopen((dumpDir + "/sync_" + std::to_string(framesTested) + "_" + names[i] + ".bin").c_str(), "wb"))
			{
				fwrite(bufs[i]->data(), 1, bufs[i]->size(), f);
				fclose(f);
			}
	}
	if (framesTested % 60 == 0 || framesTested == maxFrames)
		fprintf(stderr, "SC synctest: %u frames tested, %u mismatched\n", framesTested, framesBad);
	if (framesTested == maxFrames && logFile)
		fflush(logFile);
}

// returns true when it changed ctx->pc
static bool hook(Sh4Context *ctx)
{
	static const bool pcScan = getenv("SC_PCSCAN") != nullptr;
	if (pcScan)
	{
		// record 4 KB pages of executed code outside 1ST_READ.BIN (0x8c010000-0x8c245620)
		static std::map<u32, u32> pages;
		u32 a = ctx->pc;
		if ((a < 0x8c010000 || a >= 0x8c245620) && pages[a & ~0xfffu]++ == 0)
			fprintf(stderr, "SC pcscan: new code page %08x (first pc %08x, pr %08x, mode %u)\n", a & ~0xfffu, a, ctx->pr,
					*(u32 *)&mem_b[0x8c24a584 & RAM_MASK]);
	}
	if (rngSplit && cosmeticRng(ctx))
		return true;
	const u32 pc = ctx->pc;
	if (pc == READ_TIMER && fakeTimer)
	{
		ctx->r[0] = timerCounter++ * 0x9E3779B1u;
		ctx->pc = ctx->pr;
		return true;
	}
	if (!initDone)
		init();
	if (pc == READ_PADS)
	{
		if (autoplay)
			autoplayPad();
		if (state == 1)
			pds.assign(ram(PDS_ADDR), ram(PDS_ADDR) + PDS_SIZE);
		else if (state == 2 && pds.size() == PDS_SIZE)
			memcpy(ram(PDS_ADDR), pds.data(), PDS_SIZE);
		return false;
	}
	if (pc == LOOP_TOP)
	{
		loopCount++;
		widescreenAndShots();
	}
	if (!synctest || pc != LOOP_TOP || ranges.empty())
		return false;
	if (state == 1)
	{
		vs1End = ram32(0x8c24a904);
		copyOut(end1);
		copyIn(snap);
		loadRegs(ctx, snapRegs);
		if (leakDumps < maxDumps && !fullStart.empty())
			reportLeaks(framesTested + 1), leakDumps++;
		timerCounter = snapTimerCounter;
		vs2Start = ram32(0x8c24a904);
		if (evActive == 0)
			startEvents(1);
		state = 2;
		return false;
	}
	if (state == 2)
	{
		vs2End = ram32(0x8c24a904);
		copyOut(end2);
		bool recorded = evActive == 1;
		evActive = -1;
		compare(ram32(0x8c25b788));
		if (recorded && fnv(end1) != fnv(end2) && evDumps < maxDumps)
			reportEvents(framesTested), evDumps++;
		state = 0;
	}
	static const bool anyState = getenv("SC_SYNC_ANYSTATE") != nullptr;
	if (state == 0 && framesTested < maxFrames && ram32(0x8c24a584) == 1 && (anyState || ram32(0x8c25b784) == 1))
	{
		copyOut(snap);
		saveRegs(ctx, snapRegs);
		if (evDumps < maxDumps)
			startEvents(0);
		if (leakDumps < maxDumps)
			fullStart.assign(&mem_b[0], &mem_b[0] + RAM_MASK + 1);
		snapTimerCounter = timerCounter;
		vs1Start = ram32(0x8c24a904);
		state = 1;
	}
	return false;
}
}	// namespace scsync
// ---- end SC SyncTest ----

u16 Sh4Interpreter::ReadNexOp()'''

p = root / "core/hw/sh4/interpr/sh4_interpreter.cpp"
s = p.read_text()
if "SC SyncTest" not in s:
    assert s.count("\nu16 Sh4Interpreter::ReadNexOp()") == 1
    s = s.replace("\nu16 Sh4Interpreter::ReadNexOp()", HOOK, 1)
    old = "\tu32 addr = ctx->pc;\n\tif (!mmu_enabled() && (addr & 1))"
    assert s.count(old) == 1
    s = s.replace(old, "\tu32 addr = ctx->pc;\n\tif (scsync::enabled && scsync::hook(ctx))\n\t\taddr = ctx->pc;\n\tif (!mmu_enabled() && (addr & 1))", 1)
    p.write_text(s)
    print("patched", p)
