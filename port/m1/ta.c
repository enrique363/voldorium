/* Tile accelerator input -> display lists.
 *
 * The TA takes 32-byte parameters; the first word (PCW) gives the type: 0 end of list,
 * 1 user tile clip, 2 object list set, 4 polygon / modifier volume global parameter,
 * 5 sprite global parameter, 7 vertex. The first global parameter after a list starts fixes
 * the list type (PCW bits 24-26: 0 opaque, 1 opaque modifier, 2 translucent, 3 translucent
 * modifier, 4 punch-through). A global parameter selects one of the vertex formats 0-14
 * (texture, colour type, 16-bit UV, two volumes), sprites and modifier volumes have their own;
 * formats 5, 6, 11-14, sprites and modifier volumes are 64 bytes. Vertices form triangle
 * strips that end at a vertex with PCW bit 28 set. Each end of list raises the list's
 * end-of-transfer interrupt (ISTNRM bits 7, 8, 9, 10, 21).
 *
 * Display lists are kept per TA context, keyed by TA_ISP_BASE when the list is initialised;
 * STARTRENDER draws the context whose key equals PARAM_BASE (the frame the PowerVR renders
 * while the next one is being built). Modifier volumes are parsed but not drawn yet; for
 * two-volume polygons volume 0 is used.
 */
#include "m1.h"
#include "ta.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

u32 hw_reg(u32 addr);

#define NCTX 4
static TaCtx ctx[NCTX];
static TaCtx *cur;
static u64 serial;

static struct
{
	u8 buf[64];
	u32 have, need;
	int list;			/* list type of the open list, -1 = none yet */
	int vtype;			/* vertex format of the current global parameter */
	u32 vsize;
	u32 pcw, isp, tsp, tcw;
	float face[4], face_ofs[4];	/* intensity colours (A, R, G, B) */
	u32 sprite_col, sprite_ofs;
	int strip_open;
	int mv_new;			/* the next modifier volume triangle starts a new global parameter */
	u16 clip_rect[4];	/* last user tile clip */
	int via_sq;			/* the current global parameter came by store queue */
	u8 cls;
	u16 grp;
	u32 caller;
} st = { .list = -1, .vsize = 32 };

u64 ta_params, ta_vertices, ta_lists_ended;
static const int list_bit[5] = { 7, 8, 9, 10, 21 };

static TaCtx *ctx_for(u32 key)
{
	TaCtx *oldest = &ctx[0];
	for (int i = 0; i < NCTX; i++)
	{
		if (ctx[i].used && ctx[i].key == key)
			return &ctx[i];
		if (!ctx[i].used || ctx[i].serial < oldest->serial)
			oldest = &ctx[i];
	}
	if (!oldest->v)
	{
		oldest->v = malloc(sizeof(TaVert) * TA_MAXV);
		oldest->p = malloc(sizeof(TaPoly) * TA_MAXP);
		oldest->mv = malloc(sizeof(float) * 9 * TA_MAXMV);
		oldest->mvmode = malloc(TA_MAXMV);
	}
	oldest->used = 1;
	oldest->key = key;
	return oldest;
}

void ta_list_init(void)
{
	cur = ctx_for(hw_reg(0x005f8128) & 0x7fffffu);	/* TA_ISP_BASE */
	cur->nv = cur->np = cur->nmv = 0;
	cur->serial = ++serial;
	st.have = 0;
	st.list = -1;
	st.strip_open = 0;
}

void ta_list_cont(void)
{
	st.list = -1;
	st.strip_open = 0;
}

TaCtx *ta_render_ctx(void)
{
	const u32 key = hw_reg(0x005f8020) & 0x7fffffu;	/* PARAM_BASE */
	for (int i = 0; i < NCTX; i++)
		if (ctx[i].used && ctx[i].key == key)
			return &ctx[i];
	return cur;
}

static int vertex_type(u32 pcw, int list)
{
	const int tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, uv16 = pcw & 1, vol = (pcw >> 6) & 1;
	if (list == 1 || list == 3)
		return 17;	/* modifier volume */
	if (!tex)
	{
		if (vol)
			return col == 0 ? 9 : 10;
		return col == 0 ? 0 : col == 1 ? 1 : 2;
	}
	if (vol)
		return col == 0 ? (uv16 ? 12 : 11) : (uv16 ? 14 : 13);
	return col == 0 ? (uv16 ? 4 : 3) : col == 1 ? (uv16 ? 6 : 5) : (uv16 ? 8 : 7);
}

