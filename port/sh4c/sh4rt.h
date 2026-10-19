/* Runtime for code emitted by sh4c.py (SH-4 -> C). Milestone M0. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef struct SH4
{
	u32 r[16];
	u32 pr, gbr, mach, macl, fpul, fpscr;
	u32 T, S, Q, M;
	float fr[16], xf[16];
	u32 sr_ctl, vbr;	/* SR without T/S/Q/M (MD, RB, BL, FD, IMASK); VBR */
} SH4;

/* 16 MB main RAM at 0x0C000000 (P1 0x8C000000 / P2 0xAC000000 mirrors). */
extern u8 *sh4_ram;

/* Non-RAM accesses (hardware registers, BIOS, VRAM ...) go to the host. */
u32 sh4_hw_read(u32 addr, int size);
void sh4_hw_write(u32 addr, u32 value, int size);

static inline int sh4_is_ram(u32 a) { return ((a >> 26) & 7) == 3 && (a >> 29) != 7; }

static inline u8 rd8(u32 a) { return sh4_is_ram(a) ? sh4_ram[a & 0xffffff] : (u8)sh4_hw_read(a, 1); }
static inline u16 rd16(u32 a)
{
	if (sh4_is_ram(a)) { u16 v; memcpy(&v, &sh4_ram[a & 0xffffff], 2); return v; }
	return (u16)sh4_hw_read(a, 2);
}
static inline u32 rd32(u32 a)
{
	if (sh4_is_ram(a)) { u32 v; memcpy(&v, &sh4_ram[a & 0xffffff], 4); return v; }
	return sh4_hw_read(a, 4);
}
/* SH4_TAG_WRITES (native executable only): RAM writes inside [sh4_tag_lo, +sh4_tag_len) are
   reported, so the renderer can tell which game code filled each slot of Kamui's vertex
   buffers (which reach the TA later by DMA) */
#ifdef SH4_TAG_WRITES
extern u32 sh4_tag_lo, sh4_tag_len;
void sh4_tag_write(u32 offset);
#define SH4_TAG(a) do { if ((u32)(((a) & 0xffffff) - sh4_tag_lo) < sh4_tag_len) sh4_tag_write((a) & 0xffffff); } while (0)
#else
#define SH4_TAG(a) do { } while (0)
#endif
static inline void wr8(u32 a, u8 v) { if (sh4_is_ram(a)) { sh4_ram[a & 0xffffff] = v; SH4_TAG(a); } else sh4_hw_write(a, v, 1); }
static inline void wr16(u32 a, u16 v) { if (sh4_is_ram(a)) { memcpy(&sh4_ram[a & 0xffffff], &v, 2); SH4_TAG(a); } else sh4_hw_write(a, v, 2); }
static inline void wr32(u32 a, u32 v) { if (sh4_is_ram(a)) { memcpy(&sh4_ram[a & 0xffffff], &v, 4); SH4_TAG(a); } else sh4_hw_write(a, v, 4); }

/* div1 Rm,Rn exactly as Flycast's interpreter (sh4_opcodes.cpp). */
static inline void sh4_div1(SH4 *c, int n, int m)
{
	u32 old_q = c->Q;
	c->Q = (c->r[n] & 0x80000000u) != 0;
	u32 old_rm = c->r[m];
	c->r[n] = (c->r[n] << 1) | c->T;
	u32 old_rn = c->r[n];
	if (old_q == 0)
	{
		if (c->M == 0) { c->r[n] -= old_rm; c->Q = c->Q ^ (c->r[n] > old_rn); }
		else { c->r[n] += old_rm; c->Q = !c->Q ^ (c->r[n] < old_rn); }
	}
	else
	{
		if (c->M == 0) { c->r[n] += old_rm; c->Q = c->Q ^ (c->r[n] < old_rn); }
		else { c->r[n] -= old_rm; c->Q = !c->Q ^ (c->r[n] > old_rn); }
	}
	c->T = c->Q == c->M;
}

/* SR as the game sees it. Only T/S/Q/M affect translated code; the interrupt mask and the
   other control bits are kept so that save/restore sequences round-trip. Register bank
   switching (RB) is not modelled: game code never changes it. */
#define SH4_SR_CTL 0x700080F0u	/* MD, RB, BL, FD, IMASK: the bits Flycast keeps besides T/S/Q/M */
static inline u32 sh4_get_sr(const SH4 *c) { return (c->sr_ctl & SH4_SR_CTL) | c->M << 9 | c->Q << 8 | c->S << 1 | c->T; }
static inline void sh4_set_sr(SH4 *c, u32 v)
{
	c->sr_ctl = v & SH4_SR_CTL;
	c->M = (v >> 9) & 1; c->Q = (v >> 8) & 1; c->S = (v >> 1) & 1; c->T = v & 1;
}

