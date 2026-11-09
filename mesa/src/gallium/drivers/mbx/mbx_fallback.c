// SPDX-License-Identifier: MIT
/* The CPU fallback (mbx_fallback.h): softpipe drawing into the MBX buffers' memory */
#include <poll.h>
#include "compiler/nir/nir.h"
#include "frontend/sw_winsys.h"
#include "softpipe/sp_public.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "mbx_context.h"
#include "mbx_fallback.h"
#include "mbx_program.h"
#include "mbx_resource.h"

/* --- the software winsys: a display target is an MBX buffer, mapped --- */

struct fb_dt {
	struct mbx_bo *bo;
};

struct fb_ws {
	struct sw_winsys base;
	struct mbx_bo *pending;		/* the buffer the next displaytarget_from_handle wraps */
};

static void fb_ws_destroy(struct sw_winsys *ws)
{
	FREE(ws);
}

static bool fb_ws_format_supported(struct sw_winsys *ws, unsigned usage, enum pipe_format format)
{
	return true;
}

static struct sw_displaytarget *fb_ws_dt_create(struct sw_winsys *ws, unsigned usage, enum pipe_format format,
						unsigned width, unsigned height, unsigned alignment,
						const void *front_private, unsigned *stride)
{
	return NULL;		/* only buffers that already exist are shared: softpipe allocates the rest itself */
}

static struct sw_displaytarget *fb_ws_dt_from_handle(struct sw_winsys *ws, const struct pipe_resource *templ,
						     struct winsys_handle *whandle, unsigned *stride)
{
	struct fb_ws *fws = (struct fb_ws *)ws;
	struct fb_dt *dt = CALLOC_STRUCT(fb_dt);

	if (!dt || !fws->pending)
		return NULL;
	dt->bo = mbx_bo_ref(fws->pending);
	*stride = whandle->stride;
	return (struct sw_displaytarget *)dt;
}

static bool fb_ws_dt_get_handle(struct sw_winsys *ws, struct sw_displaytarget *dt, struct winsys_handle *whandle)
{
	return false;
}

static void *fb_ws_dt_map(struct sw_winsys *ws, struct sw_displaytarget *dt, unsigned flags)
{
	return mbx_bo_map(((struct fb_dt *)dt)->bo);
}

static void fb_ws_dt_unmap(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
}

static void fb_ws_dt_display(struct sw_winsys *ws, struct sw_displaytarget *dt, void *context_private,
			     unsigned nboxes, struct pipe_box *box)
{
}

static void fb_ws_dt_destroy(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
	struct fb_dt *d = (struct fb_dt *)dt;

	mbx_bo_unref(d->bo);
	FREE(d);
}

static struct pipe_screen *fb_screen(struct mbx_screen *screen)
{
	if (!screen->sp_screen) {
		struct fb_ws *ws = CALLOC_STRUCT(fb_ws);

		if (!ws)
			return NULL;
		ws->base.destroy = fb_ws_destroy;
		ws->base.is_displaytarget_format_supported = fb_ws_format_supported;
		ws->base.displaytarget_create = fb_ws_dt_create;
		ws->base.displaytarget_from_handle = fb_ws_dt_from_handle;
		ws->base.displaytarget_get_handle = fb_ws_dt_get_handle;
		ws->base.displaytarget_map = fb_ws_dt_map;
		ws->base.displaytarget_unmap = fb_ws_dt_unmap;
		ws->base.displaytarget_display = fb_ws_dt_display;
		ws->base.displaytarget_destroy = fb_ws_dt_destroy;
		screen->sp_ws = &ws->base;
		screen->sp_screen = softpipe_create_screen(&ws->base);
		if (!screen->sp_screen) {
			FREE(ws);
			screen->sp_ws = NULL;
		}
	}
	return screen->sp_screen;
}

void mbx_fallback_screen_destroy(struct mbx_screen *screen)
{
	if (screen->sp_screen)
		screen->sp_screen->destroy(screen->sp_screen);
}

