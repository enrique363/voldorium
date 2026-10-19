/* fsca lookup table, built exactly like Flycast's sh4rom_init (core/hw/sh4/sh4_rom.cpp).
 * The coefficients come from Flycast's fsca-table.h (GPL-2); compile with -I pointing at
 * Flycast's core/hw/sh4 directory. */
#include "sh4rt.h"

static const u32 sin_coefs[0x8000] = {
#include "fsca-table.h"
};

float sh4_sin_table[0x10000][2];

void sh4_fsca_init(void)
{
	for (int i = 0; i < 0x10000; i++)
		sh4_sin_table[i][0] = i < 0x8000 ? u2f(sin_coefs[i]) : -u2f(sin_coefs[i - 0x8000]);
	for (int i = 0; i < 0x10000; i++)
		sh4_sin_table[i][1] = sh4_sin_table[(i + 0x4000) & 0xffff][0];
}