/* mac.l / mac.w as Flycast (S=1 saturation is not implemented there either) */
static inline void sh4_macl(SH4 *c, int n, int m)
{
	s32 rm = (s32)rd32(c->r[m]), rn = (s32)rd32(c->r[n] + (n == m ? 4 : 0));
	c->r[m] += 4; c->r[n] += 4;
	u64 mac = ((u64)c->mach << 32 | c->macl) + (u64)((s64)rm * (s64)rn);
	c->mach = (u32)(mac >> 32); c->macl = (u32)mac;
}
static inline void sh4_macw(SH4 *c, int n, int m)
{
	s32 rn = (s16)rd16(c->r[n]), rm = (s16)rd16(c->r[m] + (n == m ? 2 : 0));
	c->r[n] += 2; c->r[m] += 2;
	u64 mac = ((u64)c->mach << 32 | c->macl) + (u64)(s64)(rm * rn);
	c->mach = (u32)(mac >> 32); c->macl = (u32)mac;
}

/* ---- FPU (semantics copied from Flycast core/hw/sh4/interpr/sh4_fpu.cpp) ----------------
 * c->fr is the current bank, c->xf the other one (Flycast swaps the arrays on FPSCR.FR change).
 * FPSCR bits: RM 0-1, DN 18, PR 19, SZ 20, FR 21. The host must compute with the matching
 * rounding/denormal mode: sh4_set_host_fpmode() mirrors Flycast's setHostRoundingMode (x64).
 */
#include <math.h>
#include <xmmintrin.h>
#define FPSCR_PR(c) (((c)->fpscr >> 19) & 1)
#define FPSCR_SZ(c) (((c)->fpscr >> 20) & 1)
#define FPSCR_FR(c) (((c)->fpscr >> 21) & 1)

static inline u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static inline float u2f(u32 u) { float f; memcpy(&f, &u, 4); return f; }
/* bit access to FR registers through memcpy: a u32* into the float array breaks strict aliasing
   (gcc -O2 reordered an fmov.s store against a later float write; seen on project_points) */
static inline u32 frbits(SH4 *c, int i) { return f2u(c->fr[i]); }
static inline void set_frbits(SH4 *c, int i, u32 v) { c->fr[i] = u2f(v); }
/* raw 64-bit pair as used by fmov (dr_hex/xd_hex): low word = fr[2n], high = fr[2n+1] */
static inline u64 dr_hex(SH4 *c, int n) { u64 v; memcpy(&v, &c->fr[n * 2], 8); return v; }
static inline u64 xd_hex(SH4 *c, int n) { u64 v; memcpy(&v, &c->xf[n * 2], 8); return v; }
static inline void set_dr_hex(SH4 *c, int n, u64 v) { memcpy(&c->fr[n * 2], &v, 8); }
static inline void set_xd_hex(SH4 *c, int n, u64 v) { memcpy(&c->xf[n * 2], &v, 8); }
/* arithmetic double (getDR/setDR): fr[2n] is the HIGH word */
static inline double getDR(SH4 *c, int n)
{
	u64 v = ((u64)frbits(c, n * 2) << 32) | frbits(c, n * 2 + 1);
	double d; memcpy(&d, &v, 8); return d;
}
static inline void setDR(SH4 *c, int n, double d)
{
	u64 v; memcpy(&v, &d, 8);
	set_frbits(c, n * 2, (u32)(v >> 32)); set_frbits(c, n * 2 + 1, (u32)v);
}
static inline u64 rd64(u32 a) { return (u64)rd32(a) | ((u64)rd32(a + 4) << 32); }
static inline void wr64(u32 a, u64 v) { wr32(a, (u32)v); wr32(a + 4, (u32)(v >> 32)); }

static inline void sh4_set_host_fpmode(u32 fpscr)
{
	u32 m = 0x1f80;
	if ((fpscr & 3) == 1) m |= 3 << 13;		/* round to zero */
	if ((fpscr >> 18) & 1) m |= 1 << 15;	/* flush denormals to zero */
	_mm_setcsr(m);
}
static inline void sh4_set_fpscr(SH4 *c, u32 v)
{
	if (((c->fpscr ^ v) >> 21) & 1)
	{
		float t[16];
		memcpy(t, c->fr, sizeof(t)); memcpy(c->fr, c->xf, sizeof(t)); memcpy(c->xf, t, sizeof(t));
	}
	c->fpscr = v;
	sh4_set_host_fpmode(v);
}

