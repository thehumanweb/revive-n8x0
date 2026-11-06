// SPDX-License-Identifier: MIT
/*
 * lib/mbx_vgp.h against the model's interpreter (vgp.c): every instruction
 * the encoder accepts is run through vgp_exec() and compared with a
 * reference that works from the operand description, not from the bits; so
 * a swizzle, a negate, a register file, an index or a write mask placed in
 * the wrong bit shows. The encoder's refusals and the disassembler's round
 * trip are checked too.
 *
 *   make test    (from mbx/)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../emu.h"
#include "../lib/mbx_vgp.h"

static int checks, failures;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		if (failures < 20) {					\
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);			\
			fputc('\n', stderr);				\
		}							\
	}								\
} while (0)

static uint32_t rng = 12345;

static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 8;
}

static float rndf(void)
{
	return (float)((int)(rnd() % 2001) - 1000) / 64.0f;	/* exact in float: sums and products are */
}

/* A source as the reference reads it: register contents, swizzled, negated */
static void ref_src(const struct vgp *g, const struct vgp_regs *r, const struct mbx_vgp_src *s,
		    int window, float out[4])
{
	const float *p;
	int j;

	switch (s->file) {
	case MBX_VGP_INPUT: p = r->v[s->index]; break;
	case MBX_VGP_TEMP: p = r->r[s->index]; break;
	default: p = g->k[s->index + window + (s->relative ? r->a0 : 0)]; break;
	}
	for (j = 0; j < 4; j++) {
		float x = p[s->swizzle >> (2 * j) & 3];

		out[j] = s->negate ? -x : x;
	}
}

static float dotn(const float *a, const float *b, int n)
{
	float d = 0;
	int k;

	for (k = 0; k < n; k++)
		d += a[k] * b[k];
	return d;
}

/* The reference result of the opcodes whose arithmetic is exact in float on our inputs */
static int ref_op(enum mbx_vgp_op op, float s[3][4], float d[4])
{
	int k;

	switch (op) {
	case MBX_VGP_MOV: memcpy(d, s[0], 16); return 1;
	case MBX_VGP_ADD: for (k = 0; k < 4; k++) d[k] = s[0][k] + s[1][k]; return 1;
	case MBX_VGP_MUL: for (k = 0; k < 4; k++) d[k] = s[0][k] * s[1][k]; return 1;
	case MBX_VGP_MAD: for (k = 0; k < 4; k++) d[k] = s[0][k] * s[1][k] + s[2][k]; return 1;
	case MBX_VGP_DP3: d[0] = d[1] = d[2] = d[3] = dotn(s[0], s[1], 3); return 1;
	case MBX_VGP_DP4: d[0] = d[1] = d[2] = d[3] = dotn(s[0], s[1], 4); return 1;
	case MBX_VGP_MIN: for (k = 0; k < 4; k++) d[k] = s[0][k] < s[1][k] ? s[0][k] : s[1][k]; return 1;
	case MBX_VGP_MAX: for (k = 0; k < 4; k++) d[k] = s[0][k] >= s[1][k] ? s[0][k] : s[1][k]; return 1;
	case MBX_VGP_SLT: for (k = 0; k < 4; k++) d[k] = s[0][k] < s[1][k]; return 1;
	case MBX_VGP_SGE: for (k = 0; k < 4; k++) d[k] = s[0][k] >= s[1][k]; return 1;
	case MBX_VGP_FRC: for (k = 0; k < 4; k++) d[k] = s[0][k] - floorf(s[0][k]); return 1;
	case MBX_VGP_RCP: d[0] = d[1] = d[2] = d[3] = s[0][3] == 1.0f ? 1.0f : 1.0f / s[0][3]; return 1;
	case MBX_VGP_RSQ: d[0] = d[1] = d[2] = d[3] = 1.0f / sqrtf(fabsf(s[0][3])); return 1;
	case MBX_VGP_EXP: d[0] = d[1] = d[2] = d[3] = exp2f(s[0][3]); return 1;
	case MBX_VGP_LOG: d[0] = d[1] = d[2] = d[3] = s[0][3] == 0 ? -INFINITY : log2f(fabsf(s[0][3])); return 1;
	case MBX_VGP_DST: d[0] = 1; d[1] = s[0][1] * s[1][1]; d[2] = s[0][2]; d[3] = s[1][3]; return 1;
	default: return 0;
	}
}

static struct mbx_vgp_src rand_src(int *in_idx, int *c_idx, int *c_rel)
{
	struct mbx_vgp_src s = { 0 };
	int f = rnd() % 3;

