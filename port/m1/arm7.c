/* ARM7 core of the AICA sound chip (ARM7DI, ARM state only; the sound driver is uploaded by
 * the game into sound RAM and starts at address 0 when ARMRST is cleared).
 *
 * Interpreter for the ARMv4 ARM instruction set without Thumb or coprocessors: data
 * processing with all shifter operands, MRS / MSR, MUL / MLA and the long multiplies,
 * LDR / STR (word, byte, unaligned word rotate), LDRH / STRH / LDRSB / LDRSH, LDM / STM
 * (including user-bank and SPSR-restoring forms), SWP, B / BL / BX, SWI and the undefined
 * trap. Modes bank r8-r14 (FIQ) or r13-r14 (IRQ, SVC, abort, undefined) with an SPSR each.
 * R15 reads as the instruction address + 8 (+12 for a register-specified shift and for a
 * stored PC). Memory goes through aica.c (sound RAM below 0x800000, registers above).
 */
#include "m1.h"
#include "arm7.h"
#include <stdlib.h>
#include <string.h>

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1b, M_SYS = 0x1f };
enum { F_N = 1u << 31, F_Z = 1u << 30, F_C = 1u << 29, F_V = 1u << 28, F_I = 1u << 7, F_F = 1u << 6 };

Arm7 arm;

static int bank_of(u32 mode)
{
	switch (mode & 0x1f)
	{
	case M_FIQ: return 1;
	case M_IRQ: return 2;
	case M_SVC: return 3;
	case M_ABT: return 4;
	case M_UND: return 5;
	default: return 0;	/* user, system */
	}
}

static void set_mode(u32 mode)
{
	const int from = bank_of(arm.cpsr), to = bank_of(mode);
	if (from != to)
	{
		arm.r13_14[from][0] = arm.r[13];
		arm.r13_14[from][1] = arm.r[14];
		if (from == 1 || to == 1)	/* r8-r12 are banked for FIQ only */
		{
			memcpy(from == 1 ? arm.fiq_r8_12 : arm.usr_r8_12, &arm.r[8], 5 * 4);
			memcpy(&arm.r[8], to == 1 ? arm.fiq_r8_12 : arm.usr_r8_12, 5 * 4);
		}
		arm.r[13] = arm.r13_14[to][0];
		arm.r[14] = arm.r13_14[to][1];
	}
	arm.cpsr = (arm.cpsr & ~0x1fu) | (mode & 0x1f);
}

static void set_cpsr(u32 v)
{
	set_mode(v & 0x1f);
	arm.cpsr = v;
}

static u32 *spsr(void)
{
	const int b = bank_of(arm.cpsr);
	return b ? &arm.spsr[b] : &arm.cpsr;	/* user / system have none: reads give CPSR */
}

void arm7_reset(void)
{
	memset(&arm, 0, sizeof(arm));
	arm.cpsr = M_SVC | F_I | F_F;
	arm.pc = 0;
}

static int jumped;
static u32 next_pc;

static void exception(u32 mode, u32 vector, u32 ret, u32 disable)
{
	const u32 old = arm.cpsr;
	set_mode(mode);
	arm.spsr[bank_of(mode)] = old;
	arm.r[14] = ret;
	arm.cpsr |= disable;
	arm.pc = next_pc = vector;
	jumped = 1;
}

/* ---- memory -------------------------------------------------------------------------- */
static u32 rd32a(u32 a)
{
	const u32 v = aica_arm_read(a & ~3u, 4);
	const u32 rot = (a & 3) * 8;	/* unaligned LDR rotates the word */
	return rot ? (v >> rot) | (v << (32 - rot)) : v;
}

/* ---- shifter ------------------------------------------------------------------------- */
static u32 carry_out;