static u32 header_size(u32 pcw, int list)
{
	if ((pcw >> 29) != 4 || list == 1 || list == 3)
		return 32;
	const u32 col = (pcw >> 4) & 3, offset = (pcw >> 2) & 1, vol = (pcw >> 6) & 1;
	return col == 2 && (offset || vol) ? 64 : 32;
}

static u32 word(const u8 *p, int i)
{
	u32 v;
	memcpy(&v, p + 4 * i, 4);
	return v;
}
static float fword(const u8 *p, int i)
{
	float v;
	memcpy(&v, p + 4 * i, 4);
	return v;
}
static float uv_hi(u32 w) { u32 b = w & 0xffff0000u; float f; memcpy(&f, &b, 4); return f; }
static float uv_lo(u32 w) { u32 b = w << 16; float f; memcpy(&f, &b, 4); return f; }

static u32 pack(float a, float r, float g, float b)
{
	const float c[4] = { a, r, g, b };
	u32 v = 0;
	for (int i = 0; i < 4; i++)
	{
		float x = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
		v = (v << 8) | (u32)(x * 255.0f + 0.5f);
	}
	return v;
}
static u32 intensity(const float *face, float k)
{
	return pack(face[0], face[1] * k, face[2] * k, face[3] * k);
}

static void begin_strip(int sprite)
{
	if (cur->np >= TA_MAXP)
		return;
	TaPoly *p = &cur->p[cur->np++];
	p->isp = st.isp;
	p->tsp = st.tsp;
	p->tcw = st.tcw;
	p->list = (u8)st.list;
	p->sprite = (u8)sprite;
	p->first = cur->nv;
	p->count = 0;
	p->clip = (st.pcw >> 16) & 3;
	p->via_sq = (u8)st.via_sq;
	p->cls = st.cls;
	p->grp = st.grp;
	p->shadow = (u8)((st.pcw >> 7) & 1);
	p->caller = st.caller;
	memcpy(p->clip_rect, st.clip_rect, sizeof(p->clip_rect));
	st.strip_open = 1;
}

static TaVert *new_vertex(void)
{
	if (cur->nv >= TA_MAXV || cur->np == 0)
		return 0;
	cur->p[cur->np - 1].count++;
	return &cur->v[cur->nv++];
}

int ta_via_sq;
u8 ta_cur_class;
u16 ta_cur_group;
u32 m1_current_function(void);

/* write tagging: which 2D class wrote each 32-byte slot of Kamui's vertex buffers (the TA DMA
   sources, 0x0CD9xxxx-0x0CE6xxxx in this game; the window can be moved with M1_TAGWIN=lo,len) */
u32 sh4_tag_lo = 0xd00000, sh4_tag_len = 0x200000;
static u8 slot_class[0x200000 / 32];
static u16 slot_group[0x200000 / 32];
u64 tag_writes, tag_writes_2d, tag_page_2d[256];
void sh4_tag_write(u32 off)
{
	const u32 slot = (off - sh4_tag_lo) >> 5;
	if (slot < sizeof(slot_class))
	{
		slot_class[slot] = m1_2d_class;
		slot_group[slot] = m1_2d_group;
	}
	tag_writes++;
	if (m1_2d_class)
	{
		tag_writes_2d++;
		tag_page_2d[(off >> 16) & 255]++;	/* M1_TAGWIN=0,1000000 to see where 2D code writes */
	}
}
void ta_tag_config(void)
{
	const char *e = getenv("M1_TAGWIN");
	if (e)
		sscanf(e, "%x,%x", &sh4_tag_lo, &sh4_tag_len);
}
u8 ta_class_at(u32 a)
{
	const u32 off = (a & 0xffffff) - sh4_tag_lo;
	return off < sh4_tag_len ? slot_class[off >> 5] : 0;
}
u16 ta_group_at(u32 a)
{
	const u32 off = (a & 0xffffff) - sh4_tag_lo;
	return off < sh4_tag_len ? slot_group[off >> 5] : 0;
}

