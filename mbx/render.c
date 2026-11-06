// SPDX-License-Identifier: MIT
/*
 * mbxemu: the 3D core. On the kick (0x680) it walks the region array at
 * 0x608 and renders each tile it lists, as the hardware does, so a tile
 * the array leaves out keeps its pixels (software relies on that for partial
 * updates). A tile starts from the background object
 * (the parameter buffer's first words: the render target as a texture),
 * its depth from 0x6dc or, when asked, the external Z buffer; the scene's
 * triangles are drawn in the order the TA took them; the tile goes to the
 * colour buffer (0x65c, 0x660, 0x650) clipped to 0x654 and 0x658.
 *
 * Per pixel: depth test, the
 * texture units and their combiners, the offset colour and fog, the alpha
 * test, depth write, blending. The hardware's ordering by object type
 * (opaque, punch-through, translucent) isn't modelled: submission order
 * is what our stack asks for.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

/* ISP/TSP state word bits */
#define ST_SATURATE	(1u << 5)
#define ST_NODEPTHWRITE	(1u << 7)
#define ST_ALPHANEVER	(1u << 8)
#define ST_BLEND	(1u << 13)
#define ST_ALPHATEST	(1u << 14)
#define ST_GOURAUD	(1u << 22)
#define ST_OFFSET	(1u << 23)
#define ST_FOG		(1u << 24)
#define PRIM_ZEROOFFSET	(1u << 17)

#define MAX_REGIONS	(128 * 128)	/* entries walked before the core is taken to hang */

/* A render's registers */
struct rctx {
	uint32_t ctl, fmt, addr, stride, w, h, zaddr, zrow;
	uint32_t samp;			/* 0x61c: where pixels and texels are sampled */
	int zload_ok, zstore;
	float zclear, fog[3];
	struct texunit bg;
	int bg_target;			/* no background object: the target's own pixels */
	struct scene *s;
	struct setup *su;		/* a setup per triangle */
};

/* A triangle set up for rasterizing */
struct setup {
	double vrhw[3], vs[3];	/* the s/t plane being set up: each vertex's 1/w and coordinate (plane_of's products) */
	const struct prim *p;
	const struct rstate *st;
	float x[3], y[3], area;
	double xarea;			/* the area, exact: |p1 - p2| in double */
	int order[3];			/* vertices in the order with positive area */
	float dbx[3], dby[3];		/* barycentric gradients */
	struct texunit tu[2];
	int nunits;
	float konst[4];
	float bias;
	int affine;			/* the same 1/w at every vertex: no divide */
	double qpl[5];			/* 1/w's plane: A, B (cut), exact A, B, C at (0, 0) */
	double qplc[5];			/* the same for the colours' set-up */
	double npl[2][2][5];		/* each unit's s/w and t/w planes */
	double lql[2];			/* the same for the 1/w plane */
	double lsl[2][2][2];		/* the LOD's slopes of each unit's s/w and t/w planes: before slopes_shared */
	double cpl[2][4][5];		/* the base and offset colours' planes, in bytes / w */
	int gouraud;			/* colours through cpl (colour_at) */
	int colslope;			/* the colour planes' slopes: divided and truncated */
	float ox, oy;			/* the planes' origin: the box's top left */
	int zflat;			/* the three vertex depths are equal: a constant depth */
	double zn[3];			/* the depth plane's numerators: C sum, x and y slope sums (zplane_setup) */
	double zrc;			/* the depth plane's reciprocal of the area */
	int skip;
};

static int depth_pass(int func, float z, float old)
{
	switch (func) {
	case 0: return 0;
	case 1: return z < old;
	case 2: return z == old;
	case 3: return z <= old;
	case 4: return z > old;
	case 5: return z != old;
	case 6: return z >= old;
	default: return 1;
	}
}

static int alpha_pass(int func, int a, int ref)
{
	return depth_pass(func, (float)a, (float)ref);
}

/* --- the colour buffer --- */

static void unpack(uint32_t fmt, const uint8_t *p, float o[4])
{
	if (fmt == 9) {
		uint32_t v = p[0] | p[1] << 8;

		o[0] = (v >> 11 & 31) / 31.0f;
		o[1] = (v >> 5 & 63) / 63.0f;
		o[2] = (v & 31) / 31.0f;
		o[3] = 1;
	} else {
		uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;

		o[0] = (v >> 16 & 255) / 255.0f;
		o[1] = (v >> 8 & 255) / 255.0f;
		o[2] = (v & 255) / 255.0f;
		o[3] = (v >> 24) / 255.0f;
	}
}

/* To max (255): vertex colours are bytes already (ta.c), so an
 * interpolated colour is rounded once, plainly */
static unsigned q(float x, int max)
{
	return (unsigned)(clamp01(x) * max + 0.5f);
}

/* A colour to the 8 bits the N810 blends and adds in: it takes vertex
 * colours to 8 bits at the vertex, so a constant 0.5 is 128 at every pixel,
 * where float barycentric weights leave 127.4999 at some; a bias of
 * 1/1024 rounds halves up as it does */
static int q8(float x)
{
	return (int)(clamp01(x) * 255 + 0.5f + 1.0f / 1024);
}

static void pack(uint32_t fmt, uint8_t *p, const float c[4])
{
	if (fmt == 9) {
		/* from the 8-bit colour the pipeline works in, its top bits (the
		 * N810's RGB565 conversion isn't measured; this follows its 8-bit
		 * colours, README assumption 25) */
		uint32_t v = (q(c[0], 255) >> 3) << 11 | (q(c[1], 255) >> 2) << 5 | q(c[2], 255) >> 3;

		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
	} else {
		p[0] = (uint8_t)q(c[2], 255);
		p[1] = (uint8_t)q(c[1], 255);
		p[2] = (uint8_t)q(c[0], 255);
		p[3] = (uint8_t)q(c[3], 255);
	}
}

/* --- set-up --- */

static void konst_of(uint32_t at, float k[4])
{
	/* the environment colour: A6 R5 G5 B5 in the alpha test word's low
	 * bits, to 8 bits by repeating the top bits (24 is 198), as the N810's
	 * combiner results show (README assumption 25) */
	uint32_t r = at >> 10 & 31, g = at >> 5 & 31, b = at & 31, a = at >> 15 & 63;

	k[0] = (r << 3 | r >> 2) / 255.0f;
	k[1] = (g << 3 | g >> 2) / 255.0f;
	k[2] = (b << 3 | b >> 2) / 255.0f;
	k[3] = (a << 2 | a >> 4) / 255.0f;
}

/*
 * Perspective-correct texture coordinates, as the N810 computes them
 * (README, assumption 27).
 * 1/w and s/w (t/w) are planes, A x + B y + C, each coefficient cut to a
 * 16-bit significand (truncated), as is each plane's value at a pixel
 * centre and the reciprocal of 1/w; s is (s/w) * w kept to 12 bits below
 * the two factors' exponents together (so one bit more where their
 * significands multiply past 2), rounded. C is the plane at the top left
 * of the triangle's box. Affine triangles go the same way. Colours
 * don't: through these planes they match worse.
 */
static double trunc16(double x)
{
	int e;
	double m;

	if (x == 0 || !isfinite(x))
		return x;
	m = frexp(x, &e);
	return ldexp(trunc(ldexp(m, 16)), e - 16);
}

static double floorb(double x, int bits)
{
	int e;
	double m;

	if (x == 0 || !isfinite(x))
		return x;
	m = frexp(x, &e);
	return ldexp(floor(ldexp(m, bits)), e - bits);
}

static double floor16(double x)
{
	int e;
	double m;

	if (x == 0 || !isfinite(x))
		return x;
	m = frexp(x, &e);
	return ldexp(floor(ldexp(m, 16)), e - 16);
}

static double cut(double x, int bits, int nearest)
{
	int e;
	double m;

	if (x == 0 || !isfinite(x))
		return x;
	m = ldexp(frexp(x, &e), bits);
	/* nearest: 1 to even, 2 halves away from zero */
	return ldexp(nearest == 2 ? round(m) : nearest ? nearbyint(m) : trunc(m), e - bits);
}


/*
 * The edge constant x_i y_j - x_j y_i as the set-up keeps it: each PRODUCT
 * truncated toward zero to a 24-bit significand before the subtraction (the set-up
 * multiplier is a float one; the products are exact integers in 1/256 px^2 units, so
 * the cut only matters above 2^24 units, a product above 65,536 px^2 = 256 x 256,
 * i.e. a vertex far from the screen's origin in both x and y).
 * The share of triangles with a wrong plane constant jumps once the largest
 * x_i*y_j reaches 2^16 px^2, and the sign of the +-1 ulp16 shift that cures
 * them follows the low bit of each of the six products (an odd product loses
 * 1/256 px^2). The area's three constants (plane_of: darea) are summed from
 * the truncated products too. The device uses V*trunc24(E); E_eff/E-1 is
 * -1/256 px^2 for odd 25-bit areas, and 23 bits or fewer are rejected. So
 * the difference is cut to 24 bits too (a float subtractor); without that
 * outer cut, triangles whose E is 25 bits while one product is small go
 * wrong.
 * Fitted rather than a circuit: truncation of the products (nearest-even
 * products, or a product cut to 23 or 25 bits, are worse), and that the
 * slope path takes the same E (cc, ex, tm in plane_of).
 */
static double edge_k(const struct setup *su, int i, int j)
{
	return cut(cut((double)su->x[i] * su->y[j], 24, 0) - cut((double)su->x[j] * su->y[i], 24, 0), 24, 0);
}

