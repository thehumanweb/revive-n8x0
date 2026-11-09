// SPDX-License-Identifier: MIT
/*
 * A job is one render: the TA stream recorded as chunks, then one SUBMIT at a
 * flush (the framebuffer changes, the frame is swapped, a result is read).
 * The TA's tile grid, region array, tail pointers and the background object
 * are per render-target size and kept between jobs.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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
	/* a chunk's word count is 16 bits (the header's upper bits are the type): a long run of one type goes in several chunks */
	if (job->hdr < 0 || job->type != type || (job->stream[job->hdr] & 0xffff) == 0xffff) {
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
	job->zr = NULL;
	job->zload = job->zuse = false;
	job->clear_depth = ctx->next_clear_depth;
	pipe_resource_reference((struct pipe_resource **)&job->color, NULL);
	for (unsigned i = 0; i < MBX_MAX_TEX; i++)
		pipe_resource_reference((struct pipe_resource **)&job->tex[i], NULL);
}

/* A texture the recorded draws read: the job holds it and names it when it is submitted */
void mbx_job_use_texture(struct mbx_context *ctx, struct mbx_resource *rsc)
{
	for (unsigned i = 0; i < MBX_MAX_TEX; i++)
		if (ctx->job.tex[i] == rsc)
			return;
	for (unsigned i = 0; i < MBX_MAX_TEX; i++)
		if (!ctx->job.tex[i]) {
			pipe_resource_reference((struct pipe_resource **)&ctx->job.tex[i], &rsc->base);
			return;
		}
}

/* The TA's grid and the per-size buffers it needs */
static bool target_bos(struct mbx_context *ctx, unsigned w, unsigned h)
{
	struct mbx_target_bos *t = &ctx->tbos;
	unsigned tx, ty, mx, my, nx, ny;

	if (t->regions[0] && t->width == w && t->height == h)
		return true;
	mbx_bo_unref(t->regions[0]);
	mbx_bo_unref(t->regions[1]);
	mbx_bo_unref(t->tails);
	t->regions[0] = t->regions[1] = t->tails = NULL;
	t->which = 0;
	tx = DIV_ROUND_UP(w, MBX_TILE);
	ty = DIV_ROUND_UP(h, MBX_TILE);
	mx = mt_size(tx);
	my = mt_size(ty);
	nx = DIV_ROUND_UP(tx, mx);
	ny = DIV_ROUND_UP(ty, my);
	t->tiles_x = tx;
	t->tiles_y = ty;
	/* the region array is one entry (two words) for each tile of the macro-tile grid */
	t->regions[0] = mbx_bo_create(ctx->screen, align64((uint64_t)nx * mx * ny * my * 8, 4096));
	t->regions[1] = mbx_bo_create(ctx->screen, align64((uint64_t)nx * mx * ny * my * 8, 4096));
	/* the tail pointers: 64 KB aligned and 64 KB (the TA rounds the address down and the driver zeroes it) */
	t->tails = mbx_bo_create_align64k(ctx->screen, 65536);
	t->width = w;
	t->height = h;
	return t->regions[0] && t->regions[1] && t->tails;
}

/*
 * What a frame starts with (mbx/lib/mbx_contract.h, found during device testing): the region clip
 * control, then a depth-only clear to the far plane (a screen-space object: one triangle twice the
 * target, depth written, no colour), without which the tile's depth reads as NaN and most depth tests
 * fail. Section bit 11 names the passthrough (o0 = v0) the clear's vertices go through, kept in the
 * last VGP instruction slot, clear of any program the compiler makes.
 */
