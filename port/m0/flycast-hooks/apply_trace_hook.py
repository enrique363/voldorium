"""Adds the Soulcalibur HUD caller trace to a Flycast v2.7 checkout.

usage (inside WSL): python3 apply_trace_hook.py ~/sc-flycast

What it does
  core/hw/sh4/storeq.cpp : wraps ctx.doSqWrite. While armed, every store-queue flush is logged
                           with the SH4 PC of the `pref`, PR (caller return address), the
                           destination and the 32 bytes written.
  core/emulator.cpp      : when SC_TRACE is set, forces the SH4 interpreter (the dynarec does
                           not keep ctx.pc / ctx.pr current at the call).

Arming (all read from game RAM, Soulcalibur USA T1401N V1.000):
  0x8c25b784 g_battle_state == 1 (fight in progress)
  0x8c25b788 frames in current battle state in [SC_TRACE_FROM, SC_TRACE_FROM + SC_TRACE_FRAMES)
Environment:
  SC_TRACE=1            enable
  SC_TRACE_FILE=path    output CSV (default ~/sc_hud_trace.csv)
  SC_TRACE_FROM=120     first battle-state frame to capture (2 s into the round)
  SC_TRACE_FRAMES=3     number of frames
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).expanduser()

STOREQ_HOOK = r'''
// ---- SC HUD trace (Soulcalibur reverse-engineering project) ----
#include <chrono>
#include <cstdio>
#include <cstdlib>

static SQWriteFunc *scOrigSqWrite;
static FILE *scTraceFile;
static long scTraceLines;

static u32 scRead32(u32 addr)
{
	return *(u32 *)&mem_b[addr & RAM_MASK & ~3u];
}

static void scHeartbeat()
{
	static const bool on = getenv("SC_HEARTBEAT") != nullptr;
	if (!on)
		return;
	static auto last = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	if (now - last < std::chrono::seconds(2))
		return;
	last = now;
	fprintf(stderr, "SC heartbeat: vsync=%u mode=%u battle_state=%u state_frames=%u\n", scRead32(0x8c24a904),
			scRead32(0x8c24a584), scRead32(0x8c25b784), scRead32(0x8c25b788));
}

static void DYNACALL scTraceSqWrite(u32 dest, Sh4Context *ctx)
{
	scHeartbeat();
	static const bool enabled = getenv("SC_TRACE") != nullptr;
	if (enabled)
	{
		static const u32 from = getenv("SC_TRACE_FROM") ? strtoul(getenv("SC_TRACE_FROM"), nullptr, 0) : 120;
		static const u32 frames = getenv("SC_TRACE_FRAMES") ? strtoul(getenv("SC_TRACE_FRAMES"), nullptr, 0) : 3;
		u32 battleState = scRead32(0x8c25b784);
		u32 stateFrames = scRead32(0x8c25b788);
		if (scRead32(0x8c24a584) == 1 && battleState == 1 && stateFrames >= from && stateFrames < from + frames && scTraceLines < 500000)
		{
			if (scTraceFile == nullptr)
			{
				const char *path = getenv("SC_TRACE_FILE");
				std::string def = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/sc_hud_trace.csv";
				scTraceFile = fopen(path ? path : def.c_str(), "w");
				if (scTraceFile != nullptr)
					fputs("frame,pc,pr,r14,r15,dest,w0,w1,w2,w3,w4,w5,w6,w7\n", scTraceFile);
			}
			if (scTraceFile != nullptr)
			{
				const u32 *w = (const u32 *)&ctx->sq_buffer[(dest >> 5) & 1];
				fprintf(scTraceFile, "%u,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
						stateFrames, ctx->pc - 2, ctx->pr, ctx->r[14], ctx->r[15], dest,
						w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
				if (++scTraceLines % 4096 == 0)
					fflush(scTraceFile);
			}
		}
		else if (scTraceFile != nullptr && stateFrames >= from + frames)
		{
			fflush(scTraceFile);
		}
	}
	scOrigSqWrite(dest, ctx);
}
// ---- end SC HUD trace ----

void setSqwHandler()'''

storeq = root / "core/hw/sh4/storeq.cpp"
s = storeq.read_text()
if "SC HUD trace" not in s:
    assert s.count("\nvoid setSqwHandler()") == 1
    s = s.replace("\nvoid setSqwHandler()", STOREQ_HOOK, 1)
    # install the wrapper after the original handler is chosen (end of setSqwHandler)
    tail = "\t\tdefault:\n\t\t\tctx.doSqWrite = &sqWrite<false>;\n\t\t\tbreak;\n\t\t}\n\t}\n"
    assert s.count(tail) == 1, "setSqwHandler layout changed"
    s = s.replace(tail, tail + "\tscOrigSqWrite = ctx.doSqWrite;\n\tctx.doSqWrite = &scTraceSqWrite;\n", 1)
    if "#include <string>" not in s:
        s = s.replace('#include "sh4_if.h"', '#include "sh4_if.h"\n#include <string>', 1)
    storeq.write_text(s)
    print("patched", storeq)

emu = root / "core/emulator.cpp"
e = emu.read_text()
anchor = "\t// Reload per-game settings\n\tconfig::Settings::instance().load(true);\n"
if "SC_TRACE" not in e:
    assert e.count(anchor) == 1, "emulator.cpp layout changed"
    e = e.replace(anchor, anchor + "\tif (getenv(\"SC_TRACE\") != nullptr)\n\t\tconfig::DynarecEnabled.override(false);\t// SC HUD trace needs accurate PC/PR\n", 1)
    if "#include <cstdlib>" not in e:
        e = "#include <cstdlib>\n" + e
    emu.write_text(e)
    print("patched", emu)
