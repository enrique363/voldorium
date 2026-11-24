/* PowerVR2 textures from VRAM -> RGBA8.
 *
 * Texture control word (TCW): bit 31 mipmapped, 30 VQ compressed, 29-27 pixel format
 * (0 ARGB1555, 1 RGB565, 2 ARGB4444, 3 YUV422, 4 bump, 5 4-bit palette, 6 8-bit palette),
 * 26 scan order (1 = not twiddled), 25 stride select, 26-21 palette selector (palette
 * formats), 20-0 address / 8. Size from the TSP word: U = 8 << bits 5-3, V = 8 << bits 2-0.
 * Twiddled order interleaves y and x bits from the lowest (y first) while both dimensions
 * have bits left (Flycast texconv.cpp twiddle_slow). VQ: a 2 KB codebook of 256 entries of
 * 2x2 texels (in twiddled order), then one index byte per 2x2 block, twiddled at block level.
 * Mipmapped textures are square and store the smaller levels first (offset tables as in
 * Flycast TexCache.cpp). Palettes: 1024 entries at 005F9000, format PAL_RAM_CTRL (005F8108).
 */
#include "m1.h"
#include "ta.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <SDL2/SDL.h>

static const u32 vq_mip[11] = { 0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556 };
static const u32 other_mip[11] = { 0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558 };

u32 hw_reg(u32 addr);

static u32 twiddle(u32 x, u32 y, u32 w, u32 h)
{
	u32 rv = 0, sh = 0;
	w >>= 1;
	h >>= 1;
	while (w || h)
	{
		if (h) { rv |= (y & 1) << sh; y >>= 1; h >>= 1; sh++; }
		if (w) { rv |= (x & 1) << sh; x >>= 1; w >>= 1; sh++; }
	}
	return rv;
}

static u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }

static u32 conv16(u32 fmt, u16 v)
{
	switch (fmt)
	{
	case 0:	/* ARGB1555 */
	{
		const u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
		return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (v & 0x8000) ? 255 : 0);
	}
	case 1:	/* RGB565 */
	{
		const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		return rgba(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
	}
	case 2:	/* ARGB4444 */
	{
		const u32 a = (v >> 12) & 15, r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
		return rgba(r * 17, g * 17, b * 17, a * 17);
	}
	default:	/* bump map: shown as grey */
		return rgba(128, 128, 128, 255);
	}
}

static u32 palette(u32 index)
{
	const u32 e = hw_reg(0x005f9000u + 4 * (index & 1023));
	switch (hw_reg(0x005f8108) & 3)
	{
	case 0: return conv16(0, (u16)e);
	case 1: return conv16(1, (u16)e);
	case 2: return conv16(2, (u16)e);
	default: return rgba((e >> 16) & 255, (e >> 8) & 255, e & 255, e >> 24);
	}
}

static u32 clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u32)v; }

static u32 yuv(int y, int u, int v)
{
	u -= 128;
	v -= 128;
	return rgba(clamp255(y + (int)(1.375f * v)), clamp255(y - (int)(0.34375f * u + 0.6875f * v)),
				clamp255(y + (int)(1.71875f * u)), 255);
}

/* twiddle() interleaves the bits of x and y, each to places that depend only on w and h, so
   twiddle(x, y) = twiddle(x, 0) | twiddle(0, y): one table per axis per texture instead of a
   bit loop per texel (a fight's first frame decodes about 30 textures at once) */
static u32 tw_x[1024], tw_y[1024];
static void twiddle_tables(u32 w, u32 h)
{
	for (u32 x = 0; x < w && x < 1024; x++)
		tw_x[x] = twiddle(x, 0, w, h);
	for (u32 y = 0; y < h && y < 1024; y++)
		tw_y[y] = twiddle(0, y, w, h);
}

/* decode into out (w x h RGBA8); returns the number of VRAM bytes the texture spans */
static u32 decode(u32 tcw, u32 w, u32 h, u32 *out)
{
	const u8 *vram = hw_vram();
	const u32 fmt = (tcw >> 27) & 7, vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1;
	const int pal = fmt == 5 || fmt == 6;
	const int twiddled = pal || !((tcw >> 26) & 1);
	u32 addr = (tcw & 0x1fffff) << 3;
	const u32 start = addr;
	int lw = 0;
	while ((8u << lw) < w) lw++;
	lw += 3;	/* log2(w) */
	if (mip)
		h = w;
	const u32 bpp = fmt == 5 ? 4 : fmt == 6 ? 8 : 16;
	u32 span;
	if (vq)
	{
		const u8 *book = vram + (addr & (VRAM_SIZE - 1));
		addr += 2048 + (mip ? vq_mip[lw] : 0);
		twiddle_tables(w >> 1, h >> 1);
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++)
			{
				const u32 blk = tw_x[x >> 1] | tw_y[y >> 1];
				const u8 idx = vram[(addr + blk) & (VRAM_SIZE - 1)];
				const u32 sub = (y & 1) | (x & 1) << 1;
				u16 t;
				memcpy(&t, book + idx * 8 + sub * 2, 2);
				out[y * w + x] = fmt == 3 ? rgba(t >> 8, t >> 8, t >> 8, 255) : conv16(fmt, t);
			}
		span = addr - start + w * h / 4;
		return span;
	}
	if (mip)
		addr += other_mip[lw] * bpp / 8;
	u32 stride = w;
	if (!twiddled && ((tcw >> 25) & 1))
		stride = (hw_reg(0x005f80e4) & 31) * 32;	/* TEXT_CONTROL stride */
	if (twiddled)
		twiddle_tables(w, h);
	/* the palette entries this texture reads, converted once */
	static u32 pal_rgba[256];
	if (fmt == 5)
		for (u32 i = 0; i < 16; i++)
			pal_rgba[i] = palette((((tcw >> 21) & 63) << 4) | i);
	else if (fmt == 6)
		for (u32 i = 0; i < 256; i++)
			pal_rgba[i] = palette((((tcw >> 25) & 3) << 8) | i);
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
		{
			const u32 t = twiddled ? tw_x[x] | tw_y[y] : y * stride + x;
			u32 c;
			if (fmt == 5)
			{
				const u8 b = vram[(addr + t / 2) & (VRAM_SIZE - 1)];
				c = pal_rgba[(t & 1) ? b >> 4 : b & 15];
			}
			else if (fmt == 6)
				c = pal_rgba[vram[(addr + t) & (VRAM_SIZE - 1)]];
			else if (fmt == 3)
			{
				/* YUV422: pairs of texels share U and V (twiddled pairs run along y) */
				const u32 t0 = twiddled ? tw_x[x & ~1u] | tw_y[y] : y * stride + (x & ~1u);
				const u32 t1 = twiddled ? tw_x[x | 1u] | tw_y[y] : y * stride + (x | 1u);
				u16 a, b;
				memcpy(&a, vram + ((addr + t0 * 2) & (VRAM_SIZE - 1)), 2);
				memcpy(&b, vram + ((addr + t1 * 2) & (VRAM_SIZE - 1)), 2);
				c = yuv((x & 1) ? b >> 8 : a >> 8, a & 255, b & 255);
			}
			else
			{
				u16 v;
				memcpy(&v, vram + ((addr + t * 2) & (VRAM_SIZE - 1)), 2);
				c = conv16(fmt, v);
			}
			out[y * w + x] = c;
		}
	span = addr - start + (twiddled ? w : stride) * h * bpp / 8;
	return span;
}