static void emit_frame_start(struct mbx_context *ctx, float depth, bool zload)
{
	mbx_emit(ctx, MBX_CH_TA, 0x10000010);	/* an object header naming the one-word region clip control */
	mbx_emit(ctx, MBX_CH_TA, 0);
	mbx_emit(ctx, MBX_CH_TA, 0x80000000 | 0x800);
	mbx_emit(ctx, MBX_CH_TA, 255u << 8 | 255);
	mbx_emit(ctx, MBX_CH_TA, 0x90000000 | 255u << 16 | 255);
	mbx_emit(ctx, MBX_CH_TA, 0xe4e4e401);	/* MOV o0, v0 */
	mbx_emit(ctx, MBX_CH_TA, 0x03c00000);
	mbx_emit(ctx, MBX_CH_TA, 0x10000000 | 0x1 | 0x2 | 0x20);	/* state, alpha test, layout */
	mbx_emit(ctx, MBX_CH_TA, 0xe00 | 0x100 | (zload ? 0x80 : 0));	/* depth always, written (not when the render loads it); no colour */
	mbx_emit(ctx, MBX_CH_TA, 0xe0000000);	/* alpha function ALWAYS */
	mbx_emit(ctx, MBX_CH_TA, 0x3);		/* vertices: three floats */
	mbx_emit(ctx, MBX_CH_TA, 0x70000000);	/* a triangle list */
	mbx_emit(ctx, MBX_CH_TA, 0xb00b0800);	/* screen space */
	mbx_emit_f(ctx, MBX_CH_TA, 0); mbx_emit_f(ctx, MBX_CH_TA, 0); mbx_emit_f(ctx, MBX_CH_TA, depth);
	mbx_emit_f(ctx, MBX_CH_TA, 2.0f * ctx->fb.width); mbx_emit_f(ctx, MBX_CH_TA, 0); mbx_emit_f(ctx, MBX_CH_TA, depth);
	mbx_emit_f(ctx, MBX_CH_TA, 0); mbx_emit_f(ctx, MBX_CH_TA, 2.0f * ctx->fb.height); mbx_emit_f(ctx, MBX_CH_TA, depth);
	mbx_emit(ctx, MBX_CH_CTL, 1);		/* the object ends */
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
	if (ctx->clear.pending && ctx->clear.rsc != mbx_resource(cbuf->texture))
		mbx_clear_materialize(ctx);	/* a clear of another target: its memory is needed as it is */
	pipe_resource_reference((struct pipe_resource **)&job->color, cbuf->texture);
	job->active = true;
	if (ctx->fb.zsbuf.texture) {
		struct mbx_resource *zr = mbx_resource(ctx->fb.zsbuf.texture);

		job->zr = zr;
		job->zload = zr->zvalid && zr->zbo && zr->zw == ctx->fb.width && zr->zh == ctx->fb.height && !MBX_ENV_SET("MBX_NO_ZCARRY");
	}
	emit_frame_start(ctx, job->clear_depth, job->zload);
	return true;
}

/*
 * What a job drew into a scanout buffer, for the KMS driver's next update of it (omapdrm, kernel patch 0065): the rectangle of the rendered
 * tiles when only some were (a partial render: README assumption 50, the rest of the buffer is the previous frame by the buffer-age rule),
 * FULL when the whole target was rendered. Mirrors include/uapi/drm/omap_drm.h of that patch.
 */
struct mbx_omap_gem_damage {
	uint32_t handle, flags, x1, y1, x2, y2;
};
#define MBX_OMAP_GEM_DAMAGE_FULL 0x1
#define MBX_DRM_IOCTL_OMAP_GEM_DAMAGE DRM_IOW(DRM_COMMAND_BASE + 0x07, struct mbx_omap_gem_damage)

static void report_scanout_damage(struct mbx_context *ctx, struct mbx_resource *color, uint32_t damage)
{
	struct mbx_screen *screen = ctx->screen;
	struct mbx_omap_gem_damage d = { .handle = color->scanout->handle };

	if (screen->no_omap_damage || !screen->ro || MBX_ENV_SET("MBX_NO_OMAP_DAMAGE"))
		return;
	if (!damage) {
		d.flags = MBX_OMAP_GEM_DAMAGE_FULL;
	} else {
		d.x1 = (damage & 0xff) * 16;
		d.y1 = (damage >> 8 & 0xff) * 16;
		d.x2 = MIN2((damage >> 16 & 0xff) * 16, ctx->fb.width);
		d.y2 = MIN2((damage >> 24 & 0xff) * 16, ctx->fb.height);
	}
	if (drmIoctl(screen->ro->kms_fd, MBX_DRM_IOCTL_OMAP_GEM_DAMAGE, &d)) {
		mbx_dbg("no omapdrm damage ioctl (%d): not reporting damage of scanout buffers", errno);
		screen->no_omap_damage = true;
	}
}

/*
 * MBX_PROF=1: each job's cost, measured in the process that submits it: the SUBMIT ioctl (the kernel builds the parameter
 * buffer and starts the TA) and the time to completion (the job is waited for right after, so this changes the schedule
 * it measures: it is for finding what a job costs, not for timing a pipeline). Printed every two seconds.
 */