static struct pipe_context *fb_context(struct mbx_context *ctx)
{
	if (!ctx->sp) {
		struct pipe_screen *sps = fb_screen(ctx->screen);

		if (sps)
			ctx->sp = sps->context_create(sps, NULL, 0);
	}
	return ctx->sp;
}

/* --- twins: the softpipe resources standing for ours --- */

/* A resource the MBX renders to (or shows): softpipe wraps its memory (the stride is the buffer's) */
static struct pipe_resource *twin_shared(struct mbx_context *ctx, struct mbx_resource *rsc)
{
	struct pipe_screen *sps = ctx->sp->screen;
	struct fb_ws *ws = (struct fb_ws *)ctx->screen->sp_ws;
	struct pipe_resource t = rsc->base;
	struct winsys_handle w = { .type = WINSYS_HANDLE_TYPE_SHARED, .stride = rsc->stride };

	if (rsc->sp_twin)
		return rsc->sp_twin;
	t.last_level = 0;		/* the memory holds level 0 (and, for a texture, padded rows) */
	t.next = NULL;
	t.screen = sps;
	ws->pending = rsc->bo;
	rsc->sp_twin = sps->resource_from_handle(sps, &t, &w, 0);
	ws->pending = NULL;
	return rsc->sp_twin;
}

/* A depth buffer: softpipe wraps the external Z memory the chip's jobs store to and load from (floats, one a pixel; README assumption 7) */
static struct pipe_resource *twin_zshared(struct mbx_context *ctx, struct mbx_resource *rsc)
{
	struct pipe_screen *sps = ctx->sp->screen;
	struct fb_ws *ws = (struct fb_ws *)ctx->screen->sp_ws;
	struct pipe_resource t = rsc->base;
	struct winsys_handle w = { .type = WINSYS_HANDLE_TYPE_SHARED, .stride = align(rsc->zw, 16) * 4 };

	if (rsc->sp_twin)
		return rsc->sp_twin;
	t.format = PIPE_FORMAT_Z32_FLOAT;
	t.last_level = 0;
	t.next = NULL;
	t.screen = sps;
	t.bind = PIPE_BIND_DEPTH_STENCIL;
	ws->pending = rsc->zbo;
	rsc->sp_twin = sps->resource_from_handle(sps, &t, &w, 0);
	ws->pending = NULL;
	return rsc->sp_twin;
}

/* A sampled texture: softpipe's own copy, refreshed when the texture changed (CPU writes) */
static struct pipe_resource *twin_texture(struct mbx_context *ctx, struct mbx_resource *rsc)
{
	struct pipe_screen *sps = ctx->sp->screen;
	struct pipe_context *sp = ctx->sp;
	struct pipe_resource t = rsc->base;
	uint8_t *base;
	unsigned l;

	if ((rsc->bind_rt && !rsc->twiddled && !rsc->cube) || rsc->scanout)
		return twin_shared(ctx, rsc);
	if (!rsc->sp_twin) {
		t.next = NULL;
		t.screen = sps;
		rsc->sp_twin = sps->resource_create(sps, &t);
		rsc->sp_dirty = true;
	}
	if (rsc->imported) {
		/* another process renders into it: wait for its jobs (the dma-buf signals when the writers are done), and read it again */
		if (rsc->dmabuf_fd > 0) {
			struct pollfd pfd = { .fd = rsc->dmabuf_fd, .events = POLLIN };

			poll(&pfd, 1, 1000);
		}
		rsc->sp_dirty = true;
	}
	if (!rsc->sp_twin || !rsc->sp_dirty)
		return rsc->sp_twin;
	if (rsc->cube && (base = mbx_bo_map(rsc->bo))) {
		for (l = 0; l <= rsc->base.last_level; l++) {
			unsigned w = u_minify(rsc->base.width0, l), h = u_minify(rsc->base.height0, l), f;

			for (f = 0; f < 6; f++) {
				struct pipe_box box = { .z = (int)f, .width = (int)w, .height = (int)h, .depth = 1 };

				sp->texture_subdata(sp, rsc->sp_twin, l, 0, &box,
						    base + rsc->level_off[l] + (uint64_t)f * h * rsc->level_stride[l],
						    rsc->level_stride[l], 0);
			}
		}
		rsc->sp_dirty = false;
		return rsc->sp_twin;
	}
	if (!rsc->tex || !(base = mbx_bo_map(rsc->bo)))
		return rsc->sp_twin;
	for (l = 0; l <= rsc->base.last_level; l++) {
		struct pipe_box box = { .width = u_minify(rsc->base.width0, l), .height = u_minify(rsc->base.height0, l), .depth = 1 };

		if (rsc->twiddled) {	/* back to linear rows for softpipe */
			unsigned stride = box.width * util_format_get_blocksize(rsc->base.format);
			uint8_t *lin = malloc((size_t)stride * box.height);

			if (!lin)
				continue;
			mbx_tex_twiddle(rsc, l, &box, lin, stride, false);
			sp->texture_subdata(sp, rsc->sp_twin, l, 0, &box, lin, stride, 0);
			free(lin);
		} else {
			sp->texture_subdata(sp, rsc->sp_twin, l, 0, &box, base + rsc->level_off[l], rsc->level_stride[l], 0);
		}
	}
	rsc->sp_dirty = false;
	return rsc->sp_twin;
}