/*
 * The coverage edge value at a pixel centre: the exact A x + B y + the cut edge constant,
 * plus a bias of 2^-14 times the edge's larger slope, added on a top or left edge (a pixel that
 * lies that far outside still counts as inside) and taken off the others (a pixel that far inside
 * is left out), so that an edge that is exactly on the pixel centre still falls to the
 * top-left rule. The bias needed is 1 unit at a slope of 64 and 10 units at
 * 610; 2^-14 fits within [5.9e-5, 6.5e-5] of the slope. A copy of an edge
 * with A < 0 is left out at +4/256 where its x-mirror is drawn. The bias
 * follows the slope of the major axis (an edge that is nearer horizontal moves
 * in y), not the length of the gradient, not the sum.
 * Beyond slope 610 and for diagonal edges it is NOT 2^-14 times the slope but that slope's
 * power of two: 2^(floor(log2 max(|A|, |B|)) - 14) px^2, the last place of the slope's 15-bit
 * significand times 1. The linear 2^-14 * slope needs 3.8e-5 to 5.6e-5 times
 * the slope on diagonal edges of slope 237 to 5,880, and 6.0e-5 on others; the
 * power of two fits both. Per exponent class 6..11 the scale c of 2^(e-14) is
 * bounded to [1.000, 1.25]; ties (E = +-bias) count as drawn for a top-left edge
 * and not for the others. The exact quantisation (the floor of the slope's
 * exponent) is fitted, not a circuit.
 *
 * The edge value is formed at the 16 x 16 tile's origin pixel (the tile corner, with the
 * pixel's own centre offset) and stepped to the pixel exactly: E_tile = cut24(cut24(cut24(A tx) +
 * cut24(B ty)) + C), each cut a truncation toward zero, then + A (x - tx) + B (y - ty).
 * Cutting only the constant's products, or cutting A x and B y and their sum
 * at the pixel itself, fits worse. The cut order (A tx, B ty, their sum, then
 * + C) and tile 16 with the origin at the pixel centre of the corner pixel
 * are fitted.
 */
static double edge_at(const struct setup *su, int a, int b, float ex, float ey, int tl)
{
	double A = (double)su->y[a] - su->y[b], B = (double)su->x[b] - su->x[a];
	double n = fabs(A) > fabs(B) ? fabs(A) : fabs(B);

	double fx = floor(ex), fy = floor(ey);
	double tx = floor(fx / TILE) * TILE + (ex - fx), ty = floor(fy / TILE) * TILE + (ey - fy);
	double v = cut(cut(cut(A * tx, 24, 0) + cut(B * ty, 24, 0), 24, 0) + edge_k(su, a, b), 24, 0) + A * (ex - tx) + B * (ey - ty);

	return v + (tl ? 1 : -1) * (n > 0 ? ldexp(1, ilogb(n) - 14) : 0);
}

/*
 * A plane's slopes as the set-up stores them: one exponent for the pair,
 * the larger of the two (a zero slope takes no part), and each slope
 * floored to 16 bits below it (15 or 17 bits, or truncating or rounding, fit
 * worse than the fixed-point slopes of 25 fraction bits for textures and 17
 * for colours that this replaces).
 */
static void slopes_shared(double pl[5])
{
	int ea, eb;
	double q;

	frexp(pl[0], &ea);
	frexp(pl[1], &eb);
	if (pl[0] == 0)
		ea = eb;
	if (pl[1] == 0)
		eb = ea;
	q = ldexp(1, (ea > eb ? ea : eb) - 16);
	pl[0] = floor(pl[0] / q) * q;
	pl[1] = floor(pl[1] / q) * q;
}

/*
 * One vertex's term of a texture slope or constant sum: 1/w times the coordinate
 * times the edge's difference or constant, as two truncating 24-bit multiplies in the
 * order (rhw * E) * s, each product cut to 24 bits toward zero.
 * The grouping (rhw*s)*E with the product truncated leaves a dead zone; a
 * power of two rhw makes every grouping equal, which is where the device shows
 * no dead zone. (s*E)*rhw, nearest rounding at either stage, the association
 * on the slopes only or on C only, and a cancellation gate all fit worse.
 * Which stage is rounded how, and that it is unconditional, is fitted, not a
 * circuit. Thin 1/8 pixel quads with a nearly flat plane fit worse than before.
 */
static double vterm(const struct setup *su, int k, double e)
{
	return cut(cut(su->vrhw[k] * e, 24, 0) * su->vs[k], 24, 0);
}

/*
 * The order the three vertex terms of a texture plane (the s/t planes' and the
 * 1/w plane's slopes and constants) are added in: the vertex the TA received last
 * (order[k] == 2) last, the other two by x descending. Only the identity of
 * the last vertex matters (all six submission orders agree on it). The same
 * order serves the 1/w plane, where q fits the device's best. The order of
 * the other two is fitted; not a circuit.
 */
static void term_order(const struct setup *su, int seq[3])
{
	int last = 0, o2 = 0, kk;

	for (kk = 0; kk < 3; kk++)
		if (su->order[kk] == 2)
			last = kk;
	for (kk = 0; kk < 3; kk++)
		if (kk != last)
			seq[o2++] = kk;
	seq[2] = last;
	if (su->x[seq[1]] > su->x[seq[0]]) {
		int t = seq[0];

		seq[0] = seq[1];
		seq[1] = t;
	}
}

/* A plane through the three vertices' values: the slopes are the edge
 * sums times 1/area, that reciprocal truncated to 20 bits, each slope
 * rounded to a 16-bit significand */
/* set while the colours' 1/w plane is set up (it takes the colour channels' path
 * only for perspective triangles) */
static int g_isq;
/* plane_of's slopes before slopes_shared (side channel to the caller) */
static double le_pre[2];

