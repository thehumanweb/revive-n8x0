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

void mbx_nir_prepare(struct nir_shader *s)
{
	NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out | nir_var_uniform, type_size,
		 (nir_lower_io_options)0);
	NIR_PASS(_, s, nir_lower_load_const_to_scalar);
	optimize(s);
	NIR_PASS(_, s, nir_lower_bool_to_float, false);
	optimize(s);
	NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp, NULL);
	nir_sweep(s);
}
