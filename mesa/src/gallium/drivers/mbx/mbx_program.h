/* SPDX-License-Identifier: MIT */
#ifndef MBX_PROGRAM_H
#define MBX_PROGRAM_H

#include <stdbool.h>
#include <stdint.h>

#include "util/macros.h"

struct mbx_context;
struct nir_shader;

#define MBX_MAX_ATTRS	8
#define MBX_MAX_CONSTS	128	/* what an instruction's 7-bit constant index reaches with no window */

/* What the fragment side reduces to on the fixed pipe */
enum mbx_fs_kind {
	MBX_FS_UNSUPPORTED,
	MBX_FS_MATCHED,		/* gl_FragColor from one varying's components and constants */
};

#define MBX_MAX_FSU 4	/* fragment uniform vec4s the vertex program can take */

/* One lane of gl_FragColor: a product of up to one of each factor */
struct mbx_fs_lane {
	bool tex;			/* the texel's lane */
	bool has_var;
	unsigned var_loc, var_comp;	/* a varying's component: slot, component */
	bool has_uni;
	unsigned uni_slot, uni_comp;	/* a fragment uniform's component: vec4 slot, component */
	unsigned uni_index;		/* its index in mbx_fs_info.fsu_vec */
	bool has_k;
	float k;			/* the constants' product */
};

struct mbx_fs_info {
	enum mbx_fs_kind kind;
	struct mbx_fs_lane lane[4];
	bool flat;		/* flat shading (declared so, or nothing varies) */
	/* texturing: unit 0's texel, times the lanes' other factors (modulate) or alone (replace) */
	bool tex;
	bool modulate;
	unsigned uv_loc[2], uv_comp[2];	/* where the coordinate comes from in the vertex shader's outputs */
	unsigned nfsu, fsu_vec[MBX_MAX_FSU];	/* the fragment uniform vec4s read */
};

/*
 * The VGP program for a vertex shader and the fixed pipe set-up for a
 * fragment shader, made at a draw for the pair bound.
 */
struct mbx_vgp_program {
	const void *vs, *fs;		/* the key */
	uint32_t code[256][2];		/* high word, low word */
	unsigned ninsn;
	/* constants: the uniform buffer's vec4s first, then the compiler's immediates */
	unsigned nuniform;		/* vec4s taken from the constant buffer */
	unsigned nconst;		/* in all */
	float consts[MBX_MAX_CONSTS][4];	/* the immediates (from nuniform on) */
	bool attr_used[MBX_MAX_ATTRS];	/* input registers the program reads */
	bool gouraud;			/* the varying colour is interpolated */
	uint32_t st_extra;		/* ISP/TSP state bits the shader wants */
	uint32_t sections;		/* the sections word this program's draws send */
	bool scissor;			/* the variant with the scissor as four user clip planes (o5.xyzw) */
	int scissor_slot;		/* its four planes: uniform slots scissor_slot .. +3 */
	unsigned nfsu, fsu_vec[MBX_MAX_FSU];	/* fragment uniform vec4s the program carries as constants */
	int fsu_base;			/* their first constant slot */
	bool tex;			/* unit 0 is sampled */
	uint32_t tex_c;			/* its combiner (texture word C) */
	int tex_scale_slot;		/* the uniform slot holding the texture's (width / padded width, height / padded height) */
	/* gl_PointSize: the TA takes one size for the object, so only a constant or a uniform works */
	enum { MBX_PSIZE_STATE, MBX_PSIZE_IMM, MBX_PSIZE_UNIFORM } psize_kind;
	float psize;			/* MBX_PSIZE_IMM */
	unsigned psize_slot, psize_lane;	/* MBX_PSIZE_UNIFORM: the constant buffer's vec4 and lane */
	struct mbx_vgp_program *next;
};

/* mbx_nir.c */
extern const struct nir_shader_compiler_options mbx_nir_options;
void mbx_nir_prepare(struct nir_shader *s);
/* mbx_compile.c */
void mbx_fs_match(struct nir_shader *fs, struct mbx_fs_info *info);
bool mbx_compile_vs(struct nir_shader *vs, const struct mbx_fs_info *fs, struct mbx_vgp_program *prog);

struct mbx_vgp_program *mbx_program_get(struct mbx_context *ctx);
void mbx_program_destroy(struct mbx_vgp_program *p);

#endif
