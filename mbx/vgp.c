// SPDX-License-Identifier: MIT
/*
 * mbxemu: the VGP, the vertex processor. An interpreter for the
 * instructions the host software's vertex-program encoder emits, with
 * the behaviour of the N810: vertex-shader opcodes, one
 * constant register and one input register an instruction, a section's
 * constant window, scalar operations reading w.
 *
 *   high: 24-31 src2 swizzle, 16-23 src1, 8-15 src0; 5, 6, 7 negate
 *         src0, 1, 2; 0-4 opcode
 *   low:  26-31 destination (0-7 outputs, 16-29 temporaries), 22-25 write
 *         mask, 16-21 register files (2 bits a source: input, constant,
 *         temporary, unknown), 15 the constant relative to a0, 12-14 the
 *         constant's index bits 4-6, 8-11 / 4-7 / 0-3 src2 / src1 / src0
 *         index bits 0-3
 */
#include <math.h>
#include <string.h>

#include "emu.h"

enum {
	OP_NOP, OP_MOV, OP_ADD, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN,
	OP_MAX, OP_SLT, OP_SGE, OP_EXPP, OP_LOGP, OP_LIT, OP_DST, OP_FRC, OP_AMV, OP_EXP,
	OP_LOG,
};

/* Sources an opcode reads: the rules for one constant and one input
 * register an instruction apply among these (assumed) */
static const int nsrc[] = {
	[OP_MOV] = 1, [OP_ADD] = 2, [OP_MAD] = 3, [OP_MUL] = 2, [OP_RCP] = 1, [OP_RSQ] = 1,
	[OP_DP3] = 2, [OP_DP4] = 2, [OP_MIN] = 2, [OP_MAX] = 2, [OP_SLT] = 2, [OP_SGE] = 2,
	[OP_EXPP] = 1, [OP_LOGP] = 1, [OP_LIT] = 1, [OP_DST] = 2, [OP_FRC] = 1, [OP_AMV] = 1,
	[OP_EXP] = 1, [OP_LOG] = 1,
};

/* The sources of one instruction, read */
static int read_sources(const struct vgp *g, const struct vgp_regs *r, uint32_t hi, uint32_t lo,
			int window, int n, float s[3][4])
{
	int k, in_idx = -1, c_low = -1, black = 0;

	memset(s, 0, 3 * sizeof(s[0]));
	for (k = 0; k < n; k++) {
		int file = lo >> (16 + 2 * k) & 3, idx = lo >> (4 * k) & 0xf, j;
		uint32_t swz = hi >> (8 + 8 * k) & 0xff;
		const float *src;
		float raw[4] = { 0, 0, 0, 0 };

		switch (file) {
		case 0:
			/* one input register an instruction: the first input
			 * source's, for every input source */
			if (in_idx < 0)
				in_idx = idx;
			src = in_idx < 8 ? r->v[in_idx] : raw;
			break;
		case 1: {
			/* one constant register an instruction: its index bits
			 * 4-6 are shared, and two different ones write black */
			int ci = (int)(lo >> 12 & 7) << 4 | idx;

			if (c_low >= 0 && c_low != idx)
				black = 1;
			c_low = idx;
			ci += window + (lo & 0x8000 ? r->a0 : 0);
			src = ci >= 0 && ci < 192 ? g->k[ci] : raw;	/* past the end reads 0 */
			break;
		}
		case 2:
			src = idx < 14 ? r->r[idx] : raw;
			break;
		default:
			/* register file 3: not understood (reads something like
			 * the position); 0 here */
			src = raw;
			break;
		}
		for (j = 0; j < 4; j++) {
			float x = src[swz >> (2 * j) & 3];

			s[k][j] = hi >> (5 + k) & 1 ? -x : x;
		}
	}
	return black;
}

static float dp(const float *a, const float *b, int n)
{
	float d = 0;
	int k;

	for (k = 0; k < n; k++)
		d += a[k] * b[k];
	return d;
}

static void splat(float *d, float x)
{
	d[0] = d[1] = d[2] = d[3] = x;
}

