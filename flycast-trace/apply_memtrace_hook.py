"""Adds the Soulcalibur memory-footprint trace to a Flycast v2.7 checkout (on top of the HUD hook).

usage (inside WSL): python3 apply_memtrace_hook.py ~/sc-flycast

Wraps ReadMem8/16/32/64 and WriteMem8/16/32/64 (set in SetMemoryHandlers). Active only when
SC_MEMTRACE is set (which also forces the interpreter through SC_TRACE logic in emulator.cpp:
run with SC_TRACE=1 as well). While armed it records:
  - RAM writes: per 32-byte line, byte mask written, frames in which the line was written,
    first and last writer PC (ctx.pc - 2) and PR
  - non-RAM reads: address, PC, count, last value (hardware registers, e.g. SH4 timers)
  - RAM reads: per 32-byte line, a read bitmap (to spot state read but never written)
Arming: g_battle_state (0x8c25b784) == 1 and battle-state frame counter (0x8c25b788) in
[SC_MEM_FROM, SC_MEM_TO). Output written once the window ends, into SC_MEM_DIR:
  mem_writes.csv, mem_hwreads.csv, mem_reads.csv
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).expanduser()

HOOK = r'''
// ---- SC memory-footprint trace (Soulcalibur reverse-engineering project) ----
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "hw/sh4/sh4_if.h"

namespace sctrace
{
void (*evHook)(int type, u32 addr, u32 value);	// set by the SyncTest; type 0 = read, 1 = write
static ReadMem8Func oR8;
static ReadMem16Func oR16;
static ReadMem32Func oR32;
static ReadMem64Func oR64;
static WriteMem8Func oW8;
static WriteMem16Func oW16;
static WriteMem32Func oW32;
static WriteMem64Func oW64;

struct Line
{
	u32 mask = 0;		// bytes written in this 32-byte line
	u32 frames = 0;		// distinct frames written
	u32 lastFrame = ~0u;
	u32 firstPc = 0, firstPr = 0, lastPc = 0, lastPr = 0;
	u32 writes = 0;
};
static std::vector<Line> lines;
static std::vector<u8> readLines;
struct HwKey
{
	u32 addr, pc, pr, stackTop;
	bool operator<(const HwKey &o) const
	{
		return addr != o.addr ? addr < o.addr : pc != o.pc ? pc < o.pc : pr != o.pr ? pr < o.pr : stackTop < o.stackTop;
	}
};
static std::map<HwKey, std::pair<u32, u32>> hwReads;	// -> (count, last value)
static std::map<std::pair<u32, u32>, u32> rngCalls;	// (state addr, caller) -> count
static bool done;
static u32 from = 120, to = 720;

static u32 ram32(u32 addr)
{
	return *(u32 *)&mem_b[addr & RAM_MASK & ~3u];
}

static bool armed(u32 &frame)
{
	if (done)
		return false;
	if (ram32(0x8c24a584) != 1 || ram32(0x8c25b784) != 1)	// GAME MODE, fight state
		return false;
	frame = ram32(0x8c25b788);
	return frame >= from && frame < to;
}

static void dump();

static void maybeFinish()
{
	if (!done && !lines.empty() && ram32(0x8c24a584) == 1 && ram32(0x8c25b784) == 1 && ram32(0x8c25b788) >= to)
		dump();
}

static bool isRam(u32 addr)
{
	return ((addr >> 26) & 7) == 3 && (addr >> 29) != 7;
}

static void noteWrite(u32 addr, u32 size)
{
	u32 frame;
	if (!armed(frame))
	{
		maybeFinish();
		return;
	}
	if (!isRam(addr))
		return;
	if (lines.empty())
		lines.resize((RAM_MASK + 1) / 32);
	u32 off = addr & RAM_MASK;
	Line &l = lines[off >> 5];
	const Sh4Context &ctx = p_sh4rcb->cntx;
	for (u32 i = 0; i < size && ((off & 31) + i) < 32; i++)
		l.mask |= 1u << ((off & 31) + i);
	if (l.lastFrame != frame)
	{
		l.frames++;
		l.lastFrame = frame;
	}
	if (l.writes++ == 0)
	{
		l.firstPc = ctx.pc - 2;
		l.firstPr = ctx.pr;
	}
	l.lastPc = ctx.pc - 2;
	l.lastPr = ctx.pr;
	// RNG state writes: attribute to the caller of the RNG function
	if (off == 0x159060)		// LFSR (rng_next15 / rng_next_float push PR first)
		rngCalls[{0x8c159060, ram32(ctx.r[15])}]++;
	else if (off == 0x21aecc)	// rand(), a leaf
		rngCalls[{0x8c21aecc, ctx.pr}]++;
}

static void noteRead(u32 addr, u64 value)
{
	u32 frame;
	if (!armed(frame))
		return;
	if (isRam(addr))
	{
		if (readLines.empty())
			readLines.resize((RAM_MASK + 1) / 32);
		readLines[(addr & RAM_MASK) >> 5] = 1;
		return;
	}
	const Sh4Context &ctx = p_sh4rcb->cntx;
	auto &e = hwReads[{addr, ctx.pc - 2, ctx.pr, ram32(ctx.r[15])}];
	e.first++;
	e.second = (u32)value;
}

static u8 DYNACALL r8(u32 a) { u8 v = oR8(a); if (evHook) evHook(0, a, v); noteRead(a, v); return v; }
static u16 DYNACALL r16(u32 a) { u16 v = oR16(a); if (evHook) evHook(0, a, v); noteRead(a, v); return v; }
static u32 DYNACALL r32(u32 a) { u32 v = oR32(a); if (evHook) evHook(0, a, v); noteRead(a, v); return v; }
static u64 DYNACALL r64(u32 a) { u64 v = oR64(a); if (evHook) { evHook(0, a, (u32)v); evHook(0, a + 4, (u32)(v >> 32)); } noteRead(a, v); return v; }
static void DYNACALL w8(u32 a, u8 d) { if (evHook) evHook(1, a, d); noteWrite(a, 1); oW8(a, d); }
static void DYNACALL w16(u32 a, u16 d) { if (evHook) evHook(1, a, d); noteWrite(a, 2); oW16(a, d); }
static void DYNACALL w32(u32 a, u32 d) { if (evHook) evHook(1, a, d); noteWrite(a, 4); oW32(a, d); }
static void DYNACALL w64(u32 a, u64 d) { if (evHook) { evHook(1, a, (u32)d); evHook(1, a + 4, (u32)(d >> 32)); } noteWrite(a, 8); oW64(a, d); }

static void dump()
{
	done = true;
	const char *dirEnv = getenv("SC_MEM_DIR");
	std::string dir = dirEnv ? dirEnv : (getenv("HOME") ? getenv("HOME") : ".");
	if (FILE *f = fopen((dir + "/mem_writes.csv").c_str(), "w"))
	{
		fputs("line,mask,frames,writes,first_pc,first_pr,last_pc,last_pr\n", f);
		for (size_t i = 0; i < lines.size(); i++)
		{
			const Line &l = lines[i];
			if (l.writes)
				fprintf(f, "%08x,%08x,%u,%u,%08x,%08x,%08x,%08x\n", (u32)(0x8c000000 + i * 32), l.mask, l.frames,
						l.writes, l.firstPc, l.firstPr, l.lastPc, l.lastPr);
		}
		fclose(f);
	}
	if (FILE *f = fopen((dir + "/mem_hwreads.csv").c_str(), "w"))
	{
		fputs("addr,pc,pr,stack_top,count,last_value\n", f);
		for (const auto &kv : hwReads)
			fprintf(f, "%08x,%08x,%08x,%08x,%u,%08x\n", kv.first.addr, kv.first.pc, kv.first.pr, kv.first.stackTop,
					kv.second.first, kv.second.second);
		fclose(f);
	}
	if (FILE *f = fopen((dir + "/mem_rng.csv").c_str(), "w"))
	{
		fputs("state,caller,count\n", f);
		for (const auto &kv : rngCalls)
			fprintf(f, "%08x,%08x,%u\n", kv.first.first, kv.first.second, kv.second);
		fclose(f);
	}
	if (FILE *f = fopen((dir + "/mem_reads.csv").c_str(), "w"))
	{
		fputs("line\n", f);
		for (size_t i = 0; i < readLines.size(); i++)
			if (readLines[i])
				fprintf(f, "%08x\n", (u32)(0x8c000000 + i * 32));
		fclose(f);
	}
	fprintf(stderr, "SC memtrace: written to %s\n", dir.c_str());
}

static void install()
{
	if (getenv("SC_MEMTRACE") == nullptr)
		return;
	if (getenv("SC_MEM_FROM"))
		from = strtoul(getenv("SC_MEM_FROM"), nullptr, 0);
	if (getenv("SC_MEM_TO"))
		to = strtoul(getenv("SC_MEM_TO"), nullptr, 0);
	oR8 = ReadMem8; oR16 = ReadMem16; oR32 = ReadMem32; oR64 = ReadMem64;
	oW8 = WriteMem8; oW16 = WriteMem16; oW32 = WriteMem32; oW64 = WriteMem64;
	ReadMem8 = &r8; ReadMem16 = &r16; ReadMem32 = &r32; ReadMem64 = &r64;
	WriteMem8 = &w8; WriteMem16 = &w16; WriteMem32 = &w32; WriteMem64 = &w64;
}

// called from the store-queue hook so SQ flushes to RAM count as writes too
void noteSqWrite(u32 ramAddr)
{
	if (getenv("SC_MEMTRACE") != nullptr)
		noteWrite(ramAddr, 32);
}
}	// namespace sctrace

static void SetMemoryHandlers_sc();
void SetMemoryHandlers()
{
	SetMemoryHandlers_sc();
	sctrace::install();
}
// ---- end SC memory-footprint trace ----

static void SetMemoryHandlers_sc()'''

mem = root / "core/hw/sh4/sh4_mem.cpp"
s = mem.read_text()
if "SC memory-footprint trace" not in s:
    assert s.count("\nvoid SetMemoryHandlers()") == 1
    s = s.replace("\nvoid SetMemoryHandlers()", HOOK, 1)
    mem.write_text(s)
    print("patched", mem)

# feed store-queue writes to RAM into the footprint too
sq = root / "core/hw/sh4/storeq.cpp"
q = sq.read_text()
if "noteSqWrite" not in q:
    assert "\tscOrigSqWrite(dest, ctx);\n}" in q, "apply_trace_hook.py must run first"
    q = q.replace("// ---- SC HUD trace", "namespace sctrace { void noteSqWrite(u32 ramAddr); }\n// ---- SC HUD trace", 1)
    q = q.replace("\tscOrigSqWrite(dest, ctx);\n}",
                  "\tif ((CCN_QACR_TR[0] >> 26 & 7) == 3)\n\t\tsctrace::noteSqWrite(0x8c000000 | ((CCN_QACR_TR[0] + (dest & ~0x1fu)) & 0x00ffffe0));\n"
                  "\tscOrigSqWrite(dest, ctx);\n}", 1)
    sq.write_text(q)
    print("patched", sq)
