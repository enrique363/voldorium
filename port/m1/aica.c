/* AICA sound chip: sound RAM, registers, timers, interrupts, the ARM7 that runs the game's
 * sound driver (arm7.c) and the 64-channel sample engine, with output to SDL audio.
 *
 * Address map (offsets; SH-4 0x00700000 + off and 0x00800000 sound RAM, ARM 0x800000 + off
 * and 0x000000 sound RAM):
 *   0x0000-0x1FFF  64 channels x 0x80: 16-bit registers at 4-byte steps (below)
 *   0x2000-0x2047  DSP output sends EFSDL / EFPAN
 *   0x2800-0x2FFF  common: master volume, monitor (MSLC -> SGC / EG / CA readback), timers
 *                  A-C, ARM interrupts SCIEB / SCIPD / SCIRE / SCILV0-2, SH-4 interrupts
 *                  MCIEB / MCIPD / MCIRE, ARMRST (0x2C00), interrupt level L (0x2D00) and
 *                  acknowledge M (0x2D04)
 *   0x3000-0x45FF  DSP coefficients, addresses, program, work registers (stored, not run yet)
 *   SH-4 0x00710000 / 04: real-time clock, seconds since 1950 (from the virtual clock)
 * Channel registers: +00 KYONEX(15) KYONB(14) SSCTL(10) LPCTL(9) PCMS(8-7) SA[22:16](6-0),
 *   +04 SA[15:0], +08 LSA, +0C LEA, +10 D2R(15-11) D1R(10-6) AR(4-0),
 *   +14 LPSLNK(14) KRS(13-10) DL(9-5) RR(4-0), +18 OCT(14-11) FNS(9-0), +1C LFO,
 *   +20 IMXL(7-4) ISEL(3-0), +24 DISDL(11-8) DIPAN(4-0), +28 TL(15-8) VOFF LPOFF Q.
 *
 * Timing: one output sample (44,100 Hz) per 200 MHz / 44,100 SH-4 cycles of the virtual
 * clock; per sample the ARM runs ARM_PER_SAMPLE instructions, then the timers step, the sample
 * interrupt (SCIPD bit 10) fires and the channels are mixed (the order Flycast uses). The ARM's
 * interrupt input is latched with its level (L) until the driver acknowledges it (M). Everything derives from the
 * virtual clock, so sound is deterministic and part of the rollback state (aica_state_*).
 *
 * Output: SDL audio unless M1_HEADLESS / M1_NOSOUND; M1_WAV=file also writes a WAV.
 * Not modelled yet: the DSP (reverb and other effects: the wet signal is missing), LFOs,
 * the filter envelope, AICA-internal DMA and MIDI.
 */
#include "m1.h"
#include "arm7.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (2u << 20)
#define ARM_PER_SAMPLE 512	/* 22.5792 MHz ARM clock / 44,100 Hz, about one instruction per cycle */

u8 aica_ram[RAM_SIZE];
static u8 regs[0x10000];
u32 arm_pc_hist[0x2000];	/* M1_ARMPROF */

void hw_ext_irq(int bit, int on);
extern int m1_resim;

static u16 rg(u32 off) { u16 v; memcpy(&v, regs + (off & 0xfffc), 2); return v; }
static void rs(u32 off, u16 v) { memcpy(regs + (off & 0xfffc), &v, 2); }

enum
{
	R_MVOL = 0x2800, R_MSLC = 0x280c, R_MON = 0x2810, R_CA = 0x2814,
	R_TIMA = 0x2890, R_SCIEB = 0x289c, R_SCIPD = 0x28a0, R_SCIRE = 0x28a4, R_SCILV0 = 0x28a8,
	R_MCIEB = 0x28b4, R_MCIPD = 0x28b8, R_MCIRE = 0x28bc, R_ARMRST = 0x2c00, R_L = 0x2d00, R_M = 0x2d04,
};

/* ---- channels ---------------------------------------------------------------------------- */
enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE };
typedef struct
{
	int on;
	u32 pos, frac;			/* sample index from SA, 16-bit fraction */
	int eg_state;
	s32 eg;					/* attenuation 0 (loud) .. 0x3FF << 16 (silent) */
	int lp;					/* loop end reached (LP readback) */
	s32 cur, next;			/* samples at pos and pos + 1 */
	u32 have;				/* index of `cur` (decoder position) */
	s32 ad_prev, ad_step;	/* ADPCM decoder */
	s32 loop_prev, loop_step;
	int loop_saved;
	u32 lfo_phase;			/* LFO phase (32-bit fraction of a cycle) */
	u32 lfo_noise;			/* LFO noise generator state */
	u32 feg;				/* filter envelope: cutoff level (13-bit FLV scale) << 16 */
	int feg_state;
	s32 f_prev1, f_prev2, f_fract;	/* low-pass filter history */
} Chan;
static Chan ch[64];

typedef struct
{
	u32 acc;				/* virtual cycles * 44,100 not yet turned into samples */
	u32 timer_acc[3];
	u64 samples;
	int irq_out;			/* the ARM's interrupt input, latched until acknowledged (M) */
	u32 irq_level;			/* L register: level of the interrupt that raised it */
} Clock;
static Clock clk;

/* envelope times in ms for the 64 effective rates (full 0x3FF range), hardware measurements as
   documented for the AICA/SCSP */
static const double attack_ms[64] = { 0, 0, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
	1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
	76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
	2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0 };
static const double decay_ms[64] = { 0, 0, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0,
	25300.0, 22200.0, 17700.0, 14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0,
	2200.0, 1800.0, 1600.0, 1400.0, 1100.0, 920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0,
	140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0, 28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1,
	5.4, 4.3, 3.6, 3.1 };
static s32 attack_div[64], decay_step[64];	/* attack: divisor << 16 (exponential); decay: attenuation << 16 per sample */
static float eg_gain[1024], tl_gain[256], level_gain[16];