static void plane_of(const struct setup *su, const double v[3], const double vc[3], double pl[5], int colour)
{
	/* 1/area truncated to 19 bits, 20 for the perspective colours' (the
	 * affine colours' at 19 too) */
	/* The texture planes' reciprocal comes from the area cut to 19 bits
	 * first: rc = trunc19(1 / trunc19(area)). Single-step changes of one
	 * vertex's 1/w and s put the pre-round C and slopes off by exactly the
	 * exact-reciprocal shift of the area cut; triangles of areas of 19 bits
	 * or less are unchanged. This is not the whole truth: two large
	 * triangles (areas of 20 and 21 bits) are borderline and fit a little
	 * worse, so the area-reciprocal story is incomplete (see the constant's
	 * accumulate below). The area is the exact one (xarea): in float32 a
	 * large area loses bits and is cut to the wrong 19. Texture planes
	 * only; the colour planes keep 1/area. */
	/* The area the reciprocal is taken of is the sum of the three edge constants
	 * as the set-up forms them (edge_k: truncated products), not the exact cross
	 * product of the vertex differences: the same unit (1/256 px^2) shift of E
	 * moves the area, hence the reciprocal, A, B and C together. Colour planes
	 * take it too, and the colour channels' (affine ones too, and the colours'
	 * 1/w plane) reciprocal is also taken of the area cut to 19 bits first,
	 * rc = trunc19(1 / trunc19(area)), like the texture planes': it is the
	 * area cut that matters, not the reciprocal's width. */
	double darea = fabs(edge_k(su, 1, 2) + edge_k(su, 2, 0) + edge_k(su, 0, 1)), xar = darea;
	/* The perspective colour planes (the three or four channels' and the colours'
	 * 1/w plane of a triangle whose vertices' 1/w differ) are set up like the
	 * texture planes: the same area cut, edge-sum accumulate, slope rounding and
	 * C accumulator. This is fitted, not a circuit: that the whole chain
	 * carries over, and the gate: the colours' 1/w plane and the slopes of
	 * triangles with equal 1/w at all vertices keep the older set-up (with a
	 * constant w it prefers the old C for the 1/w plane and the 18-bit vertex
	 * values). */
	int persp = colour != 0 && !su->affine;
	double na = 0, nb = 0, rc = cut(1.0 / cut(xar, 19, 0), colour == 1 && !persp ? 20 : 19, 0), a, b;
	int big = cut(xar, 19, 0) != xar;	/* an area of more than 19 significant bits */
	int tex = colour == 0 || persp;		/* 2: the colours' 1/w plane, the textures' slopes untruncated */
	int kk, cramer;

	/* the set-up FPU: each slope's edge sum truncated to 19 bits, x and y
	 * alike (the N810 draws every flat frame as its transpose) */
	/* each edge product truncated to 24 bits before the sum, for the
	 * texture planes (the set-up's float multiplier): two triangles of
	 * one quad on the same 1/w plane, their vertices bit for bit the
	 * same in the parameter buffer, get y slopes a 16-bit step apart on
	 * the device where the exact value is a tie; rounding to nearest
	 * instead fits worse */
	/* The texture planes' slopes (s/t and 1/w alike): each term cut to 24
	 * bits, the sum chopped to 24 bits after every add (sign-magnitude, toward
	 * zero), in submission order (term_order), then cut to 19 below.
	 * Equal-rhw triangles show a tiny slope of about -2^-27 from the product
	 * cuts, which floor16 lowers q by one ulp16 in about 70% of them, as the
	 * device does: the chopped sum in submission order fits best (an exact
	 * sum or index order fit worse). 24 bits is the optimum (22, 23, 25 and
	 * 26 are worse). The whole accumulate chain is fitted, not a circuit. */
	int seq[3] = { 0, 1, 2 };

	if (tex)
		term_order(su, seq);
	for (kk = 0; kk < 3; kk++) {
		int k = seq[kk], i = (k + 1) % 3, j = (k + 2) % 3;
		/* The perspective colour channels' terms (slopes and C) are the vterm
		 * products too, like the affine colours' C and the s/t planes': the
		 * vertex's 1/w times the edge constant cut to 24 bits, times the
		 * channel's byte, cut to 24. Every wrong plane was one C step off,
		 * the C near a tie of its 16-bit rounding, in both directions, with
		 * the vertex value rhw * byte cut first (the earlier rule). The
		 * order of the two cuts is fitted, not a circuit. */
		int vts = vc != NULL || (colour == 1 && persp);
		double pa = vts ? vterm(su, k, -((double)su->y[j] - su->y[i])) : -((double)su->y[j] - su->y[i]) * v[k], pb = vts ? vterm(su, k, (double)su->x[j] - su->x[i]) : ((double)su->x[j] - su->x[i]) * v[k];

		if (tex) {
			na += pa != 0 ? cut(pa, 24, 0) : pa;
			nb += pb != 0 ? cut(pb, 24, 0) : pb;
			if (na != 0)
				na = cut(na, 24, 0);
			if (nb != 0)
				nb = cut(nb, 24, 0);
		} else {
			na += pa;
			nb += pb;
		}
	}
	if (tex) {
		na = cut(na, 19, 0);
		nb = cut(nb, 19, 0);
	}
	a = na / darea;
	b = nb / darea;
	/* colours' planes: the slopes divided and rounded to 16 bits, nearest
	 * (truncated, or with the textures' slopes, they fit worse) */
	/* textures' slopes: halves rounded away from zero (not to even) */
	pl[0] = colour == 1 && !tex ? cut(a, 16, 1) : cut(na * rc, 16, tex ? 2 : 1);
	pl[1] = colour == 1 && !tex ? cut(b, 16, 1) : cut(nb * rc, 16, tex ? 2 : 1);
	le_pre[0] = pl[0];
	le_pre[1] = pl[1];
	if (tex)
		slopes_shared(pl);
	pl[2] = a;
	pl[3] = b;
	pl[4] = v[0] - a * su->x[0] - b * su->y[0];
	/* C by the edge equations (below): the texture planes', the perspective colour
	 * planes' and the colour channels' of affine triangles; the colours' 1/w plane of
	 * an affine triangle keeps the old C */
	cramer = tex || (colour != 0 && !g_isq);
	if (!cramer)	/* colour planes: v - A x, then - B y, truncated: A x to
			 * 20 bits, v - A x and B y to 19, the result to 21
			 * (every step at 19 bits fits worse) */
		pl[4] = cut(cut(v[0] - cut(a * su->x[0], 20, 0), 19, 0) - cut(b * su->y[0], 19, 0), 21, 0);
	if (cramer) {
		/* the texture planes' C as the edge equations give it (Cramer's
		 * rule, C = x2 y1 - y2 x1 for each edge): each
		 * vertex's value times its opposite edge's constant; no vertex
		 * is the reference, so where the plane is 0 at a vertex C
		 * cancels exactly as on the device. The
		 * s/w planes' sum runs through a 24-bit sign-magnitude
		 * accumulator that chops the result of every add, the first
		 * term's load too, toward zero, with the vertex values at 25
		 * bits truncated (vc, against the slopes' 24 rounded): the
		 * quads whose terms cancel ~4 bits land on a 2^(e - 20) grid,
		 * which fits nearly all one-step device verdicts; a 23- or
		 * 25-bit accumulator, term-level grids, pre-aligned operands,
		 * rounding or two's-complement floors all fit fewer. The 1/w
		 * plane's C runs through the same accumulator (below). The s/t sum
		 * had a fitted grid cap (never coarser than 2^(e_final - 20), cap
		 * 20, for quads not on half pixels), which the 24-bit chop now
		 * does without. */
		double cs = 0, ex = 0;
		const double *vq = vc ? vc : v;
		int cv = vc != NULL || !tex || (colour == 1 && persp);	/* the affine colour channels' terms: vterm, the 24-bit rhw * E times the byte */
		int ef = 0, kk, seq[3], gatecut = 1, kq, pcut, cap = 0;

		for (kq = 0; kq < 3; kq++)
			if (su->x[kq] * 2 != floor(su->x[kq] * 2) || su->y[kq] * 2 != floor(su->y[kq] * 2))
				gatecut = 0;
		/* The order the three vertex terms are added in: term_order, for
		 * both planes and every area. The 1/w plane takes it too: its
		 * constants fit best with the slopes in this order and the C sum in
		 * index order, and better still with both. */
		term_order(su, seq);
		/* The 1/w plane's only remaining grid cap, for an area of more
		 * than 19 bits (big): never coarser than 2^(ef - 24), ef the
		 * exact sum's exponent. The cap is fitted, not a circuit (22 and
		 * 23 give the same results; 21 and below do not). */
		if (!vc && big) {
			cap = 24;
			for (kk = 0; kk < 3; kk++)
				ex += vq[kk] * edge_k(su, (kk + 1) % 3, (kk + 2) % 3);
			if (ex != 0)
				frexp(ex, &ef);
		}
		/* The s/t planes' edge products cut to 24 bits before they are
		 * added (a float multiplier's output), for triangles whose three
		 * vertices all lie on half pixels (the cap-20 grid, which these
		 * triangles went without, is gone for all of them).
		 * Whole-pixel quads fit a model of each product cut to 24 bits, the
		 * sum chopped to 24 bits and no cap better than the cap-20 rule,
		 * including the second triangle of each quad, whose two right
		 * vertices carry equal values and cancel exactly. Caps of 19 to 23
		 * behave alike once the cut is in, so the cap-20 grid is what hurt.
		 * The gate is fitted, not a circuit: the same cut on every triangle
		 * costs triangles with 1/8 and 1/4 pixel coordinates, which want
		 * exact products. Whole-pixel and half-pixel gates score alike; an
		 * area gate and an edge-constant bit-count gate both did worse. */
		/* The 1/w plane's products are cut to 24 bits for every
		 * triangle, ungated: cut(rhw * E, 24) per term (the device's q
		 * constants fit better with the product cut and a chopped sum than
		 * without). */
		pcut = vc ? gatecut : 1;
		for (kk = 0; kk < 3; kk++) {
			int k = seq[kk], i = (k + 1) % 3, j = (k + 2) % 3;
			double tm = cv ? vterm(su, k, edge_k(su, i, j)) : vq[k] * edge_k(su, i, j);

			if (pcut)
				tm = cut(tm, 24, 0);
			if ((cs += tm) != 0) {
				int es, ge;

				frexp(cs, &es);
				ge = es - 24;
				if (cap && ex != 0 && ef - cap < ge)
					ge = ef - cap;
				cs = ldexp(trunc(ldexp(cs, -ge)), ge);
			}
		}
		pl[4] = cut(cs, 19, 0) * rc;
		/* The slopes on a grid 27 bits below C's exponent: the plane
		 * evaluator's fixed-point accumulator, positioned by C, holds no
		 * slope bit below it, so a slope tiny against C loses its low bits
		 * (a 1/w x slope of ~1e-5 against C ~0.5 shows it; 26 or 28 bits fit
		 * worse).
		 * C here is the plane constant as the evaluator uses it, ROUNDED
		 * to 16 bits (plane_at: ties away), not the exact value at the
		 * origin: a C just under a power of two (0.49999 for 0.5) that rounds
		 * up to it takes the coarser grid: C 0.5 - 0.9e-6 to 0.5 - 2.6e-6
		 * (rounds to 0.5) wants the slope one 2^-28 step lower (the 2^-27
		 * grid), while 0.5 - 3.4e-6 (rounds to 0.4999924) and 0.5 - 7.6e-6
		 * keep the 2^-28 grid. C's exponent after its own rounding (the
		 * exact cc / area before) as the grid's reference is fitted, not a
		 * circuit. */
		if (tex && pl[4] != 0) {
			int ec;

			frexp(cut(pl[4], 16, 2), &ec);
			{
				double g = ldexp(1, ec - 27);

				pl[0] = floor(pl[0] / g) * g;
				pl[1] = floor(pl[1] / g) * g;
			}
		}
	}
}

/* A plane at a pixel centre: its exact value at the origin cut, plus the
 * cut slopes times the distance, cut */
/*
 * A plane at a pixel centre, as the ISP does it (per tile line a
 * multiplier gives A x + B y + C at the start of a block, then each pixel
 * adds A): C at the screen's origin rounded to 16 bits,
 * the block's start value (A x and B y floored to 26 bits, the sum
 * floored to 26), then A stepped across the block, the value floored to
 * 16 bits: every cut towards minus infinity, as a two's complement
 * accumulator does. 26 bits, not 24: the last pixel of a frame (Gouraud red
 * 45.4991 where the device's plane, floored from its exact value, gives 45.5)
 * needs them; 28 or more fit slightly worse. A plane from the screen's origin,
 * not the triangle's corner: the device's coordinates depend on where a quad
 * lies (C exactly 1/3 at the origin, which the device truncates).
 */
