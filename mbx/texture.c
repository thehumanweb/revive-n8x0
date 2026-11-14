// SPDX-License-Identifier: MIT
/*
 * mbxemu: the TSP's textures and texture combiners.
 *
 * Word A: 31 clamp s, 29 clamp t, 24-27 log2(width) - 3, 20-23 log2(height)
 *   - 3, 17 PVRTC, 14-16 the texel format, 13 DOT3, 0 a linear stride's bit 0
 * Word B: 31 bilinear, 30 trilinear, 29 mipmapped (nearest level), 18-24 a
 *   linear stride's bits 1-7 (in eights), 0-17 the address / 128
 * Word C: the combiner, colour in 15-31 and alpha in 0-14 (tex_combine())
 */
#include <math.h>
#include <string.h>

#include "emu.h"

/* Twiddled index of texel (u, v) of a 2^lw x 2^lh texture: v's bits even,
 * u's odd, the longer side's extra bits on top */
uint32_t twiddle(uint32_t u, uint32_t v, int lw, int lh)
{
	int lmin = lw < lh ? lw : lh, b;
	uint32_t i = 0;

	for (b = 0; b < lmin; b++)
		i |= (v >> b & 1) << (2 * b) | (u >> b & 1) << (2 * b + 1);
	return i | (lw > lh ? u >> lmin : v >> lmin) << (2 * lmin);
}

void tex_decode(struct texunit *t, const uint32_t w[3])
{
	memset(t, 0, sizeof(*t));
	t->a = w[0];
	t->b = w[1];
	t->c = w[2];
	t->lw = (int)(t->a >> 24 & 0xf) + 3;
	t->lh = (int)(t->a >> 20 & 0xf) + 3;
	if (t->lw > 11)
		t->lw = 11;
	if (t->lh > 11)
		t->lh = 11;
	t->fmt = t->a >> 14 & 7;
	t->clamp_s = t->a >> 31 & 1;
	t->clamp_t = t->a >> 29 & 1;
	t->flip_s = t->a >> 30 & 1;	/* mirrored repeat (found during device testing: the unit's probe of the unexplored bits) */
	t->flip_t = t->a >> 28 & 1;
	t->dot3 = t->a >> 13 & 1;
	t->maxlev = (int)(t->a >> 9 & 0xf);
	t->pvrtc = (int)(t->a >> 17 & 1);
	/* a stride: rows read linearly (render to texture); none: twiddled */
	t->stride = ((t->b >> 18 & 0x7f) << 1 | (t->a & 1)) * 8;
	/* a linear texture with bit 29 (0x2e000000) is read with its
	 * rows the width apart, whatever the stride says (TEXB_LINEAR) */
	if (t->stride && (t->b >> 29 & 1))
		t->stride = 1u << t->lw;
	t->addr = (t->b & 0x3ffff) << 7;
	t->bilinear = t->b >> 31 & 1;
	t->half = 0.5f;
	t->trilinear = !t->stride && (t->b >> 30 & 1);
	t->mip = !t->stride && (t->trilinear || (t->b >> 29 & 1));
}

static int texel_bytes(uint32_t fmt)
{
	return fmt == 5 ? 1 : fmt == 6 ? 4 : 2;
}

/* Where level l of a mipmapped texture starts, in texels: the chain is
 * stored largest first with no padding */
/* PVRTC: 4 x 4 texel blocks at 4 bpp, 8 x 4 at 2 bpp, 8 bytes each, at
 * least 2 x 2 blocks a level, twiddled by block */
static int pvrtc_bw(const struct texunit *t) { return t->fmt == 6 ? 4 : 8; }

static uint32_t pvrtc_level_bytes(const struct texunit *t, int level)
{
	int w = 1 << (t->lw - level > 0 ? t->lw - level : 0), h = 1 << (t->lh - level > 0 ? t->lh - level : 0);
	int bx = w / pvrtc_bw(t), by = h / 4;

	return (uint32_t)(bx < 2 ? 2 : bx) * (uint32_t)(by < 2 ? 2 : by) * 8;
}

static uint32_t level_offset(const struct texunit *t, int level)
{
	uint32_t off = 0;
	int l;

	for (l = 0; l < level; l++) {
		int w = t->lw - l, h = t->lh - l;

		off += (1u << (w > 0 ? w : 0)) * (1u << (h > 0 ? h : 0));
	}
	return off;
}