static void global_param(const u8 *p, u32 type)
{
	const u32 pcw = word(p, 0);
	st.via_sq = ta_via_sq;
	st.cls = ta_cur_class;
	st.grp = ta_cur_group;
	st.caller = ta_via_sq ? m1_current_function() : 0;
	if (st.list < 0)
		st.list = (pcw >> 24) & 7;
	st.pcw = pcw;
	/* the TA replaces the ISP word's texture / offset / gouraud / 16-bit UV bits (25-22) with
	   the PCW's (bits 3-0); games often leave them 0 in the ISP word they send */
	st.isp = (word(p, 1) & ~0x03c00000u) | (pcw & 15) << 22;
	st.tsp = word(p, 2);
	st.tcw = word(p, 3);
	st.strip_open = 0;
	st.mv_new = 1;
	if (type == 5)
	{
		st.vtype = 15;
		st.vsize = 64;
		st.sprite_col = word(p, 4);
		st.sprite_ofs = word(p, 5);
		return;
	}
	st.vtype = vertex_type(pcw, st.list);
	st.vsize = (st.vtype == 5 || st.vtype == 6 || st.vtype >= 11) ? 64 : 32;
	const u32 col = (pcw >> 4) & 3;
	if (col == 2 && st.list != 1 && st.list != 3)
	{
		const int base = header_size(pcw, st.list) == 64 ? 8 : 4;
		for (int i = 0; i < 4; i++)
			st.face[i] = fword(p, base + i);
		if (base == 8)
			for (int i = 0; i < 4; i++)
				st.face_ofs[i] = fword(p, 12 + i);
	}
}

static void sprite_vertex(const u8 *p)
{
	if (!st.strip_open)
		begin_strip(1);
	const float ax = fword(p, 1), ay = fword(p, 2), az = fword(p, 3);
	const float bx = fword(p, 4), by = fword(p, 5), bz = fword(p, 6);
	const float cx = fword(p, 7), cy = fword(p, 8), cz = fword(p, 9);
	const float dx = fword(p, 10), dy = fword(p, 11);
	const u32 auv = word(p, 13), buv = word(p, 14), cuv = word(p, 15);
	const float au = uv_hi(auv), av = uv_lo(auv), bu = uv_hi(buv), bv = uv_lo(buv), cu = uv_hi(cuv), cv = uv_lo(cuv);
	/* strip A B D C covers the quad; D's depth and UV complete the parallelogram */
	const float x[4] = { ax, bx, dx, cx }, y[4] = { ay, by, dy, cy }, z[4] = { az, bz, az + cz - bz, cz };
	const float u[4] = { au, bu, au + cu - bu, cu }, v[4] = { av, bv, av + cv - bv, cv };
	for (int i = 0; i < 4; i++)
	{
		TaVert *t = new_vertex();
		if (!t)
			return;
		t->x = x[i]; t->y = y[i]; t->z = z[i]; t->u = u[i]; t->v = v[i];
		t->col = st.sprite_col;
		t->ofs = st.sprite_ofs;
	}
	st.strip_open = 0;	/* each sprite is its own quad */
}