/* ---- cache ------------------------------------------------------------------------------ */
/* An open-addressed table: a texture lives within NPROBE slots of its home slot; when they are
   all taken, the one used longest ago gives way. The home slot comes from the high bits of a
   multiplicative hash: the low bits of tcw * k depend only on tcw's low bits, which are 0 for
   aligned textures, so every texture once hashed to a few slots and each lookup searched all the
   textures seen so far, 18 to 26 ms a frame by the end of arcade mode (issue #19). */
#define NTEX_BITS 12
#define NTEX (1u << NTEX_BITS)
#define NPROBE 32
static u32 home_slot(u32 tcw, u32 size)
{
	return (u32)((((u64)tcw << 6 | size) * 0x9e3779b97f4a7c15ull) >> (64 - NTEX_BITS));
}
typedef struct
{
	u32 tcw, size, used;		/* size = TSP bits 0-5 */
	u32 start, span, gen_sum, pal_gen;
	u64 hash;					/* of the VRAM bytes it was decoded from */
	u64 pal_hash;				/* of the palette bank it used */
	unsigned id;
	unsigned hi_id;				/* the upscaled copy for smoothed 2D drawing (tex_get_hi) */
	int hi_ok;					/* hi_id holds the current contents */
	int hi_volatile;			/* changes too often to upscale: an animation, drawn plain */
	u32 hi_count;				/* upscales since hi_since */
	u64 hi_since;				/* render count */
	int hi_picture;				/* the last upscale was an opaque picture (Anime4K, render.c) */
	int hi_plain;				/* not upscaled under the current settings: drawn plain */
	int hi_edge;				/* a cut-out with a smooth outline (cut_alpha, Anime4K colour) */
	int hi_pending;				/* its cubic enlargement is with the worker (tex_get_hi) */
	unsigned rep_id;			/* with replacement art (tex_get_rep), 0: none */
	u64 rep_key;				/* the contents rep_id was made for (hash ^ pal_hash), 0: not checked */
	u64 last_use;				/* render count of its last lookup (which entry gives way) */
} TexEntry;
static TexEntry cache[NTEX];
static u64 decodes, rehashed;
u64 tex_lookups, tex_probes;	/* M1_PERF=slow: cache lookups and slots looked at */
u32 tex_slots_used(void)
{
	u32 n = 0;
	for (u32 i = 0; i < NTEX; i++)
		n += cache[i].used != 0;
	return n;
}
double tex_decode_ms;	/* wall time decoding and uploading textures (render.c M1_PERF=slow) */

/* VRAM pages are rewritten with identical data all the time (character select re-sends its
   textures every frame), so a changed generation is confirmed with a hash of the bytes */
static u64 range_hash(u32 start, u32 span)
{
	const u8 *v = hw_vram();
	if (start >= VRAM_SIZE)
		return 0;
	if (span > VRAM_SIZE - start)
		span = VRAM_SIZE - start;
	u64 h = 0x9e3779b97f4a7c15ull, x;
	u32 i = 0;
	for (; i + 8 <= span; i += 8)
	{
		memcpy(&x, v + start + i, 8);
		h = (h ^ x) * 0x100000001b3ull;
		h ^= h >> 29;
	}
	for (; i < span; i++)
		h = (h ^ v[start + i]) * 0x100000001b3ull;
	return h;
}

static u32 range_gen(u32 start, u32 span)
{
	u32 sum = 0;
	for (u32 pg = start >> 12; pg <= (start + span - 1) >> 12; pg++)
		sum += hw_vram_gen(pg) * 2654435761u + pg;
	return sum;
}

/* the palette entries a paletted texture reads (4-bit: 16 of bank TCW 26-21, 8-bit: 256 of
   bank TCW 26-25) and the palette format: the game animates other banks every frame */
