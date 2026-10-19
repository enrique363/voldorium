"""Harness generator for FPU validation (extends gen_harness.py).

usage: python -I gen_harness_fpu.py <oracle.jsonl> <out_tests.c> <entry> [entry ...]
The entries are every translated function (for the call dispatcher), not only the probed ones.
Per test: RAM is the 1ST_READ.BIN image plus the recorded ranges; registers, FR/XF banks, FPSCR
(and the matching host rounding mode), FPUL and flags are loaded; the exit state is compared
bit for bit (integer + float registers, FPSCR, FPUL, flags, memory ranges, timer reads).
"""
import json
import sys

MAXMEM = 6


def main():
    src, outp, entries = sys.argv[1], sys.argv[2], sys.argv[3:]
    calls = [json.loads(line) for line in open(src, encoding="utf-8")]
    o = ['#include "sh4rt.h"', "#include <stdio.h>", "#include <stdlib.h>", ""]
    for e in entries:
        o.append(f"void f_{e}(SH4 *c);")
    o += ["", "typedef struct { u32 addr, len; const char *hex; } Mem;",
          "typedef struct { u32 r[16], fr[16], xf[16]; u32 pr, mach, macl, gbr, fpul, fpscr, t, s, q, m, timer;"
          f" int nmem; Mem mem[{MAXMEM}]; }} St;",
          "typedef struct { u32 fn; St in, out; } Test;", "", "static const Test tests[] = {"]

    def arr(v):
        return "{ " + ", ".join(f"0x{x}u" for x in v) + " }"

    def st(s):
        mems = s["mem"][:MAXMEM]
        mi = ", ".join(f'{{ 0x{m[0]}u, {len(m[1]) // 2}u, "{m[1]}" }}' for m in mems) or "{ 0, 0, 0 }"
        return (f"{{ {arr(s['r'])}, {arr(s['fr'])}, {arr(s['xf'])}, 0x{s['pr']}u, 0x{s['mach']}u, 0x{s['macl']}u, "
                f"0x{s['gbr']}u, 0x{s['fpul']}u, 0x{s['fpscr']}u, {s['t']}, {s['s']}, {s['q']}, {s['m']}, "
                f"0x{s['timer']}u, {len(mems)}, {{ {mi} }} }}")

    for c in calls:
        o.append(f"\t{{ 0x{c['fn']}u, {st(c['in'])}, {st(c['out'])} }},")
    o += ["};", "", "static void (*fn_ptr(u32 a))(SH4 *)", "{"]
    for e in entries:
        o.append(f"\tif (a == 0x{e}u) return f_{e};")
    o += ["\treturn 0;", "}", "", HARNESS]
    open(outp, "w").write("\n".join(o))
    print(f"wrote {outp}: {len(calls)} tests, dispatcher knows {len(entries)} functions")


