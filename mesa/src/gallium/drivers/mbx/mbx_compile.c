// SPDX-License-Identifier: MIT
/*
 * NIR to the VGP, and the fragment side onto the fixed pipe.
 *
 * The VGP is a vec4 machine with write masks and source swizzles, and no
 * branches; so a shader reaches us as scalar SSA in one block. Every scalar
 * value lives in one lane of a temporary, and any scalar operation can read
 * and write any lane: the source swizzle picks the lane to read, the
 * write mask the lane to write.
 *
 * Pass one turns the NIR into a list of operations on virtual scalar
 * registers (values from inputs, uniforms and immediates are used where they
 * are). Pass two gives the virtual registers lanes of r0-r12 (r13 stages the
 * position), makes each instruction obey the hardware's one-constant-register
 * and one-input-register rule by copying operands to temporaries, and encodes
 * with mbx_vgp.h. Rules and layout: mbx/vgp.c.
 */
#include <math.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "util/log.h"
#include "util/u_math.h"

#include "mbx_program.h"
#include "mbx_screen.h"
#include "mbx_vgp.h"

#define POS_REG		13		/* r13: gl_Position, staged for the clip distances */
#define NTEMP_SLOTS	(POS_REG * 4)	/* r0-r12, four lanes each */
#define MAX_OUT_LOC	64

enum vkind { V_NONE, V_INPUT, V_CONST, V_IMM, V_VREG, V_FIXED };

struct vsrc {
	enum vkind kind;
	int idx;		/* input register, uniform slot, vreg id, or fixed temp register */
	int lane;
	bool neg;
	float imm;
};

enum dkind { D_VREG, D_OUT, D_FIXED };

struct vop {
	enum mbx_vgp_op op;
	struct vsrc src[3];
	int n;
	enum dkind dk;
	int dreg, dlane;
	bool vec;		/* the four lanes at once (the position copy) */
	bool vsrcs;		/* every source is a whole register (a dot product) */
};

struct cc {
	nir_shader *nir;
	const struct mbx_fs_info *fs;
	struct mbx_vgp_program *prog;
	struct vop *ops;
	unsigned nops, cap;
	int nvreg;
	int *vdef;		/* op index that defines a vreg */
	int *vuse;		/* uses of a vreg */
	int *vlast;		/* last op index that reads it */
	struct vsrc *val;	/* by ssa index * 4 + component */
	struct vsrc outs[MAX_OUT_LOC][4];
	int max_uniform;
	const char *why;
};

static bool fail(struct cc *c, const char *why)
{
	if (!c->why)
		c->why = why;
	return false;
}

static struct vop *add_op(struct cc *c, enum mbx_vgp_op op)
{
	struct vop *o;

	if (c->nops == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->ops = realloc(c->ops, c->cap * sizeof(*c->ops));
	}
	o = &c->ops[c->nops++];
	memset(o, 0, sizeof(*o));
	o->op = op;
	return o;
}

static struct vsrc imm(float f)
{
	struct vsrc s = { .kind = V_IMM, .imm = f };

	return s;
}

static struct vsrc fixed_temp(int reg, int lane)
{
	struct vsrc s = { .kind = V_FIXED, .idx = reg, .lane = lane };

	return s;
}

/* Emit op with sources; the result is a new virtual register */
static struct vsrc emit(struct cc *c, enum mbx_vgp_op op, int n, const struct vsrc *src)
{
	struct vop *o = add_op(c, op);
	struct vsrc r = { .kind = V_VREG, .idx = c->nvreg };
	int i;

	o->n = n;
	for (i = 0; i < n; i++)
		o->src[i] = src[i];
	o->dk = D_VREG;
	o->dreg = c->nvreg;
	c->vdef = realloc(c->vdef, (c->nvreg + 1) * sizeof(int));
	c->vuse = realloc(c->vuse, (c->nvreg + 1) * sizeof(int));
	c->vlast = realloc(c->vlast, (c->nvreg + 1) * sizeof(int));
	c->vdef[c->nvreg] = (int)c->nops - 1;
	c->vuse[c->nvreg] = 0;
	c->vlast[c->nvreg] = (int)c->nops - 1;
	c->nvreg++;
	return r;
}

static void use(struct cc *c, const struct vsrc *s)
{
	if (s->kind == V_VREG) {
		c->vuse[s->idx]++;
		c->vlast[s->idx] = (int)c->nops - 1;
	} else if (s->kind == V_CONST && s->idx > c->max_uniform) {
		c->max_uniform = s->idx;
	}
}

/* Emit and count the uses (the op just added is the user) */
static struct vsrc op1(struct cc *c, enum mbx_vgp_op op, struct vsrc a)
{
	struct vsrc s[1] = { a }, r;

	r = emit(c, op, 1, s);
	use(c, &c->ops[c->nops - 1].src[0]);
	return r;
}

static struct vsrc op2(struct cc *c, enum mbx_vgp_op op, struct vsrc a, struct vsrc b)
{
	struct vsrc s[2] = { a, b }, r;

	r = emit(c, op, 2, s);
	use(c, &c->ops[c->nops - 1].src[0]);
	use(c, &c->ops[c->nops - 1].src[1]);
	return r;
}

static struct vsrc op3(struct cc *c, enum mbx_vgp_op op, struct vsrc a, struct vsrc b, struct vsrc d)
{
	struct vsrc s[3] = { a, b, d }, r;

	r = emit(c, op, 3, s);
	use(c, &c->ops[c->nops - 1].src[0]);
	use(c, &c->ops[c->nops - 1].src[1]);
	use(c, &c->ops[c->nops - 1].src[2]);
	return r;
}

static struct vsrc negated(struct vsrc s)
{
	s.neg = !s.neg;
	return s;
}

/* --- pass one: NIR to operations --- */

static struct vsrc alu_src(struct cc *c, const nir_alu_instr *alu, unsigned i, unsigned comp)
{
	/* a sized source (a vecN's) is a scalar: swizzle 0; an unsized one follows the dest */
	unsigned sw = nir_op_infos[alu->op].input_sizes[i] ? alu->src[i].swizzle[0] : alu->src[i].swizzle[comp];

	return c->val[alu->src[i].src.ssa->index * 4 + sw];
}

