// SPDX-License-Identifier: MIT
/*
 * Shaders: the NIR the state tracker gives us, kept until a draw, when the
 * vertex shader is compiled to VGP and the fragment shader matched onto the
 * fixed pipe (mbx_compile.c).
 */
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir/nir_serialize.h"
#include "util/log.h"
#include "util/ralloc.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "mbx_context.h"
#include "mbx_fallback.h"
#include "mbx_program.h"

/*
 * A texture combiner word C (README assumption 38, 41): colour in bits 15-31 and alpha in 0-14, each A * B + C * D with
 * C = 1 - 1 = 0. Sources: A {konst, previous, texel, one}, B and D {primary, previous, texel, konst}, C {primary,
 * previous, texel, one}; the stock replace word is 0xd6887610, the model's decoding of the rest is texture.c.
 * unit 0: the texel times the primary colour (modulate), the texel alone (replace), or the primary colour (no texel here);
 * unit 1: the previous result times its texel, or the previous result.
 */
static uint32_t comb_word(unsigned has_texel, bool has_prim, unsigned unit)
{
	const uint32_t tail = 3u << 25 | 2u << 22 | 1u << 19;	/* C = one, complemented: zero; D = texel (unused) */

	if (unit == 0)
		return has_texel ? (has_prim ? 2u << 30 : 3u << 30 | 2u << 27) | tail : 3u << 30 | tail;
	return has_texel ? 1u << 30 | 2u << 27 | tail : 3u << 30 | 1u << 27 | tail;
}

static uint32_t comb_word_alpha(unsigned has_texel, bool has_prim, unsigned unit)
{
	const uint32_t tail = 3u << 9 | 1u << 4;

	if (unit == 0)
		return has_texel ? (has_prim ? 2u << 13 : 3u << 13 | 2u << 11) | tail : 3u << 13 | tail;
	return has_texel ? 1u << 13 | 2u << 11 | tail : 3u << 13 | 1u << 11 | tail;
}

/*
 * The combiner words of the texture units for a shader; l8: the units whose bound texture is L8, whose texel alpha is 1
 * where the chip returns the byte (so that lane takes the primary colour's alpha, as if the shader had no texel there).
 */
void mbx_tex_combiners(const struct mbx_fs_info *fi, unsigned ntex, unsigned l8, uint32_t out[MBX_MAX_TEX])
{
	for (unsigned u = 0; u < ntex; u++)
		out[u] = comb_word(fi->mc >> u & 1, fi->pass_c, u) |
			 comb_word_alpha((fi->ma & ~l8) >> u & 1, fi->pass_a, u);
}


/* The fragment shader's uniform loads: which vec4 slots, which components, and whether any is an integer */
static void mbx_scan_uniforms(struct mbx_shader *so)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(so->nir);
	so->nuslot = 0;
	nir_foreach_block(block, impl) {
		nir_foreach_instr(instr, block) {
			nir_intrinsic_instr *un;
			unsigned slot, k;

			if (instr->type != nir_instr_type_intrinsic)
				continue;
			un = nir_instr_as_intrinsic(instr);
			if (un->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(un->src[0]))
				continue;
			slot = nir_intrinsic_base(un) + nir_src_as_uint(un->src[0]);
			for (k = 0; k < so->nuslot && so->uslot[k].slot != slot; k++)
				;
			if (k == so->nuslot) {
				if (k == ARRAY_SIZE(so->uslot)) {
					so->nuslot = 0;
					return;
				}
				so->uslot[k].slot = slot;
				so->uslot[k].comps = 0;
				so->nuslot++;
			}
			so->uslot[k].comps |= BITFIELD_MASK(un->def.num_components);

		}
	}
}

static const float *mbx_fs_uniform_values(struct mbx_context *ctx)
{
	if (ctx->cb[1].user_buffer)
		return (const float *)((const uint8_t *)ctx->cb[1].user_buffer + ctx->cb[1].buffer_offset);
	if (ctx->cb[1].buffer)
		return (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[1].buffer)->bo) + ctx->cb[1].buffer_offset);
	return NULL;
}

