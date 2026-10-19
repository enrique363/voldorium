/* M1_ARTDUMP=dir: exports the 4:3 artwork that leaves bars in 16:9, one picture per screen.
 *
 * The renderer (render.c wide_map) reports every flat textured polygon it draws outside fights,
 * except the far backdrops of 3D scenes, which already span 16:9. When some of them reach the
 * left or right edge of the 640x480 screen, that screen has a 4:3 picture with bars beside it.
 * Its background layer is every reported polygon at the depth of those edge pieces or farther
 * back (menus, logos and text in front are left out; scrolling wallpapers, which already span
 * 16:9, too); the pieces are composed here, on the CPU,
 * into one 640x480 picture (backgrounds come as many small textures: the main menu's is 80
 * tiles of 64x64). Saved as dir/screen_<n>.png with the frame as shown (dir/shown_<n>.png) and
 * listed in dir/index.csv and dir/index.html (open it locally: the pictures are the game's, keep
 * them on this machine). A picture is saved once it has stayed the same for 10 frames (not in
 * transitions, while textures load, or in animations such as the zooming logos), and not when
 * it differs only a little from one already saved for the same game mode.
 */
#include "m1.h"
#include "ta.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

u32 tex_decode_rgba(u32 tcw, u32 tsp, u32 *out, u32 *w, u32 *h);
u64 render_count(void);

/* ---- PNG writer (stored deflate blocks: no compression library needed) ---- */
static u32 crc_table[256];
static u32 crc32_update(u32 c, const u8 *p, size_t n)
{
	if (!crc_table[1])
		for (u32 i = 0; i < 256; i++)
		{
			u32 k = i;
			for (int j = 0; j < 8; j++)
				k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1;
			crc_table[i] = k;
		}
	c = ~c;
	while (n--)
		c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
	return ~c;
}
static void be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static void chunk(FILE *f, const char *type, const u8 *data, u32 len)
{
	u8 b[4];
	be32(b, len);
	fwrite(b, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (len)
		fwrite(data, 1, len, f);
	u32 c = crc32_update(0, (const u8 *)type, 4);
	c = crc32_update(c, data, len);
	be32(b, c);
	fwrite(b, 1, 4, f);
}
/* w x h pixels of `ch` bytes (3 RGB or 4 RGBA), top row first */
static int write_png(const char *path, const u8 *px, u32 w, u32 h, int ch)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return 0;
	static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	u8 ihdr[13];
	be32(ihdr, w);
	be32(ihdr + 4, h);
	ihdr[8] = 8, ihdr[9] = ch == 4 ? 6 : 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	const size_t row = (size_t)w * ch + 1, raw = row * h;
	const size_t blocks = (raw + 65534) / 65535;
	u8 *z = malloc(2 + raw + blocks * 5 + 4), *o = z;
	u8 *r = malloc(raw);
	for (u32 y = 0; y < h; y++)
	{
		r[y * row] = 0;
		memcpy(r + y * row + 1, px + (size_t)y * w * ch, (size_t)w * ch);
	}
	*o++ = 0x78, *o++ = 0x01;
	u32 a = 1, b2 = 0;
	for (size_t i = 0; i < raw; i++)
		a = (a + r[i]) % 65521, b2 = (b2 + a) % 65521;
	for (size_t off = 0; off < raw; off += 65535)
	{
		const u32 n = (u32)(raw - off < 65535 ? raw - off : 65535);
		*o++ = off + n >= raw;
		*o++ = (u8)n, *o++ = (u8)(n >> 8), *o++ = (u8)~n, *o++ = (u8)(~n >> 8);
		memcpy(o, r + off, n);
		o += n;
	}
	be32(o, b2 << 16 | a);
	o += 4;
	chunk(f, "IDAT", z, (u32)(o - z));
	chunk(f, "IEND", 0, 0);
	free(z);
	free(r);
	fclose(f);
	return 1;
}

/* ---- this frame's candidates (in draw order) ---- */
typedef struct { const TaPoly *p; float z, x0, x1; u8 edge; } Cand;
static Cand cand[8192];
static u32 ncand;
static const char *dir;