static u32 shift(u32 v, u32 type, u32 amount, int by_reg)
{
	const u32 c = (arm.cpsr >> 29) & 1;
	carry_out = c;
	if (by_reg)
	{
		amount &= 0xff;
		if (amount == 0)
			return v;
	}
	switch (type)
	{
	case 0:	/* LSL */
		if (amount == 0) return v;
		if (amount < 32) { carry_out = (v >> (32 - amount)) & 1; return v << amount; }
		carry_out = amount == 32 ? v & 1 : 0;
		return 0;
	case 1:	/* LSR (#0 means #32) */
		if (amount == 0 && !by_reg) amount = 32;
		if (amount < 32) { carry_out = (v >> (amount - 1)) & 1; return v >> amount; }
		carry_out = amount == 32 ? v >> 31 : 0;
		return 0;
	case 2:	/* ASR (#0 means #32) */
		if (amount == 0 && !by_reg) amount = 32;
		if (amount < 32) { carry_out = (v >> (amount - 1)) & 1; return (u32)((s32)v >> amount); }
		carry_out = v >> 31;
		return (u32)((s32)v >> 31);
	default:	/* ROR, #0 = RRX */
		if (amount == 0 && !by_reg)
		{
			carry_out = v & 1;
			return (v >> 1) | (c << 31);
		}
		amount &= 31;
		if (amount == 0) { carry_out = v >> 31; return v; }
		carry_out = (v >> (amount - 1)) & 1;
		return (v >> amount) | (v << (32 - amount));
	}
}

/* ---- execution ----------------------------------------------------------------------- */

static u32 reg_r(u32 n) { return arm.r[n]; }
static void reg_w(u32 n, u32 v)
{
	if (n == 15)
	{
		next_pc = v & ~3u;
		jumped = 1;
	}
	else
		arm.r[n] = v;
}

static int cond_ok(u32 cond)
{
	const u32 f = arm.cpsr;
	const int n = (f >> 31) & 1, z = (f >> 30) & 1, c = (f >> 29) & 1, v = (f >> 28) & 1;
	switch (cond)
	{
	case 0x0: return z;
	case 0x1: return !z;
	case 0x2: return c;
	case 0x3: return !c;
	case 0x4: return n;
	case 0x5: return !n;
	case 0x6: return v;
	case 0x7: return !v;
	case 0x8: return c && !z;
	case 0x9: return !c || z;
	case 0xa: return n == v;
	case 0xb: return n != v;
	case 0xc: return !z && n == v;
	case 0xd: return z || n != v;
	case 0xe: return 1;
	default: return 0;	/* 0xF: never on ARMv4 */
	}
}

static void set_nz(u32 r) { arm.cpsr = (arm.cpsr & ~(F_N | F_Z)) | (r & F_N) | (r ? 0 : F_Z); }
static void set_c(u32 c) { arm.cpsr = (arm.cpsr & ~F_C) | (c ? F_C : 0); }
static void set_v(u32 v) { arm.cpsr = (arm.cpsr & ~F_V) | (v ? F_V : 0); }

static u32 add_flags(u32 a, u32 b, u32 cin, int s)
{
	const u64 r = (u64)a + b + cin;
	const u32 res = (u32)r;
	if (s)
	{
		set_nz(res);
		set_c((u32)(r >> 32));
		set_v(((a ^ res) & (b ^ res)) >> 31);
	}
	return res;
}

