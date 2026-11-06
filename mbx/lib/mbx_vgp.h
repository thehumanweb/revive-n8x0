/* SPDX-License-Identifier: MIT */
/*
 * Encoding, decoding and printing VGP instructions: what a shader compiler
 * for the MBX emits. The layout and the rules are the model's (vgp.c, and
 * README.md assumption 13); the tests (tests/t_vgp.c) run every encoding
 * through the model's interpreter.
 *
 * 64 bits an instruction, high word first.
 *   high: 24-31 src2 swizzle, 16-23 src1, 8-15 src0; 5, 6, 7 negate src0,
 *         src1, src2; 0-4 opcode
 *   low:  26-31 destination (0-7 outputs, 16-29 temporaries), 22-25 write
 *         mask, 16-21 register files (2 bits a source: 0 input, 1 constant,
 *         2 temporary), 15 the constant is relative to a0, 12-14 the
 *         constant's index bits 4-6, 8-11 / 4-7 / 0-3 src2 / src1 / src0
 *         index bits 0-3
 *
 * Rules the hardware imposes, which encode refuses to break:
 *  - an instruction reads one constant register (several sources may name
 *    the same one, with the same relative flag) and one input register
 *    (the first input source's index serves every input source);
 *  - sources are src0, src1, ...: an opcode reads only its first nsrc;
 *  - scalar opcodes (RCP, RSQ, EXP, LOG, EXPP, LOGP) read the source's w and
 *    write every lane;
 *  - 192 constants and 256 instructions, a section's constant window
 *    (flags bits 4-7, in sixteens) added to every constant index.
 */
#ifndef MBX_VGP_H
#define MBX_VGP_H

#include <stdint.h>
#include <stdio.h>

enum mbx_vgp_op {
	MBX_VGP_NOP, MBX_VGP_MOV, MBX_VGP_ADD, MBX_VGP_MAD, MBX_VGP_MUL, MBX_VGP_RCP,
	MBX_VGP_RSQ, MBX_VGP_DP3, MBX_VGP_DP4, MBX_VGP_MIN, MBX_VGP_MAX, MBX_VGP_SLT,
	MBX_VGP_SGE, MBX_VGP_EXPP, MBX_VGP_LOGP, MBX_VGP_LIT, MBX_VGP_DST, MBX_VGP_FRC,
	MBX_VGP_AMV, MBX_VGP_EXP, MBX_VGP_LOG,
	MBX_VGP_NUM_OPS
};

enum mbx_vgp_file { MBX_VGP_INPUT, MBX_VGP_CONST, MBX_VGP_TEMP };

/* Destinations: outputs 0-7 (0 position, 1 colour (BGRA), 2 offset colour
 * (BGRA, w the fog factor), 3 and 4 texture coordinates, 5-6 user clip
 * planes 0-3 and 4-7, 7 front and rear clip), temporaries 0-13 */
#define MBX_VGP_OUT(n)		(n)
#define MBX_VGP_TMP(n)		(16 + (n))
#define MBX_VGP_NUM_INPUTS	8
#define MBX_VGP_NUM_TEMPS	14
#define MBX_VGP_NUM_CONSTS	192
#define MBX_VGP_NUM_INSNS	256

/* User clip plane n (0-7) in a vertex's sections word */
#define MBX_VGP_CLIP(n)		(1u << (20 + (n)))

#define MBX_VGP_MASK_X		1
#define MBX_VGP_MASK_Y		2
#define MBX_VGP_MASK_Z		4
#define MBX_VGP_MASK_W		8
#define MBX_VGP_MASK_XYZW	0xf
/* A swizzle: each lane a component, 0 x, 1 y, 2 z, 3 w */
#define MBX_VGP_SWZ(x, y, z, w)	((x) | (y) << 2 | (z) << 4 | (w) << 6)
#define MBX_VGP_XYZW		MBX_VGP_SWZ(0, 1, 2, 3)

struct mbx_vgp_src {
	enum mbx_vgp_file file;
	int index;		/* inputs 0-7, constants 0-127 (plus window, a0), temporaries 0-13 */
	uint8_t swizzle;	/* MBX_VGP_SWZ() */
	int negate;
	int relative;		/* constants: plus a0 (AMV sets a0 from its source's x) */
};