void mbx_fallback_resource_destroy(struct mbx_resource *rsc)
{
	pipe_resource_reference(&rsc->sp_twin, NULL);
	pipe_resource_reference(&rsc->zs_twin, NULL);
}

/* --- the state, replayed --- */

static void twin_readback(struct mbx_context *ctx, struct mbx_resource *rsc, unsigned level, const struct pipe_box *box);

static void *fb_shader(struct mbx_context *ctx, struct mbx_shader *so)
{
	struct pipe_context *sp = ctx->sp;

	if (!so->sp_handle && so->nir_orig) {
		struct pipe_shader_state s = { .type = PIPE_SHADER_IR_NIR };

		s.ir.nir = nir_shader_clone(NULL, so->nir_orig);	/* softpipe takes it */
		so->sp_handle = so->stage == MESA_SHADER_VERTEX ? sp->create_vs_state(sp, &s) : sp->create_fs_state(sp, &s);
	}
	return so->sp_handle;
}

void mbx_fallback_shader_delete(struct mbx_context *ctx, struct mbx_shader *so)
{
	if (so->sp_handle && ctx->sp) {
		/* softpipe keeps the shader it was last given bound: it is let go of before the shader is freed */
		if (so->stage == MESA_SHADER_VERTEX) {
			ctx->sp->bind_vs_state(ctx->sp, NULL);
			ctx->sp->delete_vs_state(ctx->sp, so->sp_handle);
		} else {
			ctx->sp->bind_fs_state(ctx->sp, NULL);
			ctx->sp->delete_fs_state(ctx->sp, so->sp_handle);
		}
	}
	if (so->nir_orig)
		ralloc_free(so->nir_orig);
}

static void *buffer_ptr(const struct pipe_vertex_buffer *vb)
{
	if (vb->is_user_buffer)
		return (void *)vb->buffer.user;
	if (vb->buffer.resource)
		return (uint8_t *)mbx_bo_map(mbx_resource(vb->buffer.resource)->bo) + vb->buffer_offset;
	return NULL;
}

/* --- depth and stencil on the CPU: softpipe's combined twin, its depth a copy of the Z memory around each CPU draw --- */

static bool zs_has_stencil(struct mbx_resource *zr)
{
	return util_format_has_stencil(util_format_description(zr->base.format));
}

static struct pipe_resource *zs_twin(struct mbx_context *ctx, struct mbx_resource *zr)
{
	struct pipe_screen *sps = ctx->sp->screen;
	struct pipe_resource t = zr->base;

	if (zr->zs_twin)
		return zr->zs_twin;
	t.next = NULL;
	t.screen = sps;
	t.last_level = 0;
	zr->zs_twin = sps->resource_create(sps, &t);
	zr->sp_fresh = true;		/* a new stencil plane holds nothing: cleared to zero at its first use */
	return zr->zs_twin;
}

