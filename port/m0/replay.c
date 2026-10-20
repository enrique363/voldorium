/* Broad-oracle replay (M0): runs every recorded call through the whole translated program and
 * compares it with the emulator.
 *
 * usage: replay <oracle.bin> [results.tsv]
 * Link with every build/sweep object (all translated functions + dispatch table) and sh4c/fsca.c.
 *
 * Per call: RAM is filled with 0xA5 and only the bytes the emulator read during the call are
 * set (their first values), so a read the emulator did not make sees poison. Registers, FR/XF,
 * FPSCR (and host rounding mode), SR, FPUL, MAC, GBR come from the recorded entry state.
 * Hardware/store-queue accesses are replayed in order: reads return the recorded values,
 * writes must match address, size and value. sh4_ReadTimer is the oracle's fake counter.
 * Pass = every register equal on exit, every hardware access consumed, and RAM equal to the
 * initial image with the recorded writes applied (no other byte changed).
 */
#include "sh4rt.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct sh4_fn { u32 addr; void (*fn)(SH4 *); };
extern const struct sh4_fn sh4_fns[];
extern const int sh4_nfns;

#define RAM_SIZE (16u << 20)
#define POISON 0xA5
u8 *sh4_ram;
static u8 *expect_ram;
static sigjmp_buf jb;
static char why[256];
static const u32 *hw;
static u32 nhw, hwpos, fake_timer;
static int trace;	/* REPLAY_TRACE=1: print every call made through the dispatcher */

#define FAIL(...) do { snprintf(why, sizeof(why), __VA_ARGS__); siglongjmp(jb, 1); } while (0)

static void (*find(u32 a))(SH4 *)
{
	int lo = 0, hi = sh4_nfns - 1;
	while (lo <= hi)
	{
		int m = (lo + hi) / 2;
		if (sh4_fns[m].addr == a) return sh4_fns[m].fn;
		if (sh4_fns[m].addr < a) lo = m + 1; else hi = m - 1;
	}
	return 0;
}

u32 sh4_hw_read(u32 a, int size)
{
	if (hwpos >= nhw) FAIL("hw read %08x/%d not in oracle", a, size);
	const u32 *e = hw + 4 * hwpos++;
	if (e[0] != 0 || e[1] != a || e[2] != (u32)size)
		FAIL("hw read %08x/%d, oracle %s %08x/%u", a, size, e[0] ? "write" : "read", e[1], e[2]);
	return e[3];
}
static u8 sq_buf[64];	/* store queues SQ0/SQ1, filled by writes to 0xE0000000-0xE3FFFFFF */

void sh4_hw_write(u32 a, u32 v, int size)
{
	if ((a >> 26) == 0x38)
		memcpy(sq_buf + (a & 0x3c), &v, 4);
	if (hwpos >= nhw) FAIL("hw write %08x=%x/%d not in oracle", a, v, size);
	const u32 *e = hw + 4 * hwpos++;
	u32 mask = size == 4 ? ~0u : (1u << (8 * size)) - 1;
	if (e[0] != 1 || e[1] != a || e[2] != (u32)size || (e[3] & mask) != (v & mask))
		FAIL("hw write %08x=%x/%d, oracle %s %08x=%x/%u", a, v, size, e[0] ? "write" : "read", e[1], e[3], e[2]);
}
/* pref on a store-queue address flushes 32 bytes: to RAM (copy here) or to the TA (only the
   order and address are checked; the data was compared at the SQ writes) */