/* 5 and 6 bits to a byte by replicating the top bits (v << 3 | v >> 2),
 * which matches the N810 better than scaling by 255/31 and rounding */
/* Colour fields are shifted up, not replicated: found during device testing, a white RGB565 texel reads
 * (248, 252, 248) through the replace combiner, a white ARGB4444 one 240, a white ARGB1555 one 248. (The
 * earlier choice of replicating the top bits compared two guesses, not the shift.) A 4-bit alpha is scaled. */
static float x5(uint32_t v) { v &= 31; return (float)(v << 3) / 255.0f; }
static float x6(uint32_t v) { v &= 63; return (float)(v << 2) / 255.0f; }
static float x4(uint32_t v) { return (v & 15) / 15.0f; }
static float x4c(uint32_t v) { return (float)((v & 15) << 4) / 255.0f; }
static float x8(uint32_t v) { return (v & 255) / 255.0f; }

/*
 * One texel, RGBA. Formats: 0 ARGB1555, 1 RGB565, 2 ARGB4444, 4 LA88 (L in
 * the low byte), 5 L8 or A8 (replicated into every channel; word C picks),
 * 6 ARGB8888. 3 and 7 aren't known (PVRTC perhaps): magenta.
 */
/*
 * A PVRTC texel, as the N810 decodes it: Imagination's
 * published decompressor (the PowerVR SDK's PVRTDecompress) with colour
 * B's blue expanded from 4 to 5 bits in translucent blocks, which the
 * SDK's old copy applies to colour A instead. Each texel blends the A and
 * B colours of the 2 x 2 blocks around it (5554, bilinearly in 4ths or
 * 8ths, the texture wrapping) by its modulation: 2 bits at 4 bpp (0, 3/8,
 * 5/8, 1, or 0, 1/2, 1/2 with punch-through, 1); at 2 bpp a bit a texel,
 * or stored 2-bit values on a checkerboard and the others the mean of
 * their four neighbours.
 */
static void pvrtc_block(struct mbxemu *e, const struct texunit *t, uint32_t base, int bxn, int byn,
			int bx, int by, uint32_t blk[2])
{
	int lbx = 0, lby = 0;

	while ((1 << lbx) < bxn)
		lbx++;
	while ((1 << lby) < byn)
		lby++;
	dev_read(e, base + twiddle((uint32_t)bx, (uint32_t)by, lbx, lby) * 8, blk, 8);
	(void)t;
}

static void pvrtc_colours(const uint32_t blk[2], int ab[2][4])
{
	uint32_t raw[2] = { blk[1] & 0xfffe, blk[1] >> 16 };
	int i;

	for (i = 0; i < 2; i++) {
		if (raw[i] & 0x8000) {	/* opaque: R5 G5 B5 (A's blue 4 bits) */
			ab[i][0] = raw[i] >> 10 & 31;
			ab[i][1] = raw[i] >> 5 & 31;
			ab[i][2] = raw[i] & 31;
			if (i == 0)
				ab[i][2] |= ab[i][2] >> 4;
			ab[i][3] = 15;
		} else {		/* translucent: A3 R4 G4 B4 (A's blue 3 bits) */
			ab[i][0] = raw[i] >> 7 & 0x1e;
			ab[i][1] = raw[i] >> 3 & 0x1e;
			ab[i][0] |= ab[i][0] >> 4;
			ab[i][1] |= ab[i][1] >> 4;
			ab[i][2] = (raw[i] & 15) << 1;
			ab[i][2] |= ab[i][2] >> (i == 0 ? 3 : 4);
			ab[i][3] = raw[i] >> 11 & 0xe;
		}
	}
}

/* the modulation value (2 bits, or 1 doubled) at texel (lx, ly) of a 2 x 2
 * block neighbourhood, and the block's mode */
