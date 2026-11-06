// SPDX-License-Identifier: MIT
/*
 * mbxemu: the TA. It parses the stream host software writes to the TA slave port, runs
 * each vertex through the VGP (vgp.c), clips and projects the triangles
 * and keeps them in the current hardware context's scene for the render.
 *
 * The hardware bins into per-tile lists in the parameter buffer; the model
 * keeps the scene's triangles instead and rasterizes them at the render
 * (render.c). What the render's caller reads back of the binning is the
 * region array: the TA fills it in at the end of a scene, and at a close
 * (0x808) only for the tiles it has lists for, which is what software
 * expects of a partial render.
 *
 * The stream, word by word (top nibble):
 *   1  an object header: a presence mask, then the blocks it names
 *   4, 5, 6, 7, e  a primitive word (strip, fan, line strip, list, points),
 *      [half the point size or line width,] the sections word, then
 *      vertices until a control word (1: the object ends, 0: restart)
 *   8  a VGP section: flags and range, then a code word (9), instructions
 *   9  instructions to code memory
 *   a  constants
 *   f  0xf0000000: the scene ends (or, with 0x85c bit 7 clear, the TA stops)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <float.h>
#include "emu.h"

#define ST_GOURAUD	(1u << 22)
#define PRIM_CULL_CCW	(1u << 18)
#define PRIM_CULL_CW	(1u << 19)

/* Words of each header block, by mask bit; 0: a bit we don't know */
static const int hdr_words[16] = { 1, 1, 3, 3, 1, 1, 1, 4, 3, 1, 1, 1 };

static void stream_error(struct mbxemu *e, uint32_t v, const char *why)
{
	emu_log(e, "TA stream error at %08x: %s; the TA ignores the rest until it starts again", v, why);
	e->mode = TA_ERROR;
	e->stats.stream_errors++;
	emu_raise(e, I_TA_STREAM);
}

/*
 * A sections word without bit 17: the TA takes nothing (README assumption 30). It
 * stops consuming; what the client writes next waits in the input FIFO, which
 * INT_STATUS bits 16-23 count up to 59; no TA_COMPLETE comes, and a render kick
 * does nothing. A TA start or a soft reset ends it.
 */
static void ta_hang(struct mbxemu *e, uint32_t v)
{
	emu_log(e, "TA: the sections word %08x has bit 17 clear: the TA takes nothing until it starts again", v);
	e->mode = TA_HUNG;
	e->fifo_fill = 1;
	e->stats.ta_hangs++;
}

static void ta_fifo_wait(struct mbxemu *e)
{
	if (e->fifo_fill < TA_FIFO_WORDS)
		e->fifo_fill++;
}

void ta_reset(struct mbxemu *e)
{
	e->fifo_fill = 0;
	e->mode = TA_CMD;
	e->asm_n = 0;
	e->vin_n = 0;
	e->scene_state = -1;
	e->no_scene_warned = 0;
}

/* --- the tile grid --- */

static uint32_t mt_size(uint32_t tiles)
{
	return tiles <= 32 ? 4 : tiles <= 64 ? 8 : 16;
}

/*
 * The tiles a scene covers (0x85c: across, less 1, in bits 20-26, down in
 * 10-17; MBX1's 16-pixel tiles) and the macro tile size the hardware
 * picks for each axis
 */
void tile_grid(uint32_t config, uint32_t *tx, uint32_t *ty, uint32_t *mx, uint32_t *my)
{
	*tx = (config >> 20 & 0x7f) + 1;
	*ty = (config >> 10 & 0xff) + 1;
	*mx = mt_size(*tx);
	*my = mt_size(*ty);
}

/* Tile (x, y)'s list as the parameter buffer writer made it (0 none), or
 * ~0 when the writer isn't modelling this scene */
static uint32_t pbw_list(const struct mbxemu *e, uint32_t tx, uint32_t ty, uint32_t x, uint32_t y)
{
	const struct pbw *w = &e->pbw;

	if (!w->on || w->tx != tx || w->ty != ty)
		return ~0u;
	return x < tx && y < ty ? w->list_head[y * tx + x] : 0;
}

/*
 * The region array as the TA leaves it. Entries grouped by macro tile,
 * macro tiles row by row, each one's tiles row by row; word 0 is
 * 0x20000000 | y << 8 | x with bit 25 for a tile without a list, bit 26
 * on a macro tile's last entry and bits 31 and 26 on the screen's last
 * tile; word 1 the tile's list (pb.c's, or 0 for a scene it doesn't
 * write, a tile then counted as listed by the triangles' boxes). At a
 * close only the tiles listed since the last one are written, and macro
 * tile 0's others as empty, the end mark included: the
 * daemon's region_close() finds the rest by the bit 30 the last render's
 * entries still carry.
 */
