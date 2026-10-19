/* AICA ARM7 core (arm7.c) and the memory interface it uses (aica.c) */
#ifndef ARM7_H
#define ARM7_H
#include "m1.h"

typedef struct
{
	u32 r[16];				/* current registers (r15 = instruction + 8 while executing) */
	u32 pc, cpsr;
	u32 spsr[6];			/* per bank: -, FIQ, IRQ, SVC, abort, undefined */
	u32 r13_14[6][2];		/* banked r13 / r14: user+system, FIQ, IRQ, SVC, abort, undefined */
	u32 usr_r8_12[5], fiq_r8_12[5];
	int fiq;				/* FIQ line from the AICA interrupt controller */
	int running;			/* ARMRST clear */
	u64 instructions, fiq_instructions, fiqs, idle_skipped;
} Arm7;
extern Arm7 arm;

void arm7_reset(void);
void arm7_run(int instructions);
extern int arm_wrote;	/* aica.c sets it on ARM writes (idle skip) */

/* ARM-side bus: sound RAM 0x000000-0x7FFFFF (2 MB mirrored), AICA registers from 0x800000 */
u32 aica_arm_read(u32 addr, int size);
void aica_arm_write(u32 addr, u32 value, int size);
#endif