	s.file = f;
	s.swizzle = (uint8_t)rnd();
	s.negate = rnd() & 1;
	switch (f) {
	case MBX_VGP_INPUT:
		if (*in_idx < 0) *in_idx = rnd() % 8;
		s.index = *in_idx;
		break;
	case MBX_VGP_CONST:
		if (*c_idx < 0) { *c_idx = rnd() % 128; *c_rel = rnd() % 4 == 0; }
		s.index = *c_idx;
		s.relative = *c_rel;
		break;
	default:
		s.index = rnd() % 14;
	}
	return s;
}

static void test_ops(void)
{
	static struct vgp g;
	int op, it, k;

	for (op = MBX_VGP_MOV; op < MBX_VGP_NUM_OPS; op++) {
		int n = mbx_vgp_nsrc(op), tested = 0;

		if (!ref_op(op, (float[3][4]){ { 0 } }, (float[4]){ 0 }))
			continue;
		for (it = 0; it < 4000; it++) {
			struct mbx_vgp_src src[3];
			struct vgp_regs r;
			float s[3][4], d[4], expect[4];
			uint32_t w[2];
			int in_idx = -1, c_idx = -1, c_rel = 0, dst, mask, window = 16 * (int)(rnd() % 4);

			for (k = 0; k < n; k++)
				src[k] = rand_src(&in_idx, &c_idx, &c_rel);
			if (c_idx >= 0 && c_idx + window + (c_rel ? 4 : 0) >= MBX_VGP_NUM_CONSTS)
				continue;
			dst = rnd() & 1 ? (int)(rnd() % 8) : 16 + (int)(rnd() % 14);
			mask = (int)(rnd() % 16);
			CHECK(!mbx_vgp_encode(w, op, dst, mask, src, n), "encode op %d refused a legal instruction", op);

			memset(&g, 0, sizeof(g));
			memset(&r, 0, sizeof(r));
			for (k = 0; k < MBX_VGP_NUM_CONSTS; k++)
				for (int j = 0; j < 4; j++)
					g.k[k][j] = rndf();
			for (k = 0; k < 8; k++)
				for (int j = 0; j < 4; j++)
					r.v[k][j] = rndf();
			for (k = 0; k < 14; k++)
				for (int j = 0; j < 4; j++)
					r.r[k][j] = rndf();
			for (k = 0; k < 8; k++)
				for (int j = 0; j < 4; j++)
					r.o[k][j] = rndf();
			r.a0 = c_rel ? 4 : 0;
			g.code[0][0] = w[0];
			g.code[0][1] = w[1];

			for (k = 0; k < n; k++)
				ref_src(&g, &r, &src[k], window, s[k]);
			ref_op(op, s, d);
			if (dst < 8)
				memcpy(expect, r.o[dst], 16);
			else
				memcpy(expect, r.r[dst - 16], 16);
			for (k = 0; k < 4; k++)
				if (mask >> k & 1)
					expect[k] = d[k];

			vgp_exec(&g, &r, 0, 0, window);
			{
				float *got = dst < 8 ? r.o[dst] : r.r[dst - 16];
				char text[96];

				mbx_vgp_disasm(text, sizeof(text), w);
				CHECK(!memcmp(got, expect, 16), "op %d (%s): got %g %g %g %g, want %g %g %g %g", op,
				      text, got[0], got[1], got[2], got[3], expect[0], expect[1], expect[2],
				      expect[3]);
			}
			tested++;
		}
		CHECK(tested > 1000, "op %d: only %d cases ran", op, tested);
	}
}