static void write_regions(struct mbxemu *e, struct scene *s, int close)
{
	uint32_t tx, ty, mx, my, nx, ny, i, j, n, k = 0, x, y, w0, head;
	uint8_t *cover;
	int listed;
	int p;

	if (!s->regions)
		return;
	tile_grid(s->config, &tx, &ty, &mx, &my);
	nx = (tx + mx - 1) / mx;
	ny = (ty + my - 1) / my;
	if (!(cover = calloc(nx * mx * ny * my, 1)))
		return;
	for (p = 0; p < s->nprims; p++) {
		const struct prim *q = &s->prims[p];

		for (y = q->y0 / TILE; y < (uint32_t)(q->y1 + TILE - 1) / TILE && y < ny * my; y++)
			for (x = q->x0 / TILE; x < (uint32_t)(q->x1 + TILE - 1) / TILE && x < nx * mx; x++)
				cover[y * nx * mx + x] = 1;
	}
	for (j = 0; j < ny; j++)
		for (i = 0; i < nx; i++)
			for (n = 0; n < mx * my; n++, k++) {
				x = i * mx + n % mx;
				y = j * my + n / mx;
				/* listed: by the writer's lists where it models the
				 * scene (after a partial render the model's scene no
				 * longer holds what went before), else by coverage */
				head = pbw_list(e, tx, ty, x, y);
				listed = head == ~0u ? cover[y * nx * mx + x] : head != 0;
				/* a close writes the tiles listed since the last one,
				 * and macro tile 0's others as empty */
				if (close && (!listed || x >= tx || y >= ty) && (k >= mx * my || x >= tx || y >= ty))
					continue;
				w0 = 0x20000000 | y << 8 | x;
				if (!listed)
					w0 |= 0x02000000;
				if (n == mx * my - 1)
					w0 |= 0x04000000;
				if (x == tx - 1 && y == ty - 1)
					w0 |= 0x84000000;	/* at a close too */
				dev_write32(e, s->regions + 8 * k, w0);
				/* an empty tile's word 1 keeps what it held (found during device testing) */
				if (listed)
					dev_write32(e, s->regions + 8 * k + 4, head == ~0u ? 0 : head);
			}
	free(cover);
}

/* --- scenes --- */

/*
 * The contract at TA start (README assumption 31), as found during device testing of what a client
 * does before a scene (found during device testing; the effects are only known
 * for all of it missing at once, which the chip turns into garbage).
 */
static void check_contract(struct mbxemu *e, const struct scene *s)
{
	const uint32_t *R = e->regs;
	uint32_t base = R[R_PB_BASE / 4], first = R[R_FL_FIRST / 4];

	if (!base)
		emu_violation(e, V_PB_BASE, "0x83c (the parameter buffer's base) is not set");
	else if (first && first <= base >> 12)
		emu_violation(e, V_FL_OVERLAP, "the free list starts at or below the buffer's base page "
			      "(its first page is the background object's)");
	if (R[0x6d8 / 4] != 0x09000000 || R[0xce0 / 4] != 1)
		emu_violation(e, V_FL_REGS, "0x6d8 is not 0x09000000 or 0xce0 is not 1");
	if (s->tails & 0xffff)
		emu_violation(e, V_TAILS_ALIGN, "the tail pointers (0x840) are not 64 KB aligned: the TA uses "
			      "the address rounded down");
	if (!R[R_TA_CTX_BASE / 4] || !R[R_EVM_CTX_BASE / 4])
		emu_violation(e, V_CTX, "the TA or EVM context (0x820, 0x864) is not set");
	else if ((R[R_TA_CTX_BASE / 4] | R[R_EVM_CTX_BASE / 4]) & 0x3fff)
		emu_violation(e, V_CTX, "the TA or EVM context (0x820, 0x864) is not 16 KB aligned");
	if (s->regions && (dev_read32(e, s->regions) & 0x22000000) != 0x22000000)
		emu_violation(e, V_REGION_INIT, "the region array was not initialised "
			      "(0x22000000 | the macro tile's origin)");
}

/*
 * What the TA does with a tail pointer that is not 64 KB aligned: it uses the address rounded down,
 * and zeroes the tail area there (found during device testing: at a 16 KB aligned allocation it
 * zeroed the colour buffer's last rows, about 7 KB at 400 x 240). The area's size is not known:
 * 7 bytes for each entry of the grid's bit-interleaved index range fits that one figure.
 */
static void tails_clobber(struct mbxemu *e, const struct scene *s)
{
	uint32_t tx = (s->config >> 20 & 0x3ff) + 1, ty = (s->config >> 10 & 0x3ff) + 1, bits = 0, n;
	static const uint8_t zeros[4096];

	while ((1u << bits) < (tx > ty ? tx : ty))
		bits++;
	for (n = 7u << (2 * bits); n; ) {
		uint32_t k = n < sizeof(zeros) ? n : (uint32_t)sizeof(zeros);

		dev_write(e, (s->tails & ~0xffffu) + (7u << (2 * bits)) - n, zeros, k);
		n -= k;
	}
}

/*
 * 0x800: the TA starts on the current context's scene, a new one unless
 * the context holds one still open (a resume after a partial render or an
 * overflow, or a scene loaded back). Its setup registers are taken now.
 */