static double plane_at(const struct setup *su, const double pl[5], float px, float py)
{
	/*
	 * C rounded to a 16 bit significand, ties AWAY from zero (truncation and
	 * 15 or 17 bits fit far worse, and ties to even fit worse too). With an
	 * exact area reciprocal C has about 19 significant bits, so the cut hits
	 * an exact tie about 1 time in 8. Whether the rule belongs to the C
	 * accumulate is fitted; it only matters on exact ties. Same rule as the
	 * LOD's q constant.
	 */
	double xs = floor(px / 16) * 16 + 0.5, c = cut(pl[4], 16, 2), st;

	(void)su;
	/* The block start's sum floored at 27 significant bits (the two products
	 * keep 26). In the 1/w plane of a thin 400 px triangle at 0.5 to 0.5055,
	 * a q of 33104.9995 floors to 33104 where the device reads 33105: one q
	 * constant of 1 to 2 units of 2^-27 added over the whole frame fixes it;
	 * a 27-bit sum gives the same frames, as do 28, 30 and 40. The width is
	 * fitted, not a circuit (27 to 40 fit alike; 28 or more was called
	 * slightly worse). */
	/* The product of the x slope and the block's start x keeps 27 bits, not 26.
	 * A slope of 16 bits times a start x of x.5 below 1024 (11 bits) has 27,
	 * and flooring one bit off it put a 1/w plane that is an exact integer on
	 * the 16-bit grid one step under it (q 0.37 to 0.43, the 1/8 texel weight
	 * of u one step lower) while every pixel whose exact value is under the
	 * integer floors correctly. The y product (a 10 bit py) and the sum are as
	 * before. 27 to 30 give the same results. The width is fitted. */
	st = floorb(floorb(pl[0] * xs, 27) + floorb(pl[1] * py, 26) + c, 27);
	/* cut towards minus infinity, as a two's complement accumulator does
	 * (s/w planes below zero) */
	return floor16(st + pl[0] * (px - xs));
}

/*
 * The depth plane. The depth of a tile's first pixel (pixel centre
 * xs = x0 + .5, ys = y0 + .5) is floor20(N * rc), N = the 24-bit chop of
 * cs + na xs + nb ys, with cs, na, nb the texture planes' set-up sums of the vertex depths
 * (every product cut to 24 bits, the sum chopped to 24 bits after each add, here with the
 * lowest vertex, the largest y and among equal y the largest x, added last; the order of the other
 * two cannot matter: the first add is symmetric) and rc = trunc21(1 / trunc21(area)); floor20 is
 * the floor to the depth's 20-bit significand. Inside the tile the depth is that start plus
 * A dx + B dy, A and B (na rc, nb rc) cut toward zero to 2^-6 of the start's 20-bit unit, floored
 * to the 20-bit significand. A triangle whose three vertex depths are equal is exactly that
 * depth (the plane above would put it 1 unit under). The vertex added last matters: the
 * lowest-left of two bottom vertices last is wrong, the lowest-right right. The widths 21
 * of the area and the reciprocal are fitted, not a circuit (19 and 20 bits miss 20-30%).
 */
static void zplane_setup(struct setup *su)
{
	const struct prim *p = su->p;
	double vz[3], darea, cs = 0, na = 0, nb = 0;
	int k, kk, last = 0, seq[3], o2 = 0;

	for (k = 0; k < 3; k++)
		vz[k] = p->v[su->order[k]].z;
	su->zflat = vz[0] == vz[1] && vz[1] == vz[2];
	if (su->zflat)
		return;
	darea = fabs(edge_k(su, 1, 2) + edge_k(su, 2, 0) + edge_k(su, 0, 1));
	su->zrc = darea > 0 ? cut(1.0 / cut(darea, 21, 0), 21, 0) : 0;
	for (k = 1; k < 3; k++)
		if (su->y[k] > su->y[last] || (su->y[k] == su->y[last] && su->x[k] > su->x[last]))
			last = k;
	for (kk = 0; kk < 3; kk++)
		if (kk != last)
			seq[o2++] = kk;
	seq[2] = last;
	for (kk = 0; kk < 3; kk++) {
		int i, j;
		double tm, pa, pb;

		k = seq[kk];
		i = (k + 1) % 3;
		j = (k + 2) % 3;
		tm = vz[k] * edge_k(su, i, j);
		pa = -((double)su->y[j] - su->y[i]) * vz[k];
		pb = ((double)su->x[j] - su->x[i]) * vz[k];
		cs += tm != 0 ? cut(tm, 24, 0) : 0;
		na += pa != 0 ? cut(pa, 24, 0) : 0;
		nb += pb != 0 ? cut(pb, 24, 0) : 0;
		if (cs != 0)
			cs = cut(cs, 24, 0);
		if (na != 0)
			na = cut(na, 24, 0);
		if (nb != 0)
			nb = cut(nb, 24, 0);
	}
	su->zn[0] = cs;
	su->zn[1] = na;
	su->zn[2] = nb;
}

/* The depth plane's start value of the tile at (tx0, ty0) and its two in-tile slopes */
static void zplane_tile(const struct setup *su, int tx0, int ty0, double *z0, double *a, double *b)
{
	double n = cut(su->zn[0] + su->zn[1] * (tx0 + 0.5) + su->zn[2] * (ty0 + 0.5), 24, 0), pv = n * su->zrc, u;
	int e;

	if (!(pv > 0)) {
		*z0 = pv;
		*a = *b = 0;
		return;
	}
	frexp(pv, &e);
	u = ldexp(1, e - 20);
	*z0 = floor(pv / u) * u;
	*a = trunc(su->zn[1] * su->zrc / u * 64) / 64 * u;
	*b = trunc(su->zn[2] * su->zrc / u * 64) / 64 * u;
}

/*
 * The ISP's 1/q: a 16-bit significand from q's 16-bit significand, as
 * the device's reciprocal ROM gives it (rcptab.c): trunc16(1/q), one ulp
 * up or down for some 13% of significands.
 */
static double rcp16(double q)
{
	double r = trunc16(1 / q);
	int e, m;

	if (!(q > 0) || !isfinite(r))
		return r;
	m = (int)ldexp(frexp(q, &e), 16);
	frexp(r, &e);
	return r + rcp_offset(m) * ldexp(1, e - 16);
}

static float persp_attr(const struct setup *su, const double npl[5], float px, float py)
{
	double q = plane_at(su, su->qpl, px, py), n = plane_at(su, npl, px, py), r, ulp;
	int en, er;

	if (n == 0 || !(q > 0))
		return 0;
	r = rcp16(q);
	/* A negative power of two sits at the top of its two's complement
	 * binade, so its exponent is one lower than the magnitude's and the
	 * product's grid one bit finer than for any other value: every negative
	 * 0x8000-mantissa case fits one more bit, rounded, ties toward
	 * +infinity; a positive power of two keeps the 12 bit grid. This is
	 * fitted, not a circuit. */
	if (frexp(n, &en) == -0.5)
		en--;
	frexp(r, &er);
	ulp = ldexp(1, (en - 1) + (er - 1) - 12);
	/* half up, not to even (affine planes land on ties) */
	return (float)(floor(n * r / ulp + 0.5) * ulp);
}

/* A colour channel at a pixel, as a byte: the same planes, in bytes / w,
 * the product rounded half up (README, assumption 27) */