/*
 * The combined twin's memory, written directly (no softpipe clear: its tile cache keeps a lazy clear across a rebind and rewrote the
 * stencil plane). Z24_UNORM_S8_UINT: depth in bits 0-23, stencil above; S8_UINT: a byte. Softpipe is flushed and the framebuffer unbound
 * first by the callers, so no tile of this resource is cached.
 */
/* depth 0..1 to 24 bits; in float 1.0 * 16777215 + 0.5 rounds to 16777216 and carries into the stencil byte, so double and a mask */
static uint32_t z24_of(float d)
{
	return (uint32_t)((double)CLAMP(d, 0.0f, 1.0f) * 16777215.0 + 0.5) & 0xffffffu;
}

static uint8_t *zs_map(struct mbx_context *ctx, struct mbx_resource *zr, unsigned flags, struct pipe_transfer **xfer)
{
	struct pipe_box box = { .width = (int)zr->base.width0, .height = (int)zr->base.height0, .depth = 1 };

	return ctx->sp->texture_map(ctx->sp, zr->zs_twin, 0, flags, &box, xfer);
}

static void zs_unbind(struct mbx_context *ctx)
{
	struct pipe_framebuffer_state none;

	memset(&none, 0, sizeof(none));
	ctx->sp->flush(ctx->sp, NULL, 0);
	ctx->sp->set_framebuffer_state(ctx->sp, &none);
}

/* depth: set the depth plane to d when set_depth; stencil: set the stencil plane to s when set_stencil */
static void zs_fill(struct mbx_context *ctx, struct mbx_resource *zr, bool set_depth, float d, bool set_stencil, unsigned s)
{
	struct pipe_transfer *xfer;
	uint8_t *p = zs_map(ctx, zr, PIPE_MAP_READ | PIPE_MAP_WRITE, &xfer);
	uint32_t z24 = z24_of(d);

	if (!p)
		return;
	for (unsigned y = 0; y < zr->base.height0; y++) {
		if (zr->base.format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
			uint32_t *row = (uint32_t *)(p + (size_t)y * xfer->stride);

			for (unsigned x = 0; x < zr->base.width0; x++) {
				uint32_t v = row[x];

				if (set_depth)
					v = (v & 0xff000000u) | z24;
				if (set_stencil)
					v = (v & 0x00ffffffu) | (s & 0xff) << 24;
				row[x] = v;
			}
		} else if (set_stencil) {	/* S8_UINT */
			memset(p + (size_t)y * xfer->stride, (int)(s & 0xff), zr->base.width0);
		}
	}
	ctx->sp->texture_unmap(ctx->sp, xfer);
}

/* The Z memory's depth into the twin's depth plane (when the twin has one), and back after the CPU drew */
static void zs_copy(struct mbx_context *ctx, struct mbx_resource *zr, bool in)
{
	struct pipe_transfer *xfer;
	uint8_t *p;
	float *zm = zr->zbo ? mbx_bo_map(zr->zbo) : NULL;
	unsigned stride_px = align(zr->zw, 16);

	if (!zm || zr->base.format != PIPE_FORMAT_Z24_UNORM_S8_UINT)
		return;
	p = zs_map(ctx, zr, in ? PIPE_MAP_READ | PIPE_MAP_WRITE : PIPE_MAP_READ, &xfer);
	if (!p)
		return;
	for (unsigned y = 0; y < zr->zh && y < zr->base.height0; y++) {
		uint32_t *row = (uint32_t *)(p + (size_t)y * xfer->stride);
		uint32_t *mem = (uint32_t *)(zm + (size_t)y * stride_px);

		for (unsigned x = 0; x < zr->zw && x < zr->base.width0; x++) {
			if (in) {
				float f;

				memcpy(&f, &mem[x], 4);
				row[x] = (row[x] & 0xff000000u) | z24_of(f);
			} else {
				float f = (float)(row[x] & 0xffffffu) / 16777215.0f;
				uint32_t v;

				memcpy(&v, &f, 4);
				mem[x] = v | 1;			/* stored with the low bit set, as the chip does */
			}
		}
	}
	ctx->sp->texture_unmap(ctx->sp, xfer);
	if (!in)
		zr->zvalid = true;
}