static bool lower_alu(struct cc *c, nir_alu_instr *alu)
{
	unsigned ncomp = alu->def.num_components, k, base = alu->def.index * 4;
	struct vsrc a, b, d, t;

	/* aliases: the value of a component is the value of its source */
	switch (alu->op) {
	case nir_op_mov:
	case nir_op_vec2:
	case nir_op_vec3:
	case nir_op_vec4:
		for (k = 0; k < ncomp; k++)
			c->val[base + k] = alu_src(c, alu, alu->op == nir_op_mov ? 0 : k, k);
		return true;
	case nir_op_fneg:
		for (k = 0; k < ncomp; k++)
			c->val[base + k] = negated(alu_src(c, alu, 0, k));
		return true;
	default:
		break;
	}
	if (ncomp != 1)
		return fail(c, "a vector ALU operation reached the compiler (not scalarized)");

	a = alu_src(c, alu, 0, 0);
	b = nir_op_infos[alu->op].num_inputs > 1 ? alu_src(c, alu, 1, 0) : a;
	switch (alu->op) {
	case nir_op_fadd: d = op2(c, MBX_VGP_ADD, a, b); break;
	case nir_op_fsub: d = op2(c, MBX_VGP_ADD, a, negated(b)); break;
	case nir_op_fmul: d = op2(c, MBX_VGP_MUL, a, b); break;
	case nir_op_ffma: d = op3(c, MBX_VGP_MAD, a, b, alu_src(c, alu, 2, 0)); break;
	case nir_op_fmin: d = op2(c, MBX_VGP_MIN, a, b); break;
	case nir_op_fmax: d = op2(c, MBX_VGP_MAX, a, b); break;
	case nir_op_fabs: d = op2(c, MBX_VGP_MAX, a, negated(a)); break;
	case nir_op_slt: d = op2(c, MBX_VGP_SLT, a, b); break;
	case nir_op_sge: d = op2(c, MBX_VGP_SGE, a, b); break;
	case nir_op_seq:	/* a == b: a >= b and b >= a */
		d = op2(c, MBX_VGP_MUL, op2(c, MBX_VGP_SGE, a, b), op2(c, MBX_VGP_SGE, b, a));
		break;
	case nir_op_sne:	/* a < b or b < a (never both) */
		d = op2(c, MBX_VGP_ADD, op2(c, MBX_VGP_SLT, a, b), op2(c, MBX_VGP_SLT, b, a));
		break;
	case nir_op_frcp: d = op1(c, MBX_VGP_RCP, a); break;
	case nir_op_frsq: d = op1(c, MBX_VGP_RSQ, a); break;
	case nir_op_fexp2: d = op1(c, MBX_VGP_EXP, a); break;
	case nir_op_flog2: d = op1(c, MBX_VGP_LOG, a); break;
	case nir_op_ffract: d = op1(c, MBX_VGP_FRC, a); break;
	case nir_op_fcsel:
	case nir_op_fcsel_gt:	/* the condition is 1.0 or 0.0 here: b + c * (a - b) */
		t = op2(c, MBX_VGP_ADD, b, negated(alu_src(c, alu, 2, 0)));
		d = op3(c, MBX_VGP_MAD, a, t, alu_src(c, alu, 2, 0));
		break;
	default:
		mesa_loge("mbx: NIR operation %s is not supported", nir_op_infos[alu->op].name);
		return fail(c, "an unsupported ALU operation");
	}
	c->val[base] = d;
	return true;
}

static struct vsrc const_val(struct cc *c, nir_load_const_instr *lc, unsigned comp)
{
	return imm(lc->value[comp].f32);
}

static bool lower_intrinsic(struct cc *c, nir_intrinsic_instr *intr)
{
	unsigned k, n, comp, base = intr->def.index * 4;

	switch (intr->intrinsic) {
	case nir_intrinsic_load_input: {
		int reg;

		if (!nir_src_is_const(intr->src[0]))
			return fail(c, "an indirect vertex attribute read");
		reg = (int)(nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]));
		if (reg >= MBX_MAX_ATTRS)
			return fail(c, "more than 8 vertex attributes");
		c->prog->attr_used[reg] = true;
		comp = nir_intrinsic_component(intr);
		for (k = 0; k < intr->def.num_components; k++)
			c->val[base + k] = (struct vsrc){ .kind = V_INPUT, .idx = reg, .lane = (int)(comp + k) };
		return true;
	}
	case nir_intrinsic_load_uniform: {
		int slot;

		if (!nir_src_is_const(intr->src[0]))
			return fail(c, "an indirect uniform read");
		slot = (int)(nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0]));
		/* load_uniform has no component index (reading one gives another field's value):
		 * the load starts at the first component of its vec4 slot */
		comp = 0;
		for (k = 0; k < intr->def.num_components; k++) {
			c->val[base + k] = (struct vsrc){ .kind = V_CONST, .idx = slot, .lane = (int)(comp + k) };
			if (slot > c->max_uniform)
				c->max_uniform = slot;
		}
		return true;
	}
	case nir_intrinsic_store_output: {
		unsigned loc = nir_intrinsic_io_semantics(intr).location, wm = nir_intrinsic_write_mask(intr);

		comp = nir_intrinsic_component(intr);
		if (loc >= MAX_OUT_LOC)
			return true;	/* a slot we do not use */
		n = nir_src_num_components(intr->src[0]);
		for (k = 0; k < n; k++)
			if (wm >> k & 1)
				c->outs[loc][comp + k] = c->val[intr->src[0].ssa->index * 4 + k];
		return true;
	}
	default:
		mesa_loge("mbx: intrinsic %s is not supported in a vertex shader", nir_intrinsic_infos[intr->intrinsic].name);
		return fail(c, "an unsupported intrinsic");
	}
}

/* An output value into o<reg>.<lane>, or into a fixed temporary: retarget its producer when this is its only use */
static void write_dest(struct cc *c, struct vsrc v, enum dkind dk, int reg, int lane)
{
	struct vop *o;

	if (v.kind == V_VREG && !v.neg && c->vuse[v.idx] == 1) {
		o = &c->ops[c->vdef[v.idx]];
		o->dk = dk;
		o->dreg = reg;
		o->dlane = lane;
		return;
	}
	o = add_op(c, MBX_VGP_MOV);
	o->n = 1;
	o->src[0] = v;
	o->dk = dk;
	o->dreg = reg;
	o->dlane = lane;
	if (v.kind == V_VREG)
		c->vlast[v.idx] = (int)c->nops - 1;
	else if (v.kind == V_CONST && v.idx > c->max_uniform)
		c->max_uniform = v.idx;
}

/* One NIR operation of the fragment shader on values in the vertex program (the same rules as lower_alu) */
static struct vsrc eval_nir_op(struct cc *c, nir_op op, struct vsrc a, struct vsrc b, struct vsrc cc)
{
	struct vsrc t;

	switch (op) {
	case nir_op_fmin: return op2(c, MBX_VGP_MIN, a, b);
	case nir_op_fmax: return op2(c, MBX_VGP_MAX, a, b);
	case nir_op_fabs: return op2(c, MBX_VGP_MAX, a, negated(a));
	case nir_op_slt: return op2(c, MBX_VGP_SLT, a, b);
	case nir_op_sge: return op2(c, MBX_VGP_SGE, a, b);
	case nir_op_seq: return op2(c, MBX_VGP_MUL, op2(c, MBX_VGP_SGE, a, b), op2(c, MBX_VGP_SGE, b, a));
	case nir_op_sne: return op2(c, MBX_VGP_ADD, op2(c, MBX_VGP_SLT, a, b), op2(c, MBX_VGP_SLT, b, a));
	case nir_op_frcp: return op1(c, MBX_VGP_RCP, a);
	case nir_op_frsq: return op1(c, MBX_VGP_RSQ, a);
	case nir_op_fexp2: return op1(c, MBX_VGP_EXP, a);
	case nir_op_flog2: return op1(c, MBX_VGP_LOG, a);
	case nir_op_ffract: return op1(c, MBX_VGP_FRC, a);
	case nir_op_fcsel:	/* the condition is 1.0 or 0.0: b + c * (a - b) */
		t = op2(c, MBX_VGP_ADD, b, negated(cc));
		return op3(c, MBX_VGP_MAD, a, t, cc);
	default:
		fail(c, "an operation the grid cannot evaluate");
		return imm(0.0f);
	}
}

/* An expression of the fragment shader's non-texel part, in the vertex program */
static struct vsrc fs_eval(struct cc *c, int n)
{
	const struct mbx_fs_info *fs = c->fs;
	const struct mbx_fs_node *nd = &fs->node[n];
	struct vsrc a, b, r = imm(0.0f);