HARNESS = r'''
u8 *sh4_ram;
static u8 *ram_image;
static u32 fake_timer;

u32 sh4_hw_read(u32 addr, int size) { fprintf(stderr, "unexpected hw read %08x/%d\n", addr, size); exit(2); }
void sh4_hw_write(u32 addr, u32 v, int size) { fprintf(stderr, "unexpected hw write %08x=%x/%d\n", addr, v, size); exit(2); }
void sh4_pref(SH4 *c, u32 a) { (void)c; (void)a; }
void sh4_unimpl(SH4 *c, u32 a, u32 op) { (void)c; fprintf(stderr, "unimplemented %04x at %08x\n", op, a); exit(2); }
void sh4_jump(SH4 *c, u32 a) { (void)c; fprintf(stderr, "unexpected jump %08x\n", a); exit(2); }
void sh4_call(SH4 *c, u32 a)
{
	if (a == 0x8c220060u) { c->r[0] = fake_timer++ * 0x9E3779B1u; return; }	/* fake sh4_ReadTimer */
	void (*f)(SH4 *) = fn_ptr(a);
	if (f) { f(c); return; }
	fprintf(stderr, "unexpected call %08x\n", a);
	exit(2);
}

static void put_mem(const Mem *m)
{
	for (u32 i = 0; i < m->len; i++) { unsigned v; sscanf(m->hex + 2 * i, "%2x", &v); sh4_ram[(m->addr + i) & 0xffffff] = (u8)v; }
}
static int bad_mem(const Mem *m)
{
	for (u32 i = 0; i < m->len; i++)
	{
		unsigned v; sscanf(m->hex + 2 * i, "%2x", &v);
		if (sh4_ram[(m->addr + i) & 0xffffff] != (u8)v) return (int)i + 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	sh4_fsca_init();
	sh4_ram = calloc(1, 16 << 20);
	ram_image = calloc(1, 16 << 20);
	FILE *bin = fopen(argc > 1 ? argv[1] : "1ST_READ.BIN", "rb");
	if (!bin) { fprintf(stderr, "need 1ST_READ.BIN path\n"); return 2; }
	size_t got = fread(ram_image + 0x10000, 1, (16 << 20) - 0x10000, bin);
	fclose(bin);
	(void)got;
	int pass = 0, fail = 0, ntests = sizeof(tests) / sizeof(tests[0]);
	u32 perfn[64][3]; int nfn = 0;
	for (int k = 0; k < ntests; k++)
	{
		const Test *t = &tests[k];
		memcpy(sh4_ram, ram_image, 16 << 20);
		SH4 c; memset(&c, 0, sizeof(c));
		memcpy(c.r, t->in.r, 64); memcpy(c.fr, t->in.fr, 64); memcpy(c.xf, t->in.xf, 64);
		c.pr = t->in.pr; c.mach = t->in.mach; c.macl = t->in.macl; c.gbr = t->in.gbr; c.fpul = t->in.fpul;
		c.fpscr = t->in.fpscr; c.T = t->in.t; c.S = t->in.s; c.Q = t->in.q; c.M = t->in.m;
		fake_timer = t->in.timer;
		for (int i = 0; i < t->in.nmem; i++) put_mem(&t->in.mem[i]);
		sh4_set_host_fpmode(c.fpscr);
		fn_ptr(t->fn)(&c);
		_mm_setcsr(0x1f80);
		char why[200] = "";
		for (int i = 0; i < 16 && !why[0]; i++)
			if (c.r[i] != t->out.r[i]) snprintf(why, sizeof(why), "r%d %08x != %08x", i, c.r[i], t->out.r[i]);
		for (int i = 0; i < 16 && !why[0]; i++)
			if (f2u(c.fr[i]) != t->out.fr[i]) snprintf(why, sizeof(why), "fr%d %08x != %08x", i, f2u(c.fr[i]), t->out.fr[i]);
		for (int i = 0; i < 16 && !why[0]; i++)
			if (f2u(c.xf[i]) != t->out.xf[i]) snprintf(why, sizeof(why), "xf%d %08x != %08x", i, f2u(c.xf[i]), t->out.xf[i]);
		if (!why[0] && c.pr != t->out.pr) snprintf(why, sizeof(why), "pr %08x != %08x", c.pr, t->out.pr);
		if (!why[0] && c.fpscr != t->out.fpscr) snprintf(why, sizeof(why), "fpscr %08x != %08x", c.fpscr, t->out.fpscr);
		if (!why[0] && c.fpul != t->out.fpul) snprintf(why, sizeof(why), "fpul %08x != %08x", c.fpul, t->out.fpul);
		if (!why[0] && (c.mach != t->out.mach || c.macl != t->out.macl)) snprintf(why, sizeof(why), "mac");
		if (!why[0] && c.T != t->out.t) snprintf(why, sizeof(why), "T %u != %u", c.T, t->out.t);
		if (!why[0] && (c.S != t->out.s || c.Q != t->out.q || c.M != t->out.m)) snprintf(why, sizeof(why), "S/Q/M");
		if (!why[0] && fake_timer != t->out.timer) snprintf(why, sizeof(why), "timer calls");
		for (int i = 0; i < t->out.nmem && !why[0]; i++)
		{
			int b = bad_mem(&t->out.mem[i]);
			if (b)
			{
				u32 a = t->out.mem[i].addr + b - 1;
				snprintf(why, sizeof(why), "mem %08x (range %08x+%u) got %08x want %.8s", a & ~3u, t->out.mem[i].addr, t->out.mem[i].len,
					rd32(a & ~3u), t->out.mem[i].hex + 2 * ((a & ~3u) - t->out.mem[i].addr));
			}
		}
		int f = 0;
		while (f < nfn && perfn[f][0] != t->fn) f++;
		if (f == nfn) { perfn[nfn][0] = t->fn; perfn[nfn][1] = perfn[nfn][2] = 0; nfn++; }
		if (why[0]) { if (fail < 25) printf("FAIL test %d fn %08x: %s\n", k, t->fn, why); fail++; perfn[f][2]++; }
		else { pass++; perfn[f][1]++; }
	}
	for (int f = 0; f < nfn; f++) printf("  %08x: %u passed, %u failed\n", perfn[f][0], perfn[f][1], perfn[f][2]);
	printf("%d passed, %d failed of %d\n", pass, fail, ntests);
	return fail != 0;
}
'''

if __name__ == "__main__":
    main()