static void sync_framebuffer(struct mbx_context *ctx)
{
	struct pipe_context *sp = ctx->sp;
	struct pipe_framebuffer_state fb = ctx->fb;
	struct mbx_resource *zs_in = NULL;
	unsigned i;

	for (i = 0; i < fb.nr_cbufs; i++)
		if (fb.cbufs[i].texture) {
			struct mbx_resource *c = mbx_resource(ctx->fb.cbufs[i].texture);

			/* a twiddled texture the chip cannot render to: softpipe draws into its own copy, written back after */
			fb.cbufs[i].texture = c->twiddled ? twin_texture(ctx, c) : twin_shared(ctx, c);
		}
	if (fb.zsbuf.texture) {
		struct mbx_resource *zr = mbx_resource(ctx->fb.zsbuf.texture);

		/* the chip's jobs and softpipe share one depth buffer in memory: current before the CPU draws into it */
		if (zs_has_stencil(zr)) {
			/* a stencil plane lives only in softpipe's combined twin; its depth is a copy of the Z memory around each CPU draw */
			fb.zsbuf.texture = zs_twin(ctx, zr);
			zs_in = zr;
		} else if (mbx_z_ensure(ctx, zr)) {
			mbx_resource_wait(ctx->screen, zr);
			mbx_z_fill_if_stale(ctx, zr);
			fb.zsbuf.texture = twin_zshared(ctx, zr);
			fb.zsbuf.format = PIPE_FORMAT_Z32_FLOAT;
		}
	}
	/* the surfaces are new each time, which also drops softpipe's tile caches: the CPU and the MBX have
	 * written the memory since */
	/* the chip has written the memory since softpipe last read it: unbind first so its tile caches (colour and depth) are dropped */
	zs_unbind(ctx);
	if (zs_in) {
		if (zs_in->sp_fresh) {			/* a new combined twin: depth as cleared, stencil zero */
			zs_in->sp_fresh = false;
			zs_fill(ctx, zs_in, true, ctx->next_clear_depth, true, 0);
		}
		if (mbx_z_ensure(ctx, zs_in)) {	/* the chip's depth, when the memory holds the current one */
			mbx_resource_wait(ctx->screen, zs_in);
			mbx_z_fill_if_stale(ctx, zs_in);
			zs_copy(ctx, zs_in, true);
		}
	}
	sp->set_framebuffer_state(sp, &fb);
}

