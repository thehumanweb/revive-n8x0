// SPDX-License-Identifier: MIT
/*
 * What the compiler wants of the NIR the state tracker gives us: I/O as
 * intrinsics in vec4 slots, scalar ALU, SSA, constants folded, loops unrolled,
 * booleans as floats (the VGP's compares give 1.0 or 0.0). The same shape
 * vc4 asks for, which has the same kind of scalar, unbranching vertex side.
 */
#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"

#include "mbx_program.h"
#include "mbx_screen.h"

/* What NIR should lower before it reaches us: no trigonometry, no integers on the VGP; the rest */
const nir_shader_compiler_options mbx_nir_options = {
	.lower_fdiv = true,
	.lower_flrp32 = true,
	.lower_fpow = true,
	.lower_fsat = true,
	.lower_fsqrt = true,
	.lower_fmod = true,
	.lower_ffloor = true,
	.lower_fceil = true,
	.lower_ftrunc = true,
	.lower_fround_even = true,
	.lower_fsign = true,
	.lower_extract_byte = true,
	.lower_extract_word = true,
	.lower_insert_byte = true,
	.lower_insert_word = true,
	.lower_to_scalar = true,
	.has_fsub = true,
	.max_unroll_iterations = 32,
	.force_indirect_unrolling = (nir_var_shader_in | nir_var_shader_out | nir_var_function_temp),
};

static int type_size(const struct glsl_type *type, bool bindless)
{
	return glsl_count_attribute_slots(type, false);
}

static void optimize(struct nir_shader *s);
void mbx_nir_optimize(struct nir_shader *s)
{
	optimize(s);
}

static void optimize(struct nir_shader *s)
{
	bool progress;

	do {
		progress = false;
		NIR_PASS(_, s, nir_lower_vars_to_ssa);
		NIR_PASS(progress, s, nir_lower_alu_to_scalar, NULL, NULL);
		NIR_PASS(progress, s, nir_lower_phis_to_scalar, NULL, NULL);
		NIR_PASS(progress, s, nir_opt_copy_prop);
		NIR_PASS(progress, s, nir_opt_remove_phis);
		NIR_PASS(progress, s, nir_opt_dce);
		NIR_PASS(progress, s, nir_opt_dead_cf);
		NIR_PASS(progress, s, nir_opt_cse);
		nir_opt_peephole_select_options peephole = {
			.limit = 8,
			.indirect_load_ok = true,
			.expensive_alu_ok = true,
		};
		NIR_PASS(progress, s, nir_opt_peephole_select, &peephole);
		NIR_PASS(progress, s, nir_opt_algebraic);
		NIR_PASS(progress, s, nir_opt_constant_folding);
		NIR_PASS(progress, s, nir_opt_undef);
		NIR_PASS(progress, s, nir_opt_loop_unroll);
	} while (progress);
}

/* Integer work in the shader is nothing but compares (of constants and uniforms), and no array is indexed by a variable */
static bool int_compares_only(nir_shader *s)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(s);
	bool any = false;

	nir_foreach_block(block, impl) {
		nir_foreach_instr(instr, block) {
			if (instr->type == nir_instr_type_deref) {
				nir_deref_instr *d = nir_instr_as_deref(instr);

				if (d->deref_type == nir_deref_type_array && !nir_src_is_const(d->arr.index))
					return false;
			} else if (instr->type == nir_instr_type_intrinsic) {
				nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

				/* the lowering would turn the constant offset of a uniform load into a float */
				if (in->intrinsic == nir_intrinsic_load_uniform && !(nir_src_is_const(in->src[0]) && nir_src_as_uint(in->src[0]) == 0))
					return false;
			} else if (instr->type == nir_instr_type_alu) {
				nir_alu_instr *alu = nir_instr_as_alu(instr);
				const nir_op_info *info = &nir_op_infos[alu->op];
				bool ints = false;

				for (unsigned k = 0; k < info->num_inputs; k++) {
					nir_alu_type t = nir_alu_type_get_base_type(info->input_types[k]);

					ints |= t == nir_type_int || t == nir_type_uint;
				}
				if (!ints)
					continue;
				switch (alu->op) {
				case nir_op_ieq: case nir_op_ine: case nir_op_ilt: case nir_op_ige: case nir_op_ult: case nir_op_uge:
					any = true;
					break;
				case nir_op_mov: case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:	/* typeless */
				case nir_op_bcsel:	/* its data inputs are typed uint in NIR but it moves floats as well (dEQP vec2_dynamic_subscript_write_*) */
					break;
				default:
					return false;
				}
			}
		}
	}
	return any;
}

void mbx_nir_prepare(struct nir_shader *s)
{
	/* the screen has no integers: a constant of an integer compare is its value as a float (dEQP constants.int_*: -321 was a NaN) */
	if (s->info.stage == MESA_SHADER_FRAGMENT && int_compares_only(s)) {
		NIR_PASS(_, s, nir_lower_int_to_float);
		/* the compares are float now, and the uniform data is float: a load typed int would be folded in as integer bits (1 as a denormal;
		 * dEQP vector_subscript.vec2_dynamic_subscript_write_*) */
		nir_foreach_block(block, nir_shader_get_entrypoint(s)) {
			nir_foreach_instr(instr, block) {
				nir_intrinsic_instr *un;

				if (instr->type != nir_instr_type_intrinsic)
					continue;
				un = nir_instr_as_intrinsic(instr);
				if (un->intrinsic == nir_intrinsic_load_uniform)
					nir_intrinsic_set_dest_type(un, nir_type_float32);
			}
		}
	}
	NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out | nir_var_uniform, type_size,
		 (nir_lower_io_options)0);
	NIR_PASS(_, s, nir_lower_load_const_to_scalar);
	optimize(s);
	NIR_PASS(_, s, nir_lower_bool_to_float, false);
	optimize(s);
	NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp, NULL);
	nir_sweep(s);
}
