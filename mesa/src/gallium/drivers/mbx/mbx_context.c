// SPDX-License-Identifier: MIT
#include <string.h>

#include "util/format/u_format.h"
#include "util/u_pack_color.h"
#include "util/u_debug_cb.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_helpers.h"
#include "util/u_framebuffer.h"
#include "util/log.h"
#include "util/ralloc.h"

#include "mbx_context.h"
#include "mbx_fallback.h"

static void mbx_context_destroy(struct pipe_context *pctx)
{
	struct mbx_context *ctx = mbx_context(pctx);

	mbx_job_flush(ctx);
	mbx_fallback_context_destroy(ctx);
	while (ctx->programs) {
		struct mbx_vgp_program *n = ctx->programs->next;

		mbx_program_destroy(ctx->programs);
		ctx->programs = n;
	}
	mbx_job_fini(ctx);
	util_unreference_framebuffer_state(&ctx->fb);
	for (unsigned i = 0; i < 2; i++)
		pipe_resource_reference(&ctx->cb[i].buffer, NULL);
	for (unsigned i = 0; i < ctx->num_vb; i++)
		pipe_vertex_buffer_unreference(&ctx->vb[i]);
	if (pctx->stream_uploader)
		u_upload_destroy(pctx->stream_uploader);
	ralloc_free(ctx);
}

static void mbx_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence, unsigned flags)
{
	struct mbx_context *ctx = mbx_context(pctx);

	mbx_job_flush(ctx);
	if (fence) {
		/* NULL means done; otherwise a fence for the last job */
		struct pipe_fence_handle *f = NULL;

		if (ctx->last_fence > ctx->screen->fence_done && (f = calloc(1, sizeof(*f)))) {
			pipe_reference_init(&f->reference, 1);
			f->seqno = ctx->last_fence;
		}
		pctx->screen->fence_reference(pctx->screen, fence, f);
	}
}

static void mbx_invalidate_resource(struct pipe_context *pctx, struct pipe_resource *prsc) { }

static void mbx_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
	mbx_job_flush(mbx_context(pctx));
}

/* The colour of a clear in the target's own format, for the CPU to fill with */
static uint32_t pack_clear_colour(enum pipe_format format, const union pipe_color_union *color)
{
	union util_color uc;

	util_pack_color_union(format, &uc, color);
	return format == PIPE_FORMAT_B5G6R5_UNORM ? uc.us : uc.ui[0];
}

static void mbx_clear(struct pipe_context *pctx, unsigned buffers, uint32_t color_clear_mask,
		      uint8_t stencil_clear_mask, const struct pipe_scissor_state *scissor,
		      const union pipe_color_union *color, double depth, unsigned stencil)
{
	struct mbx_context *ctx = mbx_context(pctx);

	if (buffers & PIPE_CLEAR_DEPTH) {
		/* depth lives on chip and starts every render at this value: what
		 * was recorded finishes first */
		mbx_job_flush(ctx);
		mbx_fallback_clear_depth(ctx, depth);	/* softpipe's own depth, when it has drawn */
		ctx->next_clear_depth = (float)depth;
		ctx->job.clear_depth = (float)depth;
	}
	if ((buffers & PIPE_CLEAR_COLOR0) && ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture) {
		struct mbx_resource *rsc = mbx_resource(ctx->fb.cbufs[0].texture);
		unsigned bpp = util_format_get_blocksize(rsc->base.format), x, y;
		uint8_t *map;
		uint32_t v;

		unsigned x0 = 0, y0 = 0, x1 = ctx->fb.width, y1 = ctx->fb.height;

		if (scissor) {		/* the CPU fill is exactly the rectangle */
			x0 = MIN2(MAX2(scissor->minx, 0), x1);
			y0 = MIN2(MAX2(scissor->miny, 0), y1);
			x1 = MIN2(scissor->maxx, x1);
			y1 = MIN2(scissor->maxy, y1);
		}
		mbx_job_flush(ctx);	/* the CPU fill comes after what was drawn */
		mbx_resource_wait(ctx->screen, rsc);
		map = mbx_bo_map(rsc->bo);
		mbx_dbg("clear: %ux%u, stride %u, bo size %llu, map %p", ctx->fb.width, ctx->fb.height, rsc->stride,
			(unsigned long long)rsc->bo->size, (void *)map);
		if (!map)
			return;
		v = pack_clear_colour(rsc->base.format, color);
		for (y = y0; y < y1; y++) {
			uint8_t *row = map + (uint64_t)y * rsc->stride;

			if (bpp == 4)
				for (x = x0; x < x1; x++)
					((uint32_t *)row)[x] = v;
			else
				for (x = x0; x < x1; x++)
					((uint16_t *)row)[x] = (uint16_t)v;
		}
	}
}

struct pipe_context *mbx_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
	struct mbx_context *ctx = rzalloc(NULL, struct mbx_context);
	struct pipe_context *pctx;

	if (!ctx)
		return NULL;
	pctx = &ctx->base;
	ctx->screen = mbx_screen(pscreen);
	pctx->screen = pscreen;
	pctx->priv = priv;
	pctx->destroy = mbx_context_destroy;
	pctx->flush = mbx_flush;
	pctx->clear = mbx_clear;
	pctx->invalidate_resource = mbx_invalidate_resource;
	pctx->texture_barrier = mbx_texture_barrier;
	pctx->set_debug_callback = u_default_set_debug_callback;

	mbx_state_init(pctx);
	mbx_program_init(pctx);
	mbx_draw_init(pctx);
	mbx_resource_context_init(pctx);

	pctx->stream_uploader = u_upload_create_default(pctx);
	pctx->const_uploader = pctx->stream_uploader;
	if (!pctx->stream_uploader) {
		mbx_context_destroy(pctx);
		return NULL;
	}
	ctx->next_clear_depth = 1.0f;
	ctx->job.hdr = -1;
	mbx_job_reset(ctx);
	return pctx;
}
