/* Minimal PNG reader for the 16:9 replacement pictures (artswap.c): 8-bit RGB or RGBA (also
 * grey, grey + alpha and palette), not interlaced. Includes its own inflate (stored, fixed and
 * dynamic Huffman blocks), so no compression library is needed.
 * png_load returns malloc'd RGBA (top row first) or 0 with a reason in *err. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	const uint8_t *in, *end;
	uint32_t bits, nbits;
	uint8_t *out;
	size_t len, cap;
	int bad;
} Inflate;

static uint32_t getbits(Inflate *s, int n)
{
	while (s->nbits < (uint32_t)n)
	{
		if (s->in >= s->end)
		{
			s->bad = 1;
			return 0;
		}
		s->bits |= (uint32_t)*s->in++ << s->nbits;
		s->nbits += 8;
	}
	const uint32_t v = s->bits & ((1u << n) - 1);
	s->bits >>= n;
	s->nbits -= (uint32_t)n;
	return v;
}

static void put(Inflate *s, uint8_t b)
{
	if (s->len == s->cap)
	{
		s->cap = s->cap ? s->cap * 2 : 1 << 16;
		s->out = realloc(s->out, s->cap);
	}
	s->out[s->len++] = b;
}

/* canonical Huffman table: counts per length, symbols sorted by code */
typedef struct { uint16_t count[16], sym[320]; } Huff;

static void build(Huff *h, const uint8_t *lens, int n)
{
	uint16_t offs[16];
	memset(h->count, 0, sizeof(h->count));
	for (int i = 0; i < n; i++)
		h->count[lens[i]]++;
	h->count[0] = 0;
	offs[1] = 0;
	for (int i = 1; i < 15; i++)
		offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
	for (int i = 0; i < n; i++)
		if (lens[i])
			h->sym[offs[lens[i]]++] = (uint16_t)i;
}

static int decode(Inflate *s, const Huff *h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len < 16; len++)
	{
		code |= (int)getbits(s, 1);
		const int count = h->count[len];
		if (code - count < first)
			return h->sym[index + (code - first)];
		index += count;
		first = (first + count) << 1;
		code <<= 1;
		if (s->bad)
			return -1;
	}
	s->bad = 1;
	return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
									   131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
										2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(Inflate *s, const Huff *lit, const Huff *dist)
{
	for (;;)
	{
		const int sym = decode(s, lit);
		if (sym < 0 || s->bad)
			return;
		if (sym < 256)
			put(s, (uint8_t)sym);
		else if (sym == 256)
			return;
		else
		{
			const int li = sym - 257;
			if (li >= 29)
			{
				s->bad = 1;
				return;
			}
			const uint32_t len = len_base[li] + getbits(s, len_extra[li]);
			const int di = decode(s, dist);
			if (di < 0 || di >= 30)
			{
				s->bad = 1;
				return;
			}
			const uint32_t d = dist_base[di] + getbits(s, dist_extra[di]);
			if (d > s->len)
			{
				s->bad = 1;
				return;
			}
			for (uint32_t i = 0; i < len; i++)
				put(s, s->out[s->len - d]);
		}
	}
}