uint64_t prof_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* MBX_PROF=1 waits for every job and times it; 2 only times the driver's own calls (no waiting: the schedule is the real one) */
int mbx_prof_level(void)
{
	static int level = -1;

	if (level < 0) {
		const char *e = getenv("MBX_PROF");

		level = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0;
	}
	return level;
}

/* Time spent in the driver's calls (draw_vbo, flush, the SUBMIT ioctl), printed every two seconds with their counts */
void mbx_prof_note(unsigned what, uint64_t us)
{
	static struct { uint64_t n[MBX_PROF_N], us[MBX_PROF_N], t0; } p;
	static const char *const name[MBX_PROF_N] = { "draw_vbo", "flush", "SUBMIT ioctl" };
	uint64_t now = prof_now_us();

	if (!p.t0)
		p.t0 = now;
	p.n[what]++;
	p.us[what] += us;
	if (now - p.t0 >= 2000000) {
		fprintf(stderr, "mbx prof calls (%s), per second:", program_invocation_short_name);
		for (unsigned k = 0; k < MBX_PROF_N; k++)
			fprintf(stderr, " %s %llu calls %llu us;", name[k], (unsigned long long)(p.n[k] / 2), (unsigned long long)(p.us[k] / 2));
		fprintf(stderr, "\n");
		memset(&p, 0, sizeof(p));
		p.t0 = now;
	}
}

static void prof_job(struct mbx_context *ctx, uint64_t fence, uint64_t t_before, uint64_t t_after, unsigned ndraws, unsigned tiles, bool flat)
{
	static struct { unsigned n; uint64_t submit, run, submit_max, run_max, draws, t0, tiles, flat; } p;
	uint64_t t_done;

	mbx_fence_wait(ctx->screen, fence, 5ull * 1000000000ull);
	t_done = prof_now_us();
	if (!p.t0)
		p.t0 = t_before;
	p.n++;
	p.submit += t_after - t_before;
	p.run += t_done - t_after;
	p.draws += ndraws;
	p.tiles += tiles;
	p.flat += flat;
	if (t_after - t_before > p.submit_max)
		p.submit_max = t_after - t_before;
	if (t_done - t_after > p.run_max)
		p.run_max = t_done - t_after;
	if (t_done - p.t0 >= 2000000) {
		fprintf(stderr, "mbx prof (%s): %u jobs in %.1f s, %.1f draws and %llu tiles a job, flat background in %llu; SUBMIT ioctl avg %llu us max %llu us; to done avg %llu us max %llu us\n",
			program_invocation_short_name, p.n, (double)(t_done - p.t0) / 1e6, (double)p.draws / p.n, (unsigned long long)(p.tiles / p.n),
			(unsigned long long)p.flat,
			(unsigned long long)(p.submit / p.n), (unsigned long long)p.submit_max,
			(unsigned long long)(p.run / p.n), (unsigned long long)p.run_max);
		memset(&p, 0, sizeof(p));
	}
}