void ta_start(struct mbxemu *e)
{
	struct scene *s = &e->ctx[e->regs[R_TA_CONTEXT_ID / 4] & 1];
	int fresh = !s->used || s->complete;

	if (fresh) {
		scene_free(s);
		s->used = 1;
		e->scene_state = -1;
	}
	if (!s->nprims)
		e->stats.scenes++;
	s->regions = e->regs[R_TA_REGIONS / 4];
	s->tails = e->regs[R_TA_TAILS / 4];
	s->closed = 0;
	if (fresh)
		e->viol = 0;
	check_contract(e, s);
	if (s->tails & 0xffff)
		tails_clobber(e, s);
	s->config = e->regs[R_TA_CONFIG / 4];
	s->width = e->regs[R_TA_WIDTH / 4] >> 16;
	s->height = e->regs[R_TA_HEIGHT / 4] >> 16;
	e->mode = TA_CMD;
	e->fifo_fill = 0;
	e->asm_n = 0;
	e->vin_n = 0;
	e->no_scene_warned = 0;
	if (fresh || !e->pbw.on)
		pb_scene_start(e, s->config);
	else
		pb_restart(e);
	emu_trace(e, "TA start: context %u, regions %#x, %ux%u, config %08x",
		  e->regs[R_TA_CONTEXT_ID / 4] & 1, s->regions, s->width, s->height, s->config);
}

/*
 * The end word, or a close (0x808, a partial render's): TA_COMPLETE. The
 * end word ends the scene, for the render that follows; with 0x85c bit 7
 * clear it only stops the TA, as a stop word does before
 * storing a context (assumed: that is the difference software relies on).
 * A closed scene stays open: the TA goes on with it after the render.
 */
void ta_end_scene(struct mbxemu *e, int close)
{
	struct scene *s = ta_scene(e);

	if (s) {
		if (close) {
			s->closed = 1;
			write_regions(e, s, 1);
			emu_trace(e, "TA close: %d triangles so far", s->nprims);
		} else if (e->regs[R_TA_CONFIG / 4] & TAC_END_IS_END) {
			write_regions(e, s, 0);
			s->complete = 1;
			/* the EVM context is written past its 520 bytes (found during device testing; how far
			 * is not known: 1 KB assumed); a client pads it */
			if (e->regs[R_EVM_CTX_BASE / 4]) {
				static const uint8_t zeros[1024];

				dev_write(e, e->regs[R_EVM_CTX_BASE / 4] + 520, zeros, sizeof(zeros));
			}
			emu_trace(e, "TA end: %d triangles", s->nprims);
		} else {
			emu_trace(e, "TA stopped mid-scene");
		}
	}
	e->mode = TA_CMD;
	e->asm_n = 0;
	emu_raise(e, I_TA_DONE);
}

/* --- clipping, projection, triangles --- */

static void cv_lerp(struct cvtx *o, const struct cvtx *a, const struct cvtx *b, float t)
{
	const float *pa = (const float *)a, *pb = (const float *)b;
	float *po = (float *)o;
	unsigned int k;

	for (k = 0; k < sizeof(*o) / sizeof(float); k++)
		po[k] = pa[k] + (pb[k] - pa[k]) * t;
}

/* Clip a polygon against plane k (keep cd[k] >= 0): the new count */
static int clip_plane(struct cvtx *out, const struct cvtx *in, int n, int k)
{
	int i, m = 0;

	for (i = 0; i < n; i++) {
		const struct cvtx *a = &in[i], *b = &in[(i + 1) % n];
		float da = a->cd[k], db = b->cd[k];

		if (da >= 0)
			out[m++] = *a;
		if ((da >= 0) != (db >= 0))
			cv_lerp(&out[m++], a, b, da / (da - db));
	}
	return m;
}

static int screen_space(const struct mbxemu *e)
{
	/* 0xb00b0800 names section 0x800 for vertices already in surface
	 * pixels: no perspective divide, viewport or depth transform
	 * (assumed: the clear comes before any viewport block in a scene) */
	return e->sections >> 28 == 0xb;
}

/*
 * A vertex colour as the N810 keeps it: a float of 8 significant bits,
 * truncated, then rounded to 8 bits at the vertex, before interpolation
 * (a 0.25 to 0.75 ramp interpolates 64 to 191;
 * README assumption 25)
 */
static float colour8(float x)
{
	int ex;
	float m;

	x = clamp01(x);
	if (x <= 0)
		return 0;
	m = frexpf(x, &ex);	/* x = m * 2^ex, m in [0.5, 1) */
	return floorf(ldexpf(floorf(m * 256) / 256, ex) * 255 + 0.5f) / 255;
}

/*
 * The TA's floats: IEEE single with the low 4 mantissa bits dropped (20
 * significant bits, truncated), as its 1/w and depth come out in the
 * parameter buffer (1/3 is 3eaaaaa0, not float's
 * 3eaaaaab).
 */
static float f20(float x)
{
	uint32_t u;

	memcpy(&u, &x, 4);
	u &= 0xfffffff0u;
	memcpy(&x, &u, 4);
	return x;
}