static int inflate_zlib(const uint8_t *in, size_t n, uint8_t **out, size_t *outlen)
{
	if (n < 6)
		return 0;
	Inflate s = { in + 2, in + n, 0, 0, 0, 0, 0, 0 };	/* skip the zlib header */
	int last;
	do
	{
		last = (int)getbits(&s, 1);
		const uint32_t type = getbits(&s, 2);
		if (type == 0)
		{
			s.bits = 0, s.nbits = 0;	/* to a byte boundary */
			if (s.end - s.in < 4)
				return 0;
			const uint32_t len = (uint32_t)(s.in[0] | s.in[1] << 8);
			s.in += 4;
			if ((size_t)(s.end - s.in) < len)
				return 0;
			for (uint32_t i = 0; i < len; i++)
				put(&s, s.in[i]);
			s.in += len;
		}
		else if (type == 1)
		{
			static Huff lit, dist;
			static int ready;
			if (!ready)
			{
				uint8_t l[288];
				for (int i = 0; i < 288; i++)
					l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
				build(&lit, l, 288);
				for (int i = 0; i < 30; i++)
					l[i] = 5;
				build(&dist, l, 30);
				ready = 1;
			}
			codes(&s, &lit, &dist);
		}
		else if (type == 2)
		{
			const int hlit = (int)getbits(&s, 5) + 257, hdist = (int)getbits(&s, 5) + 1, hclen = (int)getbits(&s, 4) + 4;
			static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			uint8_t cl[19] = { 0 }, lens[320] = { 0 };
			for (int i = 0; i < hclen; i++)
				cl[order[i]] = (uint8_t)getbits(&s, 3);
			Huff ch, lit, dist;
			build(&ch, cl, 19);
			for (int i = 0; i < hlit + hdist;)
			{
				const int sym = decode(&s, &ch);
				if (sym < 0 || s.bad)
					return 0;
				if (sym < 16)
					lens[i++] = (uint8_t)sym;
				else
				{
					int rep, val = 0;
					if (sym == 16)
					{
						if (!i)
							return 0;
						val = lens[i - 1];
						rep = 3 + (int)getbits(&s, 2);
					}
					else if (sym == 17)
						rep = 3 + (int)getbits(&s, 3);
					else
						rep = 11 + (int)getbits(&s, 7);
					while (rep-- && i < hlit + hdist)
						lens[i++] = (uint8_t)val;
				}
			}
			build(&lit, lens, hlit);
			build(&dist, lens + hlit, hdist);
			codes(&s, &lit, &dist);
		}
		else
			return 0;
		if (s.bad)
		{
			free(s.out);
			return 0;
		}
	} while (!last);
	*out = s.out;
	*outlen = s.len;
	return 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c)
{
	const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

uint8_t *png_load(const char *path, uint32_t *w, uint32_t *h, const char **err)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		*err = "cannot open";
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t)size);
	const size_t got = fread(d, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size || size < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8))
	{
		free(d);
		*err = "not a PNG file";
		return 0;
	}
	uint32_t width = 0, height = 0;
	int depth = 0, ctype = 0, interlace = 0;
	uint8_t pal[256][4];
	memset(pal, 255, sizeof(pal));
	uint8_t *idat = 0;
	size_t nidat = 0;
	for (long i = 8; i + 12 <= size;)
	{
		const uint32_t n = be32(d + i);
		const uint8_t *t = d + i + 4, *c = d + i + 8;
		if (i + 12 + (long)n > size)
			break;
		if (!memcmp(t, "IHDR", 4))
			width = be32(c), height = be32(c + 4), depth = c[8], ctype = c[9], interlace = c[12];
		else if (!memcmp(t, "PLTE", 4))
			for (uint32_t k = 0; k < n / 3 && k < 256; k++)
				pal[k][0] = c[k * 3], pal[k][1] = c[k * 3 + 1], pal[k][2] = c[k * 3 + 2];
		else if (!memcmp(t, "tRNS", 4) && ctype == 3)
			for (uint32_t k = 0; k < n && k < 256; k++)
				pal[k][3] = c[k];
		else if (!memcmp(t, "IDAT", 4))
		{
			idat = realloc(idat, nidat + n);
			memcpy(idat + nidat, c, n);
			nidat += n;
		}
		i += 12 + (long)n;
	}
	free(d);
	const int ch = ctype == 2 ? 3 : ctype == 6 ? 4 : ctype == 0 ? 1 : ctype == 4 ? 2 : ctype == 3 ? 1 : 0;
	if (!width || !height || width > 16384 || height > 16384 || depth != 8 || !ch || interlace)
	{
		free(idat);
		*err = "unsupported PNG (needs 8 bits per channel, not interlaced)";
		return 0;
	}
	uint8_t *raw;
	size_t nraw;
	const int ok = inflate_zlib(idat, nidat, &raw, &nraw);
	free(idat);
	const size_t stride = (size_t)width * ch;
	if (!ok || nraw < (stride + 1) * height)
	{
		if (ok)
			free(raw);
		*err = "damaged PNG data";
		return 0;
	}
	/* undo the filters, row by row (in place: each row's filter byte precedes it) */
	for (uint32_t y = 0; y < height; y++)
	{
		uint8_t *row = raw + y * (stride + 1) + 1;
		const uint8_t *prev = y ? raw + (y - 1) * (stride + 1) + 1 : 0;
		const int ft = row[-1];
		for (size_t x = 0; x < stride; x++)
		{
			const int a = x >= (size_t)ch ? row[x - ch] : 0, b = prev ? prev[x] : 0, c = prev && x >= (size_t)ch ? prev[x - ch] : 0;
			switch (ft)
			{
			case 1: row[x] = (uint8_t)(row[x] + a); break;
			case 2: row[x] = (uint8_t)(row[x] + b); break;
			case 3: row[x] = (uint8_t)(row[x] + ((a + b) >> 1)); break;
			case 4: row[x] = (uint8_t)(row[x] + paeth(a, b, c)); break;
			default: break;
			}
		}
	}
	uint8_t *rgba = malloc((size_t)width * height * 4);
	for (uint32_t y = 0; y < height; y++)
	{
		const uint8_t *row = raw + y * (stride + 1) + 1;
		uint8_t *o = rgba + (size_t)y * width * 4;
		for (uint32_t x = 0; x < width; x++, o += 4)
		{
			const uint8_t *p = row + (size_t)x * ch;
			switch (ctype)
			{
			case 2: o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = 255; break;
			case 6: memcpy(o, p, 4); break;
			case 0: o[0] = o[1] = o[2] = p[0], o[3] = 255; break;
			case 4: o[0] = o[1] = o[2] = p[0], o[3] = p[1]; break;
			default: memcpy(o, pal[p[0]], 4); break;
			}
		}
	}
	free(raw);
	*w = width, *h = height;
	return rgba;
}
