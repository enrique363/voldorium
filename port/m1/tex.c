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
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++)
			{
				const u32 blk = twiddle(x >> 1, y >> 1, w >> 1, h >> 1);
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
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
		{
			const u32 t = twiddled ? twiddle(x, y, w, h) : y * stride + x;
			u32 c;
			if (fmt == 5)
			{
				const u8 b = vram[(addr + t / 2) & (VRAM_SIZE - 1)];
				c = palette((((tcw >> 21) & 63) << 4) | ((t & 1) ? b >> 4 : b & 15));
			}
			else if (fmt == 6)
				c = palette((((tcw >> 25) & 3) << 8) | vram[(addr + t) & (VRAM_SIZE - 1)]);
			else if (fmt == 3)
			{
				/* YUV422: pairs of texels share U and V (twiddled pairs run along y) */
				const u32 t0 = twiddled ? twiddle(x & ~1u, y, w, h) : y * stride + (x & ~1u);
				const u32 t1 = twiddled ? twiddle(x | 1u, y, w, h) : y * stride + (x | 1u);
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
#define NTEX 2048
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
} TexEntry;
static TexEntry cache[NTEX];
static u64 decodes, rehashed;

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
	u32 slot = ((tcw * 2654435761u) ^ size) % NTEX;
	for (int probe = 0; probe < NTEX; probe++, slot = (slot + 1) % NTEX)
	{
		TexEntry *e = &cache[slot];
		if (e->used && e->tcw == tcw && e->size == size)
		{
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
		if (!e->used)
			break;
	}
	TexEntry *e = &cache[slot];
	static u32 buf[1024 * 1024];
	const u32 hh = (tcw >> 31) ? w : h;
	const u32 span = decode(tcw, w, h, buf);
	decodes++;
	{
		/* M1_TEXLOG=n: list the textures decoded during render n */
		static long want = -2;
		if (want == -2)
			want = getenv("M1_TEXLOG") ? atol(getenv("M1_TEXLOG")) : -1;
		u64 render_count(void);
		if ((long)render_count() == want)
			printf("texlog: tcw %08x %ux%u fmt %u pal %d start %06x span %u gen %s\n", tcw, w, hh, (tcw >> 27) & 7, pal, start, span,
				   e->used && e->tcw == tcw ? (range_gen(e->start, e->span) != e->gen_sum ? "vram" : "palette") : "new");
	}
	e->id = gl_upload_texture(e->used ? e->id : 0, w, hh, buf);
	if (!e->used || e->tcw != tcw || e->size != size)
		e->hi_volatile = 0, e->hi_count = 0;	/* another texture in this slot */
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

/* the texture upscaled (4x up to 256 texels, 2x up to 512) for 2D drawn with bilinear
   filtering at high resolutions (render.c: text and HUD); *scale = the factor,
   1 when the texture is too large (then it is the plain one) */
unsigned tex_get_hi(u32 tcw, u32 tsp, int *scale)
{
	TexEntry *e = lookup(tcw, tsp);
	const u32 w = 8u << ((tsp >> 3) & 7), h = (tcw >> 31) ? w : 8u << (tsp & 7);
	const u32 big = w > h ? w : h;
	const int k = big <= 256 ? 4 : big <= 512 ? 2 : 1;
	*scale = k;
	if (k == 1 || e->hi_volatile)
	{
		*scale = 1;
		return e->id;
	}
	if (!e->hi_ok)
	{
		/* a texture rewritten every few frames (animated surfaces, palette cycling) is not
		   text: more than 6 upscales within 300 renders and it stays plain */
		u64 render_count(void);
		const u64 now = render_count();
		if (now - e->hi_since > 300)
			e->hi_since = now, e->hi_count = 0;
		if (++e->hi_count > 6)
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
		upscale(a, w, h, (u32)k, b);
		e->hi_id = gl_upload_texture(e->hi_id, w * k, h * k, b);
		if (log)
		{
			u64 render_count(void);
			printf("hi: render %llu tcw %08x %ux%u x%d %.1f ms\n", (unsigned long long)render_count(), tcw, w, h, k,
				   (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
		}
		e->hi_ok = 1;
	}
	return e->hi_id;
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
	u32 slot = ((tcw * 2654435761u) ^ size) % NTEX;
	for (int probe = 0; probe < NTEX; probe++, slot = (slot + 1) % NTEX)
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