/* a double chopped toward zero to float32 */
/* The TA's 1/w range: a vertex whose 1/w is 2^17 or more (w < 7.6e-6) or under 2^-121 (w > 2^121)
 * does not project: below, as if w <= 0 (the viewport's centre); above, 1/w is huge and the vertex is
 * at infinity in the direction of its clip position. On the N810 a
 * triangle with all three w equal 2^-12..2^-22 and 2^116..2^128 (drawn from 1/w = 1e5 and from 2^-121,
 * not from 2^17 and 2^-122); with only one vertex's w small (1e-6, 1e-8, either vertex) the
 * triangle reaches infinity along the vertex's clip direction (a wedge: parallel edges at the
 * direction of (x, y)); with all three vertices' w small nothing is drawn. The value of
 * the huge 1/w (1e20: only the direction shows), and the clip-space sx * x overflow: x * sx beyond
 * float32 rejects the triangle (w = 1e10: a vertex x of 3e28 pixels is drawn, 5e28 not).
 */
static int rhw_over(float w)
{
	return w > 0 && (float)(1.0 / w) >= 0x1p17f;
}

static float trunc32(double d)
{
	float f = (float)d;

	if (fabs((double)f) > fabs(d))
		f = nextafterf(f, 0);
	return f;
}

static void project(const struct mbxemu *e, const struct cvtx *c, struct vtx *o)
{
	const struct rstate *s = &e->cur;
	int k;

	if (screen_space(e)) {
		o->x = c->pos[0];
		o->y = c->pos[1];
		o->z = c->pos[2];
		o->rhw = 1;
	} else {
		/* w <= 0 projects as if 1/w were 0, to the viewport's centre: the
		 * N810 draws a triangle with its apex behind the eye (and no o7)
		 * to the screen's centre (README, assumption 14) */
		float rhw = 0, rhw_t = 0;
		double zd, m;
		int ex;

		if (c->pos[3] > 0) {
			/* 1/w truncated, not rounded: the N810's perspective-correct
			 * colours put exact halves just below (README
			 * assumption 25) */
			double r = 1.0 / c->pos[3];

			rhw_t = (float)r;
			if ((double)rhw_t > r)
				rhw_t = nextafterf(rhw_t, 0);
			rhw = f20(rhw_t);
			if (rhw_t >= 0x1p17f)
				rhw = rhw_t = 1e20f;
			else if (rhw_t < 0x1p-121f)
				rhw = rhw_t = 0;
		}

		/* The VGP's viewport transform chops, as the depth's does: each
		 * float multiply and the add truncates toward zero, and the 1/w
		 * is the full-width truncated one, not the 20 bits the TA stores.
		 * The 1/w in these products is the full-width truncated one with
		 * its last bit cleared (23 significant bits) */
		{
			uint32_t ru;
			float rx;

			memcpy(&ru, &rhw_t, 4);
			ru &= ~1u;
			memcpy(&rx, &ru, 4);
			o->x = trunc32(trunc32(trunc32((double)c->pos[0] * rx) * (double)s->vp[0]) + (double)s->vp[1]);
			o->y = trunc32(trunc32(trunc32((double)c->pos[1] * rx) * (double)s->vp[3]) + (double)s->vp[2]);
			if (fabs((double)c->pos[0] * s->vp[0]) > FLT_MAX)
				o->x = copysignf(INFINITY, c->pos[0]);
			if (fabs((double)c->pos[1] * s->vp[3]) > FLT_MAX)
				o->y = copysignf(INFINITY, c->pos[1]);
		}
		/* depth from the full-width truncated 1/w (not the 20-bit one
		 * stored), multiplied and added without intermediate rounding,
		 * then truncated once to 20 significant bits */
		zd = (double)c->pos[2] * rhw_t * s->dz[0] + s->dz[1];
		/* a result that rounds up to a power of two in float32 (24 bits)
		 * is that power of two before the 20-bit truncation: z0 0.5
		 * with red 2^-26 nearer equals green on the N810, not 2^-21
		 * below it. Other binade edges (0.25, 0.125, 0.375) are
		 * unverified */
		{
			float zf = (float)zd;
			int e2;

			if (zf > zd && frexpf(zf, &e2) == 0.5f)
				zd = zf;
		}
		m = frexp(zd, &ex);
		o->z = (float)ldexp(trunc(ldexp(m, 20)), ex - 20);
		o->rhw = rhw;
	}
	/* o1 is BGRA, clamped a vertex; o2 taken the same way (assumed), its
	 * w the fog factor */
	for (k = 0; k < 2; k++) {
		float *d = k ? o->off : o->col;
		const float *x = c->o[k];

		d[0] = colour8(x[2]);
		d[1] = colour8(x[1]);
		d[2] = colour8(x[0]);
		d[3] = colour8(x[3]);
	}
	for (k = 0; k < 2; k++) {
		o->tc[k][0] = c->o[2 + k][0];
		o->tc[k][1] = c->o[2 + k][1];
	}
}