	switch (nd->op) {
	case FSN_CONST:
		return imm(nd->k);
	case FSN_VAR:
		if (nd->loc == VARYING_SLOT_POS) {
			/* gl_FragCoord: the window coordinates of the vertex, (x, y, z) / w times the viewport's scale plus its translate (two
			 * constants the draw fills in, c->prog->fc_slot) and 1 / w; the chip interpolates them in screen space, where they
			 * are linear */
			struct vsrc w = c->outs[VARYING_SLOT_POS][3], v = c->outs[VARYING_SLOT_POS][nd->comp < 3 ? nd->comp : 0], rw;

			if (w.kind == V_NONE)
				w = imm(1.0f);
			if (v.kind == V_NONE)
				v = imm(0.0f);
			rw = eval_nir_op(c, nir_op_frcp, w, w, w);
			if (nd->comp == 3)
				return rw;
			if (c->prog->fc_slot < 0) {
				c->prog->fc_slot = c->max_uniform + 1;
				c->max_uniform = c->prog->fc_slot + 1;
			}
			{
				int slot = c->prog->fc_slot + (nd->comp == 2), lane = nd->comp == 2 ? 0 : (int)nd->comp * 2;
				struct vsrc sc = { .kind = V_CONST, .idx = slot, .lane = lane }, tr = { .kind = V_CONST, .idx = slot, .lane = lane + 1 };

				r = op2(c, MBX_VGP_MUL, v, rw);
				r = op2(c, MBX_VGP_MUL, r, sc);
				return op2(c, MBX_VGP_ADD, r, tr);
			}
		}
		r = c->outs[nd->loc][nd->comp];
		if (r.kind == V_NONE) {
			fail(c, "the vertex shader does not write a varying the fragment shader reads");
			return imm(0.0f);
		}
		return r;
	case FSN_UNI:
		r = (struct vsrc){ .kind = V_CONST, .idx = c->prog->fsu_base + (int)nd->loc, .lane = (int)nd->comp };
		return r;
	case FSN_NEG:
		return negated(fs_eval(c, nd->a));
	case FSN_ADD:
		a = fs_eval(c, nd->a);
		b = fs_eval(c, nd->b);
		return op2(c, MBX_VGP_ADD, a, b);
	case FSN_SUB:
		a = fs_eval(c, nd->a);
		b = fs_eval(c, nd->b);
		return op2(c, MBX_VGP_ADD, a, negated(b));
	case FSN_MUL:
		a = fs_eval(c, nd->a);
		b = fs_eval(c, nd->b);
		return op2(c, MBX_VGP_MUL, a, b);
	case FSN_OP: {
		nir_op op = (nir_op)nd->loc;
		unsigned nin = nir_op_infos[op].num_inputs;
		struct vsrc cc;

		a = fs_eval(c, nd->a);
		b = nin > 1 ? fs_eval(c, nd->b) : a;
		cc = nin > 2 ? fs_eval(c, nd->c) : a;
		return eval_nir_op(c, op, a, b, cc);
	}
	}
	return r;
}

static bool pass_one(struct cc *c)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(c->nir);
	const struct mbx_fs_info *fs = c->fs;
	static const int bgra[4] = { 2, 1, 0, 3 };	/* o1 is BGRA */
	int lane;

	if (exec_list_length(&impl->body) != 1)
		return fail(c, "the vertex shader has control flow the VGP cannot do");
	nir_index_ssa_defs(impl);
	c->val = calloc(impl->ssa_alloc * 4, sizeof(*c->val));

	nir_foreach_instr(instr, nir_start_block(impl)) {
		switch (instr->type) {
		case nir_instr_type_load_const: {
			nir_load_const_instr *lc = nir_instr_as_load_const(instr);

			for (unsigned k = 0; k < lc->def.num_components; k++)
				c->val[lc->def.index * 4 + k] = const_val(c, lc, k);
			break;
		}
		case nir_instr_type_undef: {
			nir_undef_instr *u = nir_instr_as_undef(instr);

			for (unsigned k = 0; k < u->def.num_components; k++)
				c->val[u->def.index * 4 + k] = imm(0.0f);
			break;
		}
		case nir_instr_type_alu:
			if (!lower_alu(c, nir_instr_as_alu(instr)))
				return false;
			break;
		case nir_instr_type_intrinsic:
			if (!lower_intrinsic(c, nir_instr_as_intrinsic(instr)))
				return false;
			break;
		default:
			return fail(c, "an unsupported kind of NIR instruction");
		}
	}

	/* outputs: the position into r13 (and the clip distances after it), the colour into o1 */
	{
		/* every lane's use is counted before any is written: a value used by two lanes (x and y of one product) must not be
		 * taken for a single-use one and made to write its first lane's register directly */
		struct vsrc pv[4];

		for (lane = 0; lane < 4; lane++) {
			pv[lane] = c->outs[VARYING_SLOT_POS][lane];
			if (pv[lane].kind == V_NONE)
				pv[lane] = imm(lane == 3 ? 1.0f : 0.0f);
			if (pv[lane].kind == V_VREG)
				c->vuse[pv[lane].idx]++;
		}
		for (lane = 0; lane < 4; lane++)
			write_dest(c, pv[lane], D_FIXED, POS_REG, lane);
	}
	{
		/* x' = x + dx * w, dx a uniform the draw fills in from the viewport (mbx_draw.c): see struct mbx_vgp_program's vp_slot */
		int slot = c->max_uniform + 1;
		struct vop *o = add_op(c, MBX_VGP_MAD);

		o->n = 3;
		o->src[0] = fixed_temp(POS_REG, 3);
		o->src[1] = (struct vsrc){ .kind = V_CONST, .idx = slot, .lane = 0 };
		o->src[2] = fixed_temp(POS_REG, 0);
		o->dk = D_FIXED;
		o->dreg = POS_REG;
		o->dlane = 0;
		c->max_uniform = slot;
		c->prog->vp_slot = slot;
	}
	if (c->prog->scissor) {
		/* the scissor as four clip planes in o5.xyzw: each a dot product of the clip position with a constant
		 * (a, 0, 0, b) the draw fills in from the viewport and the scissor rectangle */
		int slot = c->max_uniform + 1, k;

		for (k = 0; k < 4; k++) {
			struct vop *o = add_op(c, MBX_VGP_DP4);

			o->n = 2;
			o->src[0] = fixed_temp(POS_REG, 0);
			o->src[1] = (struct vsrc){ .kind = V_CONST, .idx = slot + k, .lane = 0 };
			o->vsrcs = true;
			o->dk = D_OUT;
			o->dreg = 5;
			o->dlane = k;
		}
		c->max_uniform = slot + 3;
		c->prog->scissor_slot = slot;
	}
	{
		struct vop *o = add_op(c, MBX_VGP_MOV);	/* o0 = r13, the four lanes at once */

		o->n = 1;
		o->src[0] = fixed_temp(POS_REG, 0);
		o->dk = D_OUT;
		o->dreg = 0;
		o->vec = true;
		/* no depth clip in the hardware (README assumption 14): o7.x = z + w, o7.y = w - z */
		o = add_op(c, MBX_VGP_ADD);
		o->n = 2;
		o->src[0] = fixed_temp(POS_REG, 2);
		o->src[1] = fixed_temp(POS_REG, 3);
		o->dk = D_OUT;
		o->dreg = 7;
		o->dlane = 0;
		o = add_op(c, MBX_VGP_ADD);
		o->n = 2;
		o->src[0] = fixed_temp(POS_REG, 3);
		o->src[1] = negated(fixed_temp(POS_REG, 2));
		o->dk = D_OUT;
		o->dreg = 7;
		o->dlane = 1;
	}
	if (fs->nfsu) {		/* the fragment uniforms the colour needs, as program constants after the others */
		int slot = c->max_uniform + 1;

		c->prog->fsu_base = slot;
		c->prog->nfsu = fs->nfsu;
		memcpy(c->prog->fsu_vec, fs->fsu_vec, sizeof(fs->fsu_vec));
		c->max_uniform = slot + (int)fs->nfsu - 1;
	}
	/*
	 * All four lanes are worked out and counted before any is written: a value that feeds several lanes (the vertex shader's
	 * own varying read back, one scalar times a constant vector) has one register, and write_dest retargets a value's producer
	 * to the output when it is the value's only use, which the first lane's write would be if the others were not counted yet.
	 */
	{
		struct vsrc v4[4];

		for (lane = 0; lane < 4; lane++) {
			const struct mbx_fs_lane *L = &fs->lane[lane];

			v4[lane] = L->prim >= 0 ? fs_eval(c, L->prim) : imm(1.0f);	/* the primary colour: the texel's factor, or the whole value */
			if (c->why)
				return false;
			if (v4[lane].kind == V_VREG)
				c->vuse[v4[lane].idx]++;
		}
		for (lane = 0; lane < 4; lane++)
			write_dest(c, v4[lane], D_OUT, 1, bgra[lane]);
		if (fs->has_off) {
			/* the offset colour o2: the additive terms (red, green, blue; its alpha is the fog factor, unused) */
			for (lane = 0; lane < 4; lane++) {
				const struct mbx_fs_lane *L = &fs->lane[lane];

				v4[lane] = lane < 3 && L->off >= 0 ? fs_eval(c, L->off) : imm(0.0f);
				if (c->why)
					return false;
				if (v4[lane].kind == V_VREG)
					c->vuse[v4[lane].idx]++;
			}
			for (lane = 0; lane < 4; lane++)
				write_dest(c, v4[lane], D_OUT, 2, bgra[lane]);
		}
	}
	for (unsigned u = 0; u < MBX_MAX_TEX; u++) {
		/* unit u's texture coordinate into o(3 + u).xy, times (width / padded width, height / padded height): the
		 * chip reads a power-of-two texture and the padding repeats the edge texels */
		int slot;

		if (!(fs->units >> u & 1))
			continue;
		slot = c->max_uniform + 1;
		for (lane = 0; lane < 2; lane++) {
			struct vsrc t = c->outs[fs->uv_loc[u][lane]][fs->uv_comp[u][lane]], scale = { .kind = V_CONST, .idx = slot, .lane = lane };

			if (t.kind == V_NONE)
				return fail(c, "the vertex shader does not write the texture coordinate the fragment shader reads");
			t = op2(c, MBX_VGP_MUL, t, scale);
			c->vuse[t.idx]++;
			write_dest(c, t, D_OUT, 3 + (int)u, lane);
		}
		c->prog->tex_scale_slot[u] = slot;
	}
	return true;
}