static void data_processing(u32 op)
{
	const u32 opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
	u32 b, shc;
	int reg_shift = 0;
	if (op & (1u << 25))
	{
		const u32 imm = op & 0xff, rot = ((op >> 8) & 15) * 2;
		b = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
		shc = rot ? b >> 31 : (arm.cpsr >> 29) & 1;
	}
	else
	{
		const u32 rm = op & 15, type = (op >> 5) & 3;
		u32 v = reg_r(rm), amount;
		if (op & 0x10)
		{
			reg_shift = 1;
			if (rm == 15)
				v += 4;	/* PC + 12 with a register-specified shift */
			amount = reg_r((op >> 8) & 15);
		}
		else
			amount = (op >> 7) & 31;
		b = shift(v, type, amount, reg_shift);
		shc = carry_out;
	}
	u32 a = reg_r(rn);
	if (rn == 15 && reg_shift)
		a += 4;
	const u32 c = (arm.cpsr >> 29) & 1;
	u32 res = 0;
	int logical = 0, write = 1;
	switch (opc)
	{
	case 0x0: res = a & b; logical = 1; break;					/* AND */
	case 0x1: res = a ^ b; logical = 1; break;					/* EOR */
	case 0x2: res = add_flags(a, ~b, 1, s); break;				/* SUB */
	case 0x3: res = add_flags(b, ~a, 1, s); break;				/* RSB */
	case 0x4: res = add_flags(a, b, 0, s); break;				/* ADD */
	case 0x5: res = add_flags(a, b, c, s); break;				/* ADC */
	case 0x6: res = add_flags(a, ~b, c, s); break;				/* SBC */
	case 0x7: res = add_flags(b, ~a, c, s); break;				/* RSC */
	case 0x8: res = a & b; logical = 1; write = 0; break;		/* TST */
	case 0x9: res = a ^ b; logical = 1; write = 0; break;		/* TEQ */
	case 0xa: res = add_flags(a, ~b, 1, 1); write = 0; break;	/* CMP */
	case 0xb: res = add_flags(a, b, 0, 1); write = 0; break;	/* CMN */
	case 0xc: res = a | b; logical = 1; break;					/* ORR */
	case 0xd: res = b; logical = 1; break;						/* MOV */
	case 0xe: res = a & ~b; logical = 1; break;					/* BIC */
	case 0xf: res = ~b; logical = 1; break;						/* MVN */
	}
	if (s && logical)
	{
		set_nz(res);
		set_c(shc);
	}
	if (write)
		reg_w(rd, res);
	if (s && rd == 15)
	{
		/* exception return: CPSR = SPSR (TST/TEQ/CMP/CMN with Rd 15 are the old P forms) */
		if (bank_of(arm.cpsr))
			set_cpsr(*spsr());
	}
}

static void psr_transfer(u32 op)
{
	const int to_spsr = (op >> 22) & 1;
	if (!(op & (1u << 21)))	/* MRS */
	{
		reg_w((op >> 12) & 15, to_spsr ? *spsr() : arm.cpsr);
		return;
	}
	u32 v;
	if (op & (1u << 25))
	{
		const u32 imm = op & 0xff, rot = ((op >> 8) & 15) * 2;
		v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
	}
	else
		v = reg_r(op & 15);
	u32 mask = 0;
	if (op & (1u << 16)) mask |= 0x000000ffu;
	if (op & (1u << 17)) mask |= 0x0000ff00u;
	if (op & (1u << 18)) mask |= 0x00ff0000u;
	if (op & (1u << 19)) mask |= 0xff000000u;
	if (to_spsr)
	{
		u32 *p = spsr();
		if (p != &arm.cpsr)
			*p = (*p & ~mask) | (v & mask);
		return;
	}
	if ((arm.cpsr & 0x1f) == M_USR)
		mask &= 0xff000000u;	/* user mode may change the flags only */
	set_cpsr((arm.cpsr & ~mask) | (v & mask) | 0x10);
}

static void multiply(u32 op)
{
	const u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
	const int s = (op >> 20) & 1;
	if (!(op & (1u << 23)))
	{
		u32 r = reg_r(rm) * reg_r(rs);
		if (op & (1u << 21))
			r += reg_r(rn);	/* MLA */
		reg_w(rd, r);
		if (s)
			set_nz(r);
		return;
	}
	/* UMULL / UMLAL / SMULL / SMLAL: RdHi = rd, RdLo = rn */
	u64 r;
	if (op & (1u << 22))
		r = (u64)((s64)(s32)reg_r(rm) * (s64)(s32)reg_r(rs));
	else
		r = (u64)reg_r(rm) * reg_r(rs);
	if (op & (1u << 21))
		r += ((u64)reg_r(rd) << 32) | reg_r(rn);
	reg_w(rn, (u32)r);
	reg_w(rd, (u32)(r >> 32));
	if (s)
		arm.cpsr = (arm.cpsr & ~(F_N | F_Z)) | ((u32)(r >> 32) & F_N) | (r ? 0 : F_Z);
}