void mbx_job_flush(struct mbx_context *ctx)
{
	struct mbx_job *job = &ctx->job;
	struct mbx_target_bos *t = &ctx->tbos;
	struct drm_mbx_submit s;
	struct drm_mbx_submit_bo bo[2 + MBX_MAX_TEX];
	unsigned nbo = 0;
	struct mbx_resource *color = job->color;
	bool flat, zstored = false, use_ext;
	uint32_t zclear;
	float z = job->clear_depth;
	int ret;
	uint64_t t_before;

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
	s.ta_regions = t->regions[t->which]->va;
	s.ta_tails = t->tails->va;
	s.r_regions = t->regions[t->which]->va;	/* the one the TA used */
	s.r_ctl = 0x30000;			/* depth on chip */
	s.r_sampling = 0x00020007;
	s.r_format = color->base.format == PIPE_FORMAT_B5G6R5_UNORM ? 0x9 : 0xe;
	s.r_width = (uint32_t)(ctx->fb.width - 1) << 16;
	s.r_height = (uint32_t)(ctx->fb.height - 1) << 16;
	s.r_addr = color->bo->va;
	s.r_stride = color->stride / 4;		/* words */
	s.damage = color->damage;
	if (getenv("MBX_TEST_DAMAGE")) {	/* read live: gles2_feat sets it around one test */	/* test hook: x,y,w,h in pixels (top down) of the only region rendered, as set_damage_region would say */
		int dx, dy, dw, dh;

		if (sscanf(getenv("MBX_TEST_DAMAGE"), "%d,%d,%d,%d", &dx, &dy, &dw, &dh) == 4)
			s.damage = (uint32_t)(dx / 16) | (uint32_t)(dy / 16) << 8 | (uint32_t)DIV_ROUND_UP(dx + dw, 16) << 16 |
				   (uint32_t)DIV_ROUND_UP(dy + dh, 16) << 24;
	}
	memcpy(&zclear, &z, 4);
	s.r_6dc = zclear | 1;			/* as GLES writes 1.0: 0x3f800001 */
	/* a job that loads must load whether or not it tests depth (its clear object no longer writes depth: without the load the tile's
	 * depth is NaN and nothing draws); one that only tests stores when something needs the depth afterwards */
	use_ext = job->zr && (job->zload || (job->zuse && (ctx->zkeep || !ctx->frame_end)));
	mbx_dbg("flush: depth: zr %p zvalid %d zload %d zuse %d zkeep %d frame_end %d -> external Z %d (clear depth %g)", (void *)job->zr,
		job->zr ? job->zr->zvalid : 0, job->zload, job->zuse, ctx->zkeep, ctx->frame_end, use_ext, z);
	if (job->zuse && job->zr && !use_ext)
		job->zr->zvalid = false;	/* the depth stays on chip and is gone with the job: the memory no longer matches */
	if (use_ext && !MBX_ENV_SET("MBX_NO_ZCARRY") && mbx_z_ensure(ctx, job->zr)) {
		/* the depth goes to memory after the render (ctl bit 1), and comes from it first when it is current (bit 0 and the
		 * region entries' bit 29): chip and CPU draws and later jobs then share one depth buffer (README assumption 7) */
		unsigned stride_px = align(ctx->fb.width, 16);

		s.r_ctl = ((stride_px / 16 - 1) & 0xff) << 8 | 2 | (job->zload ? 1 : 0);
		s.r_zbuf = job->zr->zbo->va;
		if (job->zload)
			s.flags |= MBX_SUBMIT_ZLOAD;
		zstored = true;
	}
	flat = ctx->clear.pending && ctx->clear.rsc == color;
	if (ctx->clear.pending && !flat)
		mbx_clear_materialize(ctx);	/* another target's clear: the CPU fills it */
	if (flat) {
		s.flags |= MBX_SUBMIT_FLAT_BG;	/* the background pass paints the clear colour (README assumption 44) */
		s.bg_colour = ctx->clear.packed;
	}

	/* the render target is the one buffer the job writes that others may use (the display) */
	bo[nbo].handle = color->bo->handle;
	bo[nbo++].flags = MBX_SUBMIT_BO_WRITE;
	/* the textures it reads: the job waits for whoever writes them */
	for (unsigned i = 0; i < MBX_MAX_TEX; i++)
		if (job->tex[i] && job->tex[i]->bo != color->bo) {
			bo[nbo].handle = job->tex[i]->bo->handle;
			bo[nbo++].flags = 0;
		}
	if (zstored) {
		bo[nbo].handle = job->zr->zbo->handle;
		bo[nbo++].flags = MBX_SUBMIT_BO_WRITE;
	}
	s.bos = (uintptr_t)bo;
	s.n_bos = nbo;

	t_before = mbx_prof_level() ? prof_now_us() : 0;
	ret = drmIoctl(ctx->screen->fd, DRM_IOCTL_MBX_SUBMIT, &s);
	if (ret) {
		mesa_loge("mbx: SUBMIT failed (%d)", errno);
	} else {
		color->last_fence = s.fence;
		if (t_before)
			mbx_prof_note(MBX_PROF_SUBMIT, prof_now_us() - t_before);
		if (t_before && mbx_prof_level() == 1) {
			/* the tiles the job renders: its damage rectangle (x0 | y0 << 8 | x1 << 16 | y1 << 24, 0 all) or the whole frame */
			unsigned gw = (color->base.width0 + 15) / 16, gh = (color->base.height0 + 15) / 16;
			unsigned tiles = s.damage ? ((s.damage >> 16 & 0xff) - (s.damage & 0xff)) * ((s.damage >> 24) - (s.damage >> 8 & 0xff)) : gw * gh;

			prof_job(ctx, s.fence, t_before, prof_now_us(), job->ndraws, tiles, flat);
		}
		if (MBX_ENV_SET("MBX_VERIFY")) {	/* debug: what the job drew */
			const uint32_t *px;
			unsigned nz = 0;

			mbx_fence_wait(ctx->screen, s.fence, 5ull * 1000000000ull);
			px = mbx_bo_map(color->bo);
			if (px)
				for (unsigned y = 0; y < color->base.height0; y++)
					for (unsigned x = 0; x < color->base.width0; x++)
						nz += px[y * (color->stride / 4) + x] != 0;
			mbx_dbg("verify: job %llu rendered %ux%u (dma-buf inode %lu, stride %u, bo %p va %#llx, %u draws): %u non-zero pixels, flat bg %d, damage %#x",
				(unsigned long long)s.fence, color->base.width0, color->base.height0, color->dmabuf_ino, color->stride, (void *)color->bo,
				(unsigned long long)color->bo->va, job->ndraws, nz, flat, s.damage);
		}
		if (color->scanout)
			report_scanout_damage(ctx, color, s.damage);
		if (zstored) {
			job->zr->last_fence = s.fence;
			job->zr->zvalid = true;
		}
		if (flat) {
			ctx->clear.pending = false;
			pipe_resource_reference((struct pipe_resource **)&ctx->clear.rsc, NULL);
		}
		for (unsigned i = 0; i < MBX_MAX_TEX; i++)
			if (job->tex[i])
				job->tex[i]->last_fence = s.fence;	/* the CPU must not rewrite a texture a job still reads */
		ctx->last_fence = s.fence;
		t->which ^= 1;		/* the next scene uses the other region array */
	}
	mbx_job_reset(ctx);
}