/* Replace the folded slots' loads by their values, fold, and match again */
static void mbx_variant_make(struct mbx_shader *so, struct mbx_fs_variant *v)
{
	nir_shader *n = nir_shader_clone(NULL, so->nir);
	nir_function_impl *impl = nir_shader_get_entrypoint(n);

	nir_foreach_block(block, impl) {
		nir_foreach_instr_safe(instr, block) {
			nir_intrinsic_instr *un;
			nir_const_value c[4] = { 0 };
			nir_builder b;
			unsigned slot, k;

			if (instr->type != nir_instr_type_intrinsic)
				continue;
			un = nir_instr_as_intrinsic(instr);
			if (un->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(un->src[0]))
				continue;
			slot = nir_intrinsic_base(un) + nir_src_as_uint(un->src[0]);
			for (k = 0; k < so->nuslot && so->uslot[k].slot != slot; k++)
				;
			if (k == so->nuslot || !(v->fold >> k & 1))
				continue;
			for (unsigned i = 0; i < un->def.num_components; i++) {
				float f;

				memcpy(&f, &v->val[k][i], 4);
				/* the data is float (no native integers); an integer-typed load of it gets the integer */
				c[i].u32 = nir_alu_type_get_base_type(nir_intrinsic_dest_type(un)) == nir_type_float ? v->val[k][i] : (uint32_t)(int32_t)f;
			}
			b = nir_builder_at(nir_before_instr(instr));
			nir_def_rewrite_uses(&un->def, nir_build_imm(&b, un->def.num_components, 32, c));
			nir_instr_remove(instr);
		}
	}
	nir_lower_load_const_to_scalar(n);
	mbx_nir_optimize(n);
	mbx_fs_match(n, &v->info);
	ralloc_free(n);
}

/*
 * The match to draw with: the shader's own, or, for one that reads integer uniforms, a match made with the integers and
 * the 0/1 float uniforms as constants (kept per value set; a few). The integer uniforms are Weston's texture swizzles
 * on GLES2: the usual (identity) value reduces to a plain texture, the others stay on the CPU.
 */
const struct mbx_fs_info *mbx_fs_specialize(struct mbx_context *ctx, const struct mbx_fs_variant **variant)
{
	struct mbx_shader *so = ctx->fs;
	const float *u;
	struct mbx_fs_variant *v;
	uint32_t fold = 0, val[8][4] = { { 0 } };
	unsigned nvar = 0;

	*variant = NULL;
	if (!so)
		return NULL;
	if (!so->nuslot || (u = mbx_fs_uniform_values(ctx)) == NULL)
		return &so->fs_info;
	for (unsigned k = 0; k < so->nuslot; k++) {
		bool fold_it = true;

		for (unsigned c = 0; c < 4; c++) {
			float f = u[so->uslot[k].slot * 4 + c];

			if (so->uslot[k].comps >> c & 1) {
				memcpy(&val[k][c], &f, 4);
				if (!(f >= 0.0f && f <= 255.0f && f == floorf(f)))
					fold_it = false;	/* only whole numbers (switches, indices, 0 and 1) are folded in */
			}
		}
		if (fold_it)
			fold |= 1u << k;
	}
	for (v = so->variants; v; v = v->next, nvar++) {
		unsigned k;

		if (v->fold != fold)
			continue;
		for (k = 0; k < so->nuslot; k++)
			if ((fold >> k & 1) && memcmp(v->val[k], val[k], sizeof(val[k])))
				break;
		if (k == so->nuslot) {
			*variant = v;
			return &v->info;
		}
	}
	if (nvar >= 8)
		return &so->fs_info;		/* too many value sets: the unspecialised match (the CPU draws it) */
	v = CALLOC_STRUCT(mbx_fs_variant);
	if (!v)
		return &so->fs_info;
	v->fold = fold;
	memcpy(v->val, val, sizeof(val));
	mbx_variant_make(so, v);
	v->next = so->variants;
	so->variants = v;
	for (unsigned k = 0; k < so->nuslot; k++)
		mbx_dbg("  slot %u (comps %x)%s: %g %g %g %g", so->uslot[k].slot, so->uslot[k].comps, fold >> k & 1 ? " folded" : "",
			u[so->uslot[k].slot * 4], u[so->uslot[k].slot * 4 + 1], u[so->uslot[k].slot * 4 + 2], u[so->uslot[k].slot * 4 + 3]);
	mbx_dbg("fragment shader variant %u: fold mask %x -> %s", nvar, fold,
		v->info.kind == MBX_FS_MATCHED ? "matched" : "still on the CPU");
	*variant = v;
	return &v->info;
}