static int enabled(void)
{
	static int on = -1;
	if (on < 0)
	{
		dir = getenv("M1_ARTDUMP");
		on = dir && *dir;
		if (on)
		{
			char cmd[600];
#ifdef _WIN32
			snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", dir);
#else
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
#endif
			if (system(cmd))
				(void)0;
		}
	}
	return on;
}

void art_note(const TaPoly *p, float x0, float x1, float z)
{
	if (!enabled() || ncand >= sizeof(cand) / sizeof(cand[0]) || !isfinite(x0) || !isfinite(x1) || !isfinite(z))
		return;
	const u32 mode = rd32(0x8c24a584);
	if (mode == 23 || mode == 45)
		return;	/* demo fights (render.c treats only mode 1 as a fight for the HUD) */
	cand[ncand].p = p;
	cand[ncand].z = z;
	cand[ncand].x0 = x0;
	cand[ncand].x1 = x1;
	cand[ncand].edge = x0 <= 1 || x1 >= 639;
	ncand++;
}

/* ---- CPU composition of the background layer ---- */
#define CW 640
#define CH 480
static u32 canvas[CW * CH];	/* RGBA, r in the low byte (as tex.c) */

typedef struct { u32 tcw, tsp, w, h; u32 *px; } Tex;
static Tex texs[256];
static u32 ntexs;
static const Tex *tex_for(u32 tcw, u32 tsp)
{
	for (u32 i = 0; i < ntexs; i++)
		if (texs[i].tcw == tcw && texs[i].tsp == tsp)
			return &texs[i];
	if (ntexs == 256)
		return 0;
	Tex *t = &texs[ntexs++];
	t->tcw = tcw, t->tsp = tsp;
	if (!t->px)
		t->px = malloc(1024 * 1024 * 4);
	tex_decode_rgba(tcw, tsp, t->px, &t->w, &t->h);
	return t;
}

static u32 channel(u32 c, int sh) { return (c >> sh) & 255; }

static void raster(const TaVert *a, const TaVert *b, const TaVert *c, const TaPoly *p, const Tex *t)
{
	const float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
	if (fabsf(area) < 1e-6f)
		return;
	int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
	int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > CW ? CW : x1, y1 = y1 > CH ? CH : y1;
	const u32 shade = (p->tsp >> 6) & 3;	/* 0 decal, 1 modulate, 2 decal alpha, 3 modulate alpha */
	const int translucent = p->list == 2, punch = p->list == 4;
	const int clamp_u = (p->tsp >> 16) & 1, clamp_v = (p->tsp >> 15) & 1;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
		{
			const float px = x + 0.5f, py = y + 0.5f;
			const float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) / area;
			const float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) / area;
			const float w2 = 1 - w0 - w1;
			if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
				continue;
			float u = w0 * a->u + w1 * b->u + w2 * c->u, v = w0 * a->v + w1 * b->v + w2 * c->v;
			u = clamp_u ? fminf(fmaxf(u, 0), 0.9999f) : u - floorf(u);
			v = clamp_v ? fminf(fmaxf(v, 0), 0.9999f) : v - floorf(v);
			const u32 tx = t->px[(u32)(v * t->h) * t->w + (u32)(u * t->w)];
			/* vertex colour: the first vertex's (flat-shaded artwork is white or one tint) */
			const u32 vc = a->col;	/* ARGB */
			u32 r = channel(tx, 0), g = channel(tx, 8), bl = channel(tx, 16), al = channel(tx, 24);
			if (shade & 1)
			{
				r = r * channel(vc, 16) / 255, g = g * channel(vc, 8) / 255, bl = bl * channel(vc, 0) / 255;
				if (shade == 3)
					al = al * channel(vc, 24) / 255;
			}
			if (shade == 0)
				al = 255;
			u32 *d = &canvas[y * CW + x];
			if (punch && al < 128)
				continue;
			if (translucent && al < 255)
			{
				const u32 o = *d;
				r = (r * al + channel(o, 0) * (255 - al)) / 255;
				g = (g * al + channel(o, 8) * (255 - al)) / 255;
				bl = (bl * al + channel(o, 16) * (255 - al)) / 255;
			}
			*d = r | g << 8 | bl << 16 | 0xffu << 24;
		}
}