bool mbx_fallback_draw(struct mbx_context *ctx, const struct pipe_draw_info *info, unsigned drawid_offset,
		       const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
	struct pipe_context *sp = fb_context(ctx);
	struct pipe_draw_info di = *info;
	struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
	struct pipe_sampler_view *views[MBX_MAX_SAMPLERS] = { NULL };
	void *samplers[MBX_MAX_SAMPLERS] = { NULL };
	void *blend, *rast, *dsa, *velems, *smp[MBX_MAX_SAMPLERS] = { NULL };
	void *vs, *fs;
	unsigned i;

	if (!sp || !ctx->vs || !ctx->fs || !ctx->velems || !ctx->blend || !ctx->rast || !ctx->dsa || !ctx->fb.nr_cbufs ||
	    !ctx->fb.cbufs[0].texture)
		return false;
	mbx_dbg("fallback: a draw on the CPU (softpipe)");
	/* what was recorded for the MBX finishes first, and the memory is settled */
	ctx->zkeep = true;
	mbx_clear_settle(ctx);
	ctx->zkeep = false;
	mbx_resource_wait(ctx->screen, mbx_resource(ctx->fb.cbufs[0].texture));
	if (!(vs = fb_shader(ctx, ctx->vs)) || !(fs = fb_shader(ctx, ctx->fs)))
		return false;

	blend = sp->create_blend_state(sp, ctx->blend);
	rast = sp->create_rasterizer_state(sp, ctx->rast);
	{
		/* softpipe's stencil path reads the depth function even when the depth test is off (gallium leaves it NEVER there): say
		 * "always, no write" explicitly */
		struct pipe_depth_stencil_alpha_state d = *ctx->dsa;

		if (!d.depth_enabled) {
			d.depth_enabled = true;
			d.depth_func = PIPE_FUNC_ALWAYS;
			d.depth_writemask = false;
		}
		dsa = sp->create_depth_stencil_alpha_state(sp, &d);
	}
	velems = sp->create_vertex_elements_state(sp, ctx->velems->count, ctx->velems->e);
	sp->bind_blend_state(sp, blend);
	sp->bind_rasterizer_state(sp, rast);
	sp->bind_depth_stencil_alpha_state(sp, dsa);
	sp->bind_vertex_elements_state(sp, velems);
	sp->bind_vs_state(sp, vs);
	sp->bind_fs_state(sp, fs);
	sp->set_blend_color(sp, &ctx->blend_color);
	sp->set_viewport_states(sp, 0, 1, &ctx->viewport);
	sp->set_scissor_states(sp, 0, 1, &ctx->scissor);
	sync_framebuffer(ctx);

	for (i = 0; i < ctx->num_vb; i++) {
		memset(&vb[i], 0, sizeof(vb[i]));
		vb[i].is_user_buffer = true;
		vb[i].buffer.user = buffer_ptr(&ctx->vb[i]);
	}
	sp->set_vertex_buffers(sp, ctx->num_vb, vb);
	for (i = 0; i < 2; i++) {
		struct pipe_constant_buffer cb = { .buffer_size = ctx->cb[i].buffer_size };

		if (ctx->cb[i].user_buffer)
			cb.user_buffer = (const uint8_t *)ctx->cb[i].user_buffer + ctx->cb[i].buffer_offset;
		else if (ctx->cb[i].buffer)
			cb.user_buffer = (const uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[i].buffer)->bo) + ctx->cb[i].buffer_offset;
		sp->set_constant_buffer(sp, i ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX, 0, &cb);
	}
	for (i = 0; i < MBX_MAX_SAMPLERS; i++) {
		if (ctx->views[i] && ctx->views[i]->texture) {
			struct pipe_sampler_view t = *ctx->views[i];
			struct pipe_resource *tw = twin_texture(ctx, mbx_resource(ctx->views[i]->texture));

			if (tw) {
				t.texture = NULL;
				t.context = NULL;
				views[i] = sp->create_sampler_view(sp, tw, &t);
			}
		}
		if (ctx->samplers[i])
			smp[i] = samplers[i] = sp->create_sampler_state(sp, ctx->samplers[i]);
	}
	sp->set_sampler_views(sp, MESA_SHADER_FRAGMENT, 0, MBX_MAX_SAMPLERS, 0, views);
	sp->bind_sampler_states(sp, MESA_SHADER_FRAGMENT, 0, MBX_MAX_SAMPLERS, samplers);

	/* an index buffer is read through the CPU */
	if (info->index_size && !info->has_user_indices) {
		di.index.user = mbx_bo_map(mbx_resource(info->index.resource)->bo);
		di.has_user_indices = true;
	}
	sp->bind_depth_stencil_alpha_state(sp, dsa);
	sp->set_stencil_ref(sp, ctx->stencil_ref);
	sp->draw_vbo(sp, &di, drawid_offset, NULL, draws, num_draws);
	sp->flush(sp, NULL, 0);		/* softpipe's tiles are written back to the memory */
	if (ctx->fb.zsbuf.texture && zs_has_stencil(mbx_resource(ctx->fb.zsbuf.texture)) && mbx_resource(ctx->fb.zsbuf.texture)->zs_twin) {
		zs_unbind(ctx);
		zs_copy(ctx, mbx_resource(ctx->fb.zsbuf.texture), false);
	}

	if (mbx_resource(ctx->fb.cbufs[0].texture)->twiddled) {
		struct pipe_box box = { .width = ctx->fb.width, .height = ctx->fb.height, .depth = 1 };

		twin_readback(ctx, mbx_resource(ctx->fb.cbufs[0].texture), ctx->fb.cbufs[0].level, &box);
	}

	sp->bind_blend_state(sp, NULL);
	sp->bind_rasterizer_state(sp, NULL);
	sp->bind_depth_stencil_alpha_state(sp, NULL);
	sp->bind_vertex_elements_state(sp, NULL);
	/* nothing of this draw stays bound in softpipe: the sampler states and views are deleted below, the shaders may be next */
	{
		void *no_samplers[MBX_MAX_SAMPLERS] = { NULL };
		struct pipe_sampler_view *no_views[MBX_MAX_SAMPLERS] = { NULL };

		sp->bind_sampler_states(sp, MESA_SHADER_FRAGMENT, 0, MBX_MAX_SAMPLERS, no_samplers);
		sp->set_sampler_views(sp, MESA_SHADER_FRAGMENT, 0, 0, MBX_MAX_SAMPLERS, no_views);
	}
	sp->bind_vs_state(sp, NULL);
	sp->bind_fs_state(sp, NULL);
	sp->delete_blend_state(sp, blend);
	sp->delete_rasterizer_state(sp, rast);
	sp->delete_depth_stencil_alpha_state(sp, dsa);
	sp->delete_vertex_elements_state(sp, velems);
	for (i = 0; i < MBX_MAX_SAMPLERS; i++) {
		if (samplers[i])
			sp->delete_sampler_state(sp, samplers[i]);
		pipe_sampler_view_reference(&views[i], NULL);
	}
	return true;
}