/*
 * The screen has no native integers: glUniform*i data is stored as floats, so softpipe, which has them, would read the
 * bits of 1.0 for an integer 1. The fallback's fragment shader gets its integer operations as float ones, with the
 * constant offsets of its loads kept integers (the pass converts those too).
 */
/* A shift by a constant is a multiplication: the int-to-float pass makes `a << n` of it with exp2(n), which softpipe's is not exact for
 * (dEQP functions.qualifiers.in_int: -1 << 1 was not -2) */
static void mbx_shifts_to_mul(nir_shader *n)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(n);
	bool progress = false;

	nir_foreach_block(block, impl) {
		nir_foreach_instr_safe(instr, block) {
			nir_alu_instr *alu;
			nir_builder b;
			nir_def *r;

			if (instr->type != nir_instr_type_alu)
				continue;
			alu = nir_instr_as_alu(instr);
			if (alu->op != nir_op_ishl || alu->def.bit_size != 32 || !nir_src_is_const(alu->src[1].src) || alu->def.num_components != 1)
				continue;
			if (nir_src_as_uint(alu->src[1].src) > 30)
				continue;
			b = nir_builder_at(nir_before_instr(instr));
			r = nir_imul(&b, nir_mov_alu(&b, alu->src[0], 1), nir_imm_int(&b, 1 << nir_src_as_uint(alu->src[1].src)))	/* not nir_imul_imm: it makes a shift of it */;
			nir_def_rewrite_uses(&alu->def, r);
			nir_instr_remove(instr);
			progress = true;
		}
	}
	nir_progress(progress, impl, nir_metadata_none);
}

/* The CPU's select is `cond * a + (1 - cond) * b`: a NaN in the branch not taken (the root of a negative: refract's k) spoils it. The chip's RSQ takes
 * the absolute value; the fallback's roots do too (dEQP operator.geometric.refract.*_float_float) */
static void mbx_roots_of_abs(nir_shader *n)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(n);
	bool progress = false;

	nir_foreach_block(block, impl) {
		nir_foreach_instr_safe(instr, block) {
			nir_alu_instr *alu;
			nir_builder b;
			nir_def *r;

			if (instr->type != nir_instr_type_alu)
				continue;
			alu = nir_instr_as_alu(instr);
			if ((alu->op != nir_op_frsq && alu->op != nir_op_fsqrt) || alu->def.bit_size != 32 || alu->def.num_components != 1)
				continue;
			b = nir_builder_at(nir_before_instr(instr));
			r = nir_build_alu1(&b, alu->op, nir_fabs(&b, nir_mov_alu(&b, alu->src[0], 1)));
			nir_def_rewrite_uses(&alu->def, r);
			nir_instr_remove(instr);
			progress = true;
		}
	}
	nir_progress(progress, impl, nir_metadata_none);
}