/* --- pass two: lanes, operand rules, encoding --- */

struct p2 {
	struct cc *c;
	int *vslot;			/* vreg -> slot (reg * 4 + lane), -1 none */
	bool used[NTEMP_SLOTS];
	int ninsn;
	float imm_val[128];
	int nimm, imm_base;
	bool ok;
};

static int slot_alloc(struct p2 *p)
{
	for (int i = 0; i < NTEMP_SLOTS; i++)
		if (!p->used[i]) {
			p->used[i] = true;
			return i;
		}
	return -1;
}

/* The slot of an immediate: values pack four to a constant register after the uniforms */
static int imm_slot(struct p2 *p, float f)
{
	for (int i = 0; i < p->nimm; i++)
		if (!memcmp(&p->imm_val[i], &f, 4))
			return i;
	p->imm_val[p->nimm] = f;
	return p->nimm++;
}

static bool encode_one(struct p2 *p, enum mbx_vgp_op op, int dst, int mask, struct mbx_vgp_src *src, int n)
{
	uint32_t w[2];
	int ret;

	if (p->ninsn >= MBX_VGP_NUM_INSNS) {
		p->c->why = "the vertex program is longer than 256 instructions";
		return false;
	}
	ret = mbx_vgp_encode(w, op, dst, mask, src, n);
	if (ret) {
		mesa_loge("mbx: VGP encode failed (%d) for op %d", ret, op);
		p->c->why = "an instruction the VGP cannot encode";
		return false;
	}
	p->c->prog->code[p->ninsn][0] = w[0];
	p->c->prog->code[p->ninsn][1] = w[1];
	p->ninsn++;
	return true;
}

/* A value as an operand; its slot must already be known */
static struct mbx_vgp_src to_src(struct p2 *p, const struct vsrc *s)
{
	struct mbx_vgp_src r = { 0 };
	int lane = s->lane, slot;

	r.negate = s->neg;
	switch (s->kind) {
	case V_INPUT:
		r.file = MBX_VGP_INPUT;
		r.index = s->idx;
		break;
	case V_CONST:
		r.file = MBX_VGP_CONST;
		r.index = s->idx;
		break;
	case V_IMM:
		slot = imm_slot(p, s->imm);
		r.file = MBX_VGP_CONST;
		r.index = p->imm_base + slot / 4;
		lane = slot % 4;
		break;
	case V_VREG:
		slot = p->vslot[s->idx];
		r.file = MBX_VGP_TEMP;
		r.index = slot / 4;
		lane = slot % 4;
		break;
	case V_FIXED:
		r.file = MBX_VGP_TEMP;
		r.index = s->idx;
		break;
	default:
		break;
	}
	r.swizzle = (uint8_t)MBX_VGP_SWZ(lane, lane, lane, lane);
	return r;
}

static bool pass_two(struct cc *c)
{
	struct p2 p = { .c = c, .ok = true };
	struct mbx_vgp_program *prog = c->prog;
	unsigned i;

	p.vslot = malloc((c->nvreg + 1) * sizeof(int));
	for (int v = 0; v < c->nvreg; v++)
		p.vslot[v] = -1;
	prog->nuniform = c->max_uniform + 1;
	p.imm_base = (int)prog->nuniform;

	/* the immediates first, so their registers are known when instructions are encoded */
	for (i = 0; i < c->nops; i++)
		for (int k = 0; k < c->ops[i].n; k++)
			if (c->ops[i].src[k].kind == V_IMM)
				imm_slot(&p, c->ops[i].src[k].imm);
	if (p.imm_base + (p.nimm + 3) / 4 > MBX_MAX_CONSTS) {
		mbx_dbg("constants: uniforms %d (max_uniform %d), immediates %d", p.imm_base, c->max_uniform, p.nimm);
		return fail(c, "more than 128 constants");
	}

	for (i = 0; i < c->nops; i++) {
		struct vop *o = &c->ops[i];
		struct mbx_vgp_src src[3];
		int tmp_slot[3] = { -1, -1, -1 }, cidx = -1, iidx = -1, dst, mask, k, dslot = -1;

		if (o->dk == D_VREG && c->vuse[o->dreg] == 0)
			continue;	/* dead */

		/* one constant register and one input register an instruction: copy extra ones */
		for (k = 0; k < o->n; k++) {
			struct vsrc *s = &o->src[k];
			int creg = -1, ireg = -1;

			if (s->kind == V_CONST)
				creg = s->idx;
			else if (s->kind == V_IMM)
				creg = p.imm_base + imm_slot(&p, s->imm) / 4;
			else if (s->kind == V_INPUT)
				ireg = s->idx;
			if ((creg >= 0 && cidx >= 0 && creg != cidx) || (ireg >= 0 && iidx >= 0 && ireg != iidx)) {
				struct mbx_vgp_src msrc[1];

				tmp_slot[k] = slot_alloc(&p);
				if (tmp_slot[k] < 0)
					return fail(c, "out of temporaries");
				msrc[0] = to_src(&p, s);
				if (!encode_one(&p, MBX_VGP_MOV, MBX_VGP_TMP(tmp_slot[k] / 4), 1 << (tmp_slot[k] % 4), msrc, 1))
					return false;
			} else {
				if (creg >= 0)
					cidx = creg;
				if (ireg >= 0)
					iidx = ireg;
			}
		}
		for (k = 0; k < o->n; k++) {
			if (tmp_slot[k] >= 0) {
				src[k] = (struct mbx_vgp_src){ .file = MBX_VGP_TEMP, .index = tmp_slot[k] / 4,
							       .swizzle = (uint8_t)MBX_VGP_SWZ(tmp_slot[k] % 4, tmp_slot[k] % 4, tmp_slot[k] % 4, tmp_slot[k] % 4) };
			} else {
				src[k] = to_src(&p, &o->src[k]);
			}
		}
		if (o->vec)
			src[0].swizzle = MBX_VGP_XYZW;
		if (o->vsrcs)
			for (k = 0; k < o->n; k++)
				src[k].swizzle = MBX_VGP_XYZW;

		/* a lane dies at its last reader; its slot can be this result's */
		for (k = 0; k < o->n; k++)
			if (tmp_slot[k] >= 0)
				p.used[tmp_slot[k]] = false;
		for (k = 0; k < o->n; k++) {
			struct vsrc *s = &o->src[k];

			if (s->kind == V_VREG && c->vlast[s->idx] == (int)i && p.vslot[s->idx] >= 0)
				p.used[p.vslot[s->idx]] = false;
		}
		switch (o->dk) {
		case D_VREG:
			dslot = slot_alloc(&p);
			if (dslot < 0)
				return fail(c, "out of temporaries (too many values live at once)");
			p.vslot[o->dreg] = dslot;
			dst = MBX_VGP_TMP(dslot / 4);
			mask = 1 << (dslot % 4);
			break;
		case D_FIXED:
			dst = MBX_VGP_TMP(o->dreg);
			mask = 1 << o->dlane;
			break;
		default:
			dst = MBX_VGP_OUT(o->dreg);
			mask = o->vec ? 0xf : 1 << o->dlane;
			break;
		}
		if (!encode_one(&p, o->op, dst, mask, src, o->n))
			return false;
	}

	prog->ninsn = (unsigned)p.ninsn;
	prog->nconst = prog->nuniform + (p.nimm + 3) / 4;
	for (int j = 0; j < p.nimm; j++)
		prog->consts[prog->nuniform + j / 4][j % 4] = p.imm_val[j];
	free(p.vslot);
	return true;
}

