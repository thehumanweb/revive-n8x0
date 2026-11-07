/* SPDX-License-Identifier: MIT */
/*
 * The CPU fallback: a draw the MBX cannot do (a fragment shader that does not reduce to the fixed pipe, a
 * blend state the factors cannot say, ...) is drawn by Mesa's softpipe into the same colour buffer. The
 * pending MBX job finishes first, softpipe draws into the buffer's memory (shared through a small software
 * winsys), and later MBX jobs start from what it left (the background object is the target itself).
 * The MBX keeps depth on chip and never in memory, so depth does not carry between the two.
 */
#ifndef MBX_FALLBACK_H
#define MBX_FALLBACK_H

#include "pipe/p_state.h"

struct mbx_context;
struct mbx_resource;
struct mbx_shader;
struct mbx_screen;

bool mbx_fallback_draw(struct mbx_context *ctx, const struct pipe_draw_info *info, unsigned drawid_offset,
		       const struct pipe_draw_start_count_bias *draws, unsigned num_draws);
bool mbx_fallback_blit(struct mbx_context *ctx, const struct pipe_blit_info *info);
void mbx_fallback_clear_depth(struct mbx_context *ctx, double depth);
void mbx_fallback_clear_stencil(struct mbx_context *ctx, unsigned stencil);
/* The framebuffer's stencil plane, one byte a pixel (width x height), or false */
bool mbx_fallback_read_stencil(struct mbx_context *ctx, uint8_t *out);
/* a shader's softpipe twin and the NIR kept for it */
void mbx_fallback_shader_delete(struct mbx_context *ctx, struct mbx_shader *so);
void mbx_fallback_resource_destroy(struct mbx_resource *rsc);
void mbx_fallback_context_destroy(struct mbx_context *ctx);
void mbx_fallback_screen_destroy(struct mbx_screen *screen);

#endif