static void mbx_fallback_ints_to_float(nir_shader *n)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(n);

	/* 1-bit booleans are all ones when true: as a float that is a NaN, and feq of two of them is false (dEQP swizzles.*bvec*) */
	mbx_shifts_to_mul(n);
	mbx_roots_of_abs(n);
	nir_lower_int_to_float(n);	/* first: after the booleans became floats the integer compares are float ones and their constants stay integers */
	nir_lower_bool_to_float(n, false);
	nir_foreach_block(block, impl) {
		nir_foreach_instr(instr, block) {
			nir_intrinsic_instr *in;
			nir_load_const_instr *lc;
			nir_src *off;

			if (instr->type != nir_instr_type_intrinsic)
				continue;
			in = nir_instr_as_intrinsic(instr);
			if (in->intrinsic != nir_intrinsic_load_uniform && in->intrinsic != nir_intrinsic_load_input)
				continue;
			off = &in->src[0];
			if (!nir_src_is_const(*off))
				continue;
			lc = nir_def_as_load_const(off->ssa);
			/* a constant that is also used as a float (the 1 of a loop's `i++` is an array offset too: dEQP indexing.tmp_array
			 * ...dynamic_loop_read_fragment) is not changed for everyone: the offset gets a copy of its own */
			if (!list_is_singular(&lc->def.uses)) {
				nir_load_const_instr *nc = nir_load_const_instr_create(n, lc->def.num_components, lc->def.bit_size);

				nc->value[0] = lc->value[0];
				for (unsigned k = 0; k < lc->def.num_components; k++)
					nc->value[k] = lc->value[k];
				nir_instr_insert_before(&in->instr, &nc->instr);
				nir_src_rewrite(off, &nc->def);
				lc = nc;
			}
			for (unsigned k = 0; k < lc->def.num_components; k++) {
				float f = uif(lc->value[k].u32);

				if (lc->value[k].u32 >= 0x3f800000u && f == floorf(f) && f < 65536.0f)
					lc->value[k].u32 = (uint32_t)f;	/* an offset the pass turned into a float */
			}
		}
	}
}

/* The constant vec4 slots of a uniform's type that hold integers or booleans, struct fields and array elements one after another */
static void mark_int_slots(struct mbx_shader *so, const struct glsl_type *type, unsigned *slot)
{
	if (glsl_type_is_array(type)) {
		const struct glsl_type *e = glsl_get_array_element(type);

		for (unsigned i = 0; i < glsl_get_length(type); i++)
			mark_int_slots(so, e, slot);
		return;
	}
	if (glsl_type_is_struct(type)) {
		for (unsigned i = 0; i < glsl_get_length(type); i++)
			mark_int_slots(so, glsl_get_struct_field(type, i), slot);
		return;
	}
	unsigned n = glsl_count_attribute_slots(type, false);

	if (glsl_base_type_is_integer(glsl_get_base_type(type)) || glsl_type_is_boolean(type))
		for (unsigned k = 0; k < n; k++)
			if (*slot + k < 128)
				so->int_slots[(*slot + k) / 64] |= 1ull << ((*slot + k) % 64);
	*slot += n;
}


/* The sign of a float copied by bit operations (GLSL atan: ior(iand(a, 0x7fffffff), iand(b, 0x80000000))): neither the VGP nor the
 * fallback's int-to-float pass has bit operations on floats, so the pattern becomes float operations (found with dEQP
 * operator.angle_and_trigonometry.atan*) */
static bool copysign_src(nir_alu_instr *iand, uint32_t mask, nir_scalar *val)
{
	for (unsigned k = 0; k < 2; k++) {
		nir_scalar c = nir_scalar_resolved(iand->src[1 - k].src.ssa, iand->src[1 - k].swizzle[0]);

		if (nir_scalar_is_const(c) && nir_scalar_as_uint(c) == mask) {
			*val = nir_scalar_resolved(iand->src[k].src.ssa, iand->src[k].swizzle[0]);
			return true;
		}
	}
	return false;
}

static void mbx_lower_copysign_bits(nir_shader *n)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(n);
	bool progress = false;

	nir_foreach_block(block, impl) {
		nir_foreach_instr_safe(instr, block) {
			nir_alu_instr *alu, *m, *g;
			nir_scalar mag, sgn;
			bool found = false;
			nir_builder b;

			if (instr->type != nir_instr_type_alu)
				continue;
			alu = nir_instr_as_alu(instr);
			if (alu->op != nir_op_ior || alu->def.bit_size != 32 || alu->def.num_components != 1)
				continue;
			for (unsigned k = 0; k < 2; k++) {
				nir_instr *i0 = nir_def_instr(alu->src[k].src.ssa), *i1 = nir_def_instr(alu->src[1 - k].src.ssa);

				if (i0->type != nir_instr_type_alu || i1->type != nir_instr_type_alu)
					continue;
				m = nir_instr_as_alu(i0);
				g = nir_instr_as_alu(i1);
				if (m->op != nir_op_iand || g->op != nir_op_iand)
					continue;
				if (copysign_src(m, 0x7fffffffu, &mag) && copysign_src(g, 0x80000000u, &sgn)) {
					found = true;
					break;
				}
			}
			if (!found)
				continue;
			b = nir_builder_at(nir_before_instr(instr));
			{
				nir_def *a = nir_fabs(&b, nir_mov_scalar(&b, mag));
				nir_def *neg = nir_flt(&b, nir_mov_scalar(&b, sgn), nir_imm_float(&b, 0.0f));
				nir_def *r = nir_bcsel(&b, neg, nir_fneg(&b, a), a);

				nir_def_rewrite_uses(&alu->def, r);
				nir_instr_remove(instr);
				progress = true;
			}
		}
	}
	nir_progress(progress, impl, nir_metadata_none);
	if (progress)
		nir_opt_dce(n);	/* the bit operations left behind would still type their float inputs as integers (the int-to-float pass) */
}


