/* The game module's entry point: sc_game.dll holds the translated code (build/sweep), the fsca
 * table and this file, and is built on the player's PC from their own disc. The executable
 * calls sc_game_attach with its table of runtime functions (sh4rt.h, Sh4Host) and gets the
 * translated functions back. Compile everything in the module with -DSH4_MODULE. */
#include "sh4rt.h"

#ifndef SH4_TRANSLATOR
#define SH4_TRANSLATOR "unknown"
#endif

extern const struct sh4_fn sh4_fns[];
extern const int sh4_nfns;

Sh4Host sh4_host;

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const Sh4Module *sc_game_attach(const Sh4Host *host)
{
	static Sh4Module m;
	if (!host || host->abi != SH4_MODULE_ABI)
		return 0;
	sh4_host = *host;
	m = (Sh4Module){ SH4_MODULE_ABI, sh4_fns, sh4_nfns, sh4_fsca_init, SH4_TRANSLATOR };
	return &m;
}