static void bbox(struct prim *p, const struct vtx *v, int n)
{
	float x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y;
	int k;

	for (k = 1; k < n; k++) {
		x0 = fminf(x0, v[k].x);
		y0 = fminf(y0, v[k].y);
		x1 = fmaxf(x1, v[k].x);
		y1 = fmaxf(y1, v[k].y);
	}
	/* pixels whose centres may lie inside; clamped to the largest render */
	p->x0 = (int)fmaxf(floorf(x0 - 0.5f), 0);
	p->y0 = (int)fmaxf(floorf(y0 - 0.5f), 0);
	p->x1 = (int)fminf(ceilf(x1 + 0.5f), 4096);
	p->y1 = (int)fminf(ceilf(y1 + 0.5f), 4096);
}

/* A triangle in surface pixels into the scene, with its state block */
static void add_prim(struct mbxemu *e, const struct vtx *a, const struct vtx *b,
		     const struct vtx *c, const struct vtx *flat)
{
	struct scene *s = ta_scene(e);
	struct prim *p;

	if (!s) {
		if (!e->no_scene_warned)
			emu_log(e, "TA: an object with no scene begun (no TA start, or the scene ended)");
		e->no_scene_warned = 1;
		return;
	}
	{
		/* the screen's right edge as an 11-bit pixel count: 2048 wraps to
		 * 0, so on a 2048-wide target only a triangle reaching x 0 is kept
		 * (the TA width register's 11 bits: on a 2048-wide target
		 * every primitive whose leftmost point, on the 1/16 grid, lies
		 * right of x 0 vanishes) */
		float lx = fminf(a->x, fminf(b->x, c->x));

		if (floorf(lx * 16 + 0.5f) / 16 > (float)(s->width & 0x7ff))
			return;
	}
	if (e->scene_state < 0) {
		if (s->nstates == s->cstates) {
			int n = s->cstates ? 2 * s->cstates : 16;
			struct rstate *t = realloc(s->states, n * sizeof(*t));

			if (!t)
				return;
			s->states = t;
			s->cstates = n;
		}
		s->states[s->nstates] = e->cur;
		e->scene_state = s->nstates++;
	}
	if (s->nprims == s->cprims) {
		int n = s->cprims ? 2 * s->cprims : 64;
		struct prim *t = realloc(s->prims, n * sizeof(*t));

		if (!t)
			return;
		s->prims = t;
		s->cprims = n;
	}
	p = &s->prims[s->nprims++];
	p->v[0] = *a;
	p->v[1] = *b;
	p->v[2] = *c;
	memcpy(p->fcol, flat->col, sizeof(p->fcol));
	memcpy(p->foff, flat->off, sizeof(p->foff));
	p->pw = e->prim_word;
	p->state = e->scene_state;
	bbox(p, p->v, 3);
	e->stats.prims++;
}

static float area2(const struct vtx *a, const struct vtx *b, const struct vtx *c)
{
	return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
}

/*
 * A triangle from the VGP: clipped where w <= 0 (the MBX does that by
 * itself), by o7.x and o7.y and the user clip planes the sections word
 * enables, projected, culled by its winding and kept. Primitive word bit
 * 19 culls a triangle whose area in surface
 * pixels, (b - a) x (c - a) with y down, is negative, bit 18 one whose
 * area is positive: as the N810 does. Flat shading takes the last vertex's
 * colours, whatever the clipping did.
 */
static void triangle(struct mbxemu *e, const struct cvtx *a, const struct cvtx *b,
		     const struct cvtx *c)
{
	struct cvtx buf[2][3 + NCLIP];
	struct vtx v[3 + NCLIP], flat;
	int n = 3, k, cur = 0;

	if (!screen_space(e) && rhw_over(a->pos[3]) && rhw_over(b->pos[3]) && rhw_over(c->pos[3]))
		return;
	buf[0][0] = *a;
	buf[0][1] = *b;
	buf[0][2] = *c;
	project(e, c, &flat);
	if (!screen_space(e))
		for (k = 0; k < NCLIP && n >= 3; k++) {
			int i, out = 0;

			for (i = 0; i < n; i++)
				out |= buf[cur][i].cd[k] < 0;
			if (!out)
				continue;
			n = clip_plane(buf[!cur], buf[cur], n, k);
			cur = !cur;
		}
	if (n < 3)
		return;
	for (k = 0; k < n; k++)
		project(e, &buf[cur][k], &v[k]);
	for (k = 1; k + 1 < n; k++) {
		float ar = area2(&v[0], &v[k], &v[k + 1]);

		if (ar == 0) {	/* a far vertex cancels the float32 products: the double area decides */
			double ad = ((double)v[k].x - v[0].x) * ((double)v[k + 1].y - v[0].y) -
				    ((double)v[k].y - v[0].y) * ((double)v[k + 1].x - v[0].x);

			ar = ad > 0 ? 1 : ad < 0 ? -1 : 0;
		}
		if (ar == 0 || !isfinite(ar))
			continue;
		if ((ar < 0 && (e->prim_word & PRIM_CULL_CW)) ||
		    (ar > 0 && (e->prim_word & PRIM_CULL_CCW)))
			continue;
		add_prim(e, &v[0], &v[k], &v[k + 1], &flat);
	}
}