/*
 * A load_uniform at a constant offset becomes one at offset 0 with the offset added to the base. softpipe's NIR-to-TGSI lowers uniform
 * loads to UBO ones and sets their alignment from `offset + base * 16` (the offset not scaled by 16), which puts a load at slot offset 4
 * or more into the wrong component of its slot (dEQP struct.uniform.*dynamic_index*: s[2].a read from .y).
 */
static void mbx_fold_uniform_offsets(nir_shader *n)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(n);
	bool progress = false;

	nir_foreach_block(block, impl) {
		nir_foreach_instr(instr, block) {
			nir_intrinsic_instr *in;

			if (instr->type != nir_instr_type_intrinsic)
				continue;
			in = nir_instr_as_intrinsic(instr);
			if (in->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0]) == 0)
				continue;
			nir_intrinsic_set_base(in, nir_intrinsic_base(in) + nir_src_as_uint(in->src[0]));
			{
				nir_builder b = nir_builder_at(nir_before_instr(instr));

				nir_src_rewrite(&in->src[0], nir_imm_int(&b, 0));
			}
			progress = true;
		}
	}
	nir_progress(progress, impl, nir_metadata_control_flow);
}

static void *mbx_shader_create(struct pipe_context *pctx, const struct pipe_shader_state *templ)
{
	struct mbx_shader *so = CALLOC_STRUCT(mbx_shader);

	if (!so)
		return NULL;
	if (templ->type != PIPE_SHADER_IR_NIR) {
		FREE(so);
		return NULL;
	}
	mbx_lower_copysign_bits(templ->ir.nir);
	if (getenv("MBX_DUMP_NIR_IN"))
		nir_print_shader(templ->ir.nir, stderr);
	so->nir_orig = nir_shader_clone(NULL, templ->ir.nir);	/* the fallback's, before our lowering */
	if (templ->ir.nir->info.stage == MESA_SHADER_FRAGMENT)
	{
		mbx_fallback_ints_to_float(so->nir_orig);
		if (getenv("MBX_DUMP_NIR"))
			nir_print_shader(so->nir_orig, stderr);
	}
	else if (templ->ir.nir->info.stage == MESA_SHADER_VERTEX)
		nir_foreach_variable_with_modes(var, so->nir_orig, nir_var_uniform) {
			unsigned slot = var->data.driver_location;

			mark_int_slots(so, var->type, &slot);
		}
	mbx_fold_uniform_offsets(so->nir_orig);
	so->nir = templ->ir.nir;	/* ours now */
	so->stage = so->nir->info.stage;
	mbx_nir_prepare(so->nir);
	if (so->stage == MESA_SHADER_FRAGMENT) {
		mbx_fs_match(so->nir, &so->fs_info);
		if (so->fs_info.kind != MBX_FS_MATCHED)
			mbx_scan_uniforms(so);
	}
	return so;
}

void mbx_program_destroy(struct mbx_vgp_program *p)
{
	FREE(p);
}