bool mbx_compile_vs(nir_shader *vs, const struct mbx_fs_info *fs, struct mbx_vgp_program *prog)
{
	struct cc c = { .nir = vs, .fs = fs, .prog = prog, .max_uniform = -1 };
	bool ok = pass_one(&c) && pass_two(&c);

	if (ok) {
		struct vsrc ps = c.outs[VARYING_SLOT_PSIZ][0];

		prog->psize_kind = MBX_PSIZE_STATE;	/* not written: the rasterizer state's size */
		if (ps.kind == V_IMM) {
			prog->psize_kind = MBX_PSIZE_IMM;
			prog->psize = ps.imm;
		} else if (ps.kind == V_CONST) {
			prog->psize_kind = MBX_PSIZE_UNIFORM;
			prog->psize_slot = ps.idx;
			prog->psize_lane = ps.lane;
		} else if (ps.kind == V_INPUT) {
			prog->psize_kind = MBX_PSIZE_INPUT;	/* per vertex, from an attribute: one object per point */
			prog->psize_slot = ps.idx;
			prog->psize_lane = ps.lane;
		} else if (ps.kind != V_NONE) {
			mesa_logw_once("mbx: gl_PointSize computed in the shader (not an attribute, constant or uniform) is not supported (one size per draw)");
		}
	}

	if (!ok)
		mesa_loge("mbx: vertex shader not compiled: %s", c.why ? c.why : "unknown");
	free(c.ops);
	free(c.vdef);
	free(c.vuse);
	free(c.vlast);
	free(c.val);
	return ok;
}

/* --- the fragment side --- */

/*
 * What the fixed pipe can do with a fragment shader. Each lane of gl_FragColor is parsed into a sum of terms, each a
 * product of texels (unit 0, unit 1, each at most once) and factors the vertex program can compute (constants, varyings,
 * fragment uniforms). Terms are grouped by the texels they carry; what the chip draws is
 *
 *     texel units' product x primary colour   (the term with texels: the TSP combiner, replace or modulate)
 *   + offset colour                           (the term without: added after the units, 8 bits, red green blue only)
 *
 * with the primary and the offset colour computed per vertex in the vertex program: exact because the result is affine
 * in the varyings, so the hardware's interpolation commutes with it. A shader with no texel at all is one sum, the
 * primary colour. Colour and alpha have their own combiner halves, so the texel units may differ between lane 3 and the rest.
 */
struct fsp {
	struct mbx_fs_info *info;
	nir_tex_instr *tex[MBX_MAX_TEX];
	unsigned lane;		/* the lane being parsed: a texel must be this lane's component, or (red, blue) the other of the two */
	unsigned swap_seen, plain_seen;	/* lanes 0 and 2 that read the other one's texel component / their own */
	bool bad;
	nir_intrinsic_instr *disc;	/* a discard / terminate under a condition */
};

struct fs_term {
	int coef;		/* a node, or -1 for 1 */
	unsigned mask;		/* the texel units multiplied in */
};

#define FS_MAX_TERMS 8

static int fs_node3(struct fsp *p, enum mbx_fs_op op, int a, int b, int cc, float k, unsigned loc, unsigned comp)
{
	struct mbx_fs_info *i = p->info;
	struct mbx_fs_node *n;

	if (i->nnode >= MBX_MAX_NODES) {
		p->bad = true;
		return 0;
	}
	n = &i->node[i->nnode];
	n->op = op;
	n->a = (uint16_t)a;
	n->b = (uint16_t)b;
	n->c = (uint16_t)cc;
	n->k = k;
	n->loc = loc;
	n->comp = comp;
	return (int)i->nnode++;
}

static int fs_node(struct fsp *p, enum mbx_fs_op op, int a, int b, float k, unsigned loc, unsigned comp)
{
	return fs_node3(p, op, a, b, 0, k, loc, comp);
}

static int fs_one(struct fsp *p, int c)
{
	return c >= 0 ? c : fs_node(p, FSN_CONST, 0, 0, 1.0f, 0, 0);
}

static int fs_mulc(struct fsp *p, int a, int b)
{
	if (a < 0)
		return b;
	if (b < 0)
		return a;
	return fs_node(p, FSN_MUL, a, b, 0, 0, 0);
}

static int fs_negc(struct fsp *p, int a)
{
	return a < 0 ? fs_node(p, FSN_CONST, 0, 0, -1.0f, 0, 0) : fs_node(p, FSN_NEG, a, 0, 0, 0, 0);
}

static int fs_addc(struct fsp *p, int a, int b)
{
	return fs_node(p, FSN_ADD, fs_one(p, a), fs_one(p, b), 0, 0, 0);
}

static unsigned fs_uniform_index(struct fsp *p, unsigned slot)
{
	struct mbx_fs_info *i = p->info;
	unsigned j;

	for (j = 0; j < i->nfsu; j++)
		if (i->fsu_vec[j] == slot)
			return j;
	if (i->nfsu == MBX_MAX_FSU) {
		p->bad = true;
		return 0;
	}
	i->fsu_vec[i->nfsu] = slot;
	return i->nfsu++;
}

static int fs_terms(struct fsp *p, nir_scalar s, struct fs_term *out);

static bool fs_has_tex(struct fsp *p, nir_scalar s)
{
	s = nir_scalar_chase_movs(s);
	if (nir_scalar_is_const(s))
		return false;
	for (unsigned k = 0; k < MBX_MAX_TEX; k++)
		if (p->tex[k] && nir_def_instr(s.def) == &p->tex[k]->instr)
			return true;
	if (nir_scalar_is_alu(s)) {
		for (unsigned i = 0; i < nir_op_infos[nir_scalar_alu_op(s)].num_inputs; i++)
			if (fs_has_tex(p, nir_scalar_chase_alu_src(s, i)))
				return true;
	}
	return false;
}