static int pvrtc_modval(const uint32_t blk[2][2][2], int bw, int lx, int ly, int *mode)
{
	const uint32_t *b = blk[ly / 4][lx / bw];
	int x = lx % bw, y = ly % 4, k, n = 0;

	*mode = (int)(b[1] & 1);
	if (bw == 4)
		return (int)(b[0] >> (2 * (y * 4 + x)) & 3);
	if (!*mode)
		return b[0] >> (y * 8 + x) & 1 ? 3 : 0;
	for (k = 0; k < y * 8 + x; k++)	/* the stored values before this texel */
		if (((k % 8) ^ (k / 8)) % 2 == 0)
			n++;
	return (int)(b[0] >> (2 * n) & 3);
}

static void pvrtc_texel(struct mbxemu *e, const struct texunit *t, int level, int x, int y, float out[4])
{
	static const int rep0[4] = { 0, 3, 5, 8 }, rep1[4] = { 0, 4, 4, 8 };
	int bw = pvrtc_bw(t), w = 1 << (t->lw - level > 0 ? t->lw - level : 0), h = 1 << (t->lh - level > 0 ? t->lh - level : 0);
	int bxn = w / bw < 2 ? 2 : w / bw, byn = h / 4 < 2 ? 2 : h / 4;
	int bx = ((x - bw / 2) & (w - 1)) / bw, by = ((y - 2) & (h - 1)) / 4;
	uint32_t base = t->addr, blk[2][2][2];
	int col[2][2][2][4], i, j, k, c, lx, ly, u, v, mode, val, mod, pt = 0;

	for (k = 0; k < level; k++)
		base += pvrtc_level_bytes(t, k);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++) {
			pvrtc_block(e, t, base, bxn, byn, (bx + j) % bxn, (by + i) % byn, blk[i][j]);
			pvrtc_colours(blk[i][j], col[i][j]);
		}
	/* the texel's place in the neighbourhood, and its offset from the
	 * blocks' centres */
	ly = (y & 3) | ((~y & 2) << 1);
	lx = bw == 4 ? (x & 3) | ((~x & 2) << 1) : (x & 7) | ((~x & 4) << 1);
	v = ly - 2;
	u = lx - bw / 2;
	for (k = 0; k < 2; k++) {	/* A and B */
		int r[4];

		for (c = 0; c < 4; c++) {
			int t1 = col[0][0][k][c] * bw + u * (col[0][1][k][c] - col[0][0][k][c]);
			int t2 = col[1][0][k][c] * bw + u * (col[1][1][k][c] - col[1][0][k][c]);

			r[c] = t1 * 4 + v * (t2 - t1);
			if (bw == 8)
				r[c] >>= c < 3 ? 2 : 1;
			else if (c < 3)
				r[c] >>= 1;
			r[c] += r[c] >> (c < 3 ? 5 : 4);	/* 5.3 or 4.4 to 8 bits */
		}
		for (c = 0; c < 4; c++)
			col[0][0][k][c] = r[c];
	}
	val = pvrtc_modval((const uint32_t (*)[2][2])blk, bw, lx, ly, &mode);
	if (!mode)
		mod = rep0[val];
	else if (bw == 8) {
		if (((lx ^ ly) & 1) == 0)
			mod = rep0[val];
		else {
			int m2, a = pvrtc_modval((const uint32_t (*)[2][2])blk, bw, lx, ly - 1, &m2);
			int b2 = pvrtc_modval((const uint32_t (*)[2][2])blk, bw, lx, ly + 1, &m2);
			int c2 = pvrtc_modval((const uint32_t (*)[2][2])blk, bw, lx - 1, ly, &m2);
			int d2 = pvrtc_modval((const uint32_t (*)[2][2])blk, bw, lx + 1, ly, &m2);

			mod = (rep0[a] + rep0[b2] + rep0[c2] + rep0[d2] + 2) / 4;
		}
	} else {
		mod = rep1[val];
		pt = val == 2;
	}
	for (c = 0; c < 4; c++) {
		int a = col[0][0][0][c], b = col[0][0][1][c];

		out[c] = (float)((a * 8 + mod * (b - a)) >> 3) / 255;
	}
	if (pt)
		out[3] = 0;
}

