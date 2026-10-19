"""Call probe for validating translated functions (M0). Apply after apply_synctest_hook.py.

usage (inside WSL): python3 apply_probe_hook.py ~/sc-flycast-m0

Environment:
  SC_PROBE=0x8c0639c8,0x8c0fed6c        functions to record (entry addresses)
  SC_PROBE_FILE=path                     more entries, one hex address per line
  SC_PROBE_MEM=0x8c0fed6c:r4:r6,0x8c0639c8:0x8c159060:4
                                         memory captured on entry and exit: base:length, each a
                                         register (r0..r15) or a constant; length capped at 4096
  SC_PROBE_MAX=200                       calls recorded per function
  SC_PROBE_OUT=path                      JSON lines output
  SC_PROBE_ACCESS=1                      record every memory access made during each call:
                                         "rd" = first value read from each RAM byte (the call's
                                         inputs), "wr" = final value of each RAM byte written,
                                         "hw" = ordered non-RAM accesses [type 0 read / 1 write / 2 store-queue
                                         flush, addr, size, value (flush: destination address)]
  SC_PROBE_ARM=1                         only start recording calls during a fight (GAME MODE,
                                         battle state 1, battle frame >= 120)
Each line: {"fn","in":{r,fr,xf,pr,t,s,q,m,sr,fpscr,fpul,mach,macl,gbr,timer,mem},"out":{same}[,"rd","wr","hw"]}
or, for a call that cannot be replayed, {"fn","skip":reason}: an interrupt was taken, the
SyncTest hook changed RAM behind the CPU (pad injection, loop top), or the access log overflowed.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).expanduser()

HOOK = r'''
// ---- SC call probe (M0 translator validation) ----
#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_map>
#include "hw/pvr/pvr_mem.h"
#include "hw/aica/aica_if.h"
namespace sctrace { extern void (*accHook)(int type, u32 addr, u64 value, int size); }
namespace scprobe
{
static const bool enabled = getenv("SC_PROBE") != nullptr || getenv("SC_PROBE_FILE") != nullptr;
static const bool access = getenv("SC_PROBE_ACCESS") != nullptr;
static const bool arm = getenv("SC_PROBE_ARM") != nullptr;
static const u32 MAX_BYTES = 65536, MAX_HW = 4096;
struct MemSpec { int baseReg, lenReg; u32 base, len, mul; };
struct Hw { u32 type, addr, size, value; };
struct Active
{
	u32 fn, pr, r15;
	std::string in;
	std::vector<std::pair<u32, u32>> mem;
	std::unordered_map<u32, u8> rd, wr;
	std::vector<Hw> hw;
	const char *skip = nullptr;
};
static std::multimap<u32, MemSpec> memSpecs;
static std::map<u32, u32> counts;
static std::vector<u8> isEntry;	// one flag per RAM halfword: fast per-instruction test
static std::vector<Active> stack;
static u32 maxCalls = 200;
static FILE *out;
static bool initDone;

static int parseReg(const std::string &t, u32 &val)
{
	if (!t.empty() && t[0] == 'r')
		return (int)strtoul(t.c_str() + 1, nullptr, 10);
	val = strtoul(t.c_str(), nullptr, 0);
	return -1;
}

static void onAccess(int type, u32 addr, u64 value, int size)
{
	// SC_LOG_DMA=1: print every ch2 DMA start (destination, length, source as last written)
	static const bool logDma = getenv("SC_LOG_DMA") != nullptr;
	if (logDma && type == 1)
	{
		static u32 dst, len, src;
		const u32 p = addr & 0x1fffffff;
		if (p == 0x005f6800) dst = (u32)value;
		else if (p == 0x005f6804) len = (u32)value;
		else if (addr == 0xffa00020) src = (u32)value;
		else if (p == 0x005f8050 || p == 0x005f8054 || p == 0x005f8060 || p == 0x005f8064 || p == 0x005f8124
				 || p == 0x005f8128 || p == 0x005f8020 || p == 0x005f8048 || p == 0x005f804c || p == 0x005f805c
				 || p == 0x005f8044)
		{
			static std::map<u32, u32> last;
			if (last.find(p) == last.end() || last[p] != (u32)value)
				fprintf(stderr, "SC reg vsync %u: %08x = %08x\n", *(u32 *)&mem_b[0x8c24a904 & RAM_MASK], p, (u32)value);
			last[p] = (u32)value;
		}
		else if (p == 0x005f6808 && (value & 1))
			fprintf(stderr, "SC dma ch2 vsync %u: %08x -> %08x, %u bytes, pr %08x\n",
					*(u32 *)&mem_b[0x8c24a904 & RAM_MASK], src, dst, len, p_sh4rcb->cntx.pr);
	}
	if (stack.empty())
		return;
	if (type == 2)
	{
		// store-queue flush (pref): addr = SQ address, value = external destination. A flush to
		// RAM writes the 32-byte SQ buffer behind the memory handlers, so add those bytes here.
		const u32 ext = (u32)value;
		u8 sq[32];
		memcpy(sq, &p_sh4rcb->cntx.sq_buffer[(addr >> 5) & 1], 32);
		for (Active &a : stack)
		{
			if (a.skip)
				continue;
			a.hw.push_back({ 2, addr, 32, ext });
			if (((ext >> 26) & 7) == 3)
				for (u32 i = 0; i < 32; i++)
					a.wr[((ext & ~0x1fu) + i) & RAM_MASK] = sq[i];
		}
		return;
	}
	const bool ram = ((addr >> 26) & 7) == 3 && (addr >> 29) != 7;
	for (Active &a : stack)
	{
		if (a.skip)
			continue;
		if (!ram)
		{
			if (size == 8)
			{
				a.hw.push_back({ (u32)type, addr, 4, (u32)value });
				a.hw.push_back({ (u32)type, addr + 4, 4, (u32)(value >> 32) });
			}
			else
				a.hw.push_back({ (u32)type, addr, (u32)size, (u32)value });
			if (a.hw.size() > MAX_HW)
				a.skip = "too many hardware accesses";
			continue;
		}
		const u32 off = addr & RAM_MASK;
		for (int i = 0; i < size; i++)
		{
			const u32 o = (off + i) & RAM_MASK;
			const u8 b = (u8)(value >> (8 * i));
			if (type == 1)
				a.wr[o] = b;
			else if (!a.wr.count(o))
				a.rd.emplace(o, b);	// keeps the first value read
		}
		if (a.rd.size() + a.wr.size() > MAX_BYTES)
			a.skip = "too many bytes accessed";
	}
}

static void init()
{
	initDone = true;
	for (const char *q = getenv("SC_PROBE"); q && *q; )
	{
		char *end;
		counts[strtoul(q, &end, 0)] = 0;
		q = *end ? end + 1 : end;
	}
	if (const char *f = getenv("SC_PROBE_FILE"))
	{
		std::ifstream in(f);
		std::string line;
		while (std::getline(in, line))
			if (!line.empty())
				counts[strtoul(line.c_str(), nullptr, 16)] = 0;
	}
	if (const char *m = getenv("SC_PROBE_MEM"))
	{
		std::string s(m);
		size_t p = 0;
		while (p < s.size())
		{
			size_t c = s.find(',', p);
			std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
			p = c == std::string::npos ? s.size() : c + 1;
			size_t a = item.find(':'), b = item.find(':', a + 1);
			if (a == std::string::npos || b == std::string::npos)
				continue;
			MemSpec ms{};
			ms.mul = 1;
			std::string lenTok = item.substr(b + 1);
			size_t star = lenTok.find('*');
			if (star != std::string::npos)
			{
				ms.mul = strtoul(lenTok.substr(star + 1).c_str(), nullptr, 0);
				lenTok = lenTok.substr(0, star);
			}
			ms.baseReg = parseReg(item.substr(a + 1, b - a - 1), ms.base);
			ms.lenReg = parseReg(lenTok, ms.len);
			memSpecs.insert({ (u32)strtoul(item.substr(0, a).c_str(), nullptr, 0), ms });
		}
	}
	if (getenv("SC_PROBE_MAX"))
		maxCalls = strtoul(getenv("SC_PROBE_MAX"), nullptr, 0);
	const char *o = getenv("SC_PROBE_OUT");
	out = fopen(o ? o : "/tmp/sc_probe.jsonl", "w");
	if (access)
		sctrace::accHook = &onAccess;
	isEntry.assign((RAM_MASK + 1) / 2, 0);
	for (const auto &e : counts)
		if (((e.first >> 26) & 7) == 3)
			isEntry[(e.first & RAM_MASK) >> 1] = 1;
	fprintf(stderr, "SC probe: %zu functions, max %u calls each, access log %s, output %s\n", counts.size(), maxCalls,
			access ? "on" : "off", out ? "open" : "NOT OPEN");
}

static std::string hex8(u32 v)
{
	char b[16];
	snprintf(b, sizeof(b), "\"%08x\"", v);
	return b;
}

static std::string state(const Sh4Context *c, const std::vector<std::pair<u32, u32>> &mem)
{
	std::string s = "{\"r\":[";
	for (int i = 0; i < 16; i++)
		s += (i ? "," : "") + hex8(c->r[i]);
	s += "],\"fr\":[";
	for (int i = 0; i < 16; i++)
	{
		u32 v;
		memcpy(&v, &c->fr[i], 4);
		s += (i ? "," : "") + hex8(v);
	}
	s += "],\"xf\":[";
	for (int i = 0; i < 16; i++)
	{
		u32 v;
		memcpy(&v, &c->xf[i], 4);
		s += (i ? "," : "") + hex8(v);
	}
	s += "],\"pr\":" + hex8(c->pr) + ",\"t\":" + std::to_string(c->sr.T) + ",\"s\":" + std::to_string(c->sr.S)
		+ ",\"q\":" + std::to_string(c->sr.Q) + ",\"m\":" + std::to_string(c->sr.M)
		+ ",\"sr\":" + hex8(c->sr.getFull()) + ",\"vbr\":" + hex8(c->vbr)
		+ ",\"fpscr\":" + hex8(c->fpscr.full) + ",\"fpul\":" + hex8(c->fpul)
		+ ",\"mach\":" + hex8(c->mac.h) + ",\"macl\":" + hex8(c->mac.l) + ",\"gbr\":" + hex8(c->gbr)
		+ ",\"timer\":" + hex8(scsync::timerCounter) + ",\"sq\":\"";
	{
		const u8 *sq = (const u8 *)&c->sq_buffer[0];
		for (int i = 0; i < 64; i++)
		{
			char b[4];
			snprintf(b, sizeof(b), "%02x", sq[i]);
			s += b;
		}
	}
	s += "\",\"mem\":[";
	bool first = true;
	for (const auto &m : mem)
	{
		s += first ? "[" : ",[";
		first = false;
		s += hex8(m.first) + ",\"";
		for (u32 i = 0; i < m.second; i++)
		{
			char b[4];
			snprintf(b, sizeof(b), "%02x", mem_b[(m.first + i) & RAM_MASK]);
			s += b;
		}
		s += "\"]";
	}
	return s + "]}";
}

// byte map -> [["offset","hex"],...] runs of consecutive RAM offsets
static std::string runs(const std::unordered_map<u32, u8> &m)
{
	std::vector<std::pair<u32, u8>> v(m.begin(), m.end());
	std::sort(v.begin(), v.end());
	std::string s = "[";
	for (size_t i = 0; i < v.size(); )
	{
		size_t j = i;
		s += (i ? ",[" : "[") + hex8(v[i].first) + ",\"";
		do
		{
			char b[4];
			snprintf(b, sizeof(b), "%02x", v[j].second);
			s += b;
			j++;
		} while (j < v.size() && v[j].first == v[j - 1].first + 1);
		s += "\"]";
		i = j;
	}
	return s + "]";
}

static void finish(const Active &a, const Sh4Context *c)
{
	if (!out)
		return;
	if (a.skip)
	{
		fprintf(out, "{\"fn\":\"%08x\",\"skip\":\"%s\"}\n", a.fn, a.skip);
		return;
	}
	std::string s = "{\"fn\":\"" + std::string(hex8(a.fn).substr(1, 8)) + "\",\"in\":" + a.in + ",\"out\":" + state(c, a.mem);
	if (access)
	{
		s += ",\"rd\":" + runs(a.rd) + ",\"wr\":" + runs(a.wr) + ",\"hw\":[";
		for (size_t i = 0; i < a.hw.size(); i++)
		{
			char b[64];
			snprintf(b, sizeof(b), "%s[%u,\"%08x\",%u,\"%08x\"]", i ? "," : "", a.hw[i].type, a.hw[i].addr, a.hw[i].size, a.hw[i].value);
			s += b;
		}
		s += "]";
	}
	fprintf(out, "%s}\n", s.c_str());
	fflush(out);
}

static bool armed()
{
	if (!arm)
		return true;
	auto r32 = [](u32 a) { return *(u32 *)&mem_b[a & RAM_MASK]; };
	return r32(0x8c24a584) == 1 && r32(0x8c25b784) == 1 && r32(0x8c25b788) >= 120;
}

// SC_VRAM_DUMP_AT="n1,n2,...": write all of VRAM to <SC_VRAM_DUMP_OUT>_<n>.bin when the
// game's vsync counter (8C24A904) reaches each n
static void vramDump()
{
	static std::vector<u32> at;
	static bool parsed;
	if (!parsed)
	{
		parsed = true;
		for (const char *q = getenv("SC_VRAM_DUMP_AT"); q && *q; )
		{
			char *end;
			at.push_back(strtoul(q, &end, 0));
			q = *end ? end + 1 : end;
		}
	}
	if (at.empty())
		return;
	const u32 vs = *(u32 *)&mem_b[0x8c24a904 & RAM_MASK];
	if (vs < at.front())
		return;
	const char *o = getenv("SC_VRAM_DUMP_OUT");
	std::string name = std::string(o ? o : "/tmp/vram") + "_" + std::to_string(at.front()) + ".bin";
	if (FILE *f = fopen(name.c_str(), "wb"))
	{
		fwrite(&vram[0], 1, VRAM_SIZE, f);
		fclose(f);
		fprintf(stderr, "SC probe: VRAM dumped to %s\n", name.c_str());
	}
	// SC_ARAM_DUMP=1: sound RAM too, to <SC_VRAM_DUMP_OUT>_<n>_aram.bin (the AICA driver's tables)
	if (getenv("SC_ARAM_DUMP"))
	{
		std::string an = std::string(o ? o : "/tmp/vram") + "_" + std::to_string(at.front()) + "_aram.bin";
		if (FILE *f = fopen(an.c_str(), "wb"))
		{
			fwrite(&aica::aica_ram[0], 1, 2 << 20, f);
			fclose(f);
			fprintf(stderr, "SC probe: sound RAM dumped to %s\n", an.c_str());
		}
	}
	at.erase(at.begin());
}

static void hook(Sh4Context *c)
{
	if (!initDone)
		init();
	vramDump();
	const u32 pc = c->pc;
	static u32 prevPc;	// previous instruction fetched (delay slots included)
	const u32 prev = prevPc;
	prevPc = pc;
	// returns first (innermost call)
	while (!stack.empty() && pc == stack.back().pr && c->r[15] == stack.back().r15)
	{
		finish(stack.back(), c);
		stack.pop_back();
	}
	if (!stack.empty())
	{
		// interrupt/exception entry, or the SyncTest hook about to write RAM directly
		// (pad injection at READ_PADS, widescreen/shots at LOOP_TOP): the calls in progress
		// cannot be replayed
		const char *why = nullptr;
		if (pc == c->vbr + 0x600 || pc == c->vbr + 0x100 || pc == c->vbr + 0x400)
			why = "interrupt or exception taken";
		else if (pc == 0x8c061a20 || pc == 0x8c03144e)
			why = "RAM changed by the SyncTest hook";
		if (why)
			for (Active &a : stack)
				if (!a.skip)
					a.skip = why;
	}
	if (((pc >> 26) & 7) != 3 || !isEntry[(pc & RAM_MASK) >> 1])
		return;
	// only calls (jsr/bsr/bsrf, just after their delay slot) and tail jumps (jmp/braf) open a
	// frame: some Ghidra entries are also loop labels reached by plain branches
	auto op16 = [](u32 a) { return *(u16 *)&mem_b[a & RAM_MASK]; };
	const u16 callOp = op16(c->pr - 4), jumpOp = op16(prev - 2);
	const bool viaCall = prev == c->pr - 2 && ((callOp & 0xf0ff) == 0x400b || (callOp & 0xf000) == 0xb000 || (callOp & 0xf0ff) == 0x0003);
	const bool viaJump = (jumpOp & 0xf0ff) == 0x402b || (jumpOp & 0xf0ff) == 0x0023;
	if (!viaCall && !viaJump)
		return;
	auto it = counts.find(pc);
	if (it == counts.end() || it->second >= maxCalls || !armed())
		return;
	if (++it->second >= maxCalls)
		isEntry[(pc & RAM_MASK) >> 1] = 0;	// done with this function
	Active a;
	a.fn = pc;
	a.pr = c->pr;
	a.r15 = c->r[15];
	auto range = memSpecs.equal_range(pc);
	for (auto ms = range.first; ms != range.second; ++ms)
	{
		u32 base = ms->second.baseReg >= 0 ? c->r[ms->second.baseReg] : ms->second.base;
		u32 len = (ms->second.lenReg >= 0 ? c->r[ms->second.lenReg] : ms->second.len) * ms->second.mul;
		if (len > 4096)
			len = 4096;
		if (((base >> 26) & 7) == 3)
			a.mem.push_back({ base, len });
	}
	a.in = state(c, a.mem);
	stack.push_back(std::move(a));
}
}	// namespace scprobe
// ---- end SC call probe ----

u16 Sh4Interpreter::ReadNexOp()'''

p = root / "core/hw/sh4/interpr/sh4_interpreter.cpp"
s = p.read_text()
if "SC call probe" not in s:
    assert s.count("\nu16 Sh4Interpreter::ReadNexOp()") == 1, "apply the synctest hook first"
    s = s.replace("\nu16 Sh4Interpreter::ReadNexOp()", HOOK, 1)
    old = "\tu32 addr = ctx->pc;\n\tif (scsync::enabled && scsync::hook(ctx))"
    assert s.count(old) == 1
    s = s.replace(old, "\tu32 addr = ctx->pc;\n\tif (scprobe::enabled)\n\t\tscprobe::hook(ctx);\n\tif (scsync::enabled && scsync::hook(ctx))", 1)
    p.write_text(s)
    print("patched", p)

# access hook in the memory wrappers of the memtrace hook (sh4_mem.cpp)
m = root / "core/hw/sh4/sh4_mem.cpp"
s = m.read_text()
if "accHook" not in s:
    decl = "void (*evHook)(int type, u32 addr, u32 value);"
    assert s.count(decl) == 1, "apply the memtrace hook first"
    s = s.replace(decl, decl + "\nvoid (*accHook)(int type, u32 addr, u64 value, int size);	// set by the call probe", 1)
    wrappers = {
        "static u8 DYNACALL r8(u32 a) { u8 v = oR8(a);": "static u8 DYNACALL r8(u32 a) { u8 v = oR8(a); if (accHook) accHook(0, a, v, 1);",
        "static u16 DYNACALL r16(u32 a) { u16 v = oR16(a);": "static u16 DYNACALL r16(u32 a) { u16 v = oR16(a); if (accHook) accHook(0, a, v, 2);",
        "static u32 DYNACALL r32(u32 a) { u32 v = oR32(a);": "static u32 DYNACALL r32(u32 a) { u32 v = oR32(a); if (accHook) accHook(0, a, v, 4);",
        "static u64 DYNACALL r64(u32 a) { u64 v = oR64(a);": "static u64 DYNACALL r64(u32 a) { u64 v = oR64(a); if (accHook) accHook(0, a, v, 8);",
        "static void DYNACALL w8(u32 a, u8 d) {": "static void DYNACALL w8(u32 a, u8 d) { if (accHook) accHook(1, a, d, 1);",
        "static void DYNACALL w16(u32 a, u16 d) {": "static void DYNACALL w16(u32 a, u16 d) { if (accHook) accHook(1, a, d, 2);",
        "static void DYNACALL w32(u32 a, u32 d) {": "static void DYNACALL w32(u32 a, u32 d) { if (accHook) accHook(1, a, d, 4);",
        "static void DYNACALL w64(u32 a, u64 d) {": "static void DYNACALL w64(u32 a, u64 d) { if (accHook) accHook(1, a, d, 8);",
    }
    for old, new in wrappers.items():
        assert s.count(old) == 1, old
        s = s.replace(old, new, 1)
    # install the wrappers for the probe too, with the memtrace bookkeeping off (done = true)
    old = 'if (getenv("SC_MEMTRACE") == nullptr)\n\t\treturn;'
    assert s.count(old) == 1
    s = s.replace(old, 'if (getenv("SC_MEMTRACE") == nullptr)\n\t{\n\t\tif (getenv("SC_PROBE_ACCESS") == nullptr)\n'
                  '\t\t\treturn;\n\t\tdone = true;\n\t}', 1)
    m.write_text(s)
    print("patched", m)

# store-queue flushes go around the memory handlers: report them to the probe
q = root / "core/hw/sh4/storeq.cpp"
s = q.read_text()
if "accHook" not in s:
    old = "namespace sctrace { void noteSqWrite(u32 ramAddr); }"
    assert s.count(old) == 1, "apply the trace hook first"
    s = s.replace(old, old + "\nnamespace sctrace { extern void (*accHook)(int type, u32 addr, u64 value, int size); }", 1)
    old = "\tscOrigSqWrite(dest, ctx);\n}"
    assert s.count(old) == 1
    s = s.replace(old, "\tif (sctrace::accHook)\n\t\tsctrace::accHook(2, dest, CCN_QACR_TR[0] + (dest & ~0x1fu), 32);\n" + old, 1)
    q.write_text(s)
    print("patched", q)