/* The operations the vertex program can do on the fragment shader's behalf beyond sums and products (README assumption 48) */
static bool fs_op_ok(nir_op op)
{
	switch (op) {
	case nir_op_fmin: case nir_op_fmax: case nir_op_fabs: case nir_op_slt: case nir_op_sge: case nir_op_seq: case nir_op_sne:
	case nir_op_frcp: case nir_op_frsq: case nir_op_fexp2: case nir_op_flog2: case nir_op_ffract: case nir_op_fcsel:
		return true;
	default:
		return false;
	}
}

/* A texel-free expression as a tree of nodes (any shape the vertex program can evaluate), or -1 */
static int fs_expr(struct fsp *p, nir_scalar s)
{
	int a, b, c3;
	nir_op op;

	s = nir_scalar_chase_movs(s);
	if (nir_scalar_is_const(s))
		return fs_node(p, FSN_CONST, 0, 0, nir_scalar_as_float(s), 0, 0);
	if (nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_input) {
		nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);

		return fs_node(p, FSN_VAR, 0, 0, 0, nir_intrinsic_io_semantics(in).location, nir_intrinsic_component(in) + s.comp);
	}
	if (nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_uniform) {
		nir_intrinsic_instr *un = nir_def_as_intrinsic(s.def);

		if (!nir_src_is_const(un->src[0]))
			return -1;
		return fs_node(p, FSN_UNI, 0, 0, 0, fs_uniform_index(p, nir_intrinsic_base(un) + nir_src_as_uint(un->src[0])), s.comp);
	}
	if (!nir_scalar_is_alu(s))
		return -1;
	op = nir_scalar_alu_op(s);
	a = fs_expr(p, nir_scalar_chase_alu_src(s, 0));
	if (a < 0)
		return -1;
	if (op == nir_op_fneg)
		return fs_node(p, FSN_NEG, a, 0, 0, 0, 0);
	b = nir_op_infos[op].num_inputs > 1 ? fs_expr(p, nir_scalar_chase_alu_src(s, 1)) : 0;
	c3 = nir_op_infos[op].num_inputs > 2 ? fs_expr(p, nir_scalar_chase_alu_src(s, 2)) : 0;
	if (b < 0 || c3 < 0)
		return -1;
	switch (op) {
	case nir_op_fmul: return fs_node(p, FSN_MUL, a, b, 0, 0, 0);
	case nir_op_fadd: return fs_node(p, FSN_ADD, a, b, 0, 0, 0);
	case nir_op_fsub: return fs_node(p, FSN_SUB, a, b, 0, 0, 0);
	case nir_op_ffma: return fs_node(p, FSN_ADD, fs_node(p, FSN_MUL, a, b, 0, 0, 0), c3, 0, 0, 0);
	case nir_op_flrp:	/* a + t * (b - a) */
		return fs_node(p, FSN_ADD, a, fs_node(p, FSN_MUL, c3, fs_node(p, FSN_SUB, b, a, 0, 0, 0), 0, 0, 0), 0, 0, 0);
	default:
		if (!fs_op_ok(op))
			return -1;
		return fs_node3(p, FSN_OP, a, b, c3, 0, (unsigned)op, 0);
	}
}

static int fs_concat(struct fs_term *out, int na, const struct fs_term *b, int nb)
{
	if (na < 0 || nb < 0 || na + nb > FS_MAX_TERMS)
		return -1;
	memcpy(out + na, b, sizeof(*b) * (size_t)nb);
	return na + nb;
}

static void fs_neg_terms(struct fsp *p, struct fs_term *t, int n)
{
	for (int k = 0; k < n; k++)
		t[k].coef = fs_negc(p, t[k].coef);
}

/* The product of two term lists */
static int fs_mul_terms(struct fsp *p, const struct fs_term *a, int na, const struct fs_term *b, int nb, struct fs_term *out)
{
	int n = 0;

	if (na < 0 || nb < 0 || na * nb > FS_MAX_TERMS)
		return -1;
	for (int i = 0; i < na; i++)
		for (int j = 0; j < nb; j++) {
			if (a[i].mask & b[j].mask)
				return -1;	/* a texel squared */
			out[n].mask = a[i].mask | b[j].mask;
			out[n].coef = fs_mulc(p, a[i].coef, b[j].coef);
			n++;
		}
	return n;
}

static int fs_terms(struct fsp *p, nir_scalar s, struct fs_term *out)
{
	struct fs_term x[FS_MAX_TERMS], y[FS_MAX_TERMS], z[FS_MAX_TERMS];
	int nx, ny, nz;

	s = nir_scalar_chase_movs(s);
	if (nir_scalar_is_const(s)) {
		out[0] = (struct fs_term){ fs_node(p, FSN_CONST, 0, 0, nir_scalar_as_float(s), 0, 0), 0 };
		return 1;
	}
	for (unsigned k = 0; k < MBX_MAX_TEX; k++)
		if (p->tex[k] && nir_def_instr(s.def) == &p->tex[k]->instr) {
			if (s.comp != p->lane) {
				/* red from the texel's blue and the reverse: Weston's BGRA swizzle of a wl_shm buffer; the draw samples a
				 * copy of the texture with the two swapped */
				if (!((p->lane == 0 && s.comp == 2) || (p->lane == 2 && s.comp == 0)))
					return -1;
				p->swap_seen |= 1u << p->lane;
			} else if (p->lane == 0 || p->lane == 2) {
				p->plain_seen |= 1u << p->lane;
			}
			out[0] = (struct fs_term){ -1, 1u << k };
			return 1;
		}
	if (nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_input) {
		nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);

		out[0] = (struct fs_term){ fs_node(p, FSN_VAR, 0, 0, 0, nir_intrinsic_io_semantics(in).location,
						   nir_intrinsic_component(in) + s.comp), 0 };
		return 1;
	}
	if (nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_uniform) {
		nir_intrinsic_instr *un = nir_def_as_intrinsic(s.def);

		if (!nir_src_is_const(un->src[0]))
			return -1;
		/* the load starts at the first component of its vec4 slot */
		out[0] = (struct fs_term){ fs_node(p, FSN_UNI, 0, 0, 0,
						   fs_uniform_index(p, nir_intrinsic_base(un) + nir_src_as_uint(un->src[0])), s.comp), 0 };
		return 1;
	}
	if (!nir_scalar_is_alu(s))
		return -1;
	switch (nir_scalar_alu_op(s)) {
	case nir_op_fmul:
		nx = fs_terms(p, nir_scalar_chase_alu_src(s, 0), x);
		ny = fs_terms(p, nir_scalar_chase_alu_src(s, 1), y);
		return fs_mul_terms(p, x, nx, y, ny, out);
	case nir_op_fadd:
	case nir_op_fsub:
		nx = fs_terms(p, nir_scalar_chase_alu_src(s, 0), x);
		ny = fs_terms(p, nir_scalar_chase_alu_src(s, 1), y);
		if (ny < 0)
			return -1;
		if (nir_scalar_alu_op(s) == nir_op_fsub)
			fs_neg_terms(p, y, ny);
		memcpy(out, x, sizeof(*x) * (size_t)(nx < 0 ? 0 : nx));
		return fs_concat(out, nx, y, ny);
	case nir_op_fneg:
		nx = fs_terms(p, nir_scalar_chase_alu_src(s, 0), out);
		if (nx > 0)
			fs_neg_terms(p, out, nx);
		return nx;
	case nir_op_ffma:		/* a * b + c */
		nx = fs_terms(p, nir_scalar_chase_alu_src(s, 0), x);
		ny = fs_terms(p, nir_scalar_chase_alu_src(s, 1), y);
		nz = fs_terms(p, nir_scalar_chase_alu_src(s, 2), z);
		nx = fs_mul_terms(p, x, nx, y, ny, out);
		if (nz < 0)
			return -1;
		return fs_concat(out, nx, z, nz);
	case nir_op_flrp: {		/* a * (1 - t) + b * t */
		struct fs_term t[FS_MAX_TERMS], omt[FS_MAX_TERMS + 1], p1[FS_MAX_TERMS], p2[FS_MAX_TERMS];
		int nt, nomt, n1, n2;

		nx = fs_terms(p, nir_scalar_chase_alu_src(s, 0), x);
		ny = fs_terms(p, nir_scalar_chase_alu_src(s, 1), y);
		nt = fs_terms(p, nir_scalar_chase_alu_src(s, 2), t);
		if (nx < 0 || ny < 0 || nt < 0 || nt + 1 > FS_MAX_TERMS)
			return -1;
		omt[0] = (struct fs_term){ -1, 0 };
		memcpy(omt + 1, t, sizeof(*t) * (size_t)nt);
		fs_neg_terms(p, omt + 1, nt);
		nomt = nt + 1;
		n1 = fs_mul_terms(p, x, nx, omt, nomt, p1);
		n2 = fs_mul_terms(p, y, ny, t, nt, p2);
		if (n1 < 0 || n2 < 0)
			return -1;
		memcpy(out, p1, sizeof(*p1) * (size_t)n1);
		return fs_concat(out, n1, p2, n2);
	}
	default:
		break;
	}
	/* anything else without a texel in it: an expression the vertex program evaluates (grid mode) */
	if (!fs_has_tex(p, s)) {
		int e = fs_expr(p, s);

		if (e >= 0) {
			out[0] = (struct fs_term){ e, 0 };
			return 1;
		}
	}
	return -1;
}