void sh4_pref(SH4 *c, u32 a)
{
	(void)c;
	if ((a >> 26) != 0x38)
		return;	/* cache prefetch: no effect */
	if (hwpos >= nhw) FAIL("store-queue flush %08x not in oracle", a);
	const u32 *e = hw + 4 * hwpos++;
	if (e[0] != 2 || e[1] != a)
		FAIL("store-queue flush %08x, oracle %s %08x", a, e[0] == 2 ? "flush" : e[0] ? "write" : "read", e[1]);
	if (((e[3] >> 26) & 7) == 3)
		memcpy(sh4_ram + (e[3] & 0xffffe0), sq_buf + (a & 0x20), 32);
}
void sh4_unimpl(SH4 *c, u32 a, u32 op) { (void)c; FAIL("unimplemented %04x at %08x", op, a); }
void sh4_call(SH4 *c, u32 a)
{
	if (a == 0x8c220060u) { c->r[0] = fake_timer++ * 0x9E3779B1u; return; }	/* oracle's fake sh4_ReadTimer */
	void (*f)(SH4 *) = find(a);
	if (trace)
		fprintf(stderr, "  call %08x (pr %08x, r4 %08x, hw %u/%u)\n", a, c->pr, c->r[4], hwpos, nhw);
	if (!f && a < 0x8c010000u) FAIL("BIOS call %08x (HLE in the port)", a);
	/* thread context restore: reloads r15/PR and "returns" into another thread, which a C
	   return cannot do; SDK threads are HLE in the port */
	if (a == 0x8c102e10u) FAIL("thread switch %08x (HLE in the port)", a);
	if (!f) FAIL("call/jump to %08x: no translated function", a);
	f(c);
}
void sh4_jump(SH4 *c, u32 a) { sh4_call(c, a); }
/* context-save call sites (setjmp): replayed calls never restore a context */
static sh4_jmp_buf ctx_dummy;
sh4_jmp_buf *sh4_ctx_slot(SH4 *c, u32 buf) { (void)c; (void)buf; return &ctx_dummy; }
void sh4_ctx_resumed(SH4 *c) { (void)c; }
void sh4_hook(SH4 *c, u32 addr) { (void)c; (void)addr; }
u32 sh4_loop_cycles;
void sh4_loop_poll(SH4 *c) { (void)c; sh4_loop_cycles = 0; }	/* the oracle replays single calls: no interrupts */

static void on_signal(int sig)
{
	snprintf(why, sizeof(why), sig == SIGALRM ? "timeout (endless loop?)" : "crash (signal %d)", sig);
	siglongjmp(jb, 1);
}

enum { ST = 76 };	/* see oracle_pack.py */
static void load(SH4 *c, const u32 *s)
{
	memset(c, 0, sizeof(*c));
	memcpy(c->r, s, 64);
	memcpy(c->fr, s + 16, 64);
	memcpy(c->xf, s + 32, 64);
	c->pr = s[48]; c->mach = s[49]; c->macl = s[50]; c->gbr = s[51]; c->fpul = s[52]; c->fpscr = s[53];
	c->T = s[54]; c->S = s[55]; c->Q = s[56]; c->M = s[57]; c->sr_ctl = s[58] & SH4_SR_CTL;
}

static int compare(const SH4 *c, const u32 *s)
{
	static const char *names[] = { "pr", "mach", "macl", "gbr", "fpul", "fpscr", "T", "S", "Q", "M" };
	for (int i = 0; i < 16; i++)
		if (c->r[i] != s[i]) { snprintf(why, sizeof(why), "r%d %08x != %08x", i, c->r[i], s[i]); return 0; }
	for (int i = 0; i < 16; i++)
		if (f2u(c->fr[i]) != s[16 + i]) { snprintf(why, sizeof(why), "fr%d %08x != %08x", i, f2u(c->fr[i]), s[16 + i]); return 0; }
	for (int i = 0; i < 16; i++)
		if (f2u(c->xf[i]) != s[32 + i]) { snprintf(why, sizeof(why), "xf%d %08x != %08x", i, f2u(c->xf[i]), s[32 + i]); return 0; }
	const u32 got[] = { c->pr, c->mach, c->macl, c->gbr, c->fpul, c->fpscr, c->T, c->S, c->Q, c->M };
	for (int i = 0; i < 10; i++)
		if (got[i] != s[48 + i]) { snprintf(why, sizeof(why), "%s %08x != %08x", names[i], got[i], s[48 + i]); return 0; }
	if (sh4_get_sr(c) != (s[58] & (SH4_SR_CTL | 0x303u)))
		{ snprintf(why, sizeof(why), "sr %08x != %08x", sh4_get_sr(c), s[58]); return 0; }
	if (fake_timer != s[59]) { snprintf(why, sizeof(why), "timer reads %u != %u", fake_timer, s[59]); return 0; }
	if (hwpos != nhw) { snprintf(why, sizeof(why), "%u of %u hw accesses made", hwpos, nhw); return 0; }
	if (memcmp(sh4_ram, expect_ram, RAM_SIZE))
	{
		u32 o = 0;
		while (sh4_ram[o] == expect_ram[o]) o++;
		snprintf(why, sizeof(why), "ram %08x = %02x, oracle %02x%s", 0x8c000000u | o, sh4_ram[o], expect_ram[o],
				 expect_ram[o] == POISON ? " (byte not written by the oracle)" : "");
		return 0;
	}
	return 1;
}