void tex_fetch(struct mbxemu *e, const struct texunit *t, int level, int x, int y, float out[4])
{
	int bpp = texel_bytes(t->fmt), lw = t->lw - level, lh = t->lh - level;
	uint32_t idx, v = 0;

	if (t->pvrtc) {
		pvrtc_texel(e, t, level, x, y, out);
		return;
	}
	if (lw < 0)
		lw = 0;
	if (lh < 0)
		lh = 0;
	if (t->fmt == 7 && !t->stride) {
		/* format 7: one bit a texel, set white, clear transparent black;
		 * each byte 8 texels of a row, the leftmost in bit 7, the bytes
		 * twiddled over (x / 8, y) */
		uint8_t byte = 0;
		int lb = lw - 3 < 0 ? 0 : lw - 3;

		dev_read(e, t->addr + twiddle((uint32_t)x >> 3, (uint32_t)y, lb, lh), &byte, 1);
		out[0] = out[1] = out[2] = out[3] = (float)(byte >> (7 - (x & 7)) & 1);
		return;
	}
	if (t->stride)
		idx = (uint32_t)y * t->stride + (uint32_t)x;
	else
		idx = level_offset(t, level) + twiddle((uint32_t)x, (uint32_t)y, lw, lh);
	dev_read(e, t->addr + idx * bpp, &v, bpp);
	switch (t->fmt) {
	case 0:
		out[0] = x5(v >> 10);
		out[1] = x5(v >> 5);
		out[2] = x5(v);
		out[3] = v >> 15 & 1;
		break;
	case 1:
		out[0] = x5(v >> 11);
		out[1] = x6(v >> 5);
		out[2] = x5(v);
		out[3] = 1;
		break;
	case 2:
		out[0] = x4c(v >> 8);
		out[1] = x4c(v >> 4);
		out[2] = x4c(v);
		out[3] = x4(v >> 12);
		break;
	case 3:	/* ARGB8332, the 3 and 2 bits replicated */
		out[0] = (float)(((v >> 5 & 7) << 5) | ((v >> 5 & 7) << 2) | ((v >> 5 & 7) >> 1)) / 255;
		out[1] = (float)(((v >> 2 & 7) << 5) | ((v >> 2 & 7) << 2) | ((v >> 2 & 7) >> 1)) / 255;
		out[2] = (float)((v & 3) * 85) / 255;
		out[3] = x8(v >> 8);
		break;
	case 4:
		out[0] = out[1] = out[2] = x8(v);
		out[3] = x8(v >> 8);
		break;
	case 5:
		out[0] = out[1] = out[2] = out[3] = x8(v);
		break;
	case 6:
		out[0] = x8(v >> 16);
		out[1] = x8(v >> 8);
		out[2] = x8(v);
		out[3] = x8(v >> 24);
		break;
	default:
		out[0] = out[2] = out[3] = 1;
		out[1] = 0;
	}
}

static int wrap(int x, int n, int clamp, int flip)
{
	if (clamp)
		return x < 0 ? 0 : x >= n ? n - 1 : x;
	if (flip) {		/* mirrored: the texels run 0 .. n-1, n-1 .. 0 */
		x %= 2 * n;
		if (x < 0)
			x += 2 * n;
		return x >= n ? 2 * n - 1 - x : x;
	}
	x %= n;
	return x < 0 ? x + n : x;
}