static int colour_at(const struct setup *su, const double cpl[5], float px, float py)
{
	double q = plane_at(su, su->qplc, px, py), n = plane_at(su, cpl, px, py), v;

	if (n == 0 || !(q > 0))
		return 0;
	v = floor(n * rcp16(q) + 0.5);
	return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

/* A vertex position on the rasteriser's grid: sixteenths of a pixel,
 * rounded (1/8, 1/32 or truncating fit worse) */
static float grid(float c)
{
	float t = c * 16, fl = floorf(t);

	/* An exact half goes to the even sixteenth (round half to even).
	 * Every edge is where round-half-even puts it (even
	 * k down, odd k up). Alone it only helps where the vertex position is
	 * exact: see project() for the arithmetic that makes the positions of
	 * random content inexact */
	if (t - fl == 0.5f)
		return ((long)fl & 1 ? fl + 1 : fl) / 16;
	return floorf(t + 0.5f) / 16;
}

static void setup_prim(struct setup *su, const struct prim *p, const struct rstate *st)
{
	float area, gx[3], gy[3];
	int k, mode;

	memset(su, 0, sizeof(*su));
	su->p = p;
	su->st = st;
	for (k = 0; k < 3; k++) {
		gx[k] = grid(p->v[k].x);
		gy[k] = grid(p->v[k].y);
	}
	area = (gx[1] - gx[0]) * (gy[2] - gy[0]) - (gy[1] - gy[0]) * (gx[2] - gx[0]);
	if (area == 0) {	/* products of a far vertex cancel in float32: the double area decides */
		double ad = ((double)gx[1] - gx[0]) * ((double)gy[2] - gy[0]) - ((double)gy[1] - gy[0]) * ((double)gx[2] - gx[0]);

		area = ad > 0 ? 1 : ad < 0 ? -1 : 0;
	}
	if (area == 0 || !isfinite(area)) {
		su->skip = 1;
		return;
	}
	su->order[0] = 0;
	su->order[1] = area > 0 ? 1 : 2;
	su->order[2] = area > 0 ? 2 : 1;
	for (k = 0; k < 3; k++) {
		su->x[k] = gx[su->order[k]];
		su->y[k] = gy[su->order[k]];
	}
	su->area = fabsf(area);
	/* the same area without the float32 rounding of its two products and
	 * their difference (grid coordinates make every product exact in
	 * double) */
	su->xarea = fabs((double)(gx[1] - gx[0]) * (gy[2] - gy[0]) - (double)(gy[1] - gy[0]) * (gx[2] - gx[0]));
	su->affine = p->v[0].rhw == p->v[1].rhw && p->v[1].rhw == p->v[2].rhw;
	/* b_k = E(edge opposite k) / area, E(a, b, p) = (b - a) x (p - a) */
	for (k = 0; k < 3; k++) {
		int a = (k + 1) % 3, b = (k + 2) % 3;

		su->dbx[k] = -(su->y[b] - su->y[a]) / su->area;
		su->dby[k] = (su->x[b] - su->x[a]) / su->area;
	}
	mode = st->st >> 16 & 7;
	su->nunits = mode == 0 ? 0 : mode == 5 ? 2 : 1;
	/* colours through the planes, affine ones with the textures' slopes
	 * (barycentric weights fit worse), perspective ones with the slopes divided
	 * and truncated (the textures' slopes fit worse) */
	su->gouraud = (st->st & ST_GOURAUD) != 0;
	su->colslope = !su->affine;
	if (su->nunits || su->gouraud) {
		double v[3], vc[3];
		int u, c;

		su->ox = fminf(su->x[0], fminf(su->x[1], su->x[2]));
		su->oy = fminf(su->y[0], fminf(su->y[1], su->y[2]));
		for (k = 0; k < 3; k++)
			v[k] = p->v[su->order[k]].rhw;	/* 20 bits from the TA (ta.c) */
		plane_of(su, v, NULL, su->qpl, 0);
		su->lql[0] = le_pre[0];
		su->lql[1] = le_pre[1];
		/* C rounded to 16 bits where 1/w is the same at every vertex too,
		 * as plane_at does (it was truncated, which fit only while the
		 * reciprocal was trunc16(1/q)) */
		g_isq = 1;
		plane_of(su, v, NULL, su->qplc, 2);
		g_isq = 0;
		if (su->affine)	/* the same rule */
			su->qplc[4] = cut(su->qplc[4], 16, 0);
		for (u = 0; u < su->nunits; u++)
			for (c = 0; c < 2; c++) {
				/* each vertex's s/w: its 20-bit 1/w times the coordinate,
				 * a float product (24 bits, rounded), not cut to the TA's
				 * 20 (which way two vertices' s/w differ below bit 20 decides
				 * a plane; full-width 1/w, or 20-22 bits, fit worse) */
				for (k = 0; k < 3; k++) {
					double sw = (double)p->v[su->order[k]].rhw * p->v[su->order[k]].tc[u][c];

					/* The slopes take the vertex product rhw * s unrounded
					 * where all three vertices lie on half pixels (plane_of
					 * still cuts each edge product to 24 bits afterwards), the
					 * 24-bit rounded one elsewhere. With the rounded product the
					 * y slope B can land one 16-bit step (ulp16) off; the unrounded
					 * one does not.
					 * The gate is fitted, not a circuit: without it, triangles on
					 * 1/16 pixels, which want the rounded product, get worse. A
					 * 1/8 pixel gate fits worse; whole, half and
					 * quarter pixel gates score alike. Unexplained: some triangles
					 * want the product truncated to 24 bits (a 23-bit chopping
					 * accumulator that fits both breaks others). */
					int gh = 1, gq;

					for (gq = 0; gq < 3; gq++)
						if (su->x[gq] * 2 != floor(su->x[gq] * 2) || su->y[gq] * 2 != floor(su->y[gq] * 2))
							gh = 0;
					su->vrhw[k] = p->v[su->order[k]].rhw;
					su->vs[k] = p->v[su->order[k]].tc[u][c];
					v[k] = gh ? sw : cut(sw, 24, 1);
					vc[k] = cut(sw, 25, 0);
				}
				plane_of(su, v, vc, su->npl[u][c], 0);
				su->lsl[u][c][0] = le_pre[0];
				su->lsl[u][c][1] = le_pre[1];
			}
		if (su->gouraud)
			for (u = 0; u < 2; u++)
				for (c = 0; c < 4; c++) {
					for (k = 0; k < 3; k++) {
						const struct vtx *pv = &p->v[su->order[k]];

						double byte = floor((u ? pv->off[c] : pv->col[c]) * 255 + 0.5);

						/* The fog factor (the offset colour's alpha, u 1 c 3) is
						 * interpolated as its complement, 255 - byte (the weight
						 * 256 - F of the fog colour in the blend, F = byte + 1),
						 * through the same colour chain as the RGB channels, and
						 * the pixel's byte is 255 minus the rounded plane value
						 * (colour_at's caller). Through the chain the byte itself
						 * lands on rounding ties or truncations, the device being
						 * above the emulator in most misses and rounding exact ties
						 * DOWN; the complement fits. The round half up of the
						 * complement is the device's tie down on the byte, and the
						 * plane's floors on the complement are its upward bias. Why
						 * the hardware carries 255 - byte is not known; no cutoff or
						 * constant was tuned. */
						if (u == 1 && c == 3)
							byte = 255 - byte;

						/* the vertex's byte (ta.c), times 1/w, truncated
						 * to 18 bits for an affine triangle, to 24 for a perspective
						 * one like the textures' (20 and 22 bits fit worse, 23 and
						 * 25 bits equal, rounding to nearest instead of truncating
						 * worse): the width 24, and that affine triangles keep 18,
						 * are fitted */
						v[k] = cut((double)pv->rhw * byte, su->affine ? 18 : 24, 0);
						su->vrhw[k] = pv->rhw;
						su->vs[k] = byte;
					}
					plane_of(su, v, NULL, su->cpl[u][c], su->colslope ? 1 : 2);
					slopes_shared(su->cpl[u][c]);
				}
	}
	for (k = 0; k < su->nunits; k++)
		tex_decode(&su->tu[k], st->tex[k]);
	konst_of(st->at, su->konst);
	/* polygon offset: the primitive word's low 16 bits, a magnitude, bit
	 * 16 its sign (-4 as two's
	 * complement pushes the quad far away), in units
	 * of the pixel's depth's 16-bit significand (below) */
	su->bias = (float)((p->pw & 0x10000) ? -(int)(p->pw & 0xffff) : (int)(p->pw & 0xffff));
	zplane_setup(su);
}

/* Top-left fill rule: with positive area and y down, a top edge
 * runs right along y, a left edge upwards */
static int top_left(const struct setup *su, int a, int b)
{
	float dx = su->x[b] - su->x[a], dy = su->y[b] - su->y[a];

	return (dy == 0 && dx > 0) || dy < 0;
}

/* A unit's texture coordinates at a pixel centre (persp_attr; affine
 * triangles take the same path) */
static void tc_at(const struct setup *su, float px, float py, int unit, float uv[2])
{
	uv[0] = persp_attr(su, su->npl[unit][0], px, py);
	uv[1] = persp_attr(su, su->npl[unit][1], px, py);
}

/* The significand of x cut (towards zero) to 8 bits, as a reciprocal ROM
 * and its products keep them */
static double cut8(double x)
{
	int e;
	double m = frexp(x, &e);

	return ldexp(floor(m * 256) / 256, e);
}

/*
 * len * sc, sc = (1 + eps) / 2 * 2^e the cut square of 1/q (eps = (rho -
 * 128) / 128 for its 8 bit significand rho, 0 to 127/128): the device's
 * multiplier takes len as V * 2^t, V in [1, 2), and gives
 * (V + r) / 2 * 2^t with r = floor7(floor7(V) * eps), floor7 to 1/128. So
 * the product is len * sc less up to 1/128 of V, never above, and exact
 * when eps is 0. Fitted to flat quads of 1/q from 1 to 2 (the
 * cut alone or with a constant fits worse). The circuit itself is not known;
 * the mean truncation deficit of this multiplier is what earlier fits took
 * for a constant 0.9975
 */
static double lod_mul(double len, int e, double eps)
{
	int x;
	double m, v, vg, r;

	if (!(len > 0))
		return 0;
	if (eps == 0)
		return ldexp(len, e - 1);
	m = frexp(ldexp(len, e), &x);
	v = 2 * m;
	vg = floor(v * 128) / 128;
	r = floor(vg * eps * 128) / 128;
	return ldexp((v + r) / 2, x - 1);
}

/*
 * The level of detail at a pixel: log2 of texels a pixel. The derivatives
 * of s and t are taken at the corner of the pixel's 16 x 16 block (x and y
 * multiples of 16), as the numerators A_n q0 - n0 A_q of d(n/q), and
 * divided by this pixel's own q squared; the larger of the x and y
 * lengths. The device's LOD jumps at every 16th column and drifts with q
 * between (the block's centre or the pixel centres of its corner fit
 * worse).
 * A corner outside the triangle, even with q0 <= 0, works the same way.
 */
/* A plane's constant as the LOD sees it: rounded to 16 bits, halves away from zero.
 * With the slopes unshared as below, ties away fit best; ties to even, down or up
 * fit worse. The rule itself is fitted, not a claim about a rounding circuit */
static double lod_c16(double c)
{
	return cut(c, 16, 2);
}

/*
 * The q plane's constant as the LOD sees it: rounded to 16 bits, halves AWAY from zero
 * (up, q > 0), where the colour path's plane_at and the s and t planes round halves to
 * even. The TA's 20 bit 1/w of flat quads often lands exactly
 * on a half of the 16 bit grid (w 0.75: 1.0101..0101 0 | 10 000); the device's q is then
 * the upper neighbour (1/q below the exact 8 bit value 3/4, as the device shows),
 * even rounds it down. Flat quads' 1/q ties at 4/3, 16/15, 3 and 8/5 all follow.
 * Half-away on the s and t constants changes nothing for the LOD.
 */
static double lod_c16q(double c)
{
	return cut(c, 16, 2);
}

/* A plane at a block corner, from the rounded constant, A x and B y floored to 26 bits (plane_at's start value) */
static double lod_corner(const double pl[5], double bx, double by, int isq)
{
	return floorb(floorb(pl[0] * bx, 26) + floorb(pl[1] * by, 26) + (isq ? lod_c16q(pl[4]) : lod_c16(pl[4])), 26);
}

static float lod_at(const struct setup *su, float px, float py, int unit, const float uv[2])
{
	const struct texunit *t = &su->tu[unit];
	const double *ns = su->npl[unit][0], *nt = su->npl[unit][1], *qp = su->qpl;
	double nsb[5], ntb[5], qpb[5];
	double bx = floor(px / 16) * 16, by = floor(py / 16) * 16;

	/* The LOD takes the s, t and 1/w planes' slopes as plane_of rounds them (16 bits,
	 * each on its own exponent), not as slopes_shared floors them onto the larger
	 * one's grid, perspective primitives too: across a level-blend edge the
	 * shared grid mismatches where own-exponent slopes do not, and for quads
	 * with w varying the unshared s, t and 1/w slopes and the s/t constants'
	 * ties away (lod_c16) fit best. Applying that to every primitive is
	 * fitted; no claim about a circuit. */
	memcpy(nsb, ns, sizeof(nsb));
	memcpy(ntb, nt, sizeof(ntb));
	memcpy(qpb, qp, sizeof(qpb));
	if (su->lsl[unit][0][0] != 0 || su->lsl[unit][0][1] != 0) {
		nsb[0] = su->lsl[unit][0][0];
		nsb[1] = su->lsl[unit][0][1];
	}
	if (su->lsl[unit][1][0] != 0 || su->lsl[unit][1][1] != 0) {
		ntb[0] = su->lsl[unit][1][0];
		ntb[1] = su->lsl[unit][1][1];
	}
	if (su->lql[0] != 0 || su->lql[1] != 0) {
		qpb[0] = su->lql[0];
		qpb[1] = su->lql[1];
	}
	ns = nsb;
	nt = ntb;
	qp = qpb;
	/* the LOD's planes take the same 16-bit constant (ties away from zero) the
	 * colour path's plane_at takes, the block corner's values built like its
	 * start value, and q at the pixel is plane_at's (floor16 too): a flat q of
	 * 0.999999046 rounds to 1, and the device's 1/q is then exactly 1. 15, 17
	 * and 18 bits, floor or a flat-only rounding fit worse (16 bits sharp). */
	double xs = floor(px / 16) * 16 + 0.5;
	double q0 = lod_corner(qp, bx, by, 1);
	/* plane_at with lod_c16q's constant */
	/* the pixel's own q is the plane evaluator's (plane_at): the set-up's shared slopes, as persp_attr
	 * takes it; only the numerators and the corner values above use the unshared ones.
	 * Q from the unshared slopes leaves pixels wrong that the shared ones get right.
	 * This is fitted, not a claim about a circuit. */
	double q = floor16(floorb(floorb(su->qpl[0] * xs, 26) + floorb(su->qpl[1] * py, 26) + lod_c16q(su->qpl[4]), 26) + su->qpl[0] * (px - xs));
	double n0 = lod_corner(ns, bx, by, 0), m0 = lod_corner(nt, bx, by, 0), sc, eps;
	int cut, cute;
	float w = (float)(1 << t->lw), h = (float)(1 << t->lh), dx, dy;
	double dd;

	(void)uv;
	if (!(q > 0))
		return 0;
	sc = 1 / (q * q);
	/* 1/q^2 through the device's reciprocal: the device's length is the exact
	 * one times 0.983-0.9935, a sawtooth of period 1/128 in the significand of
	 * 1/q, on the per-pixel q, not the block corner's. 1/q is cut to an 8 bit
	 * significand, squared and cut to 8 bits again, then multiplied in by
	 * lod_mul (a fit, not a circuit). Flat quads at w 1 (q = 1 - 2^-20) must
	 * stay exact, and quads with tpp on bin edges need 1/q^2 just above 1; so
	 * nothing is cut where 1/q is within 2^-19 of its 8 bit cut. Open: where
	 * 1/q is exactly an 8 bit value the device acts as if q were above the
	 * true one (w 3, 4/3, 16/15, not 1.6); ours is the TA's truncated 20 bit
	 * 1/w. A nearest-rounded float32 1/w would fix 3 of those 4 ties but is
	 * neutral overall, so it is not used */
	{
		int e2;
		/* The 1/q the LOD cuts is taken as the ISP's reciprocal (rcp16, the ROM with its
		 * +-1 ulp entries), not the exact one: with the half-away q constant this fits
		 * flat-quad sweeps that rcp16 alone, or half-away alone, leave wrong. Fitted. */
		double r1 = cut8(rcp16(q)), s8 = cut8(r1 * r1), mm = frexp(s8, &e2);

		eps = (floor(mm * 256) - 128) / 128;
		cut = 1;
		cute = e2;
	}
	/* each vector's length as max + (sqrt 2 - 1) min, the octagon's norm
	 * (the device's length is 1 at 0 and 45 degrees and 1.08 at 22.5,
	 * symmetric about it; 3/8 fits worse: it was making up for something in
	 * the perspective derivatives). The device's level rises up to 0.08 off the
	 * axes as this does. The numerators' norm is taken first and multiplied by
	 * 1/q^2 once (the product on each of the four numerators fits far worse) */
	{
		double ax = fabs((ns[0] * q0 - n0 * qp[0]) * w), bx = fabs((nt[0] * q0 - m0 * qp[0]) * h);
		double ay = fabs((ns[1] * q0 - n0 * qp[1]) * w), by = fabs((nt[1] * q0 - m0 * qp[1]) * h);
		double lx = fmax(ax, bx) + 0.41421356 * fmin(ax, bx), ly = fmax(ay, by) + 0.41421356 * fmin(ay, by);

		if (cut) {
			dd = lod_mul(fmax(lx, ly), cute, eps);
			dx = (float)dd;
		} else {
			ax = fabs((ns[0] * q0 - n0 * qp[0]) * sc * w);
			bx = fabs((nt[0] * q0 - m0 * qp[0]) * sc * h);
			ay = fabs((ns[1] * q0 - n0 * qp[1]) * sc * w);
			by = fabs((nt[1] * q0 - m0 * qp[1]) * sc * h);
			dx = (float)(fmax(ax, bx) + 0.41421356 * fmin(ax, bx));
			dy = (float)(fmax(ay, by) + 0.41421356 * fmin(ay, by));
			dx = dx > dy ? dx : dy;
			dd = dx;
		}
	}
	if (!(dx > 0))
		return 0;
	/* log2 as the N810 takes it: the exponent and the mantissa less one
	 * (piecewise linear; exact log2 misses most quad sizes). Trilinear keeps
	 * it unrounded (texture.c); the nearest level is taken from it rounded to
	 * eighths (a triangle at 2.473 takes level 3, one at 2.452 level 2;
	 * 32nds fit worse) */
	{
		int ex;
		double m = frexp(dd, &ex), l = (ex - 1) + (2 * m - 1);
		float lf;

		/* The returned float is cut toward minus infinity, not rounded, and the log2
		 * is summed in double: a length a hair below an edge of the level blend's 64ths
		 * (4.0624999 on the edge 4.0625) was rounded up onto the edge by the float
		 * sum, one kf too high. The device's mantissa is cut, not rounded (the
		 * circuit is not known) */
		/* the nearest level is (int)(lod + 0.5): the level changes where the log2
		 * (exponent + mantissa - 1) passes k + 0.4219, not k + 0.5 or the eighths' k + 0.4375.
		 * Every T from 0.075 to 0.08 fits alike, 0.0625 and 0.085 do not;
		 * 5/64 = 0.078125 is fitted (27/64 is the threshold, a guess for the circuit) */
		if (!t->trilinear)
			l += 5.0 / 64;
		lf = (float)l;
		if ((double)lf > l)
			lf = nextafterf(lf, -INFINITY);
		return lf;
	}
}

/*
 * A blend factor in 256ths, from 8-bit colours: the N810 blends in fixed
 * point, (s * fs >> 8) + (d * fd >> 8) with ONE = 256 and 1 - x = 256 - x
 * (SRC_ALPHA 0x80 over 255 gives 127, DST_COLOR 0x80 times 255 gives 127;
 * README, assumption 24)
 */
static int factor8(int code, int dst_side, int saturate, int chan, const int s[4], const int d[4])
{
	switch (code) {
	case 0:
		if (saturate)
			return chan == 3 ? 256 : s[3] < 256 - d[3] ? s[3] : 256 - d[3];
		return 0;
	case 1: return 256;
	case 2: return dst_side ? s[chan] : d[chan];
	case 3: return 256 - (dst_side ? s[chan] : d[chan]);
	case 4: return s[3];
	case 5: return 256 - s[3];
	case 6: return d[3];
	default: return 256 - d[3];
	}
}

/* 255 is one here too (alpha 255 over anything is the source) */
static int factor(int code, int dst_side, int saturate, int chan, const int s[4], const int d[4])
{
	int s1[4], d1[4], k;

	for (k = 0; k < 4; k++) {
		s1[k] = s[k] == 255 ? 256 : s[k];
		d1[k] = d[k] == 255 ? 256 : d[k];
	}
	return factor8(code, dst_side, saturate, chan, s1, d1);
}

/* --- a triangle in a tile --- */

/*
 * A prim whose state word has both object-type bits (0x6000: a blended prim with an
 * alpha test, as old software wrote it) draws nothing on the N810, but it is not
 * absent: the prim CLAIMS its pixels, and later prims do not draw there (the
 * earlier quad is untouched); the claim follows the depth test. The claim is
 * per tile (tile_claim) and gone at the next tile.
 *
 * The last prim of a tile's list that is a both-bit one and has no texture (a textured one
 * does not) also loses pixels of the tile's last row: pixels it covers at
 * x % 16 >= 8 on y % 16 == 15 return to what the tile held at its start (the
 * previous frame's colour; this does not happen when the prim was not the last
 * of its tile), and so does a covered pixel of a left-half group (x % 16 < 8)
 * whose right neighbour pixel x0 + 8 is not covered (the run ends in the group, or came
 * from the left tile and ends here). An earlier rule, a partly covered group
 * after a covered pixel, left some pixels of the old pixel unrestored.
 *
 * What the erased pixel then holds is NOT always the old pixel: the written
 * value is the old pixel ANDed bit by bit with a 32-bit mask that is constant
 * over a cell: 0xffffffff (the old pixel), 0 (black with alpha 0) or a stray word (float-like
 * 0x401a1c00, 0x3f190000 ...). Source factor ZERO or DST_COLOR with any destination factor
 * gives the old pixel in every tile but the first of a row (the mask is stable across runs);
 * the other source factors give 0 or the old pixel per tile row with no predictor found
 * (alpha test, prim colour and alpha, clear colour, BG colour do not decide it) and
 * stray words that change between runs. We keep the old pixel for all of them. */
static unsigned char tile_claim[TILE * TILE];
static float tile_col0[TILE * TILE][4];
static int tile_lastk;

static int px_cov(const struct setup *su, const struct rctx *rc, int x, int y)
{
	float ex = x + (rc->samp & 1 ? 0.5f : 0), ey = y + (rc->samp & 1 ? 0.5f : 0);
	int k;

	if (x < su->p->x0 || x >= su->p->x1 || y < su->p->y0 || y >= su->p->y1)
		return 0;
	for (k = 0; k < 3; k++) {
		int a = (k + 1) % 3, b = (k + 2) % 3;
		double ee = ((double)su->y[a] - su->y[b]) * ex + ((double)su->x[b] - su->x[a]) * ey + edge_k(su, a, b);
		float ed = ee < 0 ? -1 : ee > 0 ? 1 : 0;

		if (ed < 0 || (ed == 0 && !top_left(su, a, b)))
			return 0;
	}
	return 1;
}

static void draw(struct mbxemu *e, const struct rctx *rc, const struct setup *su, int tx0, int ty0,
		 float col[TILE * TILE][4], float zb[TILE * TILE])
{
	const struct prim *p = su->p;
	const struct rstate *st = su->st;
	uint32_t s = st->st;
	int x0 = tx0 > p->x0 ? tx0 : p->x0, y0 = ty0 > p->y0 ? ty0 : p->y0;
	int x1 = tx0 + TILE < p->x1 ? tx0 + TILE : p->x1, y1 = ty0 + TILE < p->y1 ? ty0 + TILE : p->y1;
	int zfunc = s >> 9 & 7, gouraud = s & ST_GOURAUD, x, y, k, u;
	int offset = (s & ST_OFFSET) && !(p->pw & PRIM_ZEROOFFSET);
	int tl[3];
	double zt0 = 0, za = 0, zb_ = 0;

	for (k = 0; k < 3; k++)
		tl[k] = top_left(su, (k + 1) % 3, (k + 2) % 3);
	if (!su->zflat)
		zplane_tile(su, tx0, ty0, &zt0, &za, &zb_);
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++) {
			/* coverage (the set-up FPU) and the shading and texture
			 * planes (the TSP) are each sampled at the pixel's centre, or
			 * its top left corner when 0x61c bit 0 or 1 is clear */
			float ex = x + (rc->samp & 1 ? 0.5f : 0), ey = y + (rc->samp & 1 ? 0.5f : 0);
			float px = x + (rc->samp & 2 ? 0.5f : 0), py = y + (rc->samp & 2 ? 0.5f : 0);
			float z, c[4], off[4], prev[4];
			int i = (y - ty0) * TILE + (x - tx0), in = 1;

			for (k = 0; k < 3; k++) {
				int a = (k + 1) % 3, b = (k + 2) % 3;
				/* The coverage test is the edge plane as the set-up forms it: A x + B y
				 * plus the edge constant x_a y_b - x_b y_a with EACH PRODUCT cut to
				 * 24 bits (edge_k, the constant the plane set-up and the area use),
				 * not the exact edge function: an exact edge value of -1/256 px^2 draws the
				 * pixel and +1/256 leaves it out. The cut is antisymmetric, so shared
				 * edges still agree. The exact order of the sum is fitted, not a
				 * circuit. */
				double ee = edge_at(su, a, b, ex, ey, tl[k]);
				float ed = ee < 0 ? -1 : ee > 0 ? 1 : 0;

				if (ed < 0 || (ed == 0 && !tl[k]))
					in = 0;
			}
			if (!in)
				continue;
			if (tile_claim[i])
				continue;
			/* depth: the device's plane (zplane_setup, zplane_tile). It was the
			 * exact edge values summed in double and rounded once (float
			 * barycentric weights put an ulp or two of noise on a constant z;
			 * a constant z is still exact, below) */
			{
				if (su->zflat) {
					z = p->v[su->order[0]].z;
				} else {
					double zz = zt0 + za * (x - tx0) + zb_ * (y - ty0);
					int ze;

					if (zz > 0) {
						frexp(zz, &ze);
						zz = floor(ldexp(zz, 20 - ze)) * ldexp(1, ze - 20);
					}
					z = (float)zz;
				}
			}
			if (su->bias != 0) {
				/* a unit is one ulp of the depth's 16-bit significand,
				 * 2^(e - 16) for z in [2^(e-1), 2^e) (-1 beats a move of
				 * 2^-17 at depths 0.5 to 0.99, 2^-18 at 0.25 and 0.3, 2^-20 at
				 * 0.0625 and 0.1; a fixed unit fits far worse) */
				int e;

				frexp(z > 0 ? z : 0.5, &e);
				z += su->bias * ldexp(1, e - 16);
			}
			if (!depth_pass(zfunc, z, zb[i]))
				continue;
			for (k = 0; k < 4; k++) {
				if (gouraud) {
					c[k] = colour_at(su, su->cpl[0][k], px, py) / 255.0f;
					off[k] = (k == 3 ? 255 - colour_at(su, su->cpl[1][k], px, py) : colour_at(su, su->cpl[1][k], px, py)) / 255.0f;
				} else {
					c[k] = p->fcol[k];
					off[k] = p->foff[k];
				}
			}
			/* the texture units: unit 0's previous is (.5, .5, .5, 1) */
			prev[0] = prev[1] = prev[2] = 0.5f;
			prev[3] = 1;
			if (su->nunits) {
				float prim[4];

				memcpy(prim, c, sizeof(prim));
				for (u = 0; u < su->nunits; u++) {
					float uv[2], t[4], lod = 0, o[4];

					tc_at(su, px, py, u, uv);
					if (su->tu[u].mip)
						lod = lod_at(su, px, py, u, uv);
					struct texunit tu = su->tu[u];

					tu.half = rc->samp & 4 ? 0.5f : 0;	/* 0x61c bit 2 */
					tex_sample(e, &tu, uv[0], uv[1], lod, t);
					tex_combine(su->tu[u].c, su->tu[u].dot3, prev, prim, su->konst, t, o);
					memcpy(prev, o, sizeof(prev));
				}
				memcpy(c, prev, sizeof(c));
			}
			if (offset)	/* added in 8 bits: 0.25 + 0.5 is 64 + 128 */
				for (k = 0; k < 3; k++) {
					int v = q8(c[k]) + q8(off[k]);

					c[k] = (v > 255 ? 255 : v) / 255.0f;
				}
			if (s & ST_FOG) {
				/* f is o2.w, zeroed with the offset colour by the primitive word */
				float f = (p->pw & PRIM_ZEROOFFSET) ? 0 : clamp01(off[3]);

				/* in fixed point, the factor in 256ths one more than its 8
				 * bits: f 0.25 over red fog gives (190, 64, 0) on the N810
				 * (README assumption 24) */
				int fq = q8(f) + 1;

				if (fq > 256)
					fq = 256;
				for (k = 0; k < 3; k++)
					c[k] = ((q8(c[k]) * fq + q8(rc->fog[k]) * (256 - fq)) >> 8) / 255.0f;
			}
			/* bits 13 and 14 are the object type: 0 opaque, 0x2000
			 * translucent, 0x4000 punch-through; both set draws nothing
			 * on the N810 */
			if ((s & (ST_BLEND | ST_ALPHATEST)) == (ST_BLEND | ST_ALPHATEST)) {
				tile_claim[i] = 1;
				if (!su->nunits && su == &rc->su[tile_lastk] && (y & 15) == 15) {
					int er = (x & 15) >= 8;

					/* a covered pixel of a left-half group (x % 16 < 8) goes back
					 * when pixel x0 + 8 (the group's right neighbour) is not
					 * covered, i.e. when the row's run ends inside the group or
					 * came from the tile on the left and ends here; a run that
					 * goes on into the right half leaves the left group alone */
					if (!er)
						er = !px_cov(su, rc, (x & ~7) + 8, y);
					if (er)
						memcpy(col[i], tile_col0[i], sizeof(col[i]));
				}
				continue;
			}
			if ((s & ST_ALPHATEST) &&
			    !alpha_pass(st->at >> 29, (int)q(c[3], 255), (int)(st->at >> 21 & 0xff)))
				continue;
			if (!(s & ST_NODEPTHWRITE))
				zb[i] = z;
			/* bit 8 writes no colour, but the depth still (unless bit
			 * 7): software sends a full colour mask as bit 8 alone
			 * and a never-passing alpha test as bits 8 and 7 (the
			 * masked quad hides what is behind it) */
			if (s & ST_ALPHANEVER)
				continue;
			/* a punch-through object blends by its factors too (alpha test
			 * with blending is sent as 0x4000 and the
			 * factors): (ONE, ZERO) is no blend */
			if ((s & ST_BLEND) || ((s & ST_ALPHATEST) && (s >> 26) != 8)) {
				int sc = s >> 29, dc = s >> 26 & 7, sat = (s & ST_SATURATE) && !sc;
				int s8[4], d8[4], v;

				for (k = 0; k < 4; k++) {
					s8[k] = q8(c[k]);
					d8[k] = q8(col[i][k]);
				}
				/* each product truncated on its own, then added
				 * (SRC_ALPHA, ONE_MINUS_SRC_ALPHA over random colours needs one
				 * sum shifted; the other functions have a whole product and come
				 * out the same either way) */
				for (k = 0; k < 4; k++) {
					v = (s8[k] * factor(sc, 0, sat, k, s8, d8) >> 8) +
					    (d8[k] * factor(dc, 1, 0, k, s8, d8) >> 8);
					col[i][k] = (v > 255 ? 255 : v) / 255.0f;
				}
			} else {
				for (k = 0; k < 4; k++)
					col[i][k] = clamp01(c[k]);
			}
		}
}

