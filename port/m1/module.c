/* Loads the game module, sc_game.dll: the game's code translated to C and compiled on the
 * player's PC from their own disc (tools/build.py), so the executable itself holds none of it.
 * The module reaches the runtime only through the table below (sh4rt.h, Sh4Host).
 * M1_GAME: the module's path (default: sc_game.dll, on Linux sc_game.so, next to the executable).
 * M1_BUILD_YES=1 (tests): build a missing module without asking. */
#include "m1.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u32 sh4_tag_lo, sh4_tag_len;
void sh4_tag_write(u32 offset);

static Sh4Host host = {
	SH4_MODULE_ABI, 0, &sh4_tag_lo, &sh4_tag_len, &sh4_loop_cycles,
	sh4_hw_read, sh4_hw_write, sh4_tag_write, sh4_call, sh4_jump, sh4_ctx_slot, sh4_ctx_resumed,
	sh4_hook, sh4_loop_poll, sh4_pref, sh4_unimpl,
};
static const Sh4Module *mod;
static char mod_path[1024];

/* the module's path: M1_GAME, else sc_game.dll next to the executable */
const char *module_path(void)
{
	if (mod_path[0])
		return mod_path;
	const char *e = getenv("M1_GAME");
	if (e && *e)
		snprintf(mod_path, sizeof(mod_path), "%s", e);
	else
	{
#ifdef _WIN32
		GetModuleFileNameA(0, mod_path, sizeof(mod_path) - 16);
		char *slash = strrchr(mod_path, 92);	/* backslash */
		strcpy(slash ? slash + 1 : mod_path, "sc_game.dll");
#else
		const ssize_t n = readlink("/proc/self/exe", mod_path, sizeof(mod_path) - 16);
		mod_path[n > 0 ? n : 0] = 0;
		char *slash = strrchr(mod_path, '/');
		strcpy(slash ? slash + 1 : mod_path, "sc_game.so");
#endif
	}
	return mod_path;
}

/* the translated functions (sorted by address), 0 with *why set when the module is missing,
   from another build of the runtime, or not a module */
const Sh4Module *module_load(const char **why)
{
	if (mod)
		return mod;
	typedef const Sh4Module *(*attach_fn)(const Sh4Host *);
	attach_fn attach;
#ifdef _WIN32
	HMODULE h = LoadLibraryA(module_path());
	if (!h)
		return *why = "not found", (const Sh4Module *)0;
	attach = (attach_fn)(void *)GetProcAddress(h, "sc_game_attach");
#else
	void *h = dlopen(module_path(), RTLD_NOW | RTLD_LOCAL);
	if (!h)
		return *why = "not found", (const Sh4Module *)0;
	attach = (attach_fn)dlsym(h, "sc_game_attach");
#endif
	if (!attach)
		*why = "not a Voldorium game module";
	else
	{
		host.ram = sh4_ram;	/* allocated by now (m1.c) */
		mod = attach(&host);
		if (!mod || mod->abi != SH4_MODULE_ABI)
			*why = "built for another version of Voldorium", mod = 0;
#ifdef SC_TRANSLATOR
		else if (strcmp(mod->translator, SC_TRANSLATOR))	/* the translator of this executable's sources */
			*why = "built for another version of Voldorium", mod = 0;
#endif
	}
	if (!mod)	/* let go of it: the builder replaces the file */
#ifdef _WIN32
		FreeLibrary(h);
#else
		dlclose(h);
#endif
	return mod;
}

/* The first start of a released Voldorium (or the first after an update): no module yet, or one
   for another version. builder/first_run.bat next to the executable builds it from the player's
   disc dump (tools/build.py --only module) in a console window of its own, after the player
   agrees to the downloads. 1: built (load it again), 0: not built, or no builder here */
int module_build(const char *gdi)
{
#ifdef _WIN32
	char dir[1024], bat[1100], cmd[3400];
	GetModuleFileNameA(0, dir, sizeof(dir) - 32);
	char *slash = strrchr(dir, 92);	/* backslash */
	if (slash)
		*slash = 0;
	snprintf(bat, sizeof(bat), "%s/builder/first_run.bat", dir);
	if (GetFileAttributesA(bat) == INVALID_FILE_ATTRIBUTES)
	{
		printf("module: no builder at %s\n", bat);
		return 0;
	}
	if (!getenv("M1_BUILD_YES") && MessageBoxA(0,
					"Voldorium builds the game from your own disc dump the first time it starts, and after an update. "
					"It takes a few minutes, once.\n\n"
					"It downloads, from their official sites, each checked against a fixed SHA-256:\n"
					"  - the compiler (llvm-mingw, about 190 MB)\n"
					"  - two small files from Flycast (the SH-4 sine table and the BIOS font)\n"
					"  - Python 3.12 (about 11 MB), only if Python is not installed\n\n"
					"Nothing of the game is downloaded: its code is translated from your disc, on this PC.\n\n"
					"Build it now?",
					"Voldorium: first start", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
		return 0;
	snprintf(cmd, sizeof(cmd), "cmd.exe /c \"\"%s\" \"%s\" \"%s\"\"", bat, gdi, dir);
	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	printf("module: building it: %s\n", cmd);
	fflush(stdout);
	if (!CreateProcessA(0, cmd, 0, 0, FALSE, CREATE_NEW_CONSOLE, 0, dir, &si, &pi))
	{
		printf("module: cannot start the builder (error %lu)\n", (unsigned long)GetLastError());
		return 0;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	printf("module: the builder finished with code %lu\n", (unsigned long)code);
	return code == 0;
#else
	(void)gdi;
	return 0;
#endif
}
