"""Generate a C test harness from oracle_calls.jsonl (recorded by the M0 probe in Flycast).

usage: python -I gen_harness.py <oracle_calls.jsonl> <out_tests.c>
Each recorded call becomes a test: load the entry state (registers, flags, memory ranges,
fake-timer counter), run f_<fn>(), compare against the recorded exit state.
"""
import json
import sys

FIELDS = ["pr", "mach", "macl", "gbr", "fpul"]


def hexw(s):
    return f"0x{s}u"


def main():
    src, outp = sys.argv[1], sys.argv[2]
    calls = [json.loads(line) for line in open(src, encoding="utf-8")]
    fns = sorted({c["fn"] for c in calls})
    o = ['#include "sh4rt.h"', "#include <stdio.h>", "#include <stdlib.h>", ""]
    for fn in fns:
        o.append(f"void f_{fn}(SH4 *c);")
    o += ["", "typedef struct { u32 addr, len; const char *hex; } Mem;",
          "typedef struct { u32 r[16]; u32 pr, mach, macl, gbr, fpul, t, s, q, m, timer; int nmem; Mem mem[2]; } St;",
          "typedef struct { u32 fn; St in, out; } Test;", "", "static const Test tests[] = {"]

    def st(s):
        mems = s["mem"][:2]
        mem_items = ", ".join(f'{{ 0x{m[0]}u, {len(m[1]) // 2}u, "{m[1]}" }}' for m in mems) or "{ 0, 0, 0 }"
        return ("{ { " + ", ".join(hexw(v) for v in s["r"]) + " }, "
                + ", ".join(hexw(s[f]) for f in FIELDS)
                + f", {s['t']}, {s['s']}, {s['q']}, {s['m']}, {hexw(s['timer'])}, {len(mems)}, {{ {mem_items} }} }}")

    for c in calls:
        o.append(f"\t{{ 0x{c['fn']}u, {st(c['in'])}, {st(c['out'])} }},")
    o += ["};", "", "static void (*fn_ptr(u32 a))(SH4 *)", "{"]
    for fn in fns:
        o.append(f"\tif (a == 0x{fn}u) return f_{fn};")
    o += ["\treturn 0;", "}", ""]
    o.append(HARNESS)
    open(outp, "w").write("\n".join(o))
    print(f"wrote {outp}: {len(calls)} tests for {fns}")


HARNESS = r'''
u8 *sh4_ram;
static u32 fake_timer;

u32 sh4_hw_read(u32 addr, int size) { fprintf(stderr, "unexpected hw read %08x/%d\n", addr, size); exit(2); }
void sh4_hw_write(u32 addr, u32 v, int size) { fprintf(stderr, "unexpected hw write %08x=%x/%d\n", addr, v, size); exit(2); }
void sh4_pref(SH4 *c, u32 a) { (void)c; (void)a; }
void sh4_unimpl(SH4 *c, u32 a, u32 op) { (void)c; fprintf(stderr, "unimplemented %04x at %08x\n", op, a); exit(2); }
void sh4_jump(SH4 *c, u32 a) { (void)c; fprintf(stderr, "unexpected jump %08x\n", a); exit(2); }
void sh4_call(SH4 *c, u32 a)
{
	if (a == 0x8c220060u)	/* sh4_ReadTimer, answered like the SyncTest fake timer */
	{
		c->r[0] = fake_timer++ * 0x9E3779B1u;
		return;
	}
	void (*f)(SH4 *) = fn_ptr(a);
	if (f) { f(c); return; }
	fprintf(stderr, "unexpected call %08x\n", a);
	exit(2);
}

static void load_mem(const Mem *m)
{
	for (u32 i = 0; i < m->len; i++)
	{
		unsigned v;
		sscanf(m->hex + 2 * i, "%2x", &v);
		sh4_ram[(m->addr + i) & 0xffffff] = (u8)v;
	}
}

static int check_mem(const Mem *m)
{
	for (u32 i = 0; i < m->len; i++)
	{
		unsigned v;
		sscanf(m->hex + 2 * i, "%2x", &v);
		if (sh4_ram[(m->addr + i) & 0xffffff] != (u8)v)
			return (int)i + 1;
	}
	return 0;
}

int main(void)
{
	sh4_ram = calloc(1, 16 << 20);
	int pass = 0, fail = 0;
	int ntests = sizeof(tests) / sizeof(tests[0]);
	for (int k = 0; k < ntests; k++)
	{
		const Test *t = &tests[k];
		SH4 c;
		memset(&c, 0, sizeof(c));
		memcpy(c.r, t->in.r, sizeof(c.r));
		c.pr = t->in.pr; c.mach = t->in.mach; c.macl = t->in.macl; c.gbr = t->in.gbr; c.fpul = t->in.fpul;
		c.T = t->in.t; c.S = t->in.s; c.Q = t->in.q; c.M = t->in.m;
		fake_timer = t->in.timer;
		for (int i = 0; i < t->in.nmem; i++)
			load_mem(&t->in.mem[i]);
		fn_ptr(t->fn)(&c);
		char why[160] = "";
		for (int i = 0; i < 16 && !why[0]; i++)
			if (c.r[i] != t->out.r[i]) snprintf(why, sizeof(why), "r%d %08x != %08x", i, c.r[i], t->out.r[i]);
		if (!why[0] && c.pr != t->out.pr) snprintf(why, sizeof(why), "pr %08x != %08x", c.pr, t->out.pr);
		if (!why[0] && (c.mach != t->out.mach || c.macl != t->out.macl)) snprintf(why, sizeof(why), "mac");
		if (!why[0] && c.gbr != t->out.gbr) snprintf(why, sizeof(why), "gbr");
		if (!why[0] && c.T != t->out.t) snprintf(why, sizeof(why), "T %u != %u", c.T, t->out.t);
		if (!why[0] && (c.S != t->out.s || c.Q != t->out.q || c.M != t->out.m))
			snprintf(why, sizeof(why), "S/Q/M %u%u%u != %u%u%u", c.S, c.Q, c.M, t->out.s, t->out.q, t->out.m);
		if (!why[0] && fake_timer != t->out.timer) snprintf(why, sizeof(why), "timer calls %u != %u", fake_timer, t->out.timer);
		for (int i = 0; i < t->out.nmem && !why[0]; i++)
		{
			int bad = check_mem(&t->out.mem[i]);
			if (bad) snprintf(why, sizeof(why), "mem %08x+%d", t->out.mem[i].addr, bad - 1);
		}
		if (why[0])
		{
			if (fail < 20) printf("FAIL test %d fn %08x: %s\n", k, t->fn, why);
			fail++;
		}
		else
			pass++;
	}
	printf("%d passed, %d failed of %d\n", pass, fail, ntests);
	return fail != 0;
}
'''

if __name__ == "__main__":
    main()