/* --- tiles --- */

static void tile(struct mbxemu *e, struct rctx *rc, uint32_t entry)
{
	float col[TILE * TILE][4], zb[TILE * TILE];
	uint32_t tx = entry & 0xff, ty = entry >> 8 & 0xff, x0 = tx * TILE, y0 = ty * TILE;
	uint32_t i, j, n;
	int k;

	e->stats.tiles++;
	if (x0 >= rc->w || y0 >= rc->h)
		return;		/* padding past the screen */
	/* the background object: the target as a linear texture, texel for pixel */
	for (j = 0; j < TILE; j++) {
		uint8_t row[TILE * 4];

		if (rc->bg_target) {
			dev_read(e, rc->addr + (y0 + j) * rc->stride + x0 * (rc->fmt == 9 ? 2 : 4), row,
				 TILE * (rc->fmt == 9 ? 2 : 4));
			for (i = 0; i < TILE; i++)
				unpack(rc->fmt, row + i * (rc->fmt == 9 ? 2 : 4), col[j * TILE + i]);
		} else {
			for (i = 0; i < TILE; i++)
				tex_fetch(e, &rc->bg, 0, (int)(x0 + i), (int)(y0 + j), col[j * TILE + i]);
		}
	}
	/* depth: loaded from the Z buffer when 0x610 asks and the entry has bit
	 * 29; otherwise the clear value */
	if (rc->zload_ok && (entry & 0x20000000)) {
		for (j = 0; j < TILE; j++) {
			uint32_t row[TILE];

			dev_read(e, rc->zaddr + ((y0 + j) * rc->zrow + x0) * 4, row, sizeof(row));
			for (i = 0; i < TILE; i++)
				zb[j * TILE + i] = u2f(row[i]);
		}
	} else {
		for (i = 0; i < TILE * TILE; i++)
			zb[i] = rc->zclear;
	}
	memset(tile_claim, 0, sizeof(tile_claim));
	memcpy(tile_col0, col, sizeof(tile_col0));
	tile_lastk = 0;
	if (rc->s)
		for (k = 0; k < rc->s->nprims; k++) {
			const struct setup *su = &rc->su[k];

			if (!(su->skip || su->p->x1 <= (int)x0 || su->p->x0 >= (int)(x0 + TILE) ||
			      su->p->y1 <= (int)y0 || su->p->y0 >= (int)(y0 + TILE)))
				tile_lastk = k;
		}
	if (rc->s)
		for (k = 0; k < rc->s->nprims; k++) {
			const struct setup *su = &rc->su[k];

			if (su->skip || su->p->x1 <= (int)x0 || su->p->x0 >= (int)(x0 + TILE) ||
			    su->p->y1 <= (int)y0 || su->p->y0 >= (int)(y0 + TILE))
				continue;
			draw(e, rc, su, (int)x0, (int)y0, col, zb);
		}
	if (rc->zstore)
		for (j = 0; j < TILE; j++) {
			uint32_t row[TILE];

			for (i = 0; i < TILE; i++)
				row[i] = f2u(zb[j * TILE + i]) | 1;	/* stored with the low bit set */
			dev_write(e, rc->zaddr + ((y0 + j) * rc->zrow + x0) * 4, row, sizeof(row));
		}
	n = rc->w - x0 < TILE ? rc->w - x0 : TILE;
	for (j = 0; j < TILE && y0 + j < rc->h; j++) {
		uint8_t row[TILE * 4];
		int bpp = rc->fmt == 9 ? 2 : 4;

		for (i = 0; i < n; i++)
			pack(rc->fmt, row + i * bpp, col[j * TILE + i]);
		dev_write(e, rc->addr + (y0 + j) * rc->stride + x0 * bpp, row, n * bpp);
	}
}