static u64 bank_hash(u32 tcw)
{
	const u32 fmt = (tcw >> 27) & 7;
	const u32 base = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8, n = fmt == 5 ? 16 : 256;
	u64 h = 0x9e3779b97f4a7c15ull ^ (hw_reg(0x005f8108) & 3);
	for (u32 i = 0; i < n; i++)
		h = (h ^ hw_reg(0x005f9000u + 4 * ((base + i) & 1023))) * 0x100000001b3ull;
	return h;
}

unsigned gl_upload_texture(unsigned id, u32 w, u32 h, const u32 *rgba);

static TexEntry *lookup(u32 tcw, u32 tsp)
{
	const u32 size = tsp & 63, w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
	const u32 start = (tcw & 0x1fffff) << 3;
	const int pal = ((tcw >> 27) & 7) >= 5;
	u64 render_count(void);
	const u64 now = render_count();
	u32 slot = home_slot(tcw, size), victim = slot;
	u64 oldest = ~0ull;
	tex_lookups++;
	for (u32 probe = 0; probe < NPROBE; probe++, slot = (slot + 1) & (NTEX - 1))
	{
		TexEntry *e = &cache[slot];
		tex_probes++;
		if (!e->used)
		{
			victim = slot;	/* not cached: a free slot */
			break;
		}
		if (e->last_use < oldest)
			oldest = e->last_use, victim = slot;
		if (e->tcw == tcw && e->size == size)
		{
			victim = slot;
			e->last_use = now;
			static int hashes = -1;
			if (hashes < 0)
				hashes = !getenv("M1_TEXHASH") || atoi(getenv("M1_TEXHASH"));	/* M1_TEXHASH=0: off, for comparison */
			int pal_same = !pal || e->pal_gen == hw_palette_gen();
			if (hashes && !pal_same && bank_hash(tcw) == e->pal_hash)
			{
				e->pal_gen = hw_palette_gen();	/* another bank changed */
				pal_same = 1;
			}
			static long nocache = -2;	/* M1_TEXNOCACHE=tcw (debugging; 1: all): decode every use */
			if (nocache == -2)
				nocache = getenv("M1_TEXNOCACHE") ? (long)strtoul(getenv("M1_TEXNOCACHE"), 0, 16) : -1;
			if (nocache == 1 || (nocache >= 0 && (u32)nocache == tcw))
				break;
			const u32 gen = range_gen(e->start, e->span);
			if (gen == e->gen_sum && pal_same)
				return e;
			if (hashes && pal_same && range_hash(e->start, e->span) == e->hash)
			{
				e->gen_sum = gen;	/* rewritten with the same bytes */
				rehashed++;
				return e;
			}
			break;	/* changed: decode again into this slot */
		}
	}
	TexEntry *e = &cache[victim];	/* changed, free, or the least recently used nearby */
	e->last_use = now;
	static u32 buf[1024 * 1024];
	const u32 hh = (tcw >> 31) ? w : h;
	const clock_t td0 = clock();
	const u64 q0 = SDL_GetPerformanceCounter();	/* M1_SLOWSTATE: the parts of a slow refresh */
	const u32 span = decode(tcw, w, h, buf);
	const u64 q1 = SDL_GetPerformanceCounter();
	decodes++;
	{
		/* M1_TEXLOG=n: list the textures decoded during render n */
		static long want = -2;
		if (want == -2)
			want = getenv("M1_TEXLOG") ? atol(getenv("M1_TEXLOG")) : -1;
		u64 render_count(void);
		if ((long)render_count() == want)
			printf("texlog: tcw %08x %ux%u fmt %u pal %d start %06x span %u gen %s hash %016llx\n", tcw, w, hh, (tcw >> 27) & 7, pal, start,
				   span, e->used && e->tcw == tcw ? (range_gen(e->start, e->span) != e->gen_sum ? "vram" : "palette") : "new",
				   (unsigned long long)range_hash(start, span ? span : 8));
		if ((long)render_count() == want && ((tcw >> 27) & 7) == 5)
			for (u32 i = 0; i < 16; i++)	/* a 4-bit texture's palette (for replacement art) */
				printf("texlog:   palette %2u: %08x\n", i, palette((((tcw >> 21) & 63) << 4) | i));
	}
	const int was = e->used && e->tcw == tcw && e->size == size;
	const u32 old_span = e->span;
	const u64 q2 = SDL_GetPerformanceCounter();
	e->id = gl_upload_texture(e->used ? e->id : 0, w, hh, buf);
	const u64 q3 = SDL_GetPerformanceCounter();
	tex_decode_ms += (double)(clock() - td0) * 1000.0 / CLOCKS_PER_SEC;	/* M1_PERF=slow */
	if (!e->used || e->tcw != tcw || e->size != size)
		e->hi_volatile = 0, e->hi_count = 0, e->hi_picture = 0, e->hi_plain = 0, e->hi_edge = 0;	/* another texture in this slot */
	e->used = 1;
	e->tcw = tcw;
	e->size = size;
	e->start = start;
	e->span = span ? span : 8;
	e->gen_sum = range_gen(e->start, e->span);
	e->pal_gen = hw_palette_gen();
	e->hash = range_hash(e->start, e->span);
	e->pal_hash = pal ? bank_hash(tcw) : 0;
	e->hi_ok = 0;
	e->rep_key = 0;
	{
		const u64 q4 = SDL_GetPerformanceCounter();
		static double slow = -1;
		if (slow < 0)
			slow = getenv("M1_SLOWSTATE") ? atof(getenv("M1_SLOWSTATE")) : 0;
		const double f = 1e6 / (double)SDL_GetPerformanceFrequency();
		if (slow > 0 && (double)(q4 - q0) * f > slow)
			printf("slowlookup: render %llu tcw %08x %ux%u span %u (was %u, %s): decode %.0f texlog %.0f upload %.0f hashes %.0f us\n",
				   (unsigned long long)now, tcw, w, hh, span, old_span, was ? "refresh" : "new", (double)(q1 - q0) * f,
				   (double)(q2 - q1) * f, (double)(q3 - q2) * f, (double)(q4 - q3) * f);
	}
	return e;
}