/* The degree in the varyings (the coefficient must be affine) and whether the expression subtracts */
static int fs_degree(const struct mbx_fs_info *i, int n, bool *subtracts);
/*
 * A step in the colour: a comparison, a fract or a select whose input depends on a varying. The grid mode (README assumption 48) evaluates the
 * colour at the corners of cells of about 8 pixels and the chip interpolates between them: right for smooth functions, a ramp where a step
 * should be (dEQP shaders.operator.float_compare.*_fragment). Such a shader is left to the CPU, which draws it exactly.
 */
static bool fs_has_step(const struct mbx_fs_info *i, int n)
{
	const struct mbx_fs_node *nd = &i->node[n];
	bool sub;

	switch (nd->op) {
	case FSN_CONST: case FSN_UNI: case FSN_VAR:
		return false;
	case FSN_NEG:
		return fs_has_step(i, nd->a);
	case FSN_ADD: case FSN_SUB: case FSN_MUL:
		return fs_has_step(i, nd->a) || fs_has_step(i, nd->b);
	case FSN_OP: {
		unsigned nin = nir_op_infos[nd->loc].num_inputs;
		const uint16_t ops[3] = { nd->a, nd->b, nd->c };

		for (unsigned k = 0; k < nin && k < 3; k++) {
			if (fs_has_step(i, ops[k]))
				return true;
		}
		/* an equality with a constant is a conversion to bool (vec to bvec; a `> 0` is a step and is not exempt: dEQP operator.selection), the dEQP conversion and swizzle shaders feed it constant varyings */
		bool vs_const = (nd->loc == nir_op_seq || nd->loc == nir_op_sne) &&
				(i->node[nd->a].op == FSN_CONST || i->node[nd->b].op == FSN_CONST);
		if (!vs_const && (nd->loc == nir_op_slt || nd->loc == nir_op_sge || nd->loc == nir_op_seq || nd->loc == nir_op_sne ||
				  nd->loc == nir_op_ffract || nd->loc == nir_op_fcsel))
			for (unsigned k = 0; k < nin && k < 3; k++) {
				sub = false;
				const struct mbx_fs_node *in = &i->node[ops[k]];

				/* the result of another comparison is 0 or 1 (a boolean): comparing booleans is not a step of the colour */
				if (in->op == FSN_OP && (in->loc == nir_op_slt || in->loc == nir_op_sge || in->loc == nir_op_seq || in->loc == nir_op_sne))
					continue;
				if (fs_degree(i, ops[k], &sub) > 0)
					return true;
			}
		return false;
	}
	default:
		return false;
	}
}

static int fs_degree(const struct mbx_fs_info *i, int n, bool *subtracts)
{
	const struct mbx_fs_node *nd = &i->node[n];
	int da, db;

	switch (nd->op) {
	case FSN_VAR: return 1;
	case FSN_CONST: case FSN_UNI: return 0;
	case FSN_OP:
		return 2;			/* not affine: the grid */
	case FSN_NEG:
		*subtracts = true;
		return fs_degree(i, nd->a, subtracts);
	case FSN_ADD: case FSN_SUB:
		da = fs_degree(i, nd->a, subtracts);
		db = fs_degree(i, nd->b, subtracts);
		return da > db ? da : db;
	default:
		da = fs_degree(i, nd->a, subtracts);
		db = fs_degree(i, nd->b, subtracts);
		return da + db;
	}
}

/* A lane's terms grouped by texel mask: the texel term's coefficient, the plain term's */
static bool fs_lane(struct fsp *p, nir_scalar s, struct mbx_fs_lane *L)
{
	struct fs_term t[FS_MAX_TERMS];
	int n, k;
	int coef[4], have[4] = { 0 };
	unsigned m = 0;

	p->lane = (unsigned)(L - p->info->lane);
	n = fs_terms(p, s, t);
	if (n <= 0 || p->bad)
		return false;
	for (k = 0; k < 4; k++)
		coef[k] = -1;
	for (k = 0; k < n; k++) {
		unsigned mk = t[k].mask;

		if (mk && m && mk != m)
			return false;	/* two different texel products in a lane: the chip's combiner has one */
		if (mk)
			m = mk;
		coef[mk] = have[mk] ? fs_addc(p, coef[mk], t[k].coef) : t[k].coef;
		have[mk] = 1;
	}
	L->tex = m;
	L->prim = have[m] ? coef[m] : -1;
	L->off = m && have[0] ? coef[0] : -1;
	if (m && !have[m])
		return false;
	return !p->bad;
}

/*
 * "if (alpha < k) discard;" and its three mirrors, with the alpha the shader's own gl_FragColor.a: the chip's alpha
 * function (alpha word: function in bits 29-31 as the depth test numbers them, the reference in bits 21-28) does it
 * (README assumption 45). The reference is the constant's 8-bit threshold on the side that keeps the pixels the shader keeps.
 */
static bool fs_alpha_test(nir_intrinsic_instr *disc, nir_intrinsic_instr *store, struct mbx_fs_info *info)
{
	nir_scalar cond = nir_scalar_resolved(disc->src[0].ssa, 0), a, k, alpha = nir_scalar_resolved(store->src[0].ssa, 3);
	nir_alu_instr *alu;
	bool const_second;
	float t;
	int func;

	if (!nir_scalar_is_alu(cond))
		return false;
	alu = nir_def_as_alu(cond.def);
	if (alu->op != nir_op_slt && alu->op != nir_op_sge && alu->op != nir_op_flt && alu->op != nir_op_fge)
		return false;
	a = nir_scalar_chase_alu_src(cond, 0);
	k = nir_scalar_chase_alu_src(cond, 1);
	const_second = nir_scalar_is_const(k);
	if (!const_second) {
		nir_scalar tmp = a;

		a = k;
		k = tmp;
		if (!nir_scalar_is_const(k))
			return false;
	}
	if (a.def != alpha.def || a.comp != alpha.comp)
		return false;			/* the test must be on the colour's alpha lane itself */
	t = nir_scalar_as_float(k) * 255.0f;
	if (t < 0.0f || t > 255.0f)
		return false;
	/* discard when (a OP k) or (k OP a): the pixels that stay satisfy the opposite */
	if (alu->op == nir_op_slt || alu->op == nir_op_flt)
		func = const_second ? 6 : 3;	/* a < k discarded: keep a >= k; k < a discarded: keep a <= k */
	else
		func = const_second ? 1 : 4;	/* a >= k discarded: keep a < k; k >= a discarded: keep a > k */
	/* a keep-side with ">=" or "<" rounds the threshold up, "<=" or ">" rounds it down (the alpha is 8 bits) */
	info->alpha_word = (uint32_t)func << 29 | ((func == 6 || func == 1) ? (uint32_t)ceilf(t) : (uint32_t)floorf(t)) << 21;
	return true;
}