/*
 * The kick. The scene is the one in the hardware context 0x6ec names;
 * none (no TA since) draws only the background. A complete scene is
 * consumed by its render; a closed one (a partial render) loses what it
 * held and the TA goes on with it. A region array with no end mark would
 * run the core into the weeds: no completion, for the daemon's watchdog.
 */
void render_kick(struct mbxemu *e)
{
	const uint32_t *R = e->regs;
	struct rctx rc;
	uint32_t k, entry = 0, regions = R[R_REGIONS / 4], pb = R[R_PB_BASE / 4];
	int p;

	if (e->mode == TA_HUNG) {
		/* the render waits for a TA that takes nothing: no completion (README assumption 30) */
		emu_log(e, "render: the TA is hung (a sections word without bit 17); the render does not complete");
		return;
	}
	so_render(e);	/* a render flips stream-out's side (ta.c) */
	memset(&rc, 0, sizeof(rc));
	rc.ctl = R[R_RENDER_CTL / 4];
	rc.samp = R[R_SAMPLING / 4];
	rc.fmt = R[R_FORMAT / 4];
	rc.addr = R[R_ADDR / 4];
	rc.stride = R[R_STRIDE / 4] * 4;
	rc.w = (R[R_WIDTH / 4] >> 16 & 0x7ff) + 1;
	rc.h = (R[R_HEIGHT / 4] >> 16 & 0x7ff) + 1;
	rc.zaddr = R[R_ZBUF / 4] & ~0xffu;
	rc.zrow = ((rc.ctl >> 8 & 0xff) + 1) * TILE;
	rc.zload_ok = (rc.ctl & 2) && !(rc.ctl & 0x30000) && rc.zaddr;
	/* depth is stored only by a partial render (0x85c bit 3) */
	rc.zstore = (rc.ctl & 1) && !(rc.ctl & 0x30000) && rc.zaddr &&
		    (R[R_TA_CONFIG / 4] & TAC_PARTIAL);
	rc.zclear = R[R_6DC / 4] ? u2f(R[R_6DC / 4] & ~1u) : 1.0f;
	rc.fog[0] = (R[R_FOG_COLOUR / 4] >> 16 & 255) / 255.0f;
	rc.fog[1] = (R[R_FOG_COLOUR / 4] >> 8 & 255) / 255.0f;
	rc.fog[2] = (R[R_FOG_COLOUR / 4] & 255) / 255.0f;
	if (rc.fmt != 9 && rc.fmt != 0xe) {
		emu_log(e, "render: colour format %#x isn't known; drawn as ARGB8888", rc.fmt);
		rc.fmt = 0xe;
	}
	e->stats.renders++;
	pcache_begin(e);

	/* the background object: words 1 and 2 are texture words A and B */
	{
		uint32_t w[3] = { dev_read32(e, pb + 4), dev_read32(e, pb + 8), 0 };

		/* none set up (no linear RGB565 or ARGB8888 texture there): the
		 * target's own pixels, which is what software points it at */
		tex_decode(&rc.bg, w);
		rc.bg_target = !rc.bg.stride || (rc.bg.fmt != 1 && rc.bg.fmt != 6);
	}

	rc.s = &e->ctx[R[R_HW_CONTEXT / 4] & 1];
	if (!rc.s->used)
		rc.s = NULL;
	if (rc.s && rc.s->nprims) {
		rc.su = calloc(rc.s->nprims, sizeof(*rc.su));
		if (!rc.su)
			rc.s = NULL;
		else
			for (p = 0; p < rc.s->nprims; p++)
				setup_prim(&rc.su[p], &rc.s->prims[p], &rc.s->states[rc.s->prims[p].state]);
	}
	emu_trace(e, "render: %ux%u %s at %#x (stride %u), regions %#x, context %u, %d triangles%s%s",
		  rc.w, rc.h, rc.fmt == 9 ? "RGB565" : "ARGB8888", rc.addr, rc.stride, regions,
		  R[R_HW_CONTEXT / 4] & 1, rc.s ? rc.s->nprims : 0, rc.zload_ok ? ", Z load" : "",
		  rc.zstore ? ", Z store" : "");

	for (k = 0; k < MAX_REGIONS; k++) {
		entry = dev_read32(e, regions + 8 * k);
		tile(e, &rc, entry);
		if (entry & 0x80000000)
			break;
	}
	free(rc.su);
	pcache_end(e);
	e->isp_sig++;
	if (!(entry & 0x80000000)) {
		emu_log(e, "render: no end mark in the region array at %#x: the 3D core hangs", regions);
		return;
	}
	if (rc.s) {
		if (rc.s->complete)
			scene_free(rc.s);
		else
			scene_clear_prims(rc.s);
		if (rc.s == ta_scene(e) || !rc.s->used)
			e->scene_state = -1;
	}
	emu_raise(e, I_RENDER | I_EVM_DALLOC);
}