extern float sh4_sin_table[0x10000][2];	/* fsca results, built by sh4_fsca_init() */
void sh4_fsca_init(void);

#define FBIN(name, op) \
static inline void name(SH4 *c, int n, int m) \
{ \
	if (!FPSCR_PR(c)) c->fr[n] = c->fr[n] op c->fr[m]; \
	else setDR(c, n >> 1, getDR(c, n >> 1) op getDR(c, m >> 1)); \
}
FBIN(sh4_fadd, +)
FBIN(sh4_fsub, -)
FBIN(sh4_fmul, *)
FBIN(sh4_fdiv, /)
static inline void sh4_fcmpeq(SH4 *c, int n, int m)
{ c->T = !FPSCR_PR(c) ? c->fr[m] == c->fr[n] : getDR(c, n >> 1) == getDR(c, m >> 1); }
static inline void sh4_fcmpgt(SH4 *c, int n, int m)
{ c->T = !FPSCR_PR(c) ? c->fr[n] > c->fr[m] : getDR(c, n >> 1) > getDR(c, m >> 1); }

/* fmov.s variants. SZ=1 moves 64-bit pairs; bit 0 of the register field selects XD. */
static inline void sh4_fmov_ld(SH4 *c, int n, u32 a)
{
	if (!FPSCR_SZ(c)) set_frbits(c, n, rd32(a));
	else if (!(n & 1)) set_dr_hex(c, n >> 1, rd64(a));
	else set_xd_hex(c, n >> 1, rd64(a));
}
static inline void sh4_fmov_st(SH4 *c, int m, u32 a)
{
	if (!FPSCR_SZ(c)) wr32(a, frbits(c, m));
	else if (!(m & 1)) wr64(a, dr_hex(c, m >> 1));
	else wr64(a, xd_hex(c, m >> 1));
}
static inline u32 sh4_fsz(SH4 *c) { return FPSCR_SZ(c) ? 8 : 4; }
static inline void sh4_fmov(SH4 *c, int n, int m)
{
	if (!FPSCR_SZ(c)) { c->fr[n] = c->fr[m]; return; }
	u64 v = (m & 1) ? xd_hex(c, m >> 1) : dr_hex(c, m >> 1);
	if (n & 1) set_xd_hex(c, n >> 1, v); else set_dr_hex(c, n >> 1, v);
}
static inline void sh4_fabs(SH4 *c, int n) { int i = FPSCR_PR(c) ? (n & 0xe) : n; set_frbits(c, i, frbits(c, i) & 0x7fffffffu); }
static inline void sh4_fneg(SH4 *c, int n) { set_frbits(c, n, frbits(c, n) ^ 0x80000000u); }
static inline void sh4_fsca(SH4 *c, int n)
{
	n &= 0xe;
	if (FPSCR_PR(c)) return;	/* unimplemented in Flycast too */
	u32 i = c->fpul & 0xffff;
	c->fr[n] = sh4_sin_table[i][0];
	c->fr[n + 1] = sh4_sin_table[i][1];
}
static inline void sh4_fsrra(SH4 *c, int n) { if (!FPSCR_PR(c)) c->fr[n] = 1.f / sqrtf(c->fr[n]); }
static inline void sh4_fcnvds(SH4 *c, int n) { if (FPSCR_PR(c)) { float f = (float)getDR(c, n >> 1); memcpy(&c->fpul, &f, 4); } }
static inline void sh4_fcnvsd(SH4 *c, int n) { if (FPSCR_PR(c)) setDR(c, n >> 1, (double)u2f(c->fpul)); }
static inline void sh4_fipr(SH4 *c, int nm)
{
	int n = nm & 0xc, m = (nm & 0x3) << 2;
	if (FPSCR_PR(c)) return;
	double idp = (double)c->fr[n + 0] * c->fr[m + 0];
	idp += (double)c->fr[n + 1] * c->fr[m + 1];
	idp += (double)c->fr[n + 2] * c->fr[m + 2];
	idp += (double)c->fr[n + 3] * c->fr[m + 3];
	c->fr[n + 3] = (float)idp;
}
static inline void sh4_fldi0(SH4 *c, int n) { if (!FPSCR_PR(c)) c->fr[n] = 0.0f; }
static inline void sh4_fldi1(SH4 *c, int n) { if (!FPSCR_PR(c)) c->fr[n] = 1.0f; }
static inline void sh4_float(SH4 *c, int n)
{
	if (!FPSCR_PR(c)) c->fr[n] = (float)(s32)c->fpul;
	else setDR(c, n >> 1, (double)(s32)c->fpul);
}
static inline void sh4_fsqrt(SH4 *c, int n)
{
	if (!FPSCR_PR(c)) c->fr[n] = sqrtf(c->fr[n]);
	else setDR(c, n >> 1, sqrt(getDR(c, n >> 1)));
}
static inline void sh4_ftrc(SH4 *c, int n)
{
	if (!FPSCR_PR(c))
	{
		float f = c->fr[n];
		if (isnan(f)) { c->fpul = 0x80000000u; return; }
		c->fpul = (u32)(s32)f;
		if ((s32)c->fpul > 0x7fffff80) c->fpul = 0x7fffffff;
		else if (c->fpul == 0x80000000u && f > 0) c->fpul--;
	}
	else
	{
		double f = getDR(c, n >> 1);
		if (isnan(f)) { c->fpul = 0x80000000u; return; }
		c->fpul = (u32)(s32)f;
		if (c->fpul == 0x80000000u && f > 0) c->fpul--;
	}
}
static inline void sh4_fmac(SH4 *c, int n, int m) { if (!FPSCR_PR(c)) c->fr[n] = fmaf(c->fr[0], c->fr[m], c->fr[n]); }
static inline void sh4_ftrv(SH4 *c, int n)
{
	n &= 0xc;
	if (FPSCR_PR(c)) return;
	const float *x = c->xf, *v = &c->fr[n];
	double v1 = (double)x[0] * v[0] + (double)x[4] * v[1] + (double)x[8] * v[2] + (double)x[12] * v[3];
	double v2 = (double)x[1] * v[0] + (double)x[5] * v[1] + (double)x[9] * v[2] + (double)x[13] * v[3];
	double v3 = (double)x[2] * v[0] + (double)x[6] * v[1] + (double)x[10] * v[2] + (double)x[14] * v[3];
	double v4 = (double)x[3] * v[0] + (double)x[7] * v[1] + (double)x[11] * v[2] + (double)x[15] * v[3];
	c->fr[n + 0] = (float)v1; c->fr[n + 1] = (float)v2; c->fr[n + 2] = (float)v3; c->fr[n + 3] = (float)v4;
}
static inline void sh4_frchg(SH4 *c) { sh4_set_fpscr(c, c->fpscr ^ (1u << 21)); }
static inline void sh4_fschg(SH4 *c) { c->fpscr ^= 1u << 20; }

