// SPDX-License-Identifier: MIT
/*
 * A job is one render: the TA stream recorded as chunks, then one SUBMIT at a
 * flush (the framebuffer changes, the frame is swapped, a result is read).
 * The TA's tile grid, region array, tail pointers and the background object
 * are per render-target size and kept between jobs.
 */
#include <string.h>
#include <xf86drm.h>

#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/log.h"

#include "drm-uapi/mbx_drm.h"
#include "mbx_context.h"

#define MBX_TILE 16

/* The macro tile size the TA picks for a count of tiles (mbx/ta.c, mt_size) */
static unsigned mt_size(unsigned tiles)
{
	return tiles <= 32 ? 4 : tiles <= 64 ? 8 : 16;
}

void mbx_emit(struct mbx_context *ctx, unsigned type, uint32_t v)
{
	struct mbx_job *job = &ctx->job;

	if (job->n + 2 > job->cap) {
		job->cap = job->cap ? job->cap * 2 : 4096;
		job->stream = realloc(job->stream, job->cap * sizeof(uint32_t));
	}
	if (job->hdr < 0 || job->type != type) {
		job->hdr = (int)job->n++;
		job->stream[job->hdr] = MBX_CHUNK(type, 0);
		job->type = type;
	}
	job->stream[job->n++] = v;
	job->stream[job->hdr]++;
}

void mbx_emit_f(struct mbx_context *ctx, unsigned type, float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	mbx_emit(ctx, type, u);
}

void mbx_job_reset(struct mbx_context *ctx)
{
	struct mbx_job *job = &ctx->job;

	job->n = 0;
	job->hdr = -1;
	job->active = false;
	job->ndraws = 0;
	job->loaded_prog = NULL;
	job->loaded_const_serial = ~0u;
	job->depth_cleared = false;
	job->clear_depth = ctx->next_clear_depth;
	pipe_resource_reference((struct pipe_resource **)&job->color, NULL);
}

/* The TA's grid and the per-size buffers it needs */
static bool target_bos(struct mbx_context *ctx, unsigned w, unsigned h)
{
	struct mbx_target_bos *t = &ctx->tbos;
	unsigned tx, ty, mx, my, nx, ny;

	if (t->regions && t->width == w && t->height == h)
		return true;
	mbx_bo_unref(t->regions);
	mbx_bo_unref(t->tails);
	t->regions = t->tails = NULL;
	tx = DIV_ROUND_UP(w, MBX_TILE);
	ty = DIV_ROUND_UP(h, MBX_TILE);
	mx = mt_size(tx);
	my = mt_size(ty);
	nx = DIV_ROUND_UP(tx, mx);
	ny = DIV_ROUND_UP(ty, my);
	t->tiles_x = tx;
	t->tiles_y = ty;
	/* the region array is one entry (two words) for each tile of the macro-tile grid */
	t->regions = mbx_bo_create(ctx->screen, align64((uint64_t)nx * mx * ny * my * 8, 4096));
	/* the tail pointers: 64 KB aligned and 64 KB (the TA rounds the address down and the driver zeroes it) */
	t->tails = mbx_bo_create_align64k(ctx->screen, 65536);
	t->width = w;
	t->height = h;
	return t->regions && t->tails;
}

bool mbx_job_begin(struct mbx_context *ctx)
{
	struct mbx_job *job = &ctx->job;
	struct pipe_surface *cbuf = ctx->fb.nr_cbufs ? ctx->fb.cbufs[0].texture ? &ctx->fb.cbufs[0] : NULL : NULL;

	if (job->active)
		return true;
	if (!cbuf)
		return false;
	if (!target_bos(ctx, ctx->fb.width, ctx->fb.height))
		return false;
	pipe_resource_reference((struct pipe_resource **)&job->color, cbuf->texture);
	job->active = true;
	return true;
}

void mbx_job_flush(struct mbx_context *ctx)
{
	struct mbx_job *job = &ctx->job;
	struct mbx_target_bos *t = &ctx->tbos;
	struct drm_mbx_submit s;
	struct drm_mbx_submit_bo bo;
	struct mbx_resource *color = job->color;
	uint32_t zclear;
	float z = job->clear_depth;
	int ret;

	mbx_dbg("flush: active %d, %u draws, %u stream words", job->active, job->ndraws, job->n);
	if (!job->active || !job->ndraws) {
		mbx_job_reset(ctx);
		return;
	}
	mbx_emit(ctx, MBX_CH_TA, 0xf0000000);	/* the scene ends */

	memset(&s, 0, sizeof(s));
	s.stream = (uintptr_t)job->stream;
	s.stream_words = job->n;
	s.ta_config = 0x80 | (uint32_t)(t->tiles_x - 1) << 20 | (uint32_t)(t->tiles_y - 1) << 10;
	s.ta_width = (uint32_t)ctx->fb.width << 16;
	s.ta_height = (uint32_t)ctx->fb.height << 16;
	s.ta_regions = t->regions->va;
	s.ta_tails = t->tails->va;
	s.r_regions = t->regions->va;
	s.r_ctl = 0x30000;			/* depth on chip */
	s.r_sampling = 0x00020007;
	s.r_format = color->base.format == PIPE_FORMAT_B5G6R5_UNORM ? 0x9 : 0xe;
	s.r_width = (uint32_t)(ctx->fb.width - 1) << 16;
	s.r_height = (uint32_t)(ctx->fb.height - 1) << 16;
	s.r_addr = color->bo->va;
	s.r_stride = color->stride / 4;		/* words */
	memcpy(&zclear, &z, 4);
	s.r_6dc = zclear | 1;			/* as GLES writes 1.0: 0x3f800001 */

	/* the render target is the one buffer the job writes that others may use (the display) */
	bo.handle = color->bo->handle;
	bo.flags = MBX_SUBMIT_BO_WRITE;
	s.bos = (uintptr_t)&bo;
	s.n_bos = 1;

	ret = drmIoctl(ctx->screen->fd, DRM_IOCTL_MBX_SUBMIT, &s);
	if (ret) {
		mesa_loge("mbx: SUBMIT failed (%d)", errno);
	} else {
		color->last_fence = s.fence;
		ctx->last_fence = s.fence;
	}
	mbx_job_reset(ctx);
}

void mbx_job_fini(struct mbx_context *ctx)
{
	mbx_bo_unref(ctx->tbos.regions);
	mbx_bo_unref(ctx->tbos.tails);
	free(ctx->job.stream);
}

void mbx_flush_for_resource(struct mbx_context *ctx, struct mbx_resource *rsc)
{
	if (ctx->job.active && ctx->job.color == rsc)
		mbx_job_flush(ctx);
}

void mbx_resource_wait(struct mbx_screen *screen, struct mbx_resource *rsc)
{
	if (rsc->last_fence)
		mbx_fence_wait(screen, rsc->last_fence, 10ull * 1000000000ull);
}