/* After softpipe wrote a texture it holds privately, the MBX's memory gets the texels too */
static void twin_readback(struct mbx_context *ctx, struct mbx_resource *rsc, unsigned level, const struct pipe_box *box)
{
	struct pipe_context *sp = ctx->sp;
	struct pipe_transfer *xfer;
	unsigned bpp = util_format_get_blocksize(rsc->base.format), y;
	uint8_t *dst = mbx_bo_map(rsc->bo), *src;

	if (!dst || (!rsc->tex && !rsc->cube))	/* a cube map does not have the flag (twin_texture reads it before it looks at the flag too) */
		return;
	src = sp->texture_map(sp, rsc->sp_twin, level, PIPE_MAP_READ, box, &xfer);
	if (!src)
		return;
	if (rsc->twiddled) {
		mbx_tex_twiddle(rsc, level, box, src, xfer->stride, true);
		sp->texture_unmap(sp, xfer);
		rsc->sp_dirty = false;
		return;
	}
	if (rsc->cube)		/* the faces follow one another in a level (as twin_texture reads them) */
		dst += (uint64_t)box->z * u_minify(rsc->base.height0, level) * rsc->level_stride[level];
	for (y = 0; y < (unsigned)box->height; y++)
		memcpy(dst + rsc->level_off[level] + (uint64_t)(box->y + y) * rsc->level_stride[level] + (uint64_t)box->x * bpp,
		       src + (uint64_t)y * xfer->stride, (uint64_t)box->width * bpp);
	sp->texture_unmap(sp, xfer);
	rsc->sp_dirty = false;
}