static void tables(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	for (int i = 0; i < 64; i++)
	{
		/* rates 0-1: no change; time 0: immediate. The attack is exponential: each sample removes
		   1/F of the attenuation, so that it falls from 0x280 (the key-on value) in the time */
		if (i < 2)
			attack_div[i] = 0;
		else if (attack_ms[i] == 0)
			attack_div[i] = 1 << 16;
		else
		{
			const double n = 44.1 * attack_ms[i];
			attack_div[i] = (s32)(65536.0 / (1.0 - 1.0 / pow(0x280, 1.0 / n)) + 0.5);
		}
		decay_step[i] = i < 2 ? 0 : (s32)(1023.0 * 65536 / (decay_ms[i] * 44.1));
	}
	for (int i = 0; i < 1024; i++)
		eg_gain[i] = i >= 0x3f0 ? 0 : (float)pow(10.0, -i * (96.0 / 1024) / 20);	/* about 0.094 dB per step */
	for (int i = 0; i < 256; i++)
		tl_gain[i] = (float)pow(10.0, -i * 0.375 / 20);
	for (int i = 0; i < 16; i++)
		level_gain[i] = i == 0 ? 0 : (float)pow(10.0, -(15 - i) * 3.0 / 20);	/* DISDL / MVOL: 3 dB steps */
}

static u32 creg(int c, int r) { return rg((u32)c * 0x80 + r); }

/* effective envelope rate: 2 x the register rate, plus key scaling (KRS < 15: FNS bit 9 and
   2 x (KRS + octave) when positive), at most 63 (the scaling Flycast uses) */
static int rate(int c, int r)
{
	const u32 krs = (creg(c, 0x14) >> 10) & 15, oct = (creg(c, 0x18) >> 11) & 15, fns = creg(c, 0x18) & 0x3ff;
	int e = 2 * r;
	if (krs != 15)
	{
		const int ks = ((int)krs + (int)((oct ^ 8) - 8)) * 2;
		e += (int)((fns >> 9) & 1) + (ks > 0 ? ks : 0);
	}
	return e > 63 ? 63 : e;
}

static const int adpcm_scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };

/* sample n of channel c, decoding ADPCM forward as needed */
/* one ADPCM nibble (sample i from SA) into the decoder state */
static void adpcm_nibble(u32 sa, Chan *k, u32 i)
{
	const u8 b = aica_ram[(sa + i / 2) & (RAM_SIZE - 1)];
	const u32 nib = (i & 1) ? b >> 4 : b & 15;
	const s32 delta = (s32)(((nib & 7) * 2 + 1) * (u32)k->ad_step) / 8;
	k->ad_prev += (nib & 8) ? -delta : delta;
	k->ad_prev = k->ad_prev > 32767 ? 32767 : k->ad_prev < -32768 ? -32768 : k->ad_prev;
	k->ad_step = (k->ad_step * adpcm_scale[nib & 7]) >> 8;
	k->ad_step = k->ad_step < 127 ? 127 : k->ad_step > 24576 ? 24576 : k->ad_step;
}

static s32 fetch(int c, Chan *k, u32 n)
{
	const u32 r0 = creg(c, 0x00);
	const u32 sa = ((r0 & 0x7f) << 16) | creg(c, 0x04), fmt = (r0 >> 7) & 3;
	if (fmt == 0)
	{
		s16 v;
		memcpy(&v, aica_ram + ((sa + 2 * n) & (RAM_SIZE - 2)), 2);
		return v;
	}
	if (fmt == 1)
		return (s32)(s8)aica_ram[(sa + n) & (RAM_SIZE - 1)] << 8;
	/* ADPCM: decode up to n (callers only move forward, or restart at the loop start) */
	while (k->have < n)
	{
		k->have++;
		const u32 i = k->have;
		if (fmt == 2 && i == creg(c, 0x08) && !k->loop_saved)	/* entering the loop: keep the state for repeats */
		{
			k->loop_prev = k->ad_prev;
			k->loop_step = k->ad_step;
			k->loop_saved = 1;
		}
		adpcm_nibble(sa, k, i);
	}
	return k->ad_prev;
}

static void key_on(int c)
{
	{
		/* M1_AICAKEYLOG=from-to (seconds of sound time): print key-ons */
		static int on = -1;
		static double from, to;
		if (on < 0)
		{
			const char *e = getenv("M1_AICAKEYLOG");
			on = e && sscanf(e, "%lf-%lf", &from, &to) == 2;
		}
		const double t = (double)clk.samples / 44100;
		if (on && t >= from && t < to)
		{
			const u32 r0 = creg(c, 0);
			printf("key %.3f ch%02d fmt %u loop %u sa %06x lsa %04x lea %04x oct %d fns %03x tl %02x disdl %x\n", t, c,
				   (r0 >> 7) & 3, (r0 >> 9) & 1, ((r0 & 0x7f) << 16) | creg(c, 4), creg(c, 8), creg(c, 0xc),
				   (int)(((creg(c, 0x18) >> 11) & 15) ^ 8) - 8, creg(c, 0x18) & 0x3ff, creg(c, 0x28) >> 8, (creg(c, 0x24) >> 8) & 15);
		}
	}
	Chan *k = &ch[c];
	memset(k, 0, sizeof(*k));
	k->on = 1;
	k->eg_state = EG_ATTACK;
	k->eg = 0x280 << 16;	/* key-on attenuation (as Flycast, from Highly Theoretical) */
	k->feg = (u32)(creg(c, 0x2c) & 0x1fff) << 16;	/* FLV0 */
	k->feg_state = EG_ATTACK;
	k->ad_step = 127;
	k->have = 0;
	/* ADPCM: sample 0 is the first nibble */
	const u32 r0 = creg(c, 0x00), sa = ((r0 & 0x7f) << 16) | creg(c, 0x04);
	if (((r0 >> 7) & 3) >= 2)
	{
		const u8 b = aica_ram[sa & (RAM_SIZE - 1)];
		const u32 nib = b & 15;
		const s32 delta = (s32)(((nib & 7) * 2 + 1) * 127u) / 8;
		k->ad_prev = (nib & 8) ? -delta : delta;
		k->ad_step = (127 * adpcm_scale[nib & 7]) >> 8;
		k->ad_step = k->ad_step < 127 ? 127 : k->ad_step;
		if (creg(c, 0x08) == 0)
		{
			k->loop_prev = k->ad_prev, k->loop_step = k->ad_step;
			k->loop_saved = 1;
		}
	}
	k->cur = fetch(c, k, 0);
	k->next = fetch(c, k, 1);
}

static void release(int c);

static void key_on_ex(void)
{
	for (int c = 0; c < 64; c++)
	{
		const int kyonb = (creg(c, 0x00) >> 14) & 1;
		if (kyonb && (!ch[c].on || ch[c].eg_state == EG_RELEASE))
			key_on(c);
		else if (!kyonb && ch[c].on)
			release(c);
	}
}