enum mbx_vgp_err {
	MBX_VGP_OK = 0,
	MBX_VGP_E_OP = -1,		/* no such opcode, or NOP with operands */
	MBX_VGP_E_DST = -2,		/* destination not an output or a temporary */
	MBX_VGP_E_MASK = -3,
	MBX_VGP_E_NSRC = -4,		/* the wrong number of sources for the opcode */
	MBX_VGP_E_INDEX = -5,		/* a register index out of range */
	MBX_VGP_E_ONE_CONST = -6,	/* two different constant registers */
	MBX_VGP_E_ONE_INPUT = -7,	/* two different input registers */
};

/* Sources an opcode reads */
static inline int mbx_vgp_nsrc(enum mbx_vgp_op op)
{
	static const int n[MBX_VGP_NUM_OPS] = {
		[MBX_VGP_MOV] = 1, [MBX_VGP_ADD] = 2, [MBX_VGP_MAD] = 3, [MBX_VGP_MUL] = 2,
		[MBX_VGP_RCP] = 1, [MBX_VGP_RSQ] = 1, [MBX_VGP_DP3] = 2, [MBX_VGP_DP4] = 2,
		[MBX_VGP_MIN] = 2, [MBX_VGP_MAX] = 2, [MBX_VGP_SLT] = 2, [MBX_VGP_SGE] = 2,
		[MBX_VGP_EXPP] = 1, [MBX_VGP_LOGP] = 1, [MBX_VGP_LIT] = 1, [MBX_VGP_DST] = 2,
		[MBX_VGP_FRC] = 1, [MBX_VGP_AMV] = 1, [MBX_VGP_EXP] = 1, [MBX_VGP_LOG] = 1,
	};

	return (unsigned)op < MBX_VGP_NUM_OPS ? n[op] : -1;
}

/* Scalar opcodes read the source's w */
static inline int mbx_vgp_is_scalar(enum mbx_vgp_op op)
{
	return op == MBX_VGP_RCP || op == MBX_VGP_RSQ || op == MBX_VGP_EXP || op == MBX_VGP_LOG ||
	       op == MBX_VGP_EXPP || op == MBX_VGP_LOGP;
}

/*
 * Encode an instruction into w[0] (high word) and w[1] (low word). n must be
 * the opcode's nsrc. 0, or a negative mbx_vgp_err.
 */
static inline int mbx_vgp_encode(uint32_t w[2], enum mbx_vgp_op op, int dst, int mask,
				 const struct mbx_vgp_src *src, int n)
{
	uint32_t hi, lo;
	int k, cidx = -1, crel = 0, in_idx = -1;

	if ((unsigned)op >= MBX_VGP_NUM_OPS)
		return MBX_VGP_E_OP;
	if (n != mbx_vgp_nsrc(op) && !(op == MBX_VGP_NOP && n == 0))
		return op == MBX_VGP_NOP ? MBX_VGP_E_OP : MBX_VGP_E_NSRC;
	if (dst < 0 || dst > 29 || (dst > 7 && dst < 16))
		return MBX_VGP_E_DST;
	if (dst >= 16 && dst - 16 >= MBX_VGP_NUM_TEMPS)
		return MBX_VGP_E_DST;
	if (mask < 0 || mask > 0xf)
		return MBX_VGP_E_MASK;
	hi = (uint32_t)op;
	lo = (uint32_t)dst << 26 | (uint32_t)mask << 22;
	for (k = 0; k < 3; k++) {
		const struct mbx_vgp_src *s = k < n ? &src[k] : NULL;
		int idx = s ? s->index : 0;

		hi |= (uint32_t)(s ? s->swizzle : MBX_VGP_XYZW) << (8 + 8 * k);
		if (!s)
			continue;
		if (s->negate)
			hi |= 1u << (5 + k);
		switch (s->file) {
		case MBX_VGP_CONST:
			if (idx < 0 || idx > 127)
				return MBX_VGP_E_INDEX;
			if (cidx >= 0 && (cidx != idx || crel != !!s->relative))
				return MBX_VGP_E_ONE_CONST;
			cidx = idx;
			crel = !!s->relative;
			break;
		case MBX_VGP_INPUT:
			if (idx < 0 || idx >= MBX_VGP_NUM_INPUTS)
				return MBX_VGP_E_INDEX;
			if (in_idx >= 0 && in_idx != idx)
				return MBX_VGP_E_ONE_INPUT;
			in_idx = idx;
			break;
		case MBX_VGP_TEMP:
			if (idx < 0 || idx >= MBX_VGP_NUM_TEMPS)
				return MBX_VGP_E_INDEX;
			break;
		default:
			return MBX_VGP_E_INDEX;	/* register file 3 reads nothing useful */
		}
		lo |= (uint32_t)s->file << (16 + 2 * k) | (uint32_t)(idx & 15) << (4 * k);
	}
	if (cidx >= 0)
		lo |= (uint32_t)(cidx >> 4) << 12 | (crel ? 1u << 15 : 0);
	w[0] = hi;
	w[1] = lo;
	return 0;
}