static void mbx_shader_delete(struct pipe_context *pctx, void *hwcso)
{
	struct mbx_shader *so = hwcso;
	struct mbx_context *ctx = mbx_context(pctx);
	struct mbx_vgp_program **pp = &ctx->programs;

	/* the programs made for it go: they are found by the shaders' addresses, and the next shader may be allocated at this one's
	 * (found with dEQP: a stale program, made for another shader, matched, and the draw came out wrong) */
	while (*pp) {
		struct mbx_vgp_program *p = *pp;
		bool mine = p->vs == so || p->fs == so;

		for (struct mbx_fs_variant *v = so->variants; v && !mine; v = v->next)
			mine = p->fsv == v;
		if (mine) {
			*pp = p->next;
			mbx_program_destroy(p);
		} else {
			pp = &p->next;
		}
	}
	mbx_fallback_shader_delete(ctx, so);
	while (so->variants) {
		struct mbx_fs_variant *v = so->variants;

		so->variants = v->next;
		FREE(v);
	}
	ralloc_free(so->nir);
	FREE(so);
}

static void mbx_vs_bind(struct pipe_context *pctx, void *hwcso)
{
	mbx_context(pctx)->vs = hwcso;
}

static void mbx_fs_bind(struct pipe_context *pctx, void *hwcso)
{
	mbx_context(pctx)->fs = hwcso;
}

void mbx_program_init(struct pipe_context *pctx)
{
	pctx->create_vs_state = mbx_shader_create;
	pctx->delete_vs_state = mbx_shader_delete;
	pctx->bind_vs_state = mbx_vs_bind;
	pctx->create_fs_state = mbx_shader_create;
	pctx->delete_fs_state = mbx_shader_delete;
	pctx->bind_fs_state = mbx_fs_bind;
}

/* The vertex and fragment shader bound, linked: compiled once for the pair and kept in the context */
struct mbx_vgp_program *mbx_program_get(struct mbx_context *ctx)
{
	struct mbx_vgp_program *p;

	bool scissor = ctx->rast && ctx->rast->scissor;

	for (p = ctx->programs; p; p = p->next)
		if (p->vs == ctx->vs && p->fs == ctx->fs && p->fsv == ctx->fsv && p->scissor == scissor)
			return p->ninsn ? p : NULL;
	p = CALLOC_STRUCT(mbx_vgp_program);
	if (!p)
		return NULL;
	p->vs = ctx->vs;
	p->fs = ctx->fs;
	p->fsv = ctx->fsv;
	p->fsi = ctx->fsi;
	p->scissor = scissor;
	p->vp_slot = -1;
	p->fc_slot = -1;
	p->next = ctx->programs;
	ctx->programs = p;	/* kept even when it fails (ninsn 0), so it is not tried again */
	if ((*ctx->fsi).kind != MBX_FS_MATCHED) {
		mbx_dbg("this fragment shader does not reduce to the fixed pipe: the CPU fallback draws it");
		return NULL;
	}
	p->gouraud = !(*ctx->fsi).flat;
	p->grid = (*ctx->fsi).grid;
	p->tex_scale_slot[0] = p->tex_scale_slot[1] = -1;
	if ((*ctx->fsi).tex) {
		const struct mbx_fs_info *fi = &(*ctx->fsi);

		p->tex = true;
		p->units = fi->units;
		p->ntex = fi->units & 2 ? 2 : 1;	/* unit 1 needs unit 0's block before it */
		mbx_tex_combiners(fi, p->ntex, 0, p->tex_c);
		/* state bits 16-18: one texture unit is mode 2, two are 5 (found on the chip: 1 draws nothing, 3, 4, 6, 7 hang) */
		p->st_extra |= (p->ntex == 2 ? 5u : 2u) << 16;
	}
	p->sections = 0xf0020000 | 0x4 | (scissor ? 0xfu << 20 : 0);	/* the VGP section the program is loaded in (bit 2); user clip planes 0-3 on for the scissor; bit 17 is needed on the chip (observed on an N810), the model ignores it */
	if ((*ctx->fsi).has_off)
		p->st_extra |= 1u << 23;	/* the offset colour (o2) is added after the texture units (found on the chip) */
	if (!mbx_compile_vs(ctx->vs->nir, &(*ctx->fsi), p)) {
		p->ninsn = 0;
		return NULL;
	}
	return p;
}