static void step_eg(int c, Chan *k)
{
	const u32 r10 = creg(c, 0x10), r14 = creg(c, 0x14);
	switch (k->eg_state)
	{
	case EG_ATTACK:
	{
		const s32 div = attack_div[rate(c, r10 & 31)];
		if (div)
			k->eg -= (s32)(((s64)k->eg << 16) / div) + 1;
		if (k->eg <= 0)
		{
			k->eg = 0;
			k->eg_state = EG_DECAY1;
		}
		break;
	}
	case EG_DECAY1:
	{
		k->eg += decay_step[rate(c, (r10 >> 6) & 31)];
		const s32 dl = (s32)((r14 >> 5) & 31) << (5 + 16);
		if (k->eg >= dl)
			k->eg_state = EG_DECAY2;
		break;
	}
	case EG_DECAY2:
		k->eg += decay_step[rate(c, (r10 >> 11) & 31)];
		break;
	default:
		k->eg += decay_step[rate(c, r14 & 31)];
		break;
	}
	if (k->eg >= 0x3ff << 16)
	{
		k->eg = 0x3ff << 16;
		if (k->eg_state == EG_RELEASE)
			k->on = 0;
	}
}

/* entering release clears KYONB in the channel's register (as on the chip): otherwise the next
   KYONEX, which applies every channel's KYONB, would start a finished sound again */
static void release(int c)
{
	ch[c].eg_state = EG_RELEASE;
	rs((u32)c * 0x80, (u16)(creg(c, 0) & ~0x4000u));
}

/* ---- filter envelope and low-pass filter ----------------------------------------------------
 * Active when LPOFF is clear and the levels or Q differ from "open" (FLV 0x1FF8, Q 4). The
 * cutoff moves from FLV0 to FLV1 (FAR), FLV2 (FD1R), FLV3 (FD2R), and to FLV4 on release
 * (FRR), at the decay-table rates. The filter is the two-pole resonant low-pass described for
 * the AICA by Neill Corlett (Highly Theoretical), on the sample << 4 (20 bits). */
static const s32 q_table[32] = { 2048, 1536, 1024, 512, 0, -256, -512, -768, -1024, -1280, -1536, -1792, -2048, -2176, -2304,
	-2432, -2560, -2688, -2816, -2944, -3072, -3136, -3200, -3264, -3328, -3392, -3456, -3520, -3584, -3648, -3712, -3776 };

/* ---- LFO (register 0x1C: LFORE 15, LFOF 14-10, PLFOWS 9-8, PLFOS 7-5, ALFOWS 4-3, ALFOS 2-0) ----
 * One LFO per channel drives pitch (vibrato, PLFOS cents deep) and amplitude (tremolo, ALFOS
 * dB deep) with saw, square, triangle or noise waveforms; LFORE holds it in reset. Frequencies
 * and depths are the published AICA/SCSP values. */
static const float lfo_hz[32] = { 0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f,
	1.60f, 1.87f, 2.27f, 2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f,
	86.1f, 172.3f };
static const float plfo_cents[8] = { 0, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f };
static const float alfo_db[8] = { 0, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f };

/* waveform value in [-1, 1] at the phase (noise: held per step) */
static float lfo_wave(u32 ws, Chan *k)
{
	const u32 ph = k->lfo_phase;
	switch (ws)
	{
	case 0: return (float)((double)ph / 2147483648.0 - 1.0);	/* saw */
	case 1: return ph < 0x80000000u ? 1.0f : -1.0f;				/* square */
	case 2: return ph < 0x80000000u ? (float)((double)ph / 1073741824.0 - 1.0) : (float)(3.0 - (double)ph / 1073741824.0);	/* triangle */
	default: return (float)(int)(k->lfo_noise >> 16 & 0xff) / 127.5f - 1.0f;	/* noise */
	}
}

/* advances the LFO; returns the pitch factor and sets *amp to the amplitude factor */
static float lfo_step(int c, Chan *k, float *amp)
{
	const u32 r = creg(c, 0x1c);
	*amp = 1.0f;
	if (r & 0x8000)
	{
		k->lfo_phase = 0;	/* LFORE: held in reset */
		return 1.0f;
	}
	const u32 old = k->lfo_phase;
	k->lfo_phase += (u32)(lfo_hz[(r >> 10) & 31] / 44100.0f * 4294967296.0f);
	if (k->lfo_phase < old)
		k->lfo_noise = k->lfo_noise * 1103515245u + 12345u;	/* new noise value each cycle (deterministic) */
	float pitch = 1.0f;
	const u32 ps = (r >> 5) & 7, as = r & 7;
	if (ps)
		pitch = powf(2.0f, plfo_cents[ps] * lfo_wave((r >> 8) & 3, k) / 1200.0f);
	if (as)
	{
		const float w = (lfo_wave((r >> 3) & 3, k) + 1.0f) * 0.5f;	/* 0..1: attenuation only */
		*amp = powf(10.0f, -alfo_db[as] * w / 20.0f);
	}
	return pitch;
}

static int feg_active(int c)
{
	if ((creg(c, 0x28) >> 5) & 1)
		return 0;	/* LPOFF */
	for (int i = 0; i < 5; i++)
		if ((creg(c, 0x2c + 4 * i) & 0x1fff) < 0x1ff8)
			return 1;
	return (creg(c, 0x28) & 31) != 4;
}

static s32 low_pass(int c, Chan *k, s32 sample)
{
	/* envelope step */
	static const int target_reg[4] = { 0x30, 0x34, 0x38, 0x3c };	/* FLV1..FLV4 */
	const u32 r40 = creg(c, 0x40), r44 = creg(c, 0x44);
	const int st = k->eg_state == EG_RELEASE ? 3 : k->feg_state;
	const u32 r = st == 0 ? (r40 >> 8) & 31 : st == 1 ? r40 & 31 : st == 2 ? (r44 >> 8) & 31 : r44 & 31;
	u32 delta = (u32)decay_step[rate(c, (int)r)];
	const u32 target = (u32)(creg(c, target_reg[st]) & 0x1fff) << 16;
	if (k->feg < target)
		k->feg += delta > target - k->feg ? target - k->feg : delta;
	else if (k->feg > target)
		k->feg -= delta > k->feg - target ? k->feg - target : delta;
	else if (st < 2)
		k->feg_state = st + 1;
	/* filter */
	const int bits = 30;
	const u32 fv = k->feg >> 16, ex = fv >> 9, mant = (fv & 0x1ff) | 0x200;
	u64 a0 = ((u64)mant << 30) >> ((15 - ex) * 2);
	a0 *= (mant - 1) / 8;
	a0 >>= 47 - bits;
	s64 f = ((s64)mant << ex) << (bits - 25);
	f += (s64)q_table[creg(c, 0x28) & 31] * f / 4096;
	const s64 b1 = 128ll * 1024 * (1 << (bits - 16)) - (f + (s64)a0);
	const s64 b2 = 64ll * 1024 * (1 << (bits - 16)) - f;
	if (ex == 0)
		k->f_fract = 0;
	const s64 mac = -(s64)a0 * sample + b1 * k->f_prev1 - b2 * k->f_prev2 - k->f_fract;
	s32 out = (s32)(mac >> bits);
	k->f_fract = (s32)(((s64)out << bits) - mac);
	k->f_prev2 = k->f_prev1;
	out = out < -512 * 1024 ? -512 * 1024 : out > 512 * 1024 - 1 ? 512 * 1024 - 1 : out;
	k->f_prev1 = out;
	return out;
}

