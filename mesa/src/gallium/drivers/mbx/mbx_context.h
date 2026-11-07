/* SPDX-License-Identifier: MIT */
#ifndef MBX_CONTEXT_H
#define MBX_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_dynarray.h"

#include "mbx_bo.h"
#include "mbx_resource.h"
#include "mbx_program.h"
#include "mbx_screen.h"

struct nir_shader;

#define MBX_MAX_SAMPLERS 8	/* what GLES2 needs: the CPU fallback takes the draws that use more than the chip does */

/* A shader as the state tracker hands it over; its VGP program is made at the first draw */
struct mbx_shader {
	mesa_shader_stage stage;
	struct nir_shader *nir;		/* owned */
	struct mbx_fs_info fs_info;	/* fragment shaders: what the fixed pipe does with it */
	struct nir_shader *nir_orig;	/* a copy from before our lowering, for the CPU fallback */
	void *sp_handle;		/* its softpipe shader, made at the first fallback draw */
};

struct mbx_vertex_elements {
	unsigned count;
	struct pipe_vertex_element e[PIPE_MAX_ATTRIBS];
};

/* What the TA stream of one render (a scene) is made of while it is recorded */
struct mbx_job {
	uint32_t *stream;		/* chunks (mbx_drm.h) */
	unsigned n, cap;
	int hdr;			/* index of the open chunk's header, -1 none */
	unsigned type;			/* its type */

	bool active;			/* anything recorded */
	unsigned ndraws;
	struct mbx_resource *color;	/* the render target (a reference is held) */
	struct mbx_resource *zr;	/* its depth buffer, when the framebuffer has one (not referenced: the framebuffer holds it) */
	bool zload;			/* the memory holds the current depth: the render loads it and the clear object leaves it alone */
	bool zuse;			/* a recorded draw tests depth: the render stores it */
	float clear_depth;
	bool depth_cleared;
	struct mbx_resource *tex[MBX_MAX_TEX];	/* the textures the recorded draws read (references held) */

	/* what the VGP holds for this job, so a draw sends only what changed */
	const struct mbx_vgp_program *loaded_prog;
	uint32_t loaded_const_serial;
};

/* regions, tails and the parameter buffer for a render target's size, kept across jobs */
struct mbx_target_bos {
	unsigned width, height;
	struct mbx_bo *regions[2], *tails;	/* two region arrays, used by turns (the stock stack alternates them) */
	unsigned which;			/* the one the next scene uses */
	unsigned tiles_x, tiles_y;
};

struct mbx_context {
	struct pipe_context base;
	struct mbx_screen *screen;

	struct pipe_stencil_ref stencil_ref;
	bool zkeep;			/* the job being flushed must leave its depth in memory: a CPU draw or a framebuffer switch follows */
	struct pipe_framebuffer_state fb;
	struct pipe_viewport_state viewport;
	struct pipe_scissor_state scissor;
	struct pipe_blend_color blend_color;
	struct pipe_constant_buffer cb[2];		/* vertex, fragment */
	struct pipe_sampler_state *samplers[MBX_MAX_SAMPLERS];	/* the fragment stage's */
	struct pipe_sampler_view *views[MBX_MAX_SAMPLERS];
	uint32_t const_serial;				/* bumped when uniforms change */
	struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
	unsigned num_vb;

	struct pipe_blend_state *blend;
	struct pipe_rasterizer_state *rast;
	struct pipe_depth_stencil_alpha_state *dsa;
	struct mbx_vertex_elements *velems;
	struct mbx_shader *vs, *fs;
	struct pipe_context *sp;	/* the CPU fallback's softpipe context, made at the first use */

	struct mbx_vgp_program *programs;	/* linked pairs */
	struct mbx_job job;
	uint64_t last_fence;			/* the last job this context submitted */
	struct mbx_target_bos tbos;
	/* a full-target colour clear not yet done: the next job's background pass paints it (README assumption 44), or
	 * the CPU fills the target when something needs the memory first (mbx_clear_materialize) */
	struct {
		bool pending;
		struct mbx_resource *rsc;	/* a reference is held */
		uint32_t packed;
		unsigned level, w, h;
	} clear;
	float next_clear_depth;
};

static inline struct mbx_context *mbx_context(struct pipe_context *p)
{
	return (struct mbx_context *)p;
}

/* mbx_state.c */
void mbx_state_init(struct pipe_context *pctx);
/* mbx_draw.c */
void mbx_draw_init(struct pipe_context *pctx);
/* mbx_program.c */
void mbx_program_init(struct pipe_context *pctx);
/* mbx_job.c */
void mbx_job_flush(struct mbx_context *ctx);
void mbx_job_reset(struct mbx_context *ctx);
void mbx_emit(struct mbx_context *ctx, unsigned type, uint32_t v);
void mbx_emit_f(struct mbx_context *ctx, unsigned type, float f);
bool mbx_job_begin(struct mbx_context *ctx);
void mbx_job_fini(struct mbx_context *ctx);
void mbx_job_use_texture(struct mbx_context *ctx, struct mbx_resource *rsc);
/* the pending clear, done on the CPU (before the CPU or softpipe touches the target); mbx_clear_settle also renders what is recorded */
void mbx_clear_materialize(struct mbx_context *ctx);
/* The depth buffer's external Z memory for the framebuffer's size (allocated, or dropped when the size changed) */
bool mbx_z_ensure(struct mbx_context *ctx, struct mbx_resource *zr);
/* Fill it with the clear depth when it holds nothing current (before the CPU reads it) */
void mbx_z_fill_if_stale(struct mbx_context *ctx, struct mbx_resource *zr);
void mbx_clear_settle(struct mbx_context *ctx);

#define MBX_CH_TA	0
#define MBX_CH_CTL	1

#endif