bool mbx_fallback_blit(struct mbx_context *ctx, const struct pipe_blit_info *info)
{
	struct pipe_context *sp = fb_context(ctx);
	struct mbx_resource *src = mbx_resource(info->src.resource), *dst = mbx_resource(info->dst.resource);
	struct pipe_blit_info b = *info;
	bool shared_dst;

	if (!sp || info->scissor_enable || info->render_condition_enable)
		return false;
	mbx_dbg("fallback: a blit on the CPU (softpipe)");
	ctx->zkeep = true;
	mbx_clear_settle(ctx);
	ctx->zkeep = false;
	mbx_resource_wait(ctx->screen, src);
	mbx_resource_wait(ctx->screen, dst);
	b.src.resource = twin_texture(ctx, src);
	shared_dst = (dst->bind_rt && !dst->twiddled && !dst->cube) || dst->scanout;
	b.dst.resource = shared_dst ? twin_shared(ctx, dst) : twin_texture(ctx, dst);
	if (!b.src.resource || !b.dst.resource)
		return false;
	dst->sp_dirty = false;		/* softpipe's copy is the newer one from here (read back below) */
	dst->wver++;
	sp->blit(sp, &b);
	sp->flush(sp, NULL, 0);
	if (!shared_dst) {
		struct pipe_box box = { .x = info->dst.box.x, .y = info->dst.box.y, .z = dst->cube ? info->dst.box.z : 0,
					.width = info->dst.box.width, .height = info->dst.box.height, .depth = 1 };

		if (box.width < 0) {
			box.x += box.width;
			box.width = -box.width;
		}
		if (box.height < 0) {
			box.y += box.height;
			box.height = -box.height;
		}
		twin_readback(ctx, dst, info->dst.level, &box);
	}
	return true;
}

void mbx_fallback_clear_depth(struct mbx_context *ctx, double depth)
{
	struct pipe_context *sp = ctx->sp;
	struct mbx_resource *zr = ctx->fb.zsbuf.texture ? mbx_resource(ctx->fb.zsbuf.texture) : NULL;

	if (!sp || !zr || !(zr->sp_twin || zr->zs_twin))
		return;		/* softpipe has not drawn here: nothing to clear */
	if (zr->zs_twin) {	/* the combined twin's depth plane, directly */
		zs_unbind(ctx);
		zs_fill(ctx, zr, true, (float)depth, false, 0);
		return;
	}
	sync_framebuffer(ctx);
	sp->clear(sp, PIPE_CLEAR_DEPTH, 0, 0, NULL, NULL, depth, 0);
	sp->flush(sp, NULL, 0);		/* into the shared memory now: a later flush of its tile cache would overwrite what the chip stores */
}

void mbx_fallback_clear_stencil(struct mbx_context *ctx, unsigned stencil)
{
	struct pipe_context *sp = fb_context(ctx);
	struct mbx_resource *zr = ctx->fb.zsbuf.texture ? mbx_resource(ctx->fb.zsbuf.texture) : NULL;

	if (!sp || !zr || !zs_has_stencil(zr))
		return;
	zs_twin(ctx, zr);
	zs_unbind(ctx);
	if (zr->sp_fresh) {
		zr->sp_fresh = false;
		zs_fill(ctx, zr, true, ctx->next_clear_depth, true, 0);
	}
	zs_fill(ctx, zr, false, 0.0f, true, stencil);
}

bool mbx_fallback_read_stencil(struct mbx_context *ctx, uint8_t *out)
{
	struct pipe_context *sp = fb_context(ctx);
	struct mbx_resource *zr = ctx->fb.zsbuf.texture ? mbx_resource(ctx->fb.zsbuf.texture) : NULL;
	struct pipe_transfer *xfer;
	uint8_t *p;

	if (!sp || !zr || !zs_has_stencil(zr))
		return false;
	zs_twin(ctx, zr);
	zs_unbind(ctx);
	if (zr->sp_fresh) {
		zr->sp_fresh = false;
		zs_fill(ctx, zr, true, ctx->next_clear_depth, true, 0);
	}
	p = zs_map(ctx, zr, PIPE_MAP_READ, &xfer);
	if (!p)
		return false;
	for (unsigned y = 0; y < ctx->fb.height && y < zr->base.height0; y++)
		for (unsigned x = 0; x < ctx->fb.width && x < zr->base.width0; x++)
			out[(size_t)y * ctx->fb.width + x] = zr->base.format == PIPE_FORMAT_Z24_UNORM_S8_UINT ?
				(uint8_t)(((uint32_t *)(p + (size_t)y * xfer->stride))[x] >> 24) : p[(size_t)y * xfer->stride + x];
	sp->texture_unmap(sp, xfer);
	return true;
}

void mbx_fallback_context_destroy(struct mbx_context *ctx)
{
	if (ctx->sp)
		ctx->sp->destroy(ctx->sp);
	ctx->sp = NULL;
}