/* ---- DSP state (run in dsp_step, below) ---- */
typedef struct
{
	s32 temp[128], mems[32], mixs[16], efreg[16];
	s32 frc, y, adrs;	/* FRC_REG, Y_REG, ADRS_REG */
	u32 dec;			/* MDEC_CT */
} Dsp;
static Dsp dsp;
static int dsp_active(void);

/* one output sample of channel c: advances position and envelope, adds to the mix */
static void channel(int c, float *l, float *r)
{
	Chan *k = &ch[c];
	if (!k->on)
		return;
	const u32 r18 = creg(c, 0x18), oct = (r18 >> 11) & 15, fns = r18 & 0x3ff;
	const int sh = (int)((oct ^ 8) - 8) + 6;
	float lfo_amp;
	const float lfo_pitch = lfo_step(c, k, &lfo_amp);
	u32 step = sh >= 0 ? (0x400u | fns) << sh : (0x400u | fns) >> -sh;
	if (lfo_pitch != 1.0f)
		step = (u32)((float)step * lfo_pitch);
	s32 s = k->cur + (s32)(((s64)(k->next - k->cur) * k->frac) >> 16);	/* linear interpolation */
	if (feg_active(c))
		s = low_pass(c, k, s << 4) >> 4;
	step_eg(c, k);
	const u32 r28 = creg(c, 0x28), r24 = creg(c, 0x24);
	const u32 att = (u32)(k->eg >> 16);
	const float base = (float)s * (((r28 >> 6) & 1) ? 1.0f : eg_gain[att]) * tl_gain[(r28 >> 8) & 255] * lfo_amp;	/* VOFF: no envelope */
	/* the DSP input: MIXS[ISEL] (20-bit: the sample << 4) at the IMXL send level */
	const u32 r20 = creg(c, 0x20);
	const float imxl = level_gain[(r20 >> 4) & 15];
	if (imxl > 0)
		dsp.mixs[r20 & 15] += (s32)(base * imxl * 16);
	/* the direct path: a channel at full level contributes its whole sample */
	const float v = base;
	float g = level_gain[(r24 >> 8) & 15];
	const u32 pan = r24 & 31;
	float side = (pan & 15) == 15 ? 0.0f : level_gain[15 - (pan & 15)];
	if (g == 0 && !dsp_active())
	{
		/* no DSP program: a channel that only feeds the DSP is heard directly (as Flycast with
		   its DSP off) */
		g = imxl;
		side = 1.0f;
	}
	if (g > 0)
	{
		*l += v * g * ((pan & 16) ? 1.0f : side);	/* DIPAN bit 4: the right side is attenuated */
		*r += v * g * ((pan & 16) ? side : 1.0f);
		extern double aica_stat[66];
		aica_stat[c] += (double)(v * g) * (double)(v * g);
	}
	/* advance */
	k->frac += step;
	if (k->frac >> 16)
	{
		const u32 adv = k->frac >> 16;
		k->frac &= 0xffff;
		k->pos += adv;
		const u32 lsa = creg(c, 0x08), lea = creg(c, 0x0c), r0 = creg(c, 0x00);
		if (k->pos >= lea && lea)
		{
			k->lp = 1;
			if ((r0 >> 9) & 1)
			{
				k->pos = lsa + (k->pos - lea);
				const u32 fmt = (r0 >> 7) & 3;
				if (fmt == 2)
				{
					/* ADPCM: the loop repeats the same data, from the decoder state at LSA */
					k->ad_prev = k->loop_prev, k->ad_step = k->loop_step;
					k->have = lsa;
				}
				else if (fmt == 3)
				{
					/* ADPCM long stream: the loop is a ring buffer the game keeps refilling, so the
					   decoder carries on (its state is not restored, as on the chip and in
					   Flycast); before, decoding stopped until the position passed the old end
					   again, holding one value for a whole lap: dropouts and level jumps */
					const u32 sa = ((r0 & 0x7f) << 16) | creg(c, 0x04);
					adpcm_nibble(sa, k, lsa);
					k->have = lsa;
				}
			}
			else
			{
				k->on = 0;	/* one-shot: the sample ended */
				k->eg = 0x3ff << 16;
				release(c);
				return;
			}
		}
		k->cur = fetch(c, k, k->pos);
		const u32 nx = k->pos + 1 >= lea && lea && ((r0 >> 9) & 1) ? lsa : k->pos + 1;
		if (((r0 >> 7) & 3) >= 2)
			k->next = nx == k->pos + 1 ? fetch(c, k, nx) : k->cur;
		else
			k->next = fetch(c, k, nx);
	}
}

/* ---- interrupts -------------------------------------------------------------------------- */
static void update_irq(void)
{
	const u32 p = rg(R_SCIEB) & rg(R_SCIPD) & 0x7ff;
	u32 lvl = 0;
	for (int i = 0; i < 11; i++)
		if (p & (1u << i))
		{
			const int b = i > 7 ? 7 : i;	/* bit 7 of SCILVx also covers interrupts 8-10 */
			lvl = ((rg(R_SCILV0) >> b) & 1) | (((rg(R_SCILV0 + 4) >> b) & 1) << 1) | (((rg(R_SCILV0 + 8) >> b) & 1) << 2);
			break;
		}
	/* the line to the ARM is latched with its level and stays up until the driver writes M */
	if (p && !clk.irq_out)
	{
		clk.irq_out = 1;
		clk.irq_level = lvl;
	}
	rs(R_L, (u16)clk.irq_level);
	arm.fiq = clk.irq_out;
	hw_ext_irq(1, (rg(R_MCIEB) & rg(R_MCIPD) & 0x7ff) != 0);
}