/* Decode: the inverse; the operand count is the opcode's nsrc. Returns n, or MBX_VGP_E_OP */
static inline int mbx_vgp_decode(const uint32_t w[2], enum mbx_vgp_op *op, int *dst, int *mask,
				 struct mbx_vgp_src src[3])
{
	uint32_t hi = w[0], lo = w[1];
	int k, n;

	*op = (enum mbx_vgp_op)(hi & 0x1f);
	if (*op >= MBX_VGP_NUM_OPS)
		return MBX_VGP_E_OP;
	n = mbx_vgp_nsrc(*op);
	*dst = (int)(lo >> 26);
	*mask = (int)(lo >> 22 & 0xf);
	for (k = 0; k < 3; k++) {
		src[k].file = (enum mbx_vgp_file)(lo >> (16 + 2 * k) & 3);
		src[k].index = (int)(lo >> (4 * k) & 0xf);
		if (src[k].file == MBX_VGP_CONST)
			src[k].index |= (int)(lo >> 12 & 7) << 4;
		src[k].swizzle = (uint8_t)(hi >> (8 + 8 * k));
		src[k].negate = (int)(hi >> (5 + k) & 1);
		src[k].relative = src[k].file == MBX_VGP_CONST && (lo >> 15 & 1);
	}
	return n;
}

/* One line of assembly, e.g. "dp4 o0.xy, v0, c[a0+4].zzzz" */
static inline int mbx_vgp_disasm(char *buf, size_t size, const uint32_t w[2])
{
	static const char *const names[MBX_VGP_NUM_OPS] = {
		"nop", "mov", "add", "mad", "mul", "rcp", "rsq", "dp3", "dp4", "min", "max", "slt",
		"sge", "expp", "logp", "lit", "dst", "frc", "amv", "exp", "log",
	};
	enum mbx_vgp_op op;
	struct mbx_vgp_src src[3];
	int dst, mask, n, k, len;
	const char *comp = "xyzw";

	n = mbx_vgp_decode(w, &op, &dst, &mask, src);
	if (n < 0)
		return snprintf(buf, size, ".word 0x%08x 0x%08x", w[0], w[1]);
	len = snprintf(buf, size, "%s", names[op]);
	if (op == MBX_VGP_NOP)
		return len;
	if (dst < 8)
		len += snprintf(buf + len, size - len, " o%d", dst);
	else
		len += snprintf(buf + len, size - len, " r%d", dst - 16);
	if (mask != 0xf) {
		len += snprintf(buf + len, size - len, ".");
		for (k = 0; k < 4; k++)
			if (mask >> k & 1)
				len += snprintf(buf + len, size - len, "%c", comp[k]);
	}
	for (k = 0; k < n; k++) {
		const char *neg = src[k].negate ? "-" : "";
		int j;

		len += snprintf(buf + len, size - len, ", %s", neg);
		switch (src[k].file) {
		case MBX_VGP_INPUT: len += snprintf(buf + len, size - len, "v%d", src[k].index); break;
		case MBX_VGP_TEMP: len += snprintf(buf + len, size - len, "r%d", src[k].index); break;
		case MBX_VGP_CONST:
			if (src[k].relative)
				len += snprintf(buf + len, size - len, "c[a0+%d]", src[k].index);
			else
				len += snprintf(buf + len, size - len, "c%d", src[k].index);
			break;
		default: len += snprintf(buf + len, size - len, "?%d", src[k].index);
		}
		if (src[k].swizzle != MBX_VGP_XYZW) {
			len += snprintf(buf + len, size - len, ".");
			for (j = 0; j < 4; j++)
				len += snprintf(buf + len, size - len, "%c", comp[src[k].swizzle >> (2 * j) & 3]);
		}
	}
	return len;
}

#endif
