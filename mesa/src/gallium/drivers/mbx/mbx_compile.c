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
	for (lane = 0; lane < 4; lane++) {
		struct vsrc v = c->outs[VARYING_SLOT_POS][lane];

		if (v.kind == V_NONE)
			v = imm(lane == 3 ? 1.0f : 0.0f);
		if (v.kind == V_VREG)
			c->vuse[v.idx]++;
		write_dest(c, v, D_FIXED, POS_REG, lane);
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
	for (lane = 0; lane < 4; lane++) {
		const struct mbx_fs_lane *L = &fs->lane[lane];
		struct vsrc v = { .kind = V_NONE };

		/* the lane's factors other than the texel, multiplied in the vertex program */
		if (L->has_var) {
			v = c->outs[L->var_loc][L->var_comp];
			if (v.kind == V_NONE)
				return fail(c, "the vertex shader does not write a varying the fragment shader reads");
		}
		if (L->has_uni) {
			struct vsrc u = { .kind = V_CONST, .idx = c->prog->fsu_base + (int)L->uni_index, .lane = (int)L->uni_comp };

			v = v.kind == V_NONE ? u : op2(c, MBX_VGP_MUL, v, u);
		}
		if (L->has_k && (L->k != 1.0f || v.kind == V_NONE))
			v = v.kind == V_NONE ? imm(L->k) : op2(c, MBX_VGP_MUL, v, imm(L->k));
		if (v.kind == V_NONE)
			v = imm(1.0f);	/* the texel alone */
		if (v.kind == V_VREG)
			c->vuse[v.idx]++;
		write_dest(c, v, D_OUT, 1, bgra[lane]);
	}
	if (fs->tex) {
		/* the texture coordinate into o3.xy, times (width / padded width, height / padded height): the
		 * chip reads a power-of-two texture and the padding repeats the edge texels */
		int slot = c->max_uniform + 1;

		for (lane = 0; lane < 2; lane++) {
			struct vsrc t = c->outs[fs->uv_loc[lane]][fs->uv_comp[lane]], scale = { .kind = V_CONST, .idx = slot, .lane = lane };

			if (t.kind == V_NONE)
				return fail(c, "the vertex shader does not write the texture coordinate the fragment shader reads");
			t = op2(c, MBX_VGP_MUL, t, scale);
			c->vuse[t.idx]++;
			write_dest(c, t, D_OUT, 3, lane);
		}
		c->prog->tex_scale_slot = slot;
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
	if (p.imm_base + (p.nimm + 3) / 4 > MBX_MAX_CONSTS)
		return fail(c, "more than 128 constants");

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
		} else if (ps.kind != V_NONE) {
			mesa_logw_once("mbx: gl_PointSize that varies per vertex is not supported (one size per draw)");
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
 * What the fixed pipe can do with a fragment shader: each lane of gl_FragColor is a product of factors, a texel of
 * unit 0 (the same lane), a varying's component, a fragment uniform's component, a constant. The hardware
 * interpolates what the vertex shader gives it, so everything but the texel is computed per vertex, in the
 * vertex program, and reaches the TSP as the primary colour: it is exact because each factor is linear across a
 * triangle (a varying) or constant (a uniform). The texel then multiplies it (modulate), or is it (replace).
 */
static bool fs_factors(nir_scalar s, struct mbx_fs_lane *L, nir_tex_instr *tex, unsigned lane)
{
	s = nir_scalar_chase_movs(s);
	if (nir_scalar_is_const(s)) {
		L->k *= nir_scalar_as_float(s);
		L->has_k = true;
		return true;
	}
	if (tex && nir_def_instr(s.def) == &tex->instr) {
		if (s.comp != lane || L->tex)
			return false;
		L->tex = true;
		return true;
	}
	if (nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_input) {
		nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);

		if (L->has_var)
			return false;
		L->has_var = true;
		L->var_loc = nir_intrinsic_io_semantics(in).location;
		L->var_comp = nir_intrinsic_component(in) + s.comp;
		return true;
	}
	if (nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_uniform) {
		nir_intrinsic_instr *un = nir_def_as_intrinsic(s.def);

		if (L->has_uni || !nir_src_is_const(un->src[0]))
			return false;
		L->has_uni = true;
		L->uni_slot = nir_intrinsic_base(un) + nir_src_as_uint(un->src[0]);
		L->uni_comp = s.comp;	/* the load starts at the first component of its vec4 slot */
		return true;
	}
	if (nir_scalar_is_alu(s) && nir_scalar_alu_op(s) == nir_op_fmul)
		return fs_factors(nir_scalar_chase_alu_src(s, 0), L, tex, lane) &&
		       fs_factors(nir_scalar_chase_alu_src(s, 1), L, tex, lane);
	return false;
}

void mbx_fs_match(nir_shader *fs, struct mbx_fs_info *info)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(fs);
	nir_intrinsic_instr *store = NULL;
	nir_tex_instr *tex = NULL;
	unsigned i, j;

	memset(info, 0, sizeof(*info));
	info->kind = MBX_FS_UNSUPPORTED;
	if (getenv("MBX_NIR"))
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

			if (alu->op != nir_op_mov && alu->op != nir_op_vec2 && alu->op != nir_op_vec3 &&
			    alu->op != nir_op_vec4 && alu->op != nir_op_fmul)
				return;
			break;
		}
		case nir_instr_type_tex: {
			nir_tex_instr *t = nir_instr_as_tex(instr);

			/* one plain 2D sample of unit 0 */
			if (tex || t->op != nir_texop_tex || t->sampler_dim != GLSL_SAMPLER_DIM_2D || t->is_array ||
			    t->is_shadow || t->texture_index != 0 || nir_tex_instr_src_index(t, nir_tex_src_coord) < 0)
				return;
			for (i = 0; i < t->num_srcs; i++)
				if (t->src[i].src_type != nir_tex_src_coord)
					return;	/* a bias, a projector, an offset: not yet */
			tex = t;
			break;
		}
		case nir_instr_type_intrinsic: {
			nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

			if (intr->intrinsic == nir_intrinsic_store_output) {
				unsigned loc = nir_intrinsic_io_semantics(intr).location;

				if ((loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0) || store)
					return;
				store = intr;
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

	if (tex) {
		nir_scalar u = nir_scalar_resolved(tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa, 0);
		nir_scalar v = nir_scalar_resolved(tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa, 1);

		if (!nir_scalar_is_intrinsic(u) || nir_scalar_intrinsic_op(u) != nir_intrinsic_load_input ||
		    !nir_scalar_is_intrinsic(v) || nir_scalar_intrinsic_op(v) != nir_intrinsic_load_input)
			return;
		nir_intrinsic_instr *iu = nir_def_as_intrinsic(u.def), *iv = nir_def_as_intrinsic(v.def);

		info->uv_loc[0] = nir_intrinsic_io_semantics(iu).location;
		info->uv_comp[0] = nir_intrinsic_component(iu) + u.comp;
		info->uv_loc[1] = nir_intrinsic_io_semantics(iv).location;
		info->uv_comp[1] = nir_intrinsic_component(iv) + v.comp;
		info->tex = true;
	}
	for (i = 0; i < 4; i++) {
		struct mbx_fs_lane *L = &info->lane[i];

		L->k = 1.0f;
		if (!fs_factors(nir_scalar_resolved(store->src[0].ssa, i), L, tex, i))
			return;
		if ((L->tex != 0) != (info->tex != 0))
			return;	/* the texel is in every lane or in none: it is one combiner */
	}
	/* the fragment uniforms the vertex program needs, each vec4 once */
	for (i = 0; i < 4; i++) {
		struct mbx_fs_lane *L = &info->lane[i];

		if (!L->has_uni)
			continue;
		for (j = 0; j < info->nfsu && info->fsu_vec[j] != L->uni_slot; j++)
			;
		if (j == info->nfsu) {
			if (info->nfsu == MBX_MAX_FSU)
				return;
			info->fsu_vec[info->nfsu++] = L->uni_slot;
		}
		L->uni_index = j;
	}
	/* flat when no lane takes a varying, or the varyings are declared flat */
	info->flat = true;
	for (i = 0; i < 4; i++) {
		struct mbx_fs_lane *L = &info->lane[i];
		nir_variable *var;

		if (!L->has_var)
			continue;
		var = nir_find_variable_with_location(fs, nir_var_shader_in, L->var_loc);
		if (!(var && var->data.interpolation == INTERP_MODE_FLAT))
			info->flat = false;
	}
	/* modulate when anything but the texel is a factor, else replace */
	for (i = 0; i < 4; i++)
		if (info->lane[i].has_var || info->lane[i].has_uni || (info->lane[i].has_k && info->lane[i].k != 1.0f))
			info->modulate = true;
	info->kind = MBX_FS_MATCHED;
}