/* Is a clip-space point inside every plane in force? */
static int cv_inside(const struct cvtx *c)
{
	int k;

	for (k = 0; k < NCLIP; k++)
		if (c->cd[k] < 0)
			return 0;
	return 1;
}

/* A point: a square of the point size around it, the vertex's attributes
 * everywhere; dropped if it lies outside a clip plane */
static void point(struct mbxemu *e, const struct cvtx *c)
{
	struct vtx v[4];
	float h = e->half_size;
	int k;

	if (!screen_space(e) && !cv_inside(c))
		return;
	project(e, c, &v[0]);
	for (k = 1; k < 4; k++)
		v[k] = v[0];
	v[0].x -= h; v[0].y -= h;
	v[1].x += h; v[1].y -= h;
	v[2].x -= h; v[2].y += h;
	v[3].x += h; v[3].y += h;
	add_prim(e, &v[0], &v[1], &v[2], &v[0]);
	add_prim(e, &v[1], &v[3], &v[2], &v[0]);
}

/* A line segment: clipped as a segment, then a quad of the line width
 * across it (no end caps) */
static void line(struct mbxemu *e, const struct cvtx *a0, const struct cvtx *b0)
{
	struct cvtx a = *a0, b = *b0;
	struct vtx v[4];
	float dx, dy, len, nx, ny;
	int k;

	if (!screen_space(e))
		for (k = 0; k < NCLIP; k++) {
			float da = a.cd[k], db = b.cd[k];

			if (da < 0 && db < 0)
				return;
			if (da < 0)
				cv_lerp(&a, &a, &b, da / (da - db));
			else if (db < 0)
				cv_lerp(&b, &a, &b, da / (da - db));
		}
	project(e, &a, &v[0]);
	project(e, &b, &v[2]);
	dx = v[2].x - v[0].x;
	dy = v[2].y - v[0].y;
	len = sqrtf(dx * dx + dy * dy);
	if (!(len > 0))
		return;
	nx = -dy / len * e->half_size;
	ny = dx / len * e->half_size;
	v[1] = v[0];
	v[3] = v[2];
	v[0].x += nx; v[0].y += ny;
	v[1].x -= nx; v[1].y -= ny;
	v[2].x += nx; v[2].y += ny;
	v[3].x -= nx; v[3].y -= ny;
	add_prim(e, &v[0], &v[1], &v[2], &v[2]);
	add_prim(e, &v[1], &v[3], &v[2], &v[2]);
}

/* --- VGP stream-out --- */

/*
 * What the N810 does (observed): the
 * address word is ignored, and an object whose header has the three stream
 * blocks and a non-zero enable writes each vertex's selected outputs, taken
 * before clipping, to one of two places that alternate per object: X,
 * device 0x20c, and Y, device 0 the first time after a render, 0x202e00
 * after that, and unmapped after a reset. The outputs go in register order:
 * o0 and o5 four floats, o1 and o2 four clamped to 0..1, o3 and o4 s and t.
 */
#define SO_X		0x20c
#define SO_Y_AFTER	0x202e00
#define SO_NOWHERE	DEV_SPACE	/* past the MMU's space: dropped */

void so_reset(struct mbxemu *e)
{
	e->so_on = 0;
	e->so_side = 1;
	e->so_y = SO_NOWHERE;
}

void so_render(struct mbxemu *e)
{
	e->so_side ^= 1;
	e->so_y = 0;
}

static void so_begin(struct mbxemu *e)
{
	e->so_on = (e->hdr_mask & 0xe00) == 0xe00 && e->cur.stream[2];
	if (e->so_on)
		e->so_at = e->so_side ? e->so_y : SO_X;
}

static void so_end(struct mbxemu *e)
{
	if (!e->so_on)
		return;
	if (e->so_side)
		e->so_y = SO_Y_AFTER;
	e->so_side ^= 1;
	e->so_on = 0;
}

static void so_vertex(struct mbxemu *e, const struct vgp_regs *r)
{
	static const int n[6] = { 4, 4, 4, 2, 2, 4 };
	uint32_t sel = e->cur.stream[1];
	int k, j;

	for (k = 0; k < 6; k++)
		if (sel >> k & 1)
			for (j = 0; j < n[k]; j++) {
				float f = r->o[k][j];

				if (k == 1 || k == 2)
					f = f < 0 ? 0 : f > 1 ? 1 : f;
				dev_write32(e, e->so_at, f2u(f));
				e->so_at += 4;
			}
}

/* --- vertices --- */

/* Words a vertex takes by the layout word: eight 4-bit slots, one an input
 * register: 1-4 floats, 5 four bytes packed in a word; -1 if invalid */
static int vertex_words(uint32_t layout)
{
	int k, n = 0;

	for (k = 0; k < 8; k++) {
		uint32_t f = layout >> 4 * k & 0xf;

		if (f > 5)
			return -1;
		n += f == 5 ? 1 : (int)f;
	}
	return n;
}

