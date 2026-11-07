/* SPDX-License-Identifier: MIT */
/*
 * The triangle frame shared by mbxtri (the emulator on the host) and
 * mbxtri-drm (the render node): the TA stream as chunks (a header word, type
 * << 28 | count, then the words; type 0 TA port, 1 TA control port, as
 * mbx/drm/mbx_drm.h) and the register values of the frame.
 */
#ifndef TRI_FRAME_H
#define TRI_FRAME_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef TRI_W
#define TRI_W		64
#endif
#ifndef TRI_H
#define TRI_H		64
#endif
#define TRI_CLEAR	0xff202020u

#define CH_TA		0
#define CH_CTL		1

struct tri_sw {
	uint32_t *base, *p, *hdr;
	unsigned int type;
};

static inline void tri_put(struct tri_sw *s, unsigned int type, uint32_t v)
{
	if (!s->hdr || s->type != type) {
		s->hdr = s->p++;
		*s->hdr = (uint32_t)type << 28;
		s->type = type;
	}
	*s->p++ = v;
	(*s->hdr)++;
}

static inline void tri_ta(struct tri_sw *s, uint32_t v) { tri_put(s, CH_TA, v); }
static inline void tri_taf(struct tri_sw *s, float f) { uint32_t u; memcpy(&u, &f, 4); tri_ta(s, u); }

/* The ISP/TSP state word of the triangle's object (Gouraud, depth test LESS); mbxtri-drm -S overrides it */
static uint32_t tri_state_word = 1u << 29 | 1u << 25 | 1u << 22 | 1u << 9;	/* source ONE, destination ZERO, bit 25 with the alpha function ALWAYS: the colour is exact */
static uint32_t tri_clear_state = 0xe00 | 0x100;	/* the depth-only clear's state word (mbxtri-drm -K overrides it) */
static uint32_t tri_alpha_word = 0xe0000000, tri_clear_alpha = 0xe0000000;	/* alpha function ALWAYS; 0 is NEVER */	/* the alpha-test words (mbxtri-drm -A, -B) */
/* a textured triangle (mbxtri-drm -N mode): unit 0's words A, B, C, and the state word's bits 16-18 */
static int tri_tex;
static uint32_t tri_tex_a, tri_tex_b, tri_tex_c = 0xd6887610, tri_tex_mode = 2;
static int tri_tex2;				/* -y: a second texture unit with these words (A, B2, C2) */
static uint32_t tri_tex2_b, tri_tex2_c = 0xd6887610;
/* a twiddled, mipmapped test texture (-U log2 size): each level one flat colour */
static float tri_uvscale = 1.0f;
static inline uint32_t tri_twiddle(uint32_t u, uint32_t v, int lw, int lh)
{
	int lmin = lw < lh ? lw : lh, b;
	uint32_t i = 0;

	for (b = 0; b < lmin; b++)
		i |= (v >> b & 1) << (2 * b) | (u >> b & 1) << (2 * b + 1);
	return i | (lw > lh ? u >> lmin : v >> lmin) << (2 * lmin);
}
/* writes the chain (largest level first, no padding) of 2^lw square ARGB8888 levels, level l in colour col[l]; returns the bytes */
static inline uint32_t tri_fill_mips(uint32_t *mem, int lw)
{
	static const uint32_t col[12] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xff00ffff, 0xffff00ff,
					  0xffffffff, 0xff808080, 0xff804000, 0xff008040, 0xff400080, 0xff404040 };
	uint32_t off = 0;
	int l;

	for (l = 0; l <= lw; l++) {
		int w = lw - l, n = 1 << (w > 0 ? w : 0);
		int x, y;

		for (y = 0; y < n; y++)
			for (x = 0; x < n; x++)
				mem[off + tri_twiddle(x, y, w, w)] = col[l];
		off += n * n;
	}
	return off * 4;
}
static int tri_with_clear = 1;		/* 0: leave out the depth-only clear (tests) */
static int tri_with_triangle = 1;	/* 0: leave out the triangle (experiments) */

/* VGP instruction encoding (mbx/vgp.c's header comment) */
enum { VOP_MOV = 1, VOP_DP4 = 8 };
enum { VF_IN = 0, VF_CONST = 1, VF_TEMP = 2 };
#define VOUT(n)		(n)
#define VSWZ(x, y, z, w) ((x) | (y) << 2 | (z) << 4 | (w) << 6)
#define VXYZW		VSWZ(0, 1, 2, 3)