void mbx_fs_match(nir_shader *fs, struct mbx_fs_info *info)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(fs);
	nir_intrinsic_instr *store = NULL;
	struct fsp p = { .info = info };
	unsigned i;

	memset(info, 0, sizeof(*info));
	info->kind = MBX_FS_UNSUPPORTED;
	if (MBX_ENV_SET("MBX_NIR"))
		nir_print_shader(fs, stderr);	/* debug: the fragment shader as the matcher sees it */
	if (exec_list_length(&impl->body) != 1)
		return;
	nir_foreach_instr(instr, nir_start_block(impl)) {
		switch (instr->type) {
		case nir_instr_type_load_const:
		case nir_instr_type_undef:
			break;
		case nir_instr_type_alu: {
			nir_alu_instr *alu = nir_instr_as_alu(instr);

			if (alu->op != nir_op_mov && alu->op != nir_op_vec2 && alu->op != nir_op_vec3 && alu->op != nir_op_vec4 &&
			    alu->op != nir_op_fmul && alu->op != nir_op_fadd && alu->op != nir_op_fsub && alu->op != nir_op_fneg &&
			    alu->op != nir_op_ffma && alu->op != nir_op_flrp && alu->op != nir_op_slt && alu->op != nir_op_sge &&
			    alu->op != nir_op_flt && alu->op != nir_op_fge && !fs_op_ok(alu->op))
				return;
			break;
		}
		case nir_instr_type_tex: {
			nir_tex_instr *t = nir_instr_as_tex(instr);

			/* a plain 2D sample of unit 0 or unit 1 (the chip has two), once each */
			if (t->op != nir_texop_tex || (t->sampler_dim != GLSL_SAMPLER_DIM_2D && t->sampler_dim != GLSL_SAMPLER_DIM_EXTERNAL) || t->is_array || t->is_shadow ||
			    t->texture_index >= MBX_MAX_TEX || p.tex[t->texture_index] ||
			    nir_tex_instr_src_index(t, nir_tex_src_coord) < 0)
				return;
			for (i = 0; i < t->num_srcs; i++)
				if (t->src[i].src_type != nir_tex_src_coord)
					return;	/* a bias, a projector, an offset: not yet */
			p.tex[t->texture_index] = t;
			break;
		}
		case nir_instr_type_intrinsic: {
			nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

			if (intr->intrinsic == nir_intrinsic_store_output) {
				unsigned loc = nir_intrinsic_io_semantics(intr).location;

				if ((loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0) || store)
					return;
				store = intr;
			} else if (intr->intrinsic == nir_intrinsic_terminate_if ||
				   intr->intrinsic == nir_intrinsic_demote_if) {
				if (p.disc)
					return;
				p.disc = intr;
			} else if (intr->intrinsic != nir_intrinsic_load_input && intr->intrinsic != nir_intrinsic_load_uniform) {
				return;	/* derivatives and the rest: not yet */
			}
			break;
		}
		default:
			return;
		}
	}
	if (!store || nir_src_num_components(store->src[0]) != 4)
		return;

	if (p.disc && !fs_alpha_test(p.disc, store, info))
		return;

	/* each sampled unit's coordinate: the first two components of one varying (or any two, each its own) */
	for (unsigned k = 0; k < MBX_MAX_TEX; k++) {
		nir_scalar u, v;
		nir_intrinsic_instr *iu, *iv;

		if (!p.tex[k])
			continue;
		u = nir_scalar_resolved(p.tex[k]->src[nir_tex_instr_src_index(p.tex[k], nir_tex_src_coord)].src.ssa, 0);
		v = nir_scalar_resolved(p.tex[k]->src[nir_tex_instr_src_index(p.tex[k], nir_tex_src_coord)].src.ssa, 1);
		if (!nir_scalar_is_intrinsic(u) || nir_scalar_intrinsic_op(u) != nir_intrinsic_load_input ||
		    !nir_scalar_is_intrinsic(v) || nir_scalar_intrinsic_op(v) != nir_intrinsic_load_input)
			return;
		iu = nir_def_as_intrinsic(u.def);
		iv = nir_def_as_intrinsic(v.def);
		info->uv_loc[k][0] = nir_intrinsic_io_semantics(iu).location;
		info->uv_comp[k][0] = nir_intrinsic_component(iu) + u.comp;
		info->uv_loc[k][1] = nir_intrinsic_io_semantics(iv).location;
		info->uv_comp[k][1] = nir_intrinsic_component(iv) + v.comp;
	}
	for (i = 0; i < 4; i++)
		if (!fs_lane(&p, nir_scalar_resolved(store->src[0].ssa, i), &info->lane[i]))
			return;

	if (p.swap_seen) {
		if (p.plain_seen)
			return;			/* only the pair swapped as a whole */
		info->swap_rb = true;
	}
	/* the texel units: one set for the colour lanes, one for alpha; the chain starts at unit 0 */
	info->mc = info->lane[0].tex;
	if (info->lane[1].tex != info->mc || info->lane[2].tex != info->mc)
		return;
	info->ma = info->lane[3].tex;
	info->units = info->mc | info->ma;
	info->tex = info->units != 0;
	for (unsigned k = 0; k < MBX_MAX_TEX; k++)
		if ((info->units >> k & 1) != (p.tex[k] != NULL))
			return;		/* a sample whose texel is not used, or used by only part of the lanes' sums */
	for (i = 0; i < 4; i++) {
		struct mbx_fs_lane *L = &info->lane[i];
		bool sub = false, sub_off = false;

		if (L->prim >= 0) {
			if (fs_has_step(info, L->prim))
				return;	/* a step in the colour: the grid would blur it, the CPU draws it */
			if (fs_degree(info, L->prim, &sub) > 1)
				info->grid = true;	/* not affine in the varyings: evaluated on a fine grid, the hardware interpolates between its corners */
			if (L->tex && info->node[L->prim].op == FSN_CONST &&
			    (info->node[L->prim].k > 1.0f || info->node[L->prim].k < 0.0f))
				return;	/* the primary colour is clamped to 0..1 on the chip: it cannot scale a texel up */
		}
		if (L->off >= 0) {
			if (fs_has_step(info, L->off))
				return;
			if (fs_degree(info, L->off, &sub_off) > 1)
				info->grid = true;
			if (sub_off)
				return;	/* the offset colour is clamped at 0: nothing can be taken away with it */
			if (i == 3 || !info->tex)
				return;	/* no alpha lane in the offset colour */
			info->has_off = true;
		}
		if (L->prim >= 0) {
			info->pass_c |= i < 3;
			info->pass_a |= i == 3;
		}
	}
	/* flat when no varying is read, or the varyings read are declared flat */
	info->flat = true;
	for (i = 0; i < info->nnode; i++) {
		nir_variable *var;

		if (info->node[i].op != FSN_VAR)
			continue;
		var = nir_find_variable_with_location(fs, nir_var_shader_in, info->node[i].loc);
		if (!(var && var->data.interpolation == INTERP_MODE_FLAT))
			info->flat = false;
	}
	info->kind = MBX_FS_MATCHED;
}