static void vertex(struct mbxemu *e)
{
	struct vgp_regs r;
	struct cvtx c;
	uint32_t layout = e->cur.layout, clip;
	int k, j, p = 0;

	memset(&r, 0, sizeof(r));
	for (k = 0; k < 8; k++) {
		uint32_t f = layout >> 4 * k & 0xf;

		if (f == 5) {
			/* packed: the word's bytes in memory order to x, y, z, w */
			uint32_t w = e->vin[p++];

			for (j = 0; j < 4; j++)
				r.v[k][j] = (w >> 8 * j & 0xff) / 255.0f;
		} else {
			/* the components sent; the rest 0, w 1 */
			for (j = 0; j < 4; j++)
				r.v[k][j] = j < (int)f ? u2f(e->vin[p++]) : j == 3 ? 1.0f : 0.0f;
		}
	}
	r.o[0][3] = 1;
	vgp_run(e, &r, e->sections & 0xffff);
	if (e->so_on)
		so_vertex(e, &r);

	memcpy(c.pos, r.o[0], sizeof(c.pos));
	for (k = 0; k < 4; k++)
		memcpy(c.o[k], r.o[1 + k], sizeof(c.o[k]));
	/* no clip at w = 0 (the N810 has none: w <= 0 projects to the centre);
	 * o7.x and o7.y always; user planes 0-3 in o5, 4-7 in o6 when the
	 * sections word's bit 20 + k is set */
	clip = e->sections >> 20 & 0xff;
	c.cd[0] = 1;
	c.cd[1] = r.o[7][0];
	c.cd[2] = r.o[7][1];
	for (k = 0; k < 8; k++)
		c.cd[3 + k] = clip >> k & 1 ? r.o[5 + k / 4][k % 4] : 0;
	/* no guard band: the N810 draws a triangle with a vertex 1e36 pixels away
	 * and clips nothing against the viewport's size (README, assumption 14) */

	{
		struct vtx pv;

		project(e, &c, &pv);
		pb_vertex(e, &pv);
	}

	/* primitive assembly */
	switch (e->prim_word >> 28) {
	case 0x7:	/* triangle list */
		if (e->asm_n == 2) {
			triangle(e, &e->asm_v[0], &e->asm_v[1], &c);
			e->asm_n = 0;
		} else {
			e->asm_v[e->asm_n++] = c;
		}
		return;
	case 0x4:	/* strip: every other triangle turned round to keep its winding */
		if (e->asm_n >= 2) {
			if (e->asm_n & 1)
				triangle(e, &e->asm_v[1], &e->asm_v[0], &c);
			else
				triangle(e, &e->asm_v[0], &e->asm_v[1], &c);
			e->asm_v[0] = e->asm_v[1];
			e->asm_v[1] = c;
		} else {
			e->asm_v[e->asm_n] = c;
		}
		e->asm_n++;
		return;
	case 0x5:	/* fan */
		if (!e->asm_n)
			e->first_v = c;
		else if (e->asm_n >= 2)
			triangle(e, &e->first_v, &e->asm_v[0], &c);
		e->asm_v[0] = c;
		e->asm_n++;
		return;
	case 0x6:	/* line strip */
		if (e->asm_n)
			line(e, &e->asm_v[0], &c);
		e->asm_v[0] = c;
		e->asm_n++;
		return;
	case 0xe:	/* points */
		point(e, &c);
		return;
	}
}

/* --- the stream --- */

static void hdr_next(struct mbxemu *e)
{
	while (++e->hdr_bit < 16)
		if (e->hdr_mask >> e->hdr_bit & 1) {
			e->hdr_left = hdr_words[e->hdr_bit];
			e->hdr_word = 0;
			e->mode = TA_HDR;
			return;
		}
	e->mode = TA_CMD;
	e->scene_state = -1;	/* the blocks changed: a new state block for what follows */
}

static void hdr_block(struct mbxemu *e, uint32_t v)
{
	struct rstate *s = &e->cur;
	int w = e->hdr_word++;

	switch (e->hdr_bit) {
	case 0: s->st = v; break;
	case 1: s->at = v; break;
	case 2: s->tex[0][w] = v; break;
	case 3: s->tex[1][w] = v; break;
	case 4: s->w4 = v; break;
	case 5: s->layout = v; break;
	case 6: s->w6 = v; break;
	case 7: s->vp[w] = u2f(v); break;
	case 8: s->dz[w] = u2f(v); break;
	default: s->stream[e->hdr_bit - 9] = v; break;
	}
	if (!--e->hdr_left)
		hdr_next(e);
}