/* ---- saved pictures ---- */
typedef struct { u32 mode; u64 render; u8 thumb[32 * 24 * 3]; } Saved;
static Saved *saved;
static u32 nsaved, cap;
static int shot_pending;
static u64 shot_render;

static void thumb_of(u8 *th)
{
	for (int ty = 0; ty < 24; ty++)
		for (int tx = 0; tx < 32; tx++)
			for (int k = 0; k < 3; k++)
			{
				u32 s = 0;
				for (int y = 0; y < 20; y++)
					for (int x = 0; x < 20; x++)
						s += channel(canvas[(ty * 20 + y) * CW + tx * 20 + x], k * 8);
				th[(ty * 32 + tx) * 3 + k] = (u8)(s / 400);
			}
}

static void write_index(void)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/index.csv", dir);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "picture,shown,game_mode,render\n");
	for (u32 i = 0; i < nsaved; i++)
		fprintf(f, "screen_%llu.png,shown_%llu.png,%u,%llu\n", (unsigned long long)saved[i].render,
				(unsigned long long)saved[i].render, saved[i].mode, (unsigned long long)saved[i].render);
	fclose(f);
	snprintf(path, sizeof(path), "%s/index.html", dir);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "<!doctype html><meta charset=utf-8><title>4:3 artwork</title><style>body{font:14px system-ui;background:#181818;"
			   "color:#ddd;margin:16px}.row{display:flex;flex-wrap:wrap;gap:12px;margin:12px 0;padding:10px;background:#222;"
			   "border-radius:8px}figure{margin:0}img{width:420px;max-width:100%%;display:block}figcaption{font-size:12px;"
			   "color:#aaa;margin-top:4px}</style><h1>4:3 artwork that leaves bars in 16:9</h1><p>%u screens. Left: the background "
			   "picture (640x480, menus and text in front removed); right: the screen as the port shows it now.</p>\n",
			nsaved);
	for (u32 i = 0; i < nsaved; i++)
	{
		const unsigned long long n = (unsigned long long)saved[i].render;
		fprintf(f, "<div class=row><figure><img src=\"screen_%llu.png\"><figcaption>screen_%llu.png &middot; game mode %u"
				   "</figcaption></figure><figure><img src=\"shown_%llu.png\"><figcaption>as shown</figcaption></figure></div>\n",
				n, n, saved[i].mode, n);
	}
	fclose(f);
}

/* after a frame's polygons are drawn: compose and save its background layer if it is new;
   returns 1 when the frame as shown should be saved too (art_shot) */