static void pend(u32 bits)
{
	rs(R_SCIPD, (u16)(rg(R_SCIPD) | bits));
	rs(R_MCIPD, (u16)(rg(R_MCIPD) | bits));
	update_irq();
}

/* ---- registers ----------------------------------------------------------------------------- */
static u32 reg_read(u32 off, int size)
{
	off &= 0xffff;
	u32 v;
	switch (off & ~3u)
	{
	case R_MON:
	{
		const int c = (rg(R_MSLC) >> 8) & 63;
		v = ((u32)ch[c].lp << 15) | ((u32)(ch[c].on ? ch[c].eg_state : EG_RELEASE) << 13)
			| ((u32)(ch[c].on ? ch[c].eg >> 16 : 0x3ff) << 3);
		ch[c].lp = 0;
		break;
	}
	case R_CA:
		v = ch[(rg(R_MSLC) >> 8) & 63].pos & 0xffff;
		break;
	default:
		v = rg(off);
	}
	if (size == 1)
		return (off & 1) ? (v >> 8) & 0xff : v & 0xff;
	return v;
}

static void reg_write(u32 off, u32 v, int size)
{
	off &= 0xffff;
	const u32 a = off & ~3u;
	if (off & 2)
		return;	/* upper half of a register slot: unused */
	u16 old = rg(a), nv;
	if (size == 1)
		nv = (off & 1) ? (u16)((old & 0x00ff) | (v & 0xff) << 8) : (u16)((old & 0xff00) | (v & 0xff));
	else
		nv = (u16)v;
	if (a < 0x2000 && (a & 0x7f) == 0)
	{
		rs(a, nv & 0x7fff);	/* KYONEX is not stored */
		if (nv & 0x8000)
			key_on_ex();
		return;
	}
	switch (a)
	{
	case R_SCIPD:	/* only bit 5 (CPU interrupt) can be set by writing */
		if (nv & 0x20)
			pend(0), rs(R_SCIPD, (u16)(rg(R_SCIPD) | 0x20)), update_irq();
		return;
	case R_SCIRE:
		rs(R_SCIPD, (u16)(rg(R_SCIPD) & ~nv));
		update_irq();
		return;
	case R_MCIPD:
		if (nv & 0x20)
			rs(R_MCIPD, (u16)(rg(R_MCIPD) | 0x20)), update_irq();
		return;
	case R_MCIRE:
		rs(R_MCIPD, (u16)(rg(R_MCIPD) & ~nv));
		update_irq();
		return;
	case R_M:
		if (nv & 1)
		{
			clk.irq_out = 0;	/* accept: re-raised at once if another interrupt is pending */
			update_irq();
		}
		return;
	case R_L:
		return;
	case R_ARMRST:
	{
		const int was = rg(a) & 1;
		rs(a, nv);
		if (!(nv & 1) && was && !getenv("M1_NOARM"))	/* M1_NOARM=1: the ARM stays in reset (no sound) */
		{
			arm7_reset();
			arm.running = 1;
			{
				u32 m1_current_function(void);
				void m1_print_stack(FILE *f);
				printf("AICA: ARM released from reset (SH-4 function %08x)\n", m1_current_function());
				m1_print_stack(stdout);
			}
		}
		else if (nv & 1)
			arm.running = 0;
		return;
	}
	}
	rs(a, nv);
	if (a >= R_TIMA && a <= R_TIMA + 8)
		clk.timer_acc[(a - R_TIMA) / 4] = 0;	/* a write restarts the prescaler */
	if (a == R_SCIEB || a == R_MCIEB || (a >= R_SCILV0 && a <= R_SCILV0 + 8))
		update_irq();
}

/* ---- buses ----------------------------------------------------------------------------- */
u32 aica_arm_read(u32 addr, int size)
{
	addr &= 0xffffff;
	if (addr < 0x800000)
	{
		const u32 o = addr & (RAM_SIZE - 1);
		u32 v = 0;
		memcpy(&v, aica_ram + o, (size_t)size);
		static int log = -1;
		if (log < 0)
			log = getenv("M1_CMDLOG") != 0;
		if (log && o == 0x13400 && v)
			printf("cmdlog: sample %llu ARM pc %06x reads flag %08x (mode %02x)\n", (unsigned long long)clk.samples, arm.pc, v, arm.cpsr & 0x1f);
		return v;
	}
	if (size == 4)
		return reg_read(addr - 0x800000, 4);
	return reg_read(addr - 0x800000, size);
}

void aica_arm_write(u32 addr, u32 value, int size)
{
	addr &= 0xffffff;
	if (addr < 0x800000)
	{
		/* idle skip: only a write that changes memory counts (the idle loop pushes the same
		   return address on every iteration) */
		if (memcmp(aica_ram + (addr & (RAM_SIZE - 1)), &value, (size_t)size))
			arm_wrote = 1;
		memcpy(aica_ram + (addr & (RAM_SIZE - 1)), &value, (size_t)size);
		static int log = -1;
		if (log < 0)
			log = getenv("M1_CMDLOG") != 0;
		if (log && (addr & (RAM_SIZE - 1)) >= 0x14000 && (addr & (RAM_SIZE - 1)) < 0x14300)
			printf("cmdlog: sample %llu ARM pc %06x writes %06x = %08x\n", (unsigned long long)clk.samples, arm.pc, addr, value);
		return;
	}
	arm_wrote = 1;
	reg_write(addr - 0x800000, value, size);
}

u64 m1_cycles(void);

/* SH-4 side: 0x00700000-0x0070FFFF registers, 0x00710000-0x00710008 clock */
u32 aica_sh4_read(u32 p, int size)
{
	if (p >= 0x00710000u)
	{
		const u32 secs = 1577923200u + (u32)(m1_cycles() / 200000000u);	/* 2000-01-01, seconds since 1950 */
		return (p & 4) ? secs & 0xffff : secs >> 16;
	}
	return reg_read(p - 0x00700000u, size);
}

void aica_sh4_write(u32 p, u32 v, int size)
{
	if (p >= 0x00710000u)
		return;
	if (size == 4)
		reg_write(p - 0x00700000u, v & 0xffff, 2);
	else
		reg_write(p - 0x00700000u, v, size);
}

/* ---- output -------------------------------------------------------------------------------- */
static SDL_AudioDeviceID dev;
static FILE *wav;
static u32 wav_bytes;
static s16 block[2 * 1024];	/* 512-sample blocks (11.6 ms) */
static int nblock;
static u64 dropped, underruns;