void mbx_job_fini(struct mbx_context *ctx)
{
	mbx_bo_unref(ctx->tbos.regions[0]);
	mbx_bo_unref(ctx->tbos.regions[1]);
	mbx_bo_unref(ctx->tbos.tails);
	free(ctx->job.stream);
}

void mbx_flush_for_resource(struct mbx_context *ctx, struct mbx_resource *rsc)
{
	if (ctx->clear.pending && ctx->clear.rsc == rsc) {
		mbx_clear_settle(ctx);	/* a clear nobody has drawn on yet: the CPU fills it now */
		return;
	}
	if (!ctx->job.active)
		return;
	if (ctx->job.color == rsc) {
		mbx_job_flush(ctx);
		return;
	}
	for (unsigned i = 0; i < MBX_MAX_TEX; i++)
		if (ctx->job.tex[i] == rsc) {
			mbx_job_flush(ctx);
			return;
		}
}

void mbx_resource_wait(struct mbx_screen *screen, struct mbx_resource *rsc)
{
	if (rsc->last_fence)
		mbx_fence_wait(screen, rsc->last_fence, 10ull * 1000000000ull);
}

bool mbx_z_ensure(struct mbx_context *ctx, struct mbx_resource *zr)
{
	unsigned w = ctx->fb.width, h = ctx->fb.height;

	if (zr->zbo && (zr->zw != w || zr->zh != h)) {
		mbx_bo_unref(zr->zbo);
		zr->zbo = NULL;
		zr->zvalid = false;
		pipe_resource_reference(&zr->sp_twin, NULL);	/* it wrapped the old memory */
		pipe_resource_reference(&zr->zs_twin, NULL);	/* and the stencil plane belongs to the old size */
	}
	if (!zr->zbo) {
		zr->zbo = mbx_bo_create(ctx->screen, align64((uint64_t)align(w, 16) * align(h, 16) * 4, 4096));
		zr->zw = w;
		zr->zh = h;
		zr->zvalid = false;
	}
	return zr->zbo != NULL;
}

void mbx_z_fill_if_stale(struct mbx_context *ctx, struct mbx_resource *zr)
{
	uint32_t *p, v;
	float z = ctx->next_clear_depth;
	size_t n;

	if (!zr->zbo || zr->zvalid)
		return;
	mbx_resource_wait(ctx->screen, zr);
	p = mbx_bo_map(zr->zbo);
	if (!p)
		return;
	memcpy(&v, &z, 4);
	v |= 1;
	n = (size_t)align(zr->zw, 16) * align(zr->zh, 16);
	for (size_t i = 0; i < n; i++)
		p[i] = v;
	zr->zvalid = true;
}
