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

struct mbx_fs_info {
	enum mbx_fs_kind kind;
	unsigned varying;	/* VARYING_SLOT_* of the colour's varying */
	bool lane_const[4];	/* r, g, b, a: a constant instead of the varying's */
	float colour[4];	/* those constants */
	bool flat;		/* flat shading (declared so, or nothing varies) */
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