static void single_transfer(u32 op)
{
	const int pre = (op >> 24) & 1, up = (op >> 23) & 1, byte = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	const u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
	u32 off;
	if (op & (1u << 25))
		off = shift(reg_r(op & 15), (op >> 5) & 3, (op >> 7) & 31, 0);
	else
		off = op & 0xfff;
	const u32 base = reg_r(rn);
	const u32 addr = pre ? (up ? base + off : base - off) : base;
	if (load)
	{
		const u32 v = byte ? aica_arm_read(addr, 1) : rd32a(addr);
		if (!pre || wb)
			reg_w(rn, up ? base + off : base - off);
		reg_w(rd, v);
	}
	else
	{
		const u32 v = rd == 15 ? reg_r(15) + 4 : reg_r(rd);
		if (byte)
			aica_arm_write(addr, v & 0xff, 1);
		else
			aica_arm_write(addr & ~3u, v, 4);
		if (!pre || wb)
			reg_w(rn, up ? base + off : base - off);
	}
}

static void halfword_transfer(u32 op)
{
	const int pre = (op >> 24) & 1, up = (op >> 23) & 1, imm = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	const u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
	const u32 off = imm ? ((op >> 4) & 0xf0) | (op & 15) : reg_r(op & 15);
	const u32 base = reg_r(rn);
	const u32 addr = pre ? (up ? base + off : base - off) : base;
	if (load)
	{
		u32 v;
		if (sh == 1)
			v = aica_arm_read(addr & ~1u, 2);
		else if (sh == 2)
			v = (u32)(s32)(s8)aica_arm_read(addr, 1);
		else
			v = (u32)(s32)(s16)aica_arm_read(addr & ~1u, 2);
		if (!pre || wb)
			reg_w(rn, up ? base + off : base - off);
		reg_w(rd, v);
	}
	else
	{
		aica_arm_write(addr & ~1u, (rd == 15 ? reg_r(15) + 4 : reg_r(rd)) & 0xffff, 2);
		if (!pre || wb)
			reg_w(rn, up ? base + off : base - off);
	}
}

static void block_transfer(u32 op)
{
	const int pre = (op >> 24) & 1, up = (op >> 23) & 1, psr = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
	const u32 rn = (op >> 16) & 15, list = op & 0xffff;
	int n = 0;
	for (int i = 0; i < 16; i++)
		n += (list >> i) & 1;
	const u32 base = reg_r(rn);
	u32 start = up ? base : base - 4 * n;
	if (pre == up)
		start += 4;	/* IB / DA */
	const u32 end_base = up ? base + 4 * n : base - 4 * n;
	/* S bit without PC in an LDM (or any STM): transfer the user-mode registers */
	const int user_bank = psr && !(load && (list & 0x8000));
	const u32 saved_mode = arm.cpsr & 0x1f;
	if (user_bank)
		set_mode(M_USR);
	u32 a = start;
	if (load)
	{
		if (wb)
			arm.r[rn] = end_base;	/* a loaded base wins over the write-back */
		for (int i = 0; i < 16; i++)
			if (list & (1u << i))
			{
				reg_w(i, aica_arm_read(a, 4));
				a += 4;
			}
		if (user_bank)
			set_mode(saved_mode);
		else if (psr && (list & 0x8000) && bank_of(arm.cpsr))
			set_cpsr(*spsr());	/* LDM ...^ with PC: return from exception */
	}
	else
	{
		int first = 1;
		for (int i = 0; i < 16; i++)
			if (list & (1u << i))
			{
				const u32 v = i == 15 ? reg_r(15) + 4 : (i == (int)rn && !first && wb) ? end_base : reg_r(i);
				aica_arm_write(a, v, 4);
				a += 4;
				first = 0;
			}
		if (user_bank)
			set_mode(saved_mode);
		if (wb)
			arm.r[rn] = end_base;
	}
}