unsigned tex_get(u32 tcw, u32 tsp) { return lookup(tcw, tsp)->id; }

/* Upscaling for smooth 2D: Catmull-Rom bicubic by k on premultiplied colour, then, for
   textures whose alpha is only 0 or 255 (glyphs, cut-out shapes), the interpolated alpha
   steepened around one half, so outlines come out as smooth curves at the original weight
   instead of blurred steps. Textures with graded alpha (glows, shadows) keep it as is. */
static float cr_w(float t, int i)
{
	/* weights of the four taps at -1, 0, 1, 2 for a sample t in [0, 1) past tap 0 */
	const float t2 = t * t, t3 = t2 * t;
	switch (i)
	{
	case 0: return -0.5f * t3 + t2 - 0.5f * t;
	case 1: return 1.5f * t3 - 2.5f * t2 + 1.0f;
	case 2: return -1.5f * t3 + 2.0f * t2 + 0.5f * t;
	default: return 0.5f * t3 - 0.5f * t2;
	}
}

static void upscale(const u32 *src, u32 w, u32 h, u32 k, u32 *dst)
{
	static float *pm, *row;
	static size_t cap;
	const size_t need = (size_t)w * k * h * 4;
	if (need > cap)
	{
		cap = need;
		free(pm);
		free(row);
		pm = malloc((size_t)w * h * 4 * sizeof(float));
		row = malloc(need * sizeof(float));
	}
	int binary = 1;
	for (u32 i = 0; i < w * h; i++)
	{
		const u32 c = src[i], a = c >> 24;
		binary &= a == 0 || a == 255;
		const float af = a / 255.0f;
		pm[i * 4 + 0] = (c & 255) * af;
		pm[i * 4 + 1] = (c >> 8 & 255) * af;
		pm[i * 4 + 2] = (c >> 16 & 255) * af;
		pm[i * 4 + 3] = (float)a;
	}
	const u32 W = w * k, H = h * k;
	/* horizontal: W x h */
	for (u32 y = 0; y < h; y++)
		for (u32 X = 0; X < W; X++)
		{
			const float fx = (X + 0.5f) / k - 0.5f;
			const int x0 = (int)floorf(fx);
			const float t = fx - x0;
			float acc[4] = { 0, 0, 0, 0 };
			for (int i = 0; i < 4; i++)
			{
				int x = x0 - 1 + i;
				x = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
				const float wt = cr_w(t, i);
				for (int c = 0; c < 4; c++)
					acc[c] += wt * pm[((size_t)y * w + x) * 4 + c];
			}
			for (int c = 0; c < 4; c++)
				row[((size_t)y * W + X) * 4 + c] = acc[c];
		}
	/* vertical, then back to straight colour */
	for (u32 Y = 0; Y < H; Y++)
	{
		const float fy = (Y + 0.5f) / k - 0.5f;
		const int y0 = (int)floorf(fy);
		const float t = fy - y0;
		for (u32 X = 0; X < W; X++)
		{
			float acc[4] = { 0, 0, 0, 0 };
			for (int i = 0; i < 4; i++)
			{
				int y = y0 - 1 + i;
				y = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
				const float wt = cr_w(t, i);
				for (int c = 0; c < 4; c++)
					acc[c] += wt * row[((size_t)y * W + X) * 4 + c];
			}
			float a = acc[3] < 0 ? 0 : acc[3] > 255 ? 255 : acc[3];
			u32 rgb[3];
			for (int c = 0; c < 3; c++)
			{
				const float v = a > 0.5f ? acc[c] * 255.0f / a : 0;
				rgb[c] = v < 0 ? 0 : v > 255 ? 255 : (u32)(v + 0.5f);
			}
			if (binary)
			{
				/* smoothstep over the middle half of the coverage */
				float x = (a / 255.0f - 0.25f) * 2.0f;
				x = x < 0 ? 0 : x > 1 ? 1 : x;
				a = x * x * (3 - 2 * x) * 255.0f;
			}
			dst[(size_t)Y * W + X] = rgb[0] | rgb[1] << 8 | rgb[2] << 16 | (u32)(a + 0.5f) << 24;
		}
	}
}

/* ---- cut-outs (text): Anime4K colour, smooth outline ----------------------------------------
 * A texture whose texels are either clear or solid (the fonts, punch-through) is drawn where
 * its enlarged alpha crosses one half (render.c). The cubic alpha keeps the texels' staircase
 * in that line; blurred by 0.7 texel first, the line follows the letters' curves instead. Its
 * colour is enlarged by Anime4K like the pictures, after the solid texels' colours are spread
 * into the clear ones next to them (else the edge would take the clear texels' colour). */