struct tri_src { int file, idx, swz; };

static inline void tri_enc(uint32_t w[2], int op, int dst, int mask, const struct tri_src *s, int n)
{
	uint32_t hi = op, lo = (uint32_t)dst << 26 | (uint32_t)mask << 22;
	int k;

	for (k = 0; k < 3; k++) {
		int swz = k < n ? s[k].swz : VXYZW;

		hi |= (uint32_t)swz << (8 + 8 * k);
		if (k < n)
			lo |= (uint32_t)s[k].file << (16 + 2 * k) | (uint32_t)(s[k].idx & 15) << (4 * k);
	}
	w[0] = hi;
	w[1] = lo;
}

/*
 * One frame's stream: object header, a VGP program (section bit 2: o0 = MVP
 * * v0, o1 = v1.zyxw, since o1 is BGRA), its constants (a rotation of deg
 * degrees about z), the triangle and the end word. Returns the words
 * written to out (at most 128).
 */
/* The word after the primitive: the section to run (bit 2) and bit 17, which the chip needs */
#define TRI_SECTIONS	(0xf0020000u | 0x4)

static inline uint32_t tri_stream_sections(uint32_t *out, float deg, uint32_t sections)
{
	struct tri_sw s = { .base = out, .p = out };
	static const float v[3][3] = { { 0, 0.8f, 0.5f }, { -0.8f, -0.6f, 0.5f }, { 0.8f, -0.6f, 0.5f } };
	static const uint32_t rgba[3] = { 0xff0000ff, 0xff00ff00, 0xffff0000 };	/* R G B A in memory order */
	struct tri_src p0[2] = { { VF_IN, 0, VXYZW }, { VF_CONST, 0, VXYZW } };
	struct tri_src col[1] = { { VF_IN, 1, VSWZ(2, 1, 0, 3) } };
	uint32_t code[7][2];
	float c = cosf(deg * (float)M_PI / 180), sn = sinf(deg * (float)M_PI / 180);
	float m[16] = { c, -sn, 0, 0, sn, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	int i;

	/*
	 * What a frame starts with (found during device testing): the region clip control, then a
	 * depth-only clear to the far plane, without which the tile's depth reads as NaN and most depth
	 * tests fail. The clear is a screen-space object (sections word 0xb00b0800: positions in pixels,
	 * z as sent): one triangle twice the target at z = 1.0, depth test always and written, no colour
	 * (state bit 8).
	 */
	tri_ta(&s, 0x10000010);			/* an object header naming the one-word region clip control */
	tri_ta(&s, 0);
	if (tri_with_clear) {
		/* section bit 11, the screen-space passthrough the clear names: o0 = v0, at the last
		 * instruction slot (255), clear of any driver program */
		struct tri_src pass[1] = { { VF_IN, 0, VXYZW } };
		uint32_t pc[2];

		tri_enc(pc, VOP_MOV, VOUT(0), 0xf, pass, 1);
		tri_ta(&s, 0x80000000 | 0x800);
		tri_ta(&s, 0u << 16 | 255u << 8 | 255);
		tri_ta(&s, 0x90000000 | 255u << 16 | 255);
		tri_ta(&s, pc[0]);
		tri_ta(&s, pc[1]);
		tri_ta(&s, 0x10000000 | 0x1 | 0x2 | 0x20);	/* state, alpha test, layout */
		tri_ta(&s, tri_clear_state);		/* depth ALWAYS, written; no colour */
		tri_ta(&s, tri_clear_alpha);		/* alpha test word */
		tri_ta(&s, 0x3);			/* vertices: three floats */
		tri_ta(&s, 0x70000000);			/* a triangle list */
		tri_ta(&s, 0xb00b0800);			/* screen space */
		tri_taf(&s, 0); tri_taf(&s, 0); tri_taf(&s, 1.0f);
		tri_taf(&s, 2.0f * TRI_W); tri_taf(&s, 0); tri_taf(&s, 1.0f);
		tri_taf(&s, 0); tri_taf(&s, 2.0f * TRI_H); tri_taf(&s, 1.0f);
		tri_put(&s, CH_CTL, 1);			/* the object ends */
	}

	if (tri_with_triangle) {
		/* the object header: state, alpha test, layout, viewport, depth */
		tri_ta(&s, 0x10000000 | 0x1 | 0x2 | (tri_tex ? 0x4 : 0) | (tri_tex2 ? 0x8 : 0) | 0x20 | 0x80 | 0x100);
		tri_ta(&s, tri_state_word | (tri_tex ? tri_tex_mode << 16 : 0));	/* Gouraud, depth test LESS (by default) */
		tri_ta(&s, tri_alpha_word);		/* alpha test word */
		if (tri_tex) {
			tri_ta(&s, tri_tex_a);
			tri_ta(&s, tri_tex_b);
			tri_ta(&s, tri_tex_c);
		}
		if (tri_tex2) {
			tri_ta(&s, tri_tex_a);
			tri_ta(&s, tri_tex2_b);
			tri_ta(&s, tri_tex2_c);
		}
		tri_ta(&s, tri_tex ? 0x253 : 0x53);	/* v0: three floats, v1: four bytes packed, v2: two floats (the texture coordinate) */
		tri_taf(&s, TRI_W / 2.0f); tri_taf(&s, TRI_W / 2.0f);
		tri_taf(&s, TRI_H / 2.0f); tri_taf(&s, -TRI_H / 2.0f);	/* x scale, x off, y off, y scale */
		tri_taf(&s, 0.5f); tri_taf(&s, 0.5f); tri_taf(&s, 1e-5f);	/* depth: z * 0.5 + 0.5 */

		for (i = 0; i < 4; i++) {
			p0[1].idx = i;
			tri_enc(code[i], VOP_DP4, VOUT(0), 1 << i, p0, 2);
		}
		tri_enc(code[4], VOP_MOV, VOUT(1), 0xf, col, 1);
		{
			struct tri_src uv[1] = { { VF_IN, 2, VXYZW } };

			tri_enc(code[5], VOP_MOV, VOUT(3), 0xf, uv, 1);	/* o3 = v2: texture coordinate s, t */
			tri_enc(code[6], VOP_MOV, VOUT(4), 0xf, uv, 1);	/* o4 = v2: the second unit's */
		}
		tri_ta(&s, 0x80000000 | 0x4);		/* a section: bit 2 */
		tri_ta(&s, 0u << 16 | (tri_tex2 ? 6u : tri_tex ? 5u : 4u) << 8 | 0);	/* flags 0 (once, window 0), instructions 0..4 (5, 6) */
		tri_ta(&s, 0x90000000 | 0u << 16 | (tri_tex2 ? 6 : tri_tex ? 5 : 4));
		for (i = 0; i < (tri_tex2 ? 7 : tri_tex ? 6 : 5); i++) {
			tri_ta(&s, code[i][0]);
			tri_ta(&s, code[i][1]);
		}
		tri_ta(&s, 0xa0000000 | 0u << 16 | 3);	/* c0-c3: the matrix rows, as DP4 takes them */
		for (i = 0; i < 16; i++)
			tri_taf(&s, m[i]);

		tri_ta(&s, 0x70000000);			/* a triangle list, no culling */
		tri_ta(&s, sections);			/* run section bit 2; bit 17 is needed on the chip (README 30) */
		for (i = 0; i < 3; i++) {
			tri_taf(&s, v[i][0]); tri_taf(&s, v[i][1]); tri_taf(&s, v[i][2]);
			tri_ta(&s, rgba[i]);
			if (tri_tex) {		/* s, t: the vertex's place in the target (x, y over 0..1) */
				tri_taf(&s, (v[i][0] * 0.5f + 0.5f) * tri_uvscale);
				tri_taf(&s, (v[i][1] * 0.5f + 0.5f) * tri_uvscale);
			}
		}
	tri_put(&s, CH_CTL, 1);			/* the object ends */
	}
	tri_put(&s, CH_TA, 0xf0000000);		/* the scene ends */
	return (uint32_t)(s.p - s.base);
}

static inline uint32_t tri_stream(uint32_t *out, float deg)
{
	return tri_stream_sections(out, deg, TRI_SECTIONS);
}

#endif