/* Run instructions first..last on the registers, constants offset by window */
void vgp_exec(const struct vgp *g, struct vgp_regs *r, int first, int last, int window)
{
	int pc;

	for (pc = first; pc <= last && pc < 256; pc++) {
		uint32_t hi = g->code[pc][0], lo = g->code[pc][1];
		int op = hi & 0x1f, dst = lo >> 26, mask = lo >> 22 & 0xf, k, black;
		float s[3][4], d[4], *out;

		if (op == OP_NOP || op > OP_LOG)
			continue;	/* 0x15-0x1f: nothing, the destination keeps its value */
		black = read_sources(g, r, hi, lo, window, nsrc[op], s);
		switch (op) {
		case OP_MOV:
			memcpy(d, s[0], sizeof(d));
			break;
		case OP_ADD:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] + s[1][k];
			break;
		case OP_MAD:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] * s[1][k] + s[2][k];
			break;
		case OP_MUL:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] * s[1][k];
			break;
		case OP_RCP:
			splat(d, s[0][3] == 1.0f ? 1.0f : 1.0f / s[0][3]);
			break;
		case OP_RSQ:
			splat(d, 1.0f / sqrtf(fabsf(s[0][3])));
			break;
		case OP_DP3:
			splat(d, dp(s[0], s[1], 3));
			break;
		case OP_DP4:
			splat(d, dp(s[0], s[1], 4));
			break;
		case OP_MIN:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] < s[1][k] ? s[0][k] : s[1][k];
			break;
		case OP_MAX:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] >= s[1][k] ? s[0][k] : s[1][k];
			break;
		case OP_SLT:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] < s[1][k] ? 1.0f : 0.0f;
			break;
		case OP_SGE:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] >= s[1][k] ? 1.0f : 0.0f;
			break;
		case OP_EXPP: {
			float w = s[0][3], f = floorf(w);

			d[0] = exp2f(f);
			d[1] = w - f;
			d[2] = exp2f(w);
			d[3] = 1;
			break;
		}
		case OP_LOGP: {
			float w = fabsf(s[0][3]);

			if (w == 0) {
				d[0] = d[2] = -INFINITY;
				d[1] = 1;
			} else {
				float l = log2f(w), f = floorf(l);

				d[0] = f;
				d[1] = w / exp2f(f);
				d[2] = l;
			}
			d[3] = 1;
			break;
		}
		case OP_LIT: {
			float p = s[0][3] < -128 ? -128 : s[0][3] > 128 ? 128 : s[0][3];

			d[0] = 1;
			d[1] = s[0][0] > 0 ? s[0][0] : 0;
			d[2] = s[0][0] > 0 && s[0][1] > 0 ? powf(s[0][1], p) : 0;
			d[3] = 1;
			break;
		}
		case OP_DST:
			d[0] = 1;
			d[1] = s[0][1] * s[1][1];
			d[2] = s[0][2];
			d[3] = s[1][3];
			break;
		case OP_FRC:
			for (k = 0; k < 4; k++)
				d[k] = s[0][k] - floorf(s[0][k]);
			break;
		case OP_AMV:
			r->a0 = (int)truncf(s[0][0]);	/* 4.7 -> 4 */
			continue;
		case OP_EXP:
			splat(d, exp2f(s[0][3]));
			break;
		case OP_LOG:
			splat(d, s[0][3] == 0 ? -INFINITY : log2f(fabsf(s[0][3])));
			break;
		default:
			continue;
		}
		if (black)
			splat(d, 0);
		if (dst < 8)
			out = r->o[dst];
		else if (dst >= 16 && dst < 30)
			out = r->r[dst - 16];
		else
			continue;
		for (k = 0; k < 4; k++)
			if (mask >> k & 1)
				out[k] = d[k];
	}
}

/*
 * The sections a vertex runs, in bit order (0x4 first): each its code
 * range, (flags & 15) + 1 times, its constants offset by the window
 * (flags bits 4-7, in sixteens). A section never loaded runs nothing.
 */
void vgp_run(struct mbxemu *e, struct vgp_regs *r, uint32_t sections)
{
	const struct vgp *g = &e->vgp;
	int b, l;

	for (b = 0; b < 16; b++) {
		if (!(sections >> b & 1) || !g->sec[b].loaded)
			continue;
		for (l = 0; l <= (int)(g->sec[b].flags & 0xf); l++)
			vgp_exec(g, r, g->sec[b].first, g->sec[b].last,
				 (int)(g->sec[b].flags >> 4 & 0xf) * 16);
	}
}