/* the colours of solid texels spread into clear ones, 8 texels deep (in place, alpha kept) */
static void cut_fill(u32 *a, u32 w, u32 h)
{
	static u8 *known, *next;
	static size_t cap;
	if ((size_t)w * h > cap)
	{
		cap = (size_t)w * h;
		free(known), free(next);
		known = malloc(cap), next = malloc(cap);
	}
	for (u32 i = 0; i < w * h; i++)
		known[i] = a[i] >> 24 != 0;
	for (int pass = 0; pass < 8; pass++)
	{
		int grew = 0;
		memcpy(next, known, (size_t)w * h);
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++)
			{
				const u32 i = y * w + x;
				if (known[i])
					continue;
				u32 s[3] = { 0, 0, 0 }, n = 0;
				const u32 nb[4] = { x > 0 ? i - 1 : i, x + 1 < w ? i + 1 : i, y > 0 ? i - w : i, y + 1 < h ? i + w : i };
				for (int j = 0; j < 4; j++)
					if (nb[j] != i && known[nb[j]])
					{
						const u32 c = a[nb[j]];
						s[0] += c & 255, s[1] += c >> 8 & 255, s[2] += c >> 16 & 255, n++;
					}
				if (n)
				{
					a[i] = (s[0] / n) | (s[1] / n) << 8 | (s[2] / n) << 16;	/* alpha stays 0 */
					next[i] = 1, grew = 1;
				}
			}
		memcpy(known, next, (size_t)w * h);
		if (!grew)
			break;
	}
}

/* the alpha of a (w x h) blurred by 0.7 texel and enlarged k times (Catmull-Rom), into out (bytes) */
static void cut_alpha(const u32 *a, u32 w, u32 h, u32 k, u8 *out)
{
	const u32 W = w * k, H = h * k;
	static float *s0, *s1, *t1;
	static size_t cap, scap;
	if ((size_t)w * h > scap)
	{
		scap = (size_t)w * h;
		free(s0), free(s1);
		s0 = malloc(scap * sizeof(float)), s1 = malloc(scap * sizeof(float));
	}
	if ((size_t)W * h > cap)
	{
		cap = (size_t)W * h;
		free(t1);
		t1 = malloc(cap * sizeof(float));
	}
	/* blurred first (the same as after, at a sixteenth of the work): a Gaussian of 0.7 texels */
	enum { R = 3 };
	float g[2 * R + 1], sum = 0;
	for (int i = -R; i <= R; i++)
		sum += g[i + R] = expf(-(float)(i * i) / (2 * 0.7f * 0.7f));
	for (int i = 0; i <= 2 * R; i++)
		g[i] /= sum;
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
		{
			float acc = 0;
			for (int i = -R; i <= R; i++)
			{
				int xx = (int)x + i;
				xx = xx < 0 ? 0 : xx >= (int)w ? (int)w - 1 : xx;
				acc += g[i + R] * (float)(a[y * w + xx] >> 24);
			}
			s1[y * w + x] = acc;
		}
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
		{
			float acc = 0;
			for (int i = -R; i <= R; i++)
			{
				int yy = (int)y + i;
				yy = yy < 0 ? 0 : yy >= (int)h ? (int)h - 1 : yy;
				acc += g[i + R] * s1[yy * w + x];
			}
			s0[y * w + x] = acc;
		}
	/* enlarged: horizontal into t1 (W x h), vertical out (W x H) */
	for (u32 y = 0; y < h; y++)
		for (u32 X = 0; X < W; X++)
		{
			const float fx = (X + 0.5f) / k - 0.5f;
			const int x0 = (int)floorf(fx);
			float acc = 0;
			for (int i = 0; i < 4; i++)
			{
				int x = x0 - 1 + i;
				x = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
				acc += cr_w(fx - x0, i) * s0[y * w + x];
			}
			t1[y * W + X] = acc;
		}
	for (u32 Y = 0; Y < H; Y++)
	{
		const float fy = (Y + 0.5f) / k - 0.5f;
		const int y0 = (int)floorf(fy);
		float wt[4];
		for (int i = 0; i < 4; i++)
			wt[i] = cr_w(fy - y0, i);
		for (u32 X = 0; X < W; X++)
		{
			float acc = 0;
			for (int i = 0; i < 4; i++)
			{
				int y = y0 - 1 + i;
				y = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
				acc += wt[i] * t1[(size_t)y * W + X];
			}
			out[(size_t)Y * W + X] = (u8)(acc < 0 ? 0 : acc > 255 ? 255 : acc + 0.5f);
		}
	}
}

/* the GRAPHICS menu changed TEXT or HI-RES PICTURES: every upscaled copy is made again */
/* The cubic enlargement runs on a worker thread: about 9 ms for a 512x512 texture, and some
   scenes bring several new ones a frame (enrique363/voldorium#19: Inferno's explosion, about 5 a
   frame for a second, 25 ms a frame). Meanwhile the texture is drawn plain; the result is
   uploaded at the next tex_get_hi once ready, if the texture still holds the same contents. One
   job at a time. */
static void upscale(const u32 *src, u32 w, u32 h, u32 k, u32 *dst);
static struct
{
	SDL_Thread *thread;
	SDL_sem *go;
	SDL_atomic_t state;		/* 0 idle, 1 working, 2 done */
	TexEntry *e;
	u32 tcw, size, w, h, k;
	u64 key;
	u32 *src, *dst;
} hj;
static int SDLCALL hj_main(void *arg)
{
	(void)arg;
	for (;;)
	{
		SDL_SemWait(hj.go);
		upscale(hj.src, hj.w, hj.h, hj.k, hj.dst);
		SDL_MemoryBarrierRelease();
		SDL_AtomicSet(&hj.state, 2);
	}
	return 0;
}
static u64 hj_key(const TexEntry *e) { return (e->hash ^ e->pal_hash) + e->tcw * 0x9e3779b97f4a7c15ull + e->size; }
/* a finished job: its texture uploaded if the entry still holds what it was made from */
static void hj_collect(void)
{
	if (!hj.thread || SDL_AtomicGet(&hj.state) != 2)
		return;
	SDL_MemoryBarrierAcquire();
	TexEntry *e = hj.e;
	if (e->hi_pending && !e->hi_ok && e->used && e->tcw == hj.tcw && e->size == hj.size && hj_key(e) == hj.key)
	{
		e->hi_id = gl_upload_texture(e->hi_id, hj.w * hj.k, hj.h * hj.k, hj.dst);
		e->hi_ok = 1, e->hi_picture = 0, e->hi_plain = 0, e->hi_edge = 0;
		if (getenv("M1_HILOG"))
		{
			u64 render_count(void);
			printf("hi: render %llu tcw %08x %ux%u x%u ready\n",(unsigned long long)render_count(), hj.tcw, hj.w, hj.h, hj.k);
		}
	}
	e->hi_pending = 0;
	SDL_AtomicSet(&hj.state, 0);
}