static void vertex(const u8 *p)
{
	const u32 pcw = word(p, 0);
	ta_vertices++;
	if (st.list == 1 || st.list == 3)
	{
		/* modifier volume triangle (A, B, C at words 1-9); the opaque list's volumes shade
		   opaque and punch-through polygons, the translucent list's are not used here */
		if (st.list == 1 && cur->nmv < TA_MAXMV)
		{
			for (int i = 0; i < 9; i++)
				cur->mv[cur->nmv * 9 + (u32)i] = fword(p, 1 + i);
			/* bits 0-2: volume instruction (ISP 31-29), bit 3: last of a volume (PCW bit 6),
			   bit 7: first triangle of its global parameter */
			cur->mvmode[cur->nmv++] = (u8)(((st.isp >> 29) & 7) | ((st.pcw >> 6) & 1) << 3 | (st.mv_new ? 0x80 : 0));
			st.mv_new = 0;
		}
		return;
	}
	if (st.vtype == 15)
	{
		sprite_vertex(p);
		return;
	}
	if (!st.strip_open)
		begin_strip(0);
	TaVert *t = new_vertex();
	if (t)
	{
		t->x = fword(p, 1);
		t->y = fword(p, 2);
		t->z = fword(p, 3);
		t->u = t->v = 0;
		t->col = 0xffffffffu;
		t->ofs = 0;
		switch (st.vtype)
		{
		case 0: t->col = word(p, 6); break;
		case 1: t->col = pack(fword(p, 4), fword(p, 5), fword(p, 6), fword(p, 7)); break;
		case 2: t->col = intensity(st.face, fword(p, 6)); break;
		case 3: t->u = fword(p, 4); t->v = fword(p, 5); t->col = word(p, 6); t->ofs = word(p, 7); break;
		case 4: t->u = uv_hi(word(p, 4)); t->v = uv_lo(word(p, 4)); t->col = word(p, 6); t->ofs = word(p, 7); break;
		case 5:
			t->u = fword(p, 4); t->v = fword(p, 5);
			t->col = pack(fword(p, 8), fword(p, 9), fword(p, 10), fword(p, 11));
			t->ofs = pack(fword(p, 12), fword(p, 13), fword(p, 14), fword(p, 15));
			break;
		case 6:
			t->u = uv_hi(word(p, 4)); t->v = uv_lo(word(p, 4));
			t->col = pack(fword(p, 8), fword(p, 9), fword(p, 10), fword(p, 11));
			t->ofs = pack(fword(p, 12), fword(p, 13), fword(p, 14), fword(p, 15));
			break;
		case 7:
			t->u = fword(p, 4); t->v = fword(p, 5);
			t->col = intensity(st.face, fword(p, 6)); t->ofs = intensity(st.face_ofs, fword(p, 7));
			break;
		case 8:
			t->u = uv_hi(word(p, 4)); t->v = uv_lo(word(p, 4));
			t->col = intensity(st.face, fword(p, 6)); t->ofs = intensity(st.face_ofs, fword(p, 7));
			break;
		case 9: t->col = word(p, 4); break;
		case 10: t->col = intensity(st.face, fword(p, 4)); break;
		case 11: t->u = fword(p, 4); t->v = fword(p, 5); t->col = word(p, 6); t->ofs = word(p, 7); break;
		case 12: t->u = uv_hi(word(p, 4)); t->v = uv_lo(word(p, 4)); t->col = word(p, 6); t->ofs = word(p, 7); break;
		case 13:
			t->u = fword(p, 4); t->v = fword(p, 5);
			t->col = intensity(st.face, fword(p, 6)); t->ofs = intensity(st.face_ofs, fword(p, 7));
			break;
		case 14:
			t->u = uv_hi(word(p, 4)); t->v = uv_lo(word(p, 4));
			t->col = intensity(st.face, fword(p, 6)); t->ofs = intensity(st.face_ofs, fword(p, 7));
			break;
		}
	}
	if (pcw & (1u << 28))
		st.strip_open = 0;	/* end of strip */
}

static void param(const u8 *p)
{
	const u32 type = word(p, 0) >> 29;
	ta_params++;
	switch (type)
	{
	case 0:	/* end of list */
		if (st.list >= 0 && st.list < 5)
			hw_raise(list_bit[st.list]);
		ta_lists_ended++;
		st.list = -1;
		st.strip_open = 0;
		break;
	case 1:	/* user tile clip: words 4-7 = x min, y min, x max, y max in 32-pixel tiles */
		st.clip_rect[0] = (u16)((word(p, 4) & 63) * 32);
		st.clip_rect[1] = (u16)((word(p, 5) & 31) * 32);
		st.clip_rect[2] = (u16)(((word(p, 6) & 63) + 1) * 32);
		st.clip_rect[3] = (u16)(((word(p, 7) & 31) + 1) * 32);
		break;
	case 4:
	case 5:
		global_param(p, type);
		break;
	case 7:
		vertex(p);
		break;
	}
}

void ta_input(const u8 *data, u32 len)
{
	if (!cur)
		ta_list_init();
	for (u32 i = 0; i + 32 <= len; i += 32)
	{
		memcpy(st.buf + st.have, data + i, 32);
		if (st.have == 0)
		{
			const u32 pcw = word(st.buf, 0), type = pcw >> 29;
			if (type == 7)
				st.need = st.vsize;
			else if (type == 4)
			{
				const int list = st.list < 0 ? (int)((pcw >> 24) & 7) : st.list;
				st.need = header_size(pcw, list);
			}
			else
				st.need = 32;
		}
		st.have += 32;
		if (st.have >= st.need)
		{
			param(st.buf);
			st.have = 0;
		}
	}
}