/* Calls and jumps to a Dreamcast address: translated function, HLE stub or error. */
void sh4_call(SH4 *c, u32 addr);
void sh4_jump(SH4 *c, u32 addr);
/* Calls to a context-save routine (setjmp semantics, see SETJMP_FUNCS in sh4c.py) are emitted
   as `if (!sh4_setjmp(*sh4_ctx_slot(c, buffer))) sh4_call(...); else sh4_ctx_resumed(c);`.
   Linux: glibc setjmp (users longjmp between stacks with _FORTIFY_SOURCE off). Windows x64:
   the CRT longjmp unwinds through SEH and clang's __builtin_setjmp loses locals there, so
   m1/sjlj_win64.S saves the callee-saved registers itself and jumps without unwinding. */
#ifdef _WIN32
typedef unsigned long long sh4_jmp_buf[32] __attribute__((aligned(16)));
int sh4_setjmp(sh4_jmp_buf b) __attribute__((returns_twice));
void sh4_longjmp(sh4_jmp_buf b) __attribute__((noreturn));
#else
#include <setjmp.h>
typedef jmp_buf sh4_jmp_buf;
#define sh4_setjmp(b) setjmp(b)
#define sh4_longjmp(b) longjmp(b, 1)
#endif
sh4_jmp_buf *sh4_ctx_slot(SH4 *c, u32 buffer);
void sh4_ctx_resumed(SH4 *c);
/* runtime hook before the instruction at addr (addresses from sh4c/hooks.txt) */
void sh4_hook(SH4 *c, u32 addr);
/* cycles of loop iterations not yet added to the virtual clock (backward branches add their
   length; the runtime drains it; harnesses without a clock define and ignore it) */
extern u32 sh4_loop_cycles;
/* pref @Rn: store-queue flush when Rn is in 0xE0000000-0xE3FFFFFF. */
void sh4_pref(SH4 *c, u32 addr);
void sh4_unimpl(SH4 *c, u32 addr, u32 op);
