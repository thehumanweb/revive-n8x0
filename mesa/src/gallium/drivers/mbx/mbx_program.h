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

#define MBX_MAX_TEX 2	/* texture units the TSP has */
#define MBX_MAX_FSU 4	/* fragment uniform vec4s the vertex program can take */

/*
 * What the vertex program computes for the fragment shader, as a small expression tree over what the TSP cannot
 * (everything but the texels): constants, the vertex shader's outputs (varyings), fragment uniforms, and sums and
 * products of those. Affine in the varyings, so it is exact when the hardware interpolates the result.
 */
enum mbx_fs_op { FSN_CONST, FSN_VAR, FSN_UNI, FSN_ADD, FSN_SUB, FSN_MUL, FSN_NEG, FSN_OP };

struct mbx_fs_node {
	uint8_t op;
	uint16_t a, b, c;		/* operands (node indices); FSN_OP: the nir op in loc, up to three operands */
	float k;			/* FSN_CONST */
	unsigned loc, comp;		/* FSN_VAR: slot and component; FSN_UNI: index in fsu_vec and component */
};

#define MBX_MAX_NODES 96

/* One lane of gl_FragColor: texel units' product times a coefficient, plus an additive term */
struct mbx_fs_lane {
	unsigned tex;			/* the units whose texel multiplies the coefficient, bit k for unit k (0: no texel) */
	int prim;			/* the coefficient (the whole value when no texel): a node, -1 for 1 */
	int off;			/* the additive term (the offset colour): a node, -1 for none */
};

struct mbx_fs_info {
	enum mbx_fs_kind kind;
	struct mbx_fs_lane lane[4];
	struct mbx_fs_node node[MBX_MAX_NODES];
	unsigned nnode;
	bool flat;		/* flat shading (declared so, or nothing varies) */
	bool has_off;		/* some lane has an additive term: the offset colour */
	/* texturing */
	bool tex;
	unsigned units;		/* the texture units the shader samples: 1 (unit 0), 2 (unit 1), 3 (both) */
	unsigned mc, ma;	/* the units' texels in the colour lanes (0-2) and in the alpha lane (3) */
	bool pass_c, pass_a;	/* the colour lanes / the alpha lane have a primary colour (a coefficient other than 1) */
	unsigned uv_loc[MBX_MAX_TEX][2], uv_comp[MBX_MAX_TEX][2];	/* where each unit's coordinate comes from in the vertex shader's outputs */
	unsigned nfsu, fsu_vec[MBX_MAX_FSU];	/* the fragment uniform vec4s read */
	bool swap_rb;		/* red and blue of the texel are exchanged (Weston's BGRA swizzle): the draw samples a copy of the texture with them swapped */
	bool grid;		/* a coefficient that is not affine in the varyings: evaluated at the corners of a fine grid (README assumption 48) */
	uint32_t alpha_word;	/* the alpha-test word for a "discard if alpha is on one side of a constant" (README assumption 45); 0: always */
};

/*
 * The VGP program for a vertex shader and the fixed pipe set-up for a
 * fragment shader, made at a draw for the pair bound.
 */
struct mbx_vgp_program {
	const void *vs, *fs;		/* the key */
	const void *fsv;		/* and the fragment shader's variant (folded uniforms), if any */
	const struct mbx_fs_info *fsi;	/* the match the program was made from */
	uint32_t code[256][2];		/* high word, low word */
	unsigned ninsn;
	/* constants: the uniform buffer's vec4s first, then the compiler's immediates */
	unsigned nuniform;		/* vec4s taken from the constant buffer */
	unsigned nconst;		/* in all */
	float consts[MBX_MAX_CONSTS][4];	/* the immediates (from nuniform on) */
	bool attr_used[MBX_MAX_ATTRS];	/* input registers the program reads */
	bool gouraud;			/* the varying colour is interpolated */
	bool grid;			/* the colour is not affine in the varyings: triangles are cut into a fine grid (README assumption 48) */
	uint32_t st_extra;		/* ISP/TSP state bits the shader wants */
	uint32_t sections;		/* the sections word this program's draws send */
	bool scissor;			/* the variant with the scissor as four user clip planes (o5.xyzw) */
	int scissor_slot;		/* its four planes: uniform slots scissor_slot .. +3 */
	int vp_slot;			/* the uniform slot holding the viewport's x shift (dx, 0, 0, 0): the position's x is moved by dx * w before the header's
					 * viewport (whose x translate is its scale) maps it: the chip bends the texture coordinates when the x translate
					 * differs from the x scale (found with a 64 x 64 texture on viewports of several widths and offsets: README
					 * assumption 54) */
	unsigned nfsu, fsu_vec[MBX_MAX_FSU];	/* fragment uniform vec4s the program carries as constants */
	int fsu_base;			/* their first constant slot */
	bool tex;			/* a texture is sampled */
	unsigned units;			/* which units texels the shader uses (bit k: unit k) */
	unsigned ntex;			/* texture units used */
	uint32_t tex_c[MBX_MAX_TEX];	/* their combiners (texture word C) */
	int tex_scale_slot[MBX_MAX_TEX];	/* the uniform slot holding each texture's (width / padded width, height / padded height) */
	/* gl_PointSize: the TA takes one size for the object, so only a constant or a uniform works */
	enum { MBX_PSIZE_STATE, MBX_PSIZE_IMM, MBX_PSIZE_UNIFORM, MBX_PSIZE_INPUT } psize_kind;
	float psize;			/* MBX_PSIZE_IMM */
	unsigned psize_slot, psize_lane;	/* MBX_PSIZE_UNIFORM: the constant buffer's vec4 and lane; MBX_PSIZE_INPUT: the vertex element and its component */
	struct mbx_vgp_program *next;
};

/* mbx_nir.c */
extern const struct nir_shader_compiler_options mbx_nir_options;
void mbx_nir_prepare(struct nir_shader *s);
/* mbx_compile.c */
/* The vertex program on the CPU (mbx_cpuvs.c): the clip position of one vertex */
void mbx_cpuvs_run(const uint32_t code[][2], unsigned ninsn, const float k[][4], unsigned nk, const float in[8][4],
		   float pos[4]);
void mbx_fs_match(struct nir_shader *fs, struct mbx_fs_info *info);
bool mbx_compile_vs(struct nir_shader *vs, const struct mbx_fs_info *fs, struct mbx_vgp_program *prog);

/* A fragment shader matched with some uniforms folded in as constants (their values are the key) */
struct mbx_fs_variant {
	struct mbx_fs_variant *next;
	uint32_t fold;			/* bit per entry of the shader's uniform list: folded in */
	uint32_t val[8][4];
	struct mbx_fs_info info;
};
void mbx_nir_optimize(struct nir_shader *s);
const struct mbx_fs_info *mbx_fs_specialize(struct mbx_context *ctx, const struct mbx_fs_variant **variant);
struct mbx_vgp_program *mbx_program_get(struct mbx_context *ctx);
void mbx_tex_combiners(const struct mbx_fs_info *fi, unsigned ntex, unsigned l8, uint32_t out[MBX_MAX_TEX]);
void mbx_program_destroy(struct mbx_vgp_program *p);

#endif