static void wav_header(void)
{
	const u32 rate = 44100, byte_rate = rate * 4;
	u8 h[44];
	memcpy(h, "RIFF", 4);
	const u32 riff = 36 + wav_bytes, fmt_len = 16, data = wav_bytes;
	const u16 pcm = 1, chans = 2, align = 4, bits = 16;
	memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	memcpy(h + 16, &fmt_len, 4);
	memcpy(h + 20, &pcm, 2);
	memcpy(h + 22, &chans, 2);
	memcpy(h + 24, &rate, 4);
	memcpy(h + 28, &byte_rate, 4);
	memcpy(h + 32, &align, 2);
	memcpy(h + 34, &bits, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &data, 4);
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, 44, wav);
	fseek(wav, 0, SEEK_END);
}

static void output_init(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	const char *w = getenv("M1_WAV");
	const char *record_name(const char *path);	/* render.c: name-1.wav when name.wav exists */
	if (w && (wav = fopen(record_name(w), "wb")))
		wav_header();
	if (getenv("M1_HEADLESS") || getenv("M1_NOSOUND"))
		return;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		printf("AICA: no audio (%s)\n", SDL_GetError());
		return;
	}
	SDL_AudioSpec want = { 0 }, have;
	want.freq = 44100;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	dev = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
	printf("AICA: audio %s\n", dev ? "on" : SDL_GetError());
}

static void flush_block(void)
{
	if (!nblock)
		return;
	if (wav)
	{
		fwrite(block, 4, (size_t)nblock, wav);
		wav_bytes += (u32)nblock * 4;
		if ((wav_bytes & 0xfffff) < (u32)nblock * 4)
			wav_header();
	}
	if (dev)
	{
		/* the game makes 735 samples per 60 Hz frame and the device plays 44,100 a second, so
		   the queue stays short when paced; keep at most 0.1 s (latency), drop beyond (unpaced
		   runs), and count underruns (an empty queue: audible gaps) */
		const Uint32 queued = SDL_GetQueuedAudioSize(dev);
		static int playing;
		if (queued == 0 && playing)
		{
			underruns++;
			SDL_PauseAudioDevice(dev, 1);	/* refill the cushion before playing on */
			playing = 0;
		}
		if (queued < 44100 * 4 / 10)
			SDL_QueueAudio(dev, block, (Uint32)nblock * 4);
		else
			dropped += (u64)nblock;
		if (!playing && SDL_GetQueuedAudioSize(dev) >= 2048 * 4)	/* 46 ms cushion */
		{
			SDL_PauseAudioDevice(dev, 0);
			playing = 1;
		}
	}
	nblock = 0;
}

/* ---- DSP ---------------------------------------------------------------------------------
 * 128 steps per sample. Each step (MPRO, 4 x 16 bits at 0x3400 + 16 * step) reads an input
 * (MEMS or MIXS << 4), multiplies X (the input or TEMP) by Y (FRC_REG, COEF or Y_REG fields),
 * adds B (TEMP or the accumulator, optionally negated), and through the shifter writes TEMP,
 * MEMS, EFREG, FRC_REG, ADRS_REG or the ring buffer in sound RAM (RBP, RBL; 16-bit float or
 * linear words, on odd steps). TEMP and the ring buffer are addressed relative to MDEC_CT,
 * which counts down once per sample. */
enum { R_RBP = 0x2804, R_COEF = 0x3000, R_MADRS = 0x3200, R_MPRO = 0x3400, R_EFSDL = 0x2000 };

static int dsp_active(void)
{
	static int off = -1;	/* M1_NODSP=1: no DSP (dry signal only) */
	if (off < 0)
		off = getenv("M1_NODSP") != 0;
	if (off)
		return 0;
	for (u32 i = 0; i < 128 * 4; i++)
		if (rg(R_MPRO + 4 * i))
			return 1;
	return 0;
}

static s32 sx24(s32 v) { return (s32)((u32)v << 8) >> 8; }
static s32 clamp24(s64 v) { return v > 0x7fffff ? 0x7fffff : v < -0x800000 ? -0x800000 : (s32)v; }

static u16 pack16(s32 val)
{
	const int sign = (val >> 23) & 1;
	u32 temp = (u32)(val ^ (val << 1)) & 0xffffff;
	int exponent = 0;
	for (int k = 0; k < 12; k++, exponent++)
	{
		if (temp & 0x800000)
			break;
		temp <<= 1;
	}
	val = exponent < 12 ? (val << exponent) & 0x3fffff : val << 11;
	val >>= 11;
	return (u16)((val & 0x7ff) | sign << 15 | exponent << 11);
}

static s32 unpack16(u16 val)
{
	const int sign = (val >> 15) & 1;
	int exponent = (val >> 11) & 15;
	s32 uval = (s32)(val & 0x7ff) << 11;
	if (exponent > 11)
	{
		exponent = 11;
		uval |= sign << 22;
	}
	else
		uval |= (sign ^ 1) << 22;
	uval |= sign << 23;
	uval = sx24(uval);
	return uval >> exponent;
}