static void sample_level(struct mbxemu *e, const struct texunit *t, int level, float u, float v,
			 float out[4])
{
	int lw = t->lw - level, lh = t->lh - level, w, h, k;

	w = 1 << (lw > 0 ? lw : 0);
	h = 1 << (lh > 0 ? lh : 0);
	if (t->bilinear && !(t->fmt == 7 && !t->pvrtc)) {	/* format 7 is point sampled */
		/* weights in 32nds, truncated; the texels' bytes blended across
		 * (s) and truncated, then down (t) and truncated: a ramp
		 * between 255 and 0 goes 247, 239 ... and 7, 15 ... in steps of
		 * 8, not one sum truncated once */
		float fx = u * w - t->half, fy = v * h - t->half, c[4][4];
		int x0 = (int)floorf(fx), y0 = (int)floorf(fy), ax, ay, s;

		ax = (int)((fx - x0) * 32);
		ay = (int)((fy - y0) * 32);
		tex_fetch(e, t, level, wrap(x0, w, t->clamp_s, t->flip_s), wrap(y0, h, t->clamp_t, t->flip_t), c[0]);
		tex_fetch(e, t, level, wrap(x0 + 1, w, t->clamp_s, t->flip_s), wrap(y0, h, t->clamp_t, t->flip_t), c[1]);
		tex_fetch(e, t, level, wrap(x0, w, t->clamp_s, t->flip_s), wrap(y0 + 1, h, t->clamp_t, t->flip_t), c[2]);
		tex_fetch(e, t, level, wrap(x0 + 1, w, t->clamp_s, t->flip_s), wrap(y0 + 1, h, t->clamp_t, t->flip_t), c[3]);
		/* found during device testing: when one of the four texels has no colour at all (R, G and B zero,
		 * whatever its alpha), the chip does not filter: the sample is the first texel (u, v floored) */
		for (k = 0; k < 4; k++)
			if (c[k][0] == 0 && c[k][1] == 0 && c[k][2] == 0) {
				memcpy(out, c[0], sizeof(float) * 4);
				return;
			}
		for (k = 0; k < 4; k++) {
			int h0 = ((int)(c[0][k] * 255 + 0.5f) * (32 - ax) + (int)(c[1][k] * 255 + 0.5f) * ax) >> 5;
			int h1 = ((int)(c[2][k] * 255 + 0.5f) * (32 - ax) + (int)(c[3][k] * 255 + 0.5f) * ax) >> 5;

			s = (h0 * (32 - ay) + h1 * ay) >> 5;
			out[k] = (float)s / 255;
		}
	} else {
		tex_fetch(e, t, level, wrap((int)floorf(u * w), w, t->clamp_s, t->flip_s),
			  wrap((int)floorf(v * h), h, t->clamp_t, t->flip_t), out);
	}
}

/*
 * A sample at (u, v), coordinates 0..1 across the texture (not divided by
 * q: the TSP isn't projective). Mipmapped: the nearest level to lod, or
 * with trilinear a blend of the two around it.
 */
void tex_sample(struct mbxemu *e, const struct texunit *t, float u, float v, float lod, float out[4])
{
	/* the last level taken is word A's bits 9-12, at most the chain's
	 * (software writes 0xb; the size code of the smallest side less
	 * 3, as older software wrote it, stops a 64 x 64 texture at its 8 x 8 level) */
	int top = t->lw > t->lh ? t->lw : t->lh, l0, k;

	if (t->maxlev < top)
		top = t->maxlev;
	if (!isfinite(u))
		u = 0;
	if (!isfinite(v))
		v = 0;
	if (!t->mip || !(lod > 0)) {
		sample_level(e, t, 0, u, v, out);
		return;
	}
	if (lod > top)
		lod = (float)top;
	if (!t->trilinear) {
		sample_level(e, t, (int)(lod + 0.5f), u, v, out);
		return;
	}
	l0 = (int)lod;
	sample_level(e, t, l0, u, v, out);
	if (l0 < top && lod > l0) {
		/* the two levels' bytes blended in the level of detail's 64ths,
		 * truncated, the sum rounded */
		int kf = (int)((lod - l0) * 64);
		/* the top bin (fraction >= 63/64) counts as 64/64, the pure next
		 * level */
		if (kf == 63)
			kf = 64;
		float b[4];

		sample_level(e, t, l0 + 1, u, v, b);
		/* the near level's term is cut to a multiple of 32 before the
		 * sum, the far term exact */
		for (k = 0; k < 4; k++)
			out[k] = (float)(((((int)(out[k] * 255 + 0.5f) * (64 - kf)) >> 5 << 5) +
					  (int)(b[k] * 255 + 0.5f) * kf + 32) >> 6) / 255;
	}
}

/* --- the combiner (word C) --- */

/*
 * The combiner works in 8 bits, as the N810 does (README assumption 25): a value 0-255 (255 as
 * 256, one, so white times a texel is the texel), 1 - x as 256 - x; each product (x * y) >> 8 on its own, the two
 * added, less 128 for the half, then scaled and clamped; DOT3 sums
 * ((2a - 256) * (2b - 256)) >> 8 a channel (rounding down)
 */
static int c8(float x)
{
	return (int)((x < 0 ? 0 : x > 1 ? 1 : x) * 255 + 0.5f + 1.0f / 1024);
}