int art_frame(const TaCtx *c)
{
	if (!enabled())
		return 0;
	const u32 n = ncand;
	ncand = 0;
	/* a scrolling wallpaper (tiled layer reaching well past the 4:3 screen) already spans 16:9
	   (render.c draws it again one wrap period to either side): not artwork to redraw */
	float tx0 = 1e30f, tx1 = -1e30f;
	for (u32 i = 0; i < n; i++)
		if (cand[i].p->cls == M1_2D_TILED)
			tx0 = fminf(tx0, cand[i].x0), tx1 = fmaxf(tx1, cand[i].x1);
	const int wallpaper = tx0 < -32 || tx1 > 672;
	float zmax = -1;
	for (u32 i = 0; i < n; i++)
		if (cand[i].edge && cand[i].z > zmax && !(wallpaper && cand[i].p->cls == M1_2D_TILED))
			zmax = cand[i].z;
	if (zmax < 0)
		return 0;
	memset(canvas, 0, sizeof(canvas));
	for (u32 i = 0; i < CW * CH; i++)
		canvas[i] = 0xff000000u;
	ntexs = 0;
	u32 drawn = 0;
	for (u32 i = 0; i < n; i++)
	{
		if (cand[i].z > zmax * 1.0001f + 1e-7f || (wallpaper && cand[i].p->cls == M1_2D_TILED))
			continue;	/* in front of the background, or the wallpaper */
		const TaPoly *p = cand[i].p;
		const Tex *t = tex_for(p->tcw, p->tsp);
		if (!t)
			continue;
		const TaVert *v = &c->v[p->first];
		for (u32 k = 0; k + 2 < p->count; k++)
			raster(&v[k], &v[k + 1], &v[k + 2], p, t);
		drawn++;
	}
	if (!drawn)
		return 0;
	u8 th[32 * 24 * 3];
	thumb_of(th);
	static u8 prev[32 * 24 * 3];
	static int stable;
	u32 change = 0;
	for (u32 k = 0; k < sizeof(th); k++)
		change += (u32)abs((int)th[k] - (int)prev[k]);
	memcpy(prev, th, sizeof(th));
	stable = change < sizeof(th) * 3 ? stable + 1 : 0;
	if (stable != 10)
		return 0;
	const u32 mode = rd32(0x8c24a584);
	for (u32 i = 0; i < nsaved; i++)
		if (saved[i].mode == mode)
		{
			u32 diff = 0;
			for (u32 k = 0; k < sizeof(th); k++)
				diff += (u32)abs((int)th[k] - (int)saved[i].thumb[k]);
			if (diff < sizeof(th) * 12)
				return 0;	/* about the same picture (mean difference under 12 of 255) */
		}
	if (nsaved == cap)
		saved = realloc(saved, (cap = cap ? cap * 2 : 32) * sizeof(Saved));
	Saved *s = &saved[nsaved++];
	s->mode = mode;
	s->render = render_count();
	memcpy(s->thumb, th, sizeof(th));
	static u8 rgb[CW * CH * 3];
	for (u32 i = 0; i < CW * CH; i++)
		rgb[i * 3] = (u8)canvas[i], rgb[i * 3 + 1] = (u8)(canvas[i] >> 8), rgb[i * 3 + 2] = (u8)(canvas[i] >> 16);
	char path[600];
	snprintf(path, sizeof(path), "%s/screen_%llu.png", dir, (unsigned long long)s->render);
	write_png(path, rgb, CW, CH, 3);
	printf("artdump: screen_%llu.png (game mode %u, %u pieces)\n", (unsigned long long)s->render, mode, drawn);
	write_index();
	shot_pending = 1;
	shot_render = s->render;
	return 1;
}

/* the frame as shown (rgb: w x h, bottom row first, as glReadPixels gives it) */
void art_shot(const u8 *rgb, int w, int h)
{
	if (!shot_pending)
		return;
	shot_pending = 0;
	u8 *flip = malloc((size_t)w * h * 3);
	for (int y = 0; y < h; y++)
		memcpy(flip + (size_t)y * w * 3, rgb + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
	char path[600];
	snprintf(path, sizeof(path), "%s/shown_%llu.png", dir, (unsigned long long)shot_render);
	write_png(path, flip, (u32)w, (u32)h, 3);
	free(flip);
}

/* the 32x24 fingerprint of a background layer (artswap.c): the layer's polygons (mask) composed
   like an exported picture, then averaged in 20x20 cells, RGB */
void art_fingerprint(const TaCtx *c, const u8 *mask, u8 *th)
{
	for (u32 i = 0; i < CW * CH; i++)
		canvas[i] = 0xff000000u;
	ntexs = 0;
	static const int order[3] = { 0, 4, 2 };	/* opaque, punch-through, translucent */
	for (int pass = 0; pass < 3; pass++)
		for (u32 i = 0; i < c->np; i++)
		{
			const TaPoly *p = &c->p[i];
			if (!mask[i] || p->list != order[pass])
				continue;
			const Tex *t = tex_for(p->tcw, p->tsp);
			if (!t)
				continue;
			const TaVert *v = &c->v[p->first];
			for (u32 k = 0; k + 2 < p->count; k++)
				raster(&v[k], &v[k + 1], &v[k + 2], p, t);
		}
	thumb_of(th);
}