void tex_hi_reset(void)
{
	for (int i = 0; i < NTEX; i++)
		cache[i].hi_ok = 0, cache[i].hi_volatile = 0, cache[i].hi_count = 0;
}

/* the texture upscaled (4x up to 256 texels, 2x up to 512) for 2D drawn with bilinear
   filtering at high resolutions (render.c: text and HUD); *scale = the factor,
   1 when the texture is too large (then it is the plain one). text: [video] text = smooth
   (the cubic enlargement; 2, TEXT HI-RES: cut-outs as below), pictures: [video] portraits = 1 (opaque pictures by Anime4K);
   what neither covers is drawn plain. *edge: a cut-out with a smooth outline (render.c draws
   its edge one screen pixel wide) */
unsigned tex_get_hi(u32 tcw, u32 tsp, int *scale, int *edge, int text, int pictures)
{
	TexEntry *e = lookup(tcw, tsp);
	const u32 w = 8u << ((tsp >> 3) & 7), h = (tcw >> 31) ? w : 8u << (tsp & 7);
	const u32 big = w > h ? w : h;
	const int k = big <= 256 ? 4 : big <= 512 ? 2 : 1;
	*scale = k;
	*edge = 0;
	if (k == 1 || e->hi_volatile || (e->hi_ok && e->hi_plain))
	{
		*scale = 1;
		return e->id;
	}
	hj_collect();
	if (e->hi_ok && !e->hi_plain)
	{
		*edge = e->hi_edge;
		return e->hi_id;
	}
	if (e->hi_pending || (hj.thread && SDL_AtomicGet(&hj.state) != 0))
	{
		*scale = 1;	/* the worker is busy: plain for now */
		return e->id;
	}
	if (!e->hi_ok)
	{
		/* At most HI_BUDGET_MS of upscaling per frame: past it, a texture is drawn plain this
		   frame and enlarged on a later one. A scene that brings new 2D textures every frame
		   (Inferno's explosion: about 5 a frame, each a new texture, so the count below never
		   catches them) cost up to 25 ms a frame here, every frame (enrique363/voldorium#19);
		   a screen's first frame up to 40 ms. */
		u64 render_count(void);
		double video_ms(void);
		const u64 now = render_count();
		enum { HI_BUDGET_MS = 3 };
		static u64 budget_render = ~0ull;
		static double budget_used;
		if (budget_render != now)
			budget_render = now, budget_used = 0;
		if (budget_used >= HI_BUDGET_MS)
		{
			*scale = 1;
			return e->id;
		}
		const double budget_t0 = video_ms();
		/* a texture rewritten every few frames (animated surfaces, palette cycling) is not
		   text: more than 6 upscales within 300 renders and it stays plain. A picture gets 20:
		   the select screen's large portrait is the same texture loaded again at each move of
		   the cursor */
		if (now - e->hi_since > 300)
			e->hi_since = now, e->hi_count = 0;
		if (++e->hi_count > (e->hi_picture ? 20u : 6u))
		{
			e->hi_volatile = 1;
			*scale = 1;
			return e->id;
		}
		static u32 *a, *b;
		if (!a)
		{
			a = malloc(1024 * 1024 * 4 * sizeof(u32));
			b = malloc(1024 * 1024 * 4 * sizeof(u32));
		}
		static int log = -1;	/* M1_HILOG=1: each upscale, with its time */
		if (log < 0)
			log = getenv("M1_HILOG") != 0;
		const clock_t t0 = log ? clock() : 0;
		decode(tcw, w, h, a);
		/* an opaque picture (the character select portraits) or a cut-out (text): Anime4K on
		   the GPU (render.c) */
		int solid = 0, clear = 0, other = 0;
		for (u32 i = 0; i < w * h; i++)
		{
			const u32 al = a[i] >> 24;
			solid |= al == 255, clear |= al == 0, other |= al != 0 && al != 255;
		}
		const int opaque = !clear && !other, cut = solid && clear && !other;
		unsigned gl_a4k_upscale(unsigned id, u32 w, u32 h, const u32 *rgba, int runs, const u8 *alpha, const u32 *orig);
		const int runs = k == 4 ? 2 : 1;
		unsigned nn = 0;
		e->hi_edge = 0;
		if (opaque && pictures)
			nn = gl_a4k_upscale(e->hi_id, w, h, a, runs, 0, 0);
		else if (cut && text >= 2)	/* TEXT HI-RES */
		{
			static u8 *al;
			if (!al)
				al = malloc(1024 * 1024 * 4);
			cut_alpha(a, w, h, (u32)k, al);
			memcpy(b, a, (size_t)w * h * sizeof(u32));	/* the original colours, for the clear texels */
			cut_fill(a, w, h);
			nn = gl_a4k_upscale(e->hi_id, w, h, a, runs, al, b);
			e->hi_edge = nn != 0;
			if (!nn)
				decode(tcw, w, h, a);	/* the cubic enlargement below, from the original */
		}
		e->hi_picture = nn && opaque;
		e->hi_plain = !nn && !text;
		if (nn)
			e->hi_id = nn;
		else if (!e->hi_plain)
		{
			if (!hj.thread)
			{
				hj.src = malloc(1024 * 1024 * sizeof(u32));
				hj.dst = malloc(1024 * 1024 * 16 * sizeof(u32));
				hj.go = SDL_CreateSemaphore(0);
				hj.thread = SDL_CreateThread(hj_main, "upscale", 0);
			}
			if (hj.thread)
			{
				memcpy(hj.src, a, (size_t)w * h * sizeof(u32));
				hj.e = e, hj.tcw = tcw, hj.size = e->size, hj.w = w, hj.h = h, hj.k = (u32)k, hj.key = hj_key(e);
				e->hi_pending = 1;
				SDL_AtomicSet(&hj.state, 1);
				SDL_SemPost(hj.go);
				if (log)
					printf("hi: render %llu tcw %08x %ux%u x%d queued\n",(unsigned long long)now, tcw, w, h, k);
				budget_used += video_ms() - budget_t0;
				*scale = 1;
				return e->id;	/* plain until the worker is done */
			}
			upscale(a, w, h, (u32)k, b);	/* no thread: here */
			e->hi_id = gl_upload_texture(e->hi_id, w * k, h * k, b);
		}
		if (log)
		{
			u64 render_count(void);
			printf("hi: render %llu tcw %08x %ux%u x%d %.1f ms\n", (unsigned long long)render_count(), tcw, w, h, k,
				   (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
		}
		e->hi_ok = 1;
		budget_used += video_ms() - budget_t0;
		if (e->hi_plain)
		{
			*scale = 1;
			return e->id;
		}
	}
	*edge = e->hi_edge;
	return e->hi_id;
}

/* ---- replacement art ------------------------------------------------------------------------
 * M1_TEXTURES (the player build: textures next to the executable) holds replace.txt, one line
 * per piece of new art:  <hash> <x> <y> <w> <h> <file.png>
 * hash: the texture's VRAM bytes (M1_TEXLOG prints it; the palette does not count), x y w h:
 * the texels the art covers, file: k times that size (k a whole number, the same for every
 * piece of one texture), so the art keeps the original's proportions. The texture is then drawn
 * from a copy k times as large: the original enlarged with bilinear filtering, the art pasted
 * over its texels. For a paletted texture the art is in palette terms, so every colouring the
 * game picks works: red and green = two palette entries, blue = the mix from the first to the
 * second (0-255), alpha = coverage. Other textures: plain RGBA. */
typedef struct
{
	u64 hash;
	u32 x, y, w, h;
	char file[64];
	int loaded;		/* 1 read, -1 could not be read */
	u8 *px;
	u32 pw, ph;
} Rep;
static Rep reps[64];
static int nreps, reps_ready;
static char rep_dir[512];

unsigned char *png_load(const char *path, unsigned *w, unsigned *h, const char **err);

static void reps_init(void)
{
	reps_ready = 1;
	const char *d = getenv("M1_TEXTURES");
	if (!d || !*d)
		return;
	snprintf(rep_dir, sizeof(rep_dir), "%s", d);
	char path[600], line[256];
	snprintf(path, sizeof(path), "%s/replace.txt", rep_dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && nreps < (int)(sizeof(reps) / sizeof(reps[0])))
	{
		Rep *r = &reps[nreps];
		unsigned long long h;
		if (line[0] != '#' && sscanf(line, "%llx %u %u %u %u %63s", &h, &r->x, &r->y, &r->w, &r->h, r->file) == 6 && r->w && r->h)
			r->hash = h, nreps++;
	}
	fclose(f);
	printf("textures: %d replacement pieces in %s\n", nreps, rep_dir);
}

static u32 lerp_rgba(u32 a, u32 b, u32 t)
{
	u32 out = 0;
	for (int sh = 0; sh < 32; sh += 8)
		out |= ((((a >> sh) & 255) * (255 - t) + ((b >> sh) & 255) * t + 127) / 255) << sh;
	return out;
}

/* the texture with its replacement art (render.c draws it with bilinear filtering), 0 when it
   has none */
unsigned tex_get_rep(u32 tcw, u32 tsp, const float *uv)
{
	if (!reps_ready)
		reps_init();
	if (!nreps)
		return 0;
	TexEntry *e = lookup(tcw, tsp);
	if (uv)
	{
		/* uv: the polygon's texture coordinates (u0 v0 u1 v1). A polygon that draws none of the
		   art's texels uses the plain texture: rebuilding the art copy means enlarging the whole
		   texture, and Inferno's explosion recolours the hit labels' texture (its flames are in
		   the other half) every frame, 18 to 26 ms a frame (issue #19) */
		const u32 tw = 8u << ((tsp >> 3) & 7), th = (tcw >> 31) ? tw : 8u << (tsp & 7);
		int hit = uv[0] < 0 || uv[1] < 0 || uv[2] > 1 || uv[3] > 1;	/* wrapping: any texel */
		for (int i = 0; i < nreps && !hit; i++)
		{
			const Rep *r = &reps[i];
			hit = r->hash == e->hash && uv[2] * tw > r->x && uv[0] * tw < r->x + r->w &&
				  uv[3] * th > r->y && uv[1] * th < r->y + r->h;	/* the flames end on the art's first row */
		}
		if (!hit)
			return 0;
	}
	const u64 key = (e->hash ^ e->pal_hash) | 1;
	if (e->rep_key == key)
		return e->rep_id;
	e->rep_key = key;
	const u32 w = 8u << ((tsp >> 3) & 7), h = (tcw >> 31) ? w : 8u << (tsp & 7);
	u32 k = 0;
	for (int i = 0; i < nreps; i++)
	{
		Rep *r = &reps[i];
		if (r->hash != e->hash)
			continue;
		if (!r->loaded)
		{
			char path[700];
			const char *err = "";
			snprintf(path, sizeof(path), "%s/%s", rep_dir, r->file);
			r->px = png_load(path, &r->pw, &r->ph, &err);
			r->loaded = r->px && r->pw % r->w == 0 && r->ph % r->h == 0 && r->pw / r->w == r->ph / r->h ? 1 : -1;
			if (r->loaded < 0)
				printf("textures: %s: %s\n", path, r->px ? "not a whole multiple of its texels" : err);
		}
		if (r->loaded > 0 && r->x + r->w <= w && r->y + r->h <= h)
			k = r->pw / r->w;
	}
	if (!k || w * k > 2048 || h * k > 2048)
		return e->rep_id = 0;
	static u32 *a, *b;
	if (!a)
	{
		a = malloc(1024 * 1024 * sizeof(u32));
		b = malloc(2048 * 2048 * sizeof(u32));
	}
	decode(tcw, w, h, a);
	/* the original, enlarged with bilinear filtering on premultiplied colour */
	for (u32 Y = 0; Y < h * k; Y++)
	{
		const float fy = (Y + 0.5f) / k - 0.5f;
		const int y0 = (int)floorf(fy);
		const float ty = fy - y0;
		for (u32 X = 0; X < w * k; X++)
		{
			const float fx = (X + 0.5f) / k - 0.5f;
			const int x0 = (int)floorf(fx);
			const float tx = fx - x0;
			float acc[4] = { 0, 0, 0, 0 };
			for (int j = 0; j < 4; j++)
			{
				int x = x0 + (j & 1), y = y0 + (j >> 1);
				x = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
				y = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
				const float wt = ((j & 1) ? tx : 1 - tx) * ((j >> 1) ? ty : 1 - ty);
				const u32 c = a[(u32)y * w + (u32)x];
				const float al = (float)(c >> 24);
				acc[0] += wt * (c & 255) * al, acc[1] += wt * (c >> 8 & 255) * al, acc[2] += wt * (c >> 16 & 255) * al;
				acc[3] += wt * al;
			}
			u32 out = (u32)(acc[3] + 0.5f) << 24;
			for (int c = 0; c < 3 && acc[3] > 0.5f; c++)
			{
				const float v = acc[c] / acc[3];
				out |= (u32)(v > 255 ? 255 : v + 0.5f) << (8 * c);
			}
			b[(size_t)Y * w * k + X] = out;
		}
	}
	const u32 fmt = (tcw >> 27) & 7;
	const u32 bank = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8;
	for (int i = 0; i < nreps; i++)
	{
		const Rep *r = &reps[i];
		if (r->hash != e->hash || r->loaded <= 0 || r->pw != r->w * k || r->x + r->w > w || r->y + r->h > h)
			continue;
		for (u32 y = 0; y < r->ph; y++)
			for (u32 x = 0; x < r->pw; x++)
			{
				const u8 *q = r->px + ((size_t)y * r->pw + x) * 4;
				u32 c = q[0] | q[1] << 8 | q[2] << 16 | (u32)q[3] << 24;
				if (fmt == 5 || fmt == 6)
				{
					const u32 m = lerp_rgba(palette(bank + q[0]), palette(bank + q[1]), q[2]);
					c = (m & 0xffffff) | ((m >> 24) * q[3] + 127) / 255 << 24;
				}
				b[(size_t)(r->y * k + y) * w * k + r->x * k + x] = c;
			}
	}
	e->rep_id = gl_upload_texture(e->rep_id, w * k, h * k, b);
	if (getenv("M1_SLOWSTATE"))
	{
		u64 render_count(void);
		printf("reprebuild: render %llu tcw %08x %ux%u k %u uv %s %.3f %.3f %.3f %.3f\n", (unsigned long long)render_count(), tcw, w, h, k,
			   uv ? "" : "(none)", uv ? uv[0] : 0, uv ? uv[1] : 0, uv ? uv[2] : 0, uv ? uv[3] : 0);
	}
	return e->rep_id;
}

u64 tex_decodes(void) { return decodes; }

/* the texture's pixels as RGBA (artdump.c); out holds up to 1024 x 1024 */
u32 tex_decode_rgba(u32 tcw, u32 tsp, u32 *out, u32 *w, u32 *h)
{
	*w = 8u << ((tsp >> 3) & 7);
	*h = (tcw >> 31) ? *w : 8u << (tsp & 7);
	decode(tcw, *w, *h, out);
	return 1;
}
u64 tex_rehashed(void) { return rehashed; }

/* identity of a texture's current contents (artswap.c): the hash of its VRAM bytes (and palette
   bank), recomputed only when the pages were written since it was decoded */
u64 tex_content_id(u32 tcw, u32 tsp)
{
	const u32 size = tsp & 63;
	const int pal = ((tcw >> 27) & 7) >= 5;
	u32 slot = home_slot(tcw, size);
	for (u32 probe = 0; probe < NPROBE; probe++, slot = (slot + 1) & (NTEX - 1))
	{
		const TexEntry *e = &cache[slot];
		if (!e->used)
			break;
		if (e->tcw == tcw && e->size == size)
		{
			const u64 h = range_gen(e->start, e->span) == e->gen_sum ? e->hash : range_hash(e->start, e->span);
			return h ^ (pal ? bank_hash(tcw) * 31 : 0);
		}
	}
	return (u64)hw_vram_gen(((tcw & 0x1fffff) << 3) >> 12) << 32 | hw_palette_gen();	/* not decoded yet */
}