static void dsp_step(void)
{
	memset(dsp.efreg, 0, sizeof(dsp.efreg));
	const u32 rbp = (u32)(rg(R_RBP) & 0xfff) << 11, rbl = 0x2000u << ((rg(R_RBP) >> 13) & 3);	/* words */
	s32 acc = 0, shifted = 0, memval = 0;
	for (u32 step = 0; step < 128; step++)
	{
		const u16 i0 = rg(R_MPRO + step * 16), i1 = rg(R_MPRO + step * 16 + 4), i2 = rg(R_MPRO + step * 16 + 8),
				  i3 = rg(R_MPRO + step * 16 + 12);
		if (!i0 && !i1 && !i2 && !i3)
			continue;
		const u32 tra = (i0 >> 8) & 0x7f, twt = (i0 >> 7) & 1, twa = i0 & 0x7f;
		const u32 xsel = (i1 >> 15) & 1, ysel = (i1 >> 13) & 3, ira = (i1 >> 7) & 0x3f, iwt = (i1 >> 6) & 1, iwa = (i1 >> 1) & 0x1f;
		const u32 table = (i2 >> 15) & 1, mwt = (i2 >> 14) & 1, mrd = (i2 >> 13) & 1, ewt = (i2 >> 12) & 1, ewa = (i2 >> 8) & 15,
				  adrl = (i2 >> 7) & 1, frcl = (i2 >> 6) & 1, shift = (i2 >> 4) & 3, yrl = (i2 >> 3) & 1, negb = (i2 >> 2) & 1,
				  zero = (i2 >> 1) & 1, bsel = i2 & 1;
		const u32 nofl = (i3 >> 15) & 1, masa = (i3 >> 9) & 0x3f, adreb = (i3 >> 8) & 1, nxadr = (i3 >> 7) & 1;
		/* input */
		s32 inputs = 0;
		if (ira <= 0x1f)
			inputs = dsp.mems[ira];
		else if (ira <= 0x2f)
			inputs = dsp.mixs[ira - 0x20] << 4;
		inputs = sx24(inputs);
		if (iwt)
		{
			dsp.mems[iwa] = memval;
			if (ira == iwa)
				inputs = memval;
		}
		/* operands */
		s32 b = 0;
		if (!zero)
		{
			b = bsel ? acc : sx24(dsp.temp[(tra + dsp.dec) & 0x7f]);
			if (negb)
				b = -b;
		}
		const s32 x = xsel ? inputs : sx24(dsp.temp[(tra + dsp.dec) & 0x7f]);
		s32 y;
		if (ysel == 0)
			y = dsp.frc;
		else if (ysel == 1)
			y = (s16)rg(R_COEF + step * 4) >> 3;
		else if (ysel == 2)
			y = (dsp.y >> 11) & 0x1fff;
		else
			y = (dsp.y >> 4) & 0x0fff;
		if (yrl)
			dsp.y = inputs;
		/* shifter (on the accumulator of the previous step) */
		if (shift == 0)
			shifted = clamp24(acc);
		else if (shift == 1)
			shifted = clamp24((s64)acc * 2);
		else if (shift == 2)
			shifted = sx24(acc * 2);
		else
			shifted = sx24(acc);
		/* multiply-accumulate */
		y = (s32)((u32)y << 19) >> 19;	/* 13-bit signed */
		acc = (s32)(((s64)x * y) >> 12) + b;
		if (twt)
			dsp.temp[(twa + dsp.dec) & 0x7f] = shifted;
		if (frcl)
			dsp.frc = shift == 3 ? shifted & 0xfff : (shifted >> 11) & 0x1fff;
		if ((mrd || mwt) && (step & 1))
		{
			u32 addr = rg(R_MADRS + masa * 4);
			if (!table)
				addr += dsp.dec;
			if (adreb)
				addr += (u32)dsp.adrs & 0xfff;
			if (nxadr)
				addr++;
			addr = table ? addr & 0xffff : addr & (rbl - 1);
			const u32 byte = (rbp + addr * 2) & (RAM_SIZE - 2);
			if (mrd)
			{
				u16 w;
				memcpy(&w, aica_ram + byte, 2);
				memval = nofl ? (s32)(s16)w << 8 : unpack16(w);
			}
			if (mwt)
			{
				const u16 w = nofl ? (u16)(shifted >> 8) : pack16(shifted);
				memcpy(aica_ram + byte, &w, 2);
			}
		}
		if (adrl)
			dsp.adrs = shift == 3 ? (shifted >> 12) & 0xfff : inputs >> 16;
		if (ewt)
			dsp.efreg[ewa] += shifted >> 8;
	}
	dsp.dec--;
	memset(dsp.mixs, 0, sizeof(dsp.mixs));
}

/* the DSP outputs join the mix at their send levels (EFSDL) and pans (EFPAN), at full scale */
double aica_stat[66];	/* M1_AICASTAT: energy per channel (0-63), DSP output (64), samples (65) */
static void dsp_mix(float *l, float *r)
{
	for (u32 i = 0; i < 16; i++)
	{
		const u32 e = rg(R_EFSDL + 4 * i);
		const float g = level_gain[(e >> 8) & 15];
		if (g == 0 || !dsp.efreg[i])
			continue;
		const s32 v16 = dsp.efreg[i] > 32767 ? 32767 : dsp.efreg[i] < -32768 ? -32768 : dsp.efreg[i];
		const float v = (float)v16 * g;
		const u32 pan = e & 31;
		const float side = (pan & 15) == 15 ? 0.0f : level_gain[15 - (pan & 15)];
		*l += v * ((pan & 16) ? 1.0f : side);
		*r += v * ((pan & 16) ? side : 1.0f);
		aica_stat[64] += (double)v * v;
	}
}

static void sample(void)
{
	static int per_sample = -1;	/* M1_ARMRATE: ARM instructions per output sample */
	if (per_sample < 0)
		per_sample = getenv("M1_ARMRATE") ? atoi(getenv("M1_ARMRATE")) : ARM_PER_SAMPLE;
	arm7_run(per_sample);
	/* timers A-C: count every 2^prescale samples, interrupt on overflow */
	u32 irq = 1u << 10;	/* sample interval */
	for (int t = 0; t < 3; t++)
	{
		const u32 r = rg(R_TIMA + 4 * t), pre = (r >> 8) & 7;
		if (++clk.timer_acc[t] >= (1u << pre))
		{
			clk.timer_acc[t] = 0;
			u32 cnt = (r & 0xff) + 1;
			if (cnt > 0xff)
			{
				cnt = 0;
				irq |= 1u << (6 + t);
			}
			rs(R_TIMA + 4 * t, (u16)((r & 0xff00) | cnt));
		}
	}
	pend(irq);
	float l = 0, r = 0;
	for (int c = 0; c < 64; c++)
		channel(c, &l, &r);
	if (dsp_active())
	{
		dsp_step();
		dsp_mix(&l, &r);
	}
	else
		memset(dsp.mixs, 0, sizeof(dsp.mixs));
	static float volume = -1;	/* M1_VOLUME: output volume in percent ([audio] volume) */
	if (volume < 0)
		volume = getenv("M1_VOLUME") ? (float)atof(getenv("M1_VOLUME")) / 100.0f : 1.0f;
	const float mv = level_gain[rg(R_MVOL) & 15] * ((rg(R_MVOL) >> 8) & 1 ? 0.25f : 1.0f) * volume;	/* DAC18B: 18-bit output */
	l *= mv;
	r *= mv;
	clk.samples++;
	aica_stat[65] += 1;
	{
		/* M1_AICADUMP=n: list the playing channels at output sample n */
		static long long want = -2;
		if (want == -2)
			want = getenv("M1_AICADUMP") ? atoll(getenv("M1_AICADUMP")) : -1;
		if ((long long)clk.samples == want)
			for (int c = 0; c < 64; c++)
				if (ch[c].on)
				{
					const u32 r0 = creg(c, 0);
					printf("ch%02d r28 %04x flv %04x %04x %04x %04x %04x feg %04x %04x kyonb %u fmt %u loop %u sa %06x lsa %04x lea %04x pos %04x oct %2d fns %03x eg %u/%03x ar %u d1 %u d2 %u rr %u dl %u krs %u tl %02x disdl %x dipan %02x imxl %x isel %x\n",
						   c, creg(c, 0x28), creg(c, 0x2c), creg(c, 0x30), creg(c, 0x34), creg(c, 0x38), creg(c, 0x3c), creg(c, 0x40), creg(c, 0x44), (r0 >> 14) & 1, (r0 >> 7) & 3, (r0 >> 9) & 1, ((r0 & 0x7f) << 16) | creg(c, 4), creg(c, 8), creg(c, 0xc), ch[c].pos,
						   (int)(((creg(c, 0x18) >> 11) & 15) ^ 8) - 8, creg(c, 0x18) & 0x3ff, ch[c].eg_state, (u32)(ch[c].eg >> 16),
						   creg(c, 0x10) & 31, (creg(c, 0x10) >> 6) & 31, (creg(c, 0x10) >> 11) & 31, creg(c, 0x14) & 31,
						   (creg(c, 0x14) >> 5) & 31, (creg(c, 0x14) >> 10) & 15, creg(c, 0x28) >> 8, (creg(c, 0x24) >> 8) & 15,
						   creg(c, 0x24) & 31, (creg(c, 0x20) >> 4) & 15, creg(c, 0x20) & 15);
				}
	}
	if (m1_resim)
		return;	/* re-simulated netplay frames are not heard again */
	const s32 sl = (s32)l, sr = (s32)r;
	block[2 * nblock] = (s16)(sl > 32767 ? 32767 : sl < -32768 ? -32768 : sl);
	block[2 * nblock + 1] = (s16)(sr > 32767 ? 32767 : sr < -32768 ? -32768 : sr);
	if (++nblock == 512)
		flush_block();
}