static const u32 *apply_runs(const u32 *p, u32 n, u8 *ram)
{
	for (u32 i = 0; i < n; i++)
	{
		u32 off = p[0], len = p[1];
		memcpy(ram + (off & (RAM_SIZE - 1)), p + 2, len);
		p += 2 + (len + 3) / 4;
	}
	return p;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: replay oracle.bin [results.tsv]\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u32 *buf = malloc(size);
	if (fread(buf, 1, size, f) != (size_t)size) { perror("read"); return 2; }
	fclose(f);
	if (memcmp(buf, "SCOR", 4)) { fprintf(stderr, "bad file\n"); return 2; }
	FILE *res = fopen(argc > 2 ? argv[2] : "/dev/null", "w");

	sh4_fsca_init();
	sh4_ram = malloc(RAM_SIZE);
	expect_ram = malloc(RAM_SIZE);
	static u8 altstack[1 << 16];
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
	sigaltstack(&ss, 0);
	struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_ONSTACK | SA_NODEFER };
	sigaction(SIGSEGV, &sa, 0);
	sigaction(SIGBUS, &sa, 0);
	sigaction(SIGALRM, &sa, 0);
	sigaction(SIGFPE, &sa, 0);

	u32 ntests = buf[1], pass = 0, fail = 0;
	/* REPLAY_ONLY=<fn hex>: run only that function's calls; REPLAY_TRACE=1: trace calls */
	const u32 only = getenv("REPLAY_ONLY") ? strtoul(getenv("REPLAY_ONLY"), 0, 16) : 0;
	trace = getenv("REPLAY_TRACE") != 0;
	const u32 *p = buf + 2;
	for (u32 k = 0; k < ntests; k++)
	{
		const u32 fn = p[0], *in = p + 1, *out = p + 1 + ST;
		const u32 nrd = p[1 + 2 * ST], nwr = p[2 + 2 * ST];
		nhw = p[3 + 2 * ST];
		p += 4 + 2 * ST;
		memset(sh4_ram, POISON, RAM_SIZE);
		p = apply_runs(p, nrd, sh4_ram);
		memcpy(expect_ram, sh4_ram, RAM_SIZE);
		p = apply_runs(p, nwr, expect_ram);
		hw = p;
		p += 4 * nhw;
		hwpos = 0;

		if (only && fn != only)
			continue;
		if (trace)
			fprintf(stderr, "test %u fn %08x\n", k, fn);
		static SH4 c;
		load(&c, in);
		fake_timer = in[59];
		memcpy(sq_buf, in + 60, 64);
		why[0] = 0;
		volatile int ok = 0;
		void (*fp)(SH4 *) = find(fn);
		if (!fp)
			snprintf(why, sizeof(why), "no translated function");
		else if (sigsetjmp(jb, 1) == 0)
		{
			alarm(2);
			sh4_set_host_fpmode(c.fpscr);
			fp(&c);
			_mm_setcsr(0x1f80);
			alarm(0);
			ok = compare(&c, out);
		}
		else
		{
			alarm(0);
			_mm_setcsr(0x1f80);
		}
		if (ok) pass++; else fail++;
		fprintf(res, "%08x\t%s\t%s\n", fn, ok ? "pass" : "FAIL", ok ? "" : why);
	}
	printf("%u passed, %u failed of %u calls\n", pass, fail, ntests);
	return fail != 0;
}