static void test_refusals(void)
{
	struct mbx_vgp_src a = { MBX_VGP_CONST, 3, MBX_VGP_XYZW, 0, 0 }, b = a, in0 = { MBX_VGP_INPUT, 0, MBX_VGP_XYZW, 0, 0 };
	struct mbx_vgp_src in1 = in0, two[2], t = { MBX_VGP_TEMP, 14, MBX_VGP_XYZW, 0, 0 };
	uint32_t w[2];

	b.index = 4;
	two[0] = a; two[1] = b;
	CHECK(mbx_vgp_encode(w, MBX_VGP_ADD, 16, 0xf, two, 2) == MBX_VGP_E_ONE_CONST, "two constants");
	b.index = 3; b.relative = 1;
	two[1] = b;
	CHECK(mbx_vgp_encode(w, MBX_VGP_ADD, 16, 0xf, two, 2) == MBX_VGP_E_ONE_CONST, "relative differs");
	b.relative = 0;
	two[1] = b;
	CHECK(mbx_vgp_encode(w, MBX_VGP_ADD, 16, 0xf, two, 2) == 0, "the same constant twice is fine");
	in1.index = 1;
	two[0] = in0; two[1] = in1;
	CHECK(mbx_vgp_encode(w, MBX_VGP_ADD, 16, 0xf, two, 2) == MBX_VGP_E_ONE_INPUT, "two inputs");
	CHECK(mbx_vgp_encode(w, MBX_VGP_MOV, 16, 0xf, &t, 1) == MBX_VGP_E_INDEX, "temporary 14");
	CHECK(mbx_vgp_encode(w, MBX_VGP_MOV, 8, 0xf, &in0, 1) == MBX_VGP_E_DST, "destination 8");
	CHECK(mbx_vgp_encode(w, MBX_VGP_MOV, 30, 0xf, &in0, 1) == MBX_VGP_E_DST, "destination 30");
	CHECK(mbx_vgp_encode(w, MBX_VGP_MOV, 0, 16, &in0, 1) == MBX_VGP_E_MASK, "mask 16");
	CHECK(mbx_vgp_encode(w, MBX_VGP_ADD, 0, 0xf, &in0, 1) == MBX_VGP_E_NSRC, "add with one source");
	CHECK(mbx_vgp_encode(w, 21, 0, 0xf, &in0, 1) == MBX_VGP_E_OP, "opcode 21");
	a.index = 128;
	CHECK(mbx_vgp_encode(w, MBX_VGP_MOV, 0, 0xf, &a, 1) == MBX_VGP_E_INDEX, "constant 128");
}

static void test_disasm(void)
{
	struct mbx_vgp_src s[2] = { { MBX_VGP_INPUT, 0, MBX_VGP_XYZW, 0, 0 },
				    { MBX_VGP_CONST, 70, MBX_VGP_SWZ(2, 2, 2, 2), 1, 1 } };
	enum mbx_vgp_op op;
	struct mbx_vgp_src d[3];
	uint32_t w[2];
	char text[96];
	int dst, mask;

	CHECK(!mbx_vgp_encode(w, MBX_VGP_DP4, MBX_VGP_OUT(0), 3, s, 2), "encode");
	mbx_vgp_disasm(text, sizeof(text), w);
	CHECK(!strcmp(text, "dp4 o0.xy, v0, -c[a0+70].zzzz"), "disasm: '%s'", text);
	CHECK(mbx_vgp_decode(w, &op, &dst, &mask, d) == 2 && op == MBX_VGP_DP4 && dst == 0 && mask == 3 &&
	      d[1].index == 70 && d[1].relative && d[1].negate && d[1].swizzle == MBX_VGP_SWZ(2, 2, 2, 2),
	      "decode");
}

/* a0 and relative constants: AMV takes a0 from its source's x, truncated */
static void test_amv(void)
{
	static struct vgp g;
	struct vgp_regs r;
	struct mbx_vgp_src in = { MBX_VGP_INPUT, 0, MBX_VGP_XYZW, 0, 0 };
	struct mbx_vgp_src c = { MBX_VGP_CONST, 10, MBX_VGP_XYZW, 0, 1 };
	uint32_t w[2];
	int k;

	memset(&g, 0, sizeof(g));
	memset(&r, 0, sizeof(r));
	for (k = 0; k < MBX_VGP_NUM_CONSTS; k++)
		g.k[k][0] = (float)k;
	r.v[0][0] = 7.9f;
	CHECK(!mbx_vgp_encode(w, MBX_VGP_AMV, MBX_VGP_TMP(0), 0xf, &in, 1), "amv");
	g.code[0][0] = w[0]; g.code[0][1] = w[1];
	CHECK(!mbx_vgp_encode(w, MBX_VGP_MOV, MBX_VGP_OUT(0), 1, &c, 1), "mov rel");
	g.code[1][0] = w[0]; g.code[1][1] = w[1];
	vgp_exec(&g, &r, 0, 1, 0);
	CHECK(r.a0 == 7 && r.o[0][0] == 17.0f, "a0 %d, c[a0+10].x = %g (want 7, 17)", r.a0, r.o[0][0]);
	/* a section's constant window adds to the index too */
	memset(&r, 0, sizeof(r));
	r.v[0][0] = 2;
	vgp_exec(&g, &r, 0, 1, 32);
	CHECK(r.o[0][0] == 44.0f, "window 32 + a0 2 + 10 = 44, got %g", r.o[0][0]);
}

int main(void)
{
	test_ops();
	test_refusals();
	test_disasm();
	test_amv();
	printf("t_vgp: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