/* the virtual clock advanced by n SH-4 cycles (200 MHz) */
double aica_ms;	/* M1_PERF: wall time spent in the sound chip */
void aica_tick(u32 n)
{
	tables();
	output_init();
	clk.acc += n * 441u;	/* 44,100 / 200,000,000 = 441 / 2,000,000 */
	if (clk.acc < 2000000u)
		return;
	const u64 t0 = SDL_GetPerformanceCounter();
	while (clk.acc >= 2000000u)
	{
		clk.acc -= 2000000u;
		sample();
	}
	aica_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

void aica_init(void)
{
	tables();
	rs(R_ARMRST, 1);	/* the ARM starts held in reset */
	arm7_reset();
}

void aica_report(FILE *f)
{
	flush_block();
	if (wav)
		wav_header(), fflush(wav);
	if (getenv("M1_ARMPROF"))
	{
		extern u32 arm_pc_hist[0x2000];
		u64 total = 0;
		for (int i = 0; i < 0x2000; i++)
			total += arm_pc_hist[i];
		for (int n = 0; n < 16; n++)
		{
			int best = 0;
			for (int i = 0; i < 0x2000; i++)
				if (arm_pc_hist[i] > arm_pc_hist[best])
					best = i;
			if (!arm_pc_hist[best])
				break;
			fprintf(f, "ARM pc %06x: %.1f%% (%08x)%c", best * 4, 100.0 * arm_pc_hist[best] / (double)total, aica_arm_read((u32)best * 4, 4), 10);
			arm_pc_hist[best] = 0;
		}
	}
	if (getenv("M1_AICASTAT") && aica_stat[65] > 0)
	{
		fprintf(f, "AICA loudness (RMS of each source over the run):");
		for (int c = 0; c < 65; c++)
			if (aica_stat[c] > 0)
				fprintf(f, " %s%d %.0f", c < 64 ? "ch" : "dsp", c < 64 ? c : 0, sqrt(aica_stat[c] / aica_stat[65]));
		fprintf(f, "%c", 10);
	}
	if (getenv("M1_DUMP"))
	{
		FILE *d = fopen("m1_aram.bin", "wb");
		if (d)
			fwrite(aica_ram, 1, RAM_SIZE, d), fclose(d);
	}
	fprintf(f, "AICA: MVOL register %04x\n", rg(R_MVOL));
	fprintf(f, "AICA: SCIEB %04x SCILV %02x %02x %02x MCIEB %04x; %llu FIQs, %llu instructions in FIQ mode\n", rg(R_SCIEB),
			rg(R_SCILV0), rg(R_SCILV0 + 4), rg(R_SCILV0 + 8), rg(R_MCIEB), (unsigned long long)arm.fiqs,
			(unsigned long long)arm.fiq_instructions);
	fprintf(f, "AICA: %llu samples, ARM %s, %llu ARM instructions (%llu skipped idle), %llu samples not queued, %llu audio underruns\n",
			(unsigned long long)clk.samples, arm.running ? "running" : "in reset", (unsigned long long)arm.instructions,
			(unsigned long long)arm.idle_skipped, (unsigned long long)dropped, (unsigned long long)underruns);
}

/* ---- rollback state ----------------------------------------------------------------------- */
u32 aica_state_size(void) { return RAM_SIZE + sizeof(regs) + sizeof(ch) + sizeof(clk) + sizeof(arm) + sizeof(dsp); }
void aica_state_save(u8 *p)
{
	memcpy(p, aica_ram, RAM_SIZE), p += RAM_SIZE;
	memcpy(p, regs, sizeof(regs)), p += sizeof(regs);
	memcpy(p, ch, sizeof(ch)), p += sizeof(ch);
	memcpy(p, &clk, sizeof(clk)), p += sizeof(clk);
	memcpy(p, &arm, sizeof(arm)), p += sizeof(arm);
	memcpy(p, &dsp, sizeof(dsp));
}
void aica_state_load(const u8 *p)
{
	memcpy(aica_ram, p, RAM_SIZE), p += RAM_SIZE;
	memcpy(regs, p, sizeof(regs)), p += sizeof(regs);
	memcpy(ch, p, sizeof(ch)), p += sizeof(ch);
	memcpy(&clk, p, sizeof(clk)), p += sizeof(clk);
	memcpy(&arm, p, sizeof(arm)), p += sizeof(arm);
	memcpy(&dsp, p, sizeof(dsp));
}