static void command(struct mbxemu *e, uint32_t v)
{
	switch (v >> 28) {
	case 0x1:
		if (v & 0x0ffff000) {
			stream_error(e, v, "an object header with blocks we don't know");
			return;
		}
		e->hdr_mask = v & 0xfff;
		e->hdr_bit = -1;
		hdr_next(e);
		return;
	case 0x4: case 0x5: case 0x6: case 0x7: case 0xe:
		e->prim_word = v;
		e->mode = (v >> 28 == 0x6 || v >> 28 == 0xe) ? TA_PRIM_SIZE : TA_PRIM_SECTIONS;
		e->stats.objects++;
		so_begin(e);
		return;
	case 0x8: {
		uint32_t bit = v & 0xffff;
		int b = 0;

		if (!bit || (bit & (bit - 1))) {
			stream_error(e, v, "a VGP section that isn't one bit");
			return;
		}
		while (!(bit >> b & 1))
			b++;
		e->sec_bit = b;
		e->mode = TA_SEC_WORD1;
		return;
	}
	case 0x9:
		e->code_at = v >> 16 & 0xff;
		e->code_end = v & 0xff;
		e->code_half = 0;
		if (e->code_end < e->code_at) {
			stream_error(e, v, "a code range that ends before it starts");
			return;
		}
		e->mode = TA_CODE;
		return;
	case 0xa:
		e->const_at = v >> 16 & 0xfff;
		e->const_end = v & 0xfff;
		e->const_lane = 0;
		if (e->const_end < e->const_at) {
			stream_error(e, v, "a constant range that ends before it starts");
			return;
		}
		e->mode = TA_CONST;
		return;
	case 0xf:
		if (v == 0xf0000000) {
			ta_end_scene(e, 0);
			return;
		}
		break;
	}
	stream_error(e, v, "not a command");
}

void ta_word(struct mbxemu *e, uint32_t lane, uint32_t v)
{
	(void)lane;	/* the FIFO takes words at any lane; the TA counts them */
	if (e->trace > 1)
		emu_log(e, "TA %u: %08x", lane, v);
	switch (e->mode) {
	case TA_CMD:
		command(e, v);
		return;
	case TA_HDR:
		hdr_block(e, v);
		return;
	case TA_SEC_WORD1:
		e->vgp.sec[e->sec_bit].loaded = 1;
		e->vgp.sec[e->sec_bit].flags = v >> 16 & 0xfff;
		e->vgp.sec[e->sec_bit].first = v & 0xff;
		e->vgp.sec[e->sec_bit].last = v >> 8 & 0xff;
		e->mode = TA_SEC_CODE_HDR;
		return;
	case TA_SEC_CODE_HDR:
		if (v >> 28 != 0x9)
			stream_error(e, v, "a VGP section without its code word");
		else
			command(e, v);
		return;
	case TA_CODE:
		e->vgp.code[e->code_at][e->code_half] = v;
		if (++e->code_half == 2) {
			e->code_half = 0;
			if (++e->code_at > e->code_end)
				e->mode = TA_CMD;
		}
		return;
	case TA_CONST:
		/* constants past c191 are dropped (an index past the end reads 0) */
		if (e->const_at < 192)
			e->vgp.k[e->const_at][e->const_lane] = u2f(v);
		if (++e->const_lane == 4) {
			e->const_lane = 0;
			if (++e->const_at > e->const_end)
				e->mode = TA_CMD;
		}
		return;
	case TA_PRIM_SIZE:
		e->half_size = u2f(v);
		e->mode = TA_PRIM_SECTIONS;
		return;
	case TA_PRIM_SECTIONS:
		if (v >> 28 != 0xf && v >> 28 != 0xb) {
			stream_error(e, v, "no sections word after the primitive word");
			return;
		}
		if (!(v & SEC_BIT17)) {
			ta_hang(e, v);
			return;
		}
		e->sections = v;
		e->vsize = vertex_words(e->cur.layout);
		if (e->vsize <= 0 || e->vsize > (int)(sizeof(e->vin) / sizeof(e->vin[0]))) {
			/* the hardware misparses and hangs */
			stream_error(e, e->cur.layout, "a vertex layout word that names no vertex");
			return;
		}
		e->vin_n = 0;
		e->asm_n = 0;
		e->mode = TA_VERTS;
		return;
	case TA_VERTS:
		e->vin[e->vin_n++] = v;
		if (e->vin_n == e->vsize) {
			e->vin_n = 0;
			vertex(e);
		}
		return;
	case TA_ERROR:
		return;
	case TA_HUNG:
		ta_fifo_wait(e);
		return;
	}
}

/*
 * The TA control port: 1 ends the object, 0 restarts its strip (a line
 * list is a line strip restarted after each segment). A vertex not
 * complete is dropped.
 */
void ta_ctl(struct mbxemu *e, uint32_t v)
{
	if (e->trace > 1)
		emu_log(e, "TA ctl: %u", v);
	if (e->mode == TA_HUNG) {
		ta_fifo_wait(e);
		return;
	}
	if (e->mode != TA_VERTS) {
		if (v == 1 && e->mode != TA_CMD && e->mode != TA_ERROR)
			emu_log(e, "TA: an object ends in the middle of a block (mode %d)", e->mode);
		if (e->mode != TA_ERROR)
			e->mode = TA_CMD;
		return;
	}
	if (e->vin_n)
		emu_log(e, "TA: %d words of an incomplete vertex dropped", e->vin_n);
	e->vin_n = 0;
	e->asm_n = 0;
	pb_object_end(e);
	if (v) {
		so_end(e);
		e->mode = TA_CMD;
	}
}