/* A slot's value: code 0-3 picks from the slot's sources (NULL: one) */
static void slot(const int *const src[4], int code, int alpha_of, int inv, int *o)
{
	const int *s = src[code];
	int k;

	for (k = 0; k < 4; k++) {
		int x = s ? s[alpha_of ? 3 : k] : 256;

		if (x == 255)
			x = 256;	/* 255 is one: white times a texel is the texel */

		o[k] = inv ? 256 - x : x;
	}
}

static int finish(int sum, int half, int scale)
{
	if (half)
		sum -= 128;
	switch (scale) {
	case 0: break;
	case 1: sum *= 2; break;
	case 2: sum *= 4; break;
	default: sum = 256 - sum; break;	/* 3: 1 - the sum */
	}
	return sum < 0 ? 0 : sum > 255 ? 255 : sum;
}

static int prod(int x, int y)
{
	int p = (x * y) >> 8;	/* both >= 0 here */

	return p > 255 ? 255 : p;
}

/* (x * y) >> 8, rounding down for negative products too */
static int sprod(int x, int y)
{
	int p = x * y;

	return p >= 0 ? p >> 8 : -((-p + 255) >> 8);
}

/*
 * Word C: colour (bits 15-31) and alpha (0-14) each A*B + C*D, less a half
 * (17, 2), scaled (15-16, 0-1: x1, x2, x4, 1 - sum).
 * Colour slots: A constant/previous/texture/one, B primary/previous/
 * texture/constant (29: its alpha), C primary/previous/texture/one, D as B
 * (24: its alpha), complements in 21-18. Alpha slots: A primary/previous/
 * texture/one, B as the colour's, C as A, D as B, complements in 6-3.
 * With DOT3 (word A bit 13) the colour is the dot product of (A - .5) and
 * (B - .5), D's code picking where it goes.
 */
void tex_combine(uint32_t c, int dot3, const float prevf[4], const float primf[4],
		 const float konstf[4], const float texelf[4], float out[4])
{
	int prev[4], prim[4], konst[4], texel[4], A[4], B[4], C[4], D[4], col[3], a;
	const int *ca[4] = { konst, prev, texel, NULL };	/* NULL: one */
	const int *cb[4] = { prim, prev, texel, konst };
	const int *cc[4] = { prim, prev, texel, NULL };
	const int *aa[4] = { prim, prev, texel, NULL };
	int k, dcode = c >> 22 & 3;

	for (k = 0; k < 4; k++) {
		prev[k] = c8(prevf[k]);
		prim[k] = c8(primf[k]);
		konst[k] = c8(konstf[k]);
		texel[k] = c8(texelf[k]);
	}
	slot(ca, c >> 30 & 3, 0, c >> 21 & 1, A);
	slot(cb, c >> 27 & 3, c >> 29 & 1, c >> 20 & 1, B);
	slot(cc, c >> 25 & 3, 0, c >> 19 & 1, C);
	slot(cb, dcode, c >> 24 & 1, c >> 18 & 1, D);
	for (k = 0; k < 3; k++)
		col[k] = finish(prod(A[k], B[k]) + prod(C[k], D[k]), c >> 17 & 1, c >> 15 & 3);

	slot(aa, c >> 13 & 3, 0, c >> 6 & 1, A);
	slot(cb, c >> 11 & 3, 0, c >> 5 & 1, B);
	slot(aa, c >> 9 & 3, 0, c >> 4 & 1, C);
	slot(cb, c >> 7 & 3, 0, c >> 3 & 1, D);
	a = finish(prod(A[3], B[3]) + prod(C[3], D[3]), c >> 2 & 1, c & 3);

	if (dot3) {
		int x[4], y[4], d = 0;

		slot(ca, c >> 30 & 3, 0, c >> 21 & 1, x);
		slot(cb, c >> 27 & 3, c >> 29 & 1, c >> 20 & 1, y);
		for (k = 0; k < 3; k++)
			d += dcode & 1 ? sprod(2 * x[k] - 256, 2 * y[k] - 256) :
					 sprod(x[k] - 128, y[k] - 128);
		d = d < 0 ? 0 : d > 255 ? 255 : d;
		for (k = 0; k < 3; k++)
			col[k] = d;
		if (dcode < 2)
			a = d;	/* 0, 1: to alpha too; 2, 3: alpha as usual */
	}
	for (k = 0; k < 3; k++)
		out[k] = col[k] / 255.0f;
	out[3] = a / 255.0f;
}