static void execute(u32 op)
{
	switch ((op >> 25) & 7)
	{
	case 0:
		if ((op & 0x0fc000f0u) == 0x00000090u || (op & 0x0f8000f0u) == 0x00800090u)
			multiply(op);
		else if ((op & 0x0fb00ff0u) == 0x01000090u)
		{
			/* SWP / SWPB */
			const u32 addr = reg_r((op >> 16) & 15);
			const int byte = (op >> 22) & 1;
			const u32 old = byte ? aica_arm_read(addr, 1) : rd32a(addr);
			if (byte)
				aica_arm_write(addr, reg_r(op & 15) & 0xff, 1);
			else
				aica_arm_write(addr & ~3u, reg_r(op & 15), 4);
			reg_w((op >> 12) & 15, old);
		}
		else if ((op & 0x0e000090u) == 0x00000090u && (op & 0x60))
			halfword_transfer(op);
		else if ((op & 0x0ffffff0u) == 0x012fff10u)
			reg_w(15, reg_r(op & 15));	/* BX (no Thumb on this core) */
		else if ((op & 0x0fb00000u) == 0x01000000u || (op & 0x0fb00000u) == 0x01200000u)
			psr_transfer(op);
		else
			data_processing(op);
		break;
	case 1:
		if ((op & 0x0fb00000u) == 0x03200000u)
			psr_transfer(op);
		else
			data_processing(op);
		break;
	case 2:
		single_transfer(op);
		break;
	case 3:
		if (op & 0x10)
			exception(M_UND, 0x04, arm.pc + 4, F_I);
		else
			single_transfer(op);
		break;
	case 4:
		block_transfer(op);
		break;
	case 5:
		if (op & (1u << 24))
			arm.r[14] = arm.pc + 4;	/* BL */
		reg_w(15, arm.pc + 8 + ((u32)((s32)(op << 8) >> 6)));
		break;
	case 6:
		exception(M_UND, 0x04, arm.pc + 4, F_I);	/* coprocessor transfers: no coprocessor */
		break;
	default:
		if (op & (1u << 24))
			exception(M_SVC, 0x08, arm.pc + 4, F_I);	/* SWI */
		else
			exception(M_UND, 0x04, arm.pc + 4, F_I);
		break;
	}
}

int arm_wrote;	/* set by aica.c on every ARM memory or register write */

/* Runs n instructions. Idle skip: within one call nothing outside the ARM changes (the SH-4
   does not run and the chip's timers and interrupts update between samples), so when the ARM
   returns to an instruction with the same registers and has written nothing since, it is in a
   loop that will repeat until the next call: the rest of the budget is skipped. The snapshot
   is retaken after a write or after 256 instructions without a match (M1_NOIDLE=1: off). */
void arm7_run(int n)
{
	if (!arm.running)
		return;
	static int idle_ok = -1;
	if (idle_ok < 0)
		idle_ok = !getenv("M1_NOIDLE");
	u32 snap[16], snap_cpsr = 0, snap_pc = ~0u;
	int since = 0;
	arm_wrote = 0;
	for (; n > 0; n--)
	{
		if (arm.fiq && !(arm.cpsr & F_F))
		{
			exception(M_FIQ, 0x1c, arm.pc + 4, F_I | F_F);
			arm.fiqs++;
			snap_pc = ~0u;
		}
		if (idle_ok)
		{
			if (arm_wrote || since >= 256)
			{
				snap_pc = ~0u;
				arm_wrote = 0;
			}
			if (snap_pc == ~0u)
			{
				memcpy(snap, arm.r, sizeof(snap));
				snap_cpsr = arm.cpsr;
				snap_pc = arm.pc;
				since = 0;
			}
			else if (arm.pc == snap_pc && arm.cpsr == snap_cpsr && !memcmp(snap, arm.r, 15 * 4))
			{
				arm.idle_skipped += (u64)n;
				break;
			}
			since++;
		}
		const u32 op = aica_arm_read(arm.pc, 4);
		arm.instructions++;
		{
			extern u32 arm_pc_hist[0x2000];	/* M1_ARMPROF: instructions per 4-byte pc below 0x8000 */
			if (arm.pc < 0x8000)
				arm_pc_hist[arm.pc >> 2]++;
		}
		if ((arm.cpsr & 0x1f) == M_FIQ)
			arm.fiq_instructions++;
		if (!cond_ok(op >> 28))
		{
			arm.pc += 4;
			continue;
		}
		arm.r[15] = arm.pc + 8;
		jumped = 0;
		execute(op);
		arm.pc = jumped ? next_pc : arm.pc + 4;
	}
}
