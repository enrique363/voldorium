/* 16:9 replacement pictures for the game's 4:3 backgrounds.
 *
 * The folder (M1_ART16X9; the player build: art16x9 next to the executable) holds refs.txt, one
 * line per known background: its name (as exported, tests/artdump.sh -> build/art_4x3) and the
 * 32x24 RGB fingerprint of the original picture in hex (tests/art_refs.py makes the file). A
 * replacement is <name>.png in the same folder, any 16:9 size, with the original picture
 * centred at the same scale (menus and text are still drawn by the game, at their places).
 *
 * Each frame, render.c finds the 4:3 background layer (bg_mask) and asks here for a
 * replacement. When the layer's polygons change (textures, their contents, places), its
 * fingerprint is taken
 * (artdump.c composes the layer on the CPU) and compared with the references; the closest one
 * under a mean difference of 12 of 255 that has a replacement file wins until the layer changes
 * again. The picture is read on first use (png.c). M1_ARTSWAP_LOG=1 prints each decision.
 */
#include "m1.h"
#include "ta.h"
#include <stdlib.h>
#include <string.h>

#define THUMB (32 * 24 * 3)
void art_fingerprint(const TaCtx *c, const u8 *mask, u8 *th);
u64 tex_content_id(u32 tcw, u32 tsp);
unsigned char *png_load(const char *path, unsigned *w, unsigned *h, const char **err);

typedef struct
{
	char name[64];
	u8 thumb[THUMB];
	int has_file;	/* <name>.png exists */
	int loaded;		/* 1 read, -1 could not be read */
	u8 *rgba;
	u32 w, h;
} Ref;
static Ref *refs;
static u32 nrefs;
static char dir[512];
static int ready;

static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static void init(void)
{
	ready = 1;
	const char *d = getenv("M1_ART16X9");
	if (!d || !*d)
		return;
	snprintf(dir, sizeof(dir), "%s", d);
	char path[600];
	snprintf(path, sizeof(path), "%s/refs.txt", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	static char line[THUMB * 2 + 128];
	while (fgets(line, sizeof(line), f))
	{
		char name[64], *hex = strchr(line, ' ');
		if (line[0] == '#' || !hex || hex - line >= (long)sizeof(name) || strlen(hex + 1) < THUMB * 2)
			continue;
		memcpy(name, line, (size_t)(hex - line));
		name[hex - line] = 0;
		refs = realloc(refs, (nrefs + 1) * sizeof(Ref));
		Ref *r = &refs[nrefs++];
		memset(r, 0, sizeof(*r));
		snprintf(r->name, sizeof(r->name), "%s", name);
		for (int k = 0; k < THUMB; k++)
			r->thumb[k] = (u8)(hexval(hex[1 + 2 * k]) << 4 | hexval(hex[2 + 2 * k]));
		snprintf(path, sizeof(path), "%s/%s.png", dir, r->name);
		FILE *p = fopen(path, "rb");
		if (p)
		{
			r->has_file = 1;
			fclose(p);
		}
	}
	fclose(f);
	u32 files = 0;
	for (u32 i = 0; i < nrefs; i++)
		files += refs[i].has_file;
	printf("artswap: %u known backgrounds, %u replacements in %s\n", nrefs, files, dir);
}

/* the replacement for this frame's background layer (mask: its polygons), or 0 */
const u8 *artswap_find(const TaCtx *c, const u8 *mask, u32 *w, u32 *h, int *id)
{
	if (!ready)
		init();
	if (!nrefs)
		return 0;
	/* the layer's identity: its textures and where they sit */
	u64 sig = 0xcbf29ce484222325ull;
	for (u32 i = 0; i < c->np; i++)
		if (mask[i])
		{
			const TaPoly *p = &c->p[i];
			const TaVert *v = &c->v[p->first];
			u32 x, y;
			memcpy(&x, &v->x, 4);
			memcpy(&y, &v->y, 4);
			const u64 content = tex_content_id(p->tcw, p->tsp);	/* the gallery reuses the main menu's
																   tile addresses with new pictures */
			const u32 words[7] = { p->tcw, p->tsp, p->count, x, y, (u32)content, (u32)(content >> 32) };
			for (int k = 0; k < 7; k++)
				sig = (sig ^ words[k]) * 0x100000001b3ull;
		}
	static u64 last_sig;
	static int last = -1;
	if (sig != last_sig)
	{
		last_sig = sig;
		last = -1;
		static int log = -1;
		if (log < 0)
			log = getenv("M1_ARTSWAP_LOG") != 0;
		u8 th[THUMB];
		art_fingerprint(c, mask, th);
		u32 best = ~0u;
		int bi = -1;
		for (u32 i = 0; i < nrefs; i++)
		{
			u32 diff = 0;
			for (int k = 0; k < THUMB; k++)
				diff += (u32)abs((int)th[k] - (int)refs[i].thumb[k]);
			if (diff < best)
				best = diff, bi = (int)i;
		}
		if (bi >= 0 && best < THUMB * 12 && refs[bi].has_file)
			last = bi;
		static int last_logged = -2, last_best = -2;
		if (log && (last != last_logged || bi != last_best))	/* only when the decision changes */
			printf("artswap: background changed (game mode %u): closest %s, mean difference %.1f -> %s\n", rd32(0x8c24a584),
				   bi >= 0 ? refs[bi].name : "-", bi >= 0 ? (double)best / THUMB : 0.0,
				   last >= 0 ? "replaced" : bi >= 0 && best < THUMB * 12 ? "no replacement file" : "no match");
		last_logged = last, last_best = bi;
	}
	if (last < 0)
		return 0;
	Ref *r = &refs[last];
	if (!r->loaded)
	{
		char path[600];
		const char *err = "";
		snprintf(path, sizeof(path), "%s/%s.png", dir, r->name);
		r->rgba = png_load(path, &r->w, &r->h, &err);
		r->loaded = r->rgba ? 1 : -1;
		printf("artswap: %s: %s%s\n", path, r->rgba ? "loaded" : "cannot be read: ", r->rgba ? "" : err);
		if (r->rgba)
		{
			/* bottom row first, as OpenGL draws it */
			const size_t row = (size_t)r->w * 4;
			u8 *tmp = malloc(row);
			for (u32 y = 0; y < r->h / 2; y++)
			{
				memcpy(tmp, r->rgba + y * row, row);
				memcpy(r->rgba + y * row, r->rgba + (r->h - 1 - y) * row, row);
				memcpy(r->rgba + (r->h - 1 - y) * row, tmp, row);
			}
			free(tmp);
		}
	}
	if (r->loaded < 0)
		return 0;
	*w = r->w, *h = r->h, *id = last;
	return r->rgba;
}
