// SPDX-License-Identifier: MIT
/*
 * Resources: buffers, and 2D textures and render targets laid out linearly. A
 * row is padded to a whole number of 16-pixel tiles and the height likewise,
 * so a render that writes whole tiles stays in the buffer. A depth buffer
 * takes no memory: the MBX keeps depth on chip for a render.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm-uapi/drm_fourcc.h"

#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "frontend/winsys_handle.h"

#include "mbx_context.h"
#include "mbx_fallback.h"
#include "mbx_resource.h"
#include "mbx_screen.h"

#define MBX_TILE 16

/* A buffer the display scans out: the KMS device allocates it (omapdrm's dumb buffers are
 * contiguous, as its DSS needs) and we import it by dma-buf into our address space. */
static struct pipe_resource *mbx_resource_create_scanout(struct pipe_screen *pscreen,
							 const struct pipe_resource *templ)
{
	struct mbx_screen *screen = mbx_screen(pscreen);
	struct mbx_resource *rsc = CALLOC_STRUCT(mbx_resource);
	struct pipe_resource t = *templ;
	struct winsys_handle handle = { 0 };

	if (!rsc)
		return NULL;
	/* a render writes whole tiles */
	t.width0 = align(templ->width0, MBX_TILE);
	t.height0 = align(templ->height0, MBX_TILE);
	t.screen = pscreen;
	rsc->scanout = renderonly_scanout_for_resource(&t, screen->ro, &handle);
	if (!rsc->scanout) {
		FREE(rsc);
		return NULL;
	}
	rsc->base = *templ;
	rsc->base.screen = pscreen;
	pipe_reference_init(&rsc->base.reference, 1);
	rsc->bo = mbx_bo_import(screen, (int)handle.handle);
	close((int)handle.handle);
	if (!rsc->bo) {
		renderonly_scanout_destroy(rsc->scanout, screen->ro);
		FREE(rsc);
		return NULL;
	}
	rsc->stride = handle.stride;
	rsc->size = (uint32_t)rsc->bo->size;
	mbx_dbg("scanout resource %ux%u format %s: stride %u, bo size %llu, va %#x", templ->width0, templ->height0,
		util_format_name(templ->format), rsc->stride, (unsigned long long)rsc->bo->size, rsc->bo->va);
	return &rsc->base;
}

static struct pipe_resource *mbx_resource_create(struct pipe_screen *pscreen,
						 const struct pipe_resource *templ)
{
	struct mbx_screen *screen = mbx_screen(pscreen);
	struct mbx_resource *rsc;
	uint64_t size;

	if (screen->ro && (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
	    templ->target != PIPE_BUFFER && !util_format_is_depth_or_stencil(templ->format))
		return mbx_resource_create_scanout(pscreen, templ);

	rsc = CALLOC_STRUCT(mbx_resource);
	if (!rsc)
		return NULL;
	rsc->base = *templ;
	rsc->base.screen = pscreen;
	pipe_reference_init(&rsc->base.reference, 1);
	rsc->bind_rt = (templ->bind & PIPE_BIND_RENDER_TARGET) != 0;

	if (templ->target == PIPE_BUFFER) {
		size = templ->width0;
	} else if (util_format_is_depth_or_stencil(templ->format)) {
		rsc->on_chip = true;
		size = 4096;
	} else if (templ->target == PIPE_TEXTURE_2D && (templ->bind & PIPE_BIND_SAMPLER_VIEW) &&
		   !(templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
		   mbx_chip_tex_format(templ->format) >= 0 && templ->last_level > 0 && templ->last_level < MBX_MAX_LEVELS &&
		   util_is_power_of_two_nonzero(templ->width0) && util_is_power_of_two_nonzero(templ->height0) &&
		   templ->width0 >= 8 && templ->height0 >= 8) {
		/* mipmaps: the chip reads them from a twiddled chain, the largest level first, no padding (the chip cannot
		 * render into it: draws to such a texture go through the CPU fallback, mbx_fallback.c) */
		unsigned bpp = util_format_get_blocksize(templ->format), l;
		uint64_t off = 0;

		rsc->tex = rsc->twiddled = true;
		rsc->tex_lw = util_logbase2(templ->width0);
		rsc->tex_lh = util_logbase2(templ->height0);
		for (l = 0; l <= templ->last_level; l++) {
			rsc->level_off[l] = (uint32_t)off;
			off += (uint64_t)u_minify(templ->width0, l) * u_minify(templ->height0, l) * bpp;
		}
		size = align64(off, 128);
	} else if ((templ->target == PIPE_TEXTURE_2D || templ->target == PIPE_TEXTURE_RECT) &&
		   (templ->bind & PIPE_BIND_SAMPLER_VIEW) &&
		   !(templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
		   mbx_chip_tex_format(templ->format) >= 0 && templ->last_level < MBX_MAX_LEVELS) {
		unsigned bpp = util_format_get_blocksize(templ->format), l;
		uint64_t off = 0;

		rsc->tex = true;
		for (l = 0; l <= templ->last_level; l++) {
			/* the sampled size is the next power of two of at least 8; the row a render target needs is a
			 * multiple of 16 texels (the background object's stride field) */
			unsigned w = MAX2(8, util_next_power_of_two(u_minify(templ->width0, l)));
			unsigned h = MAX2(8, util_next_power_of_two(u_minify(templ->height0, l)));
			unsigned pitch = MAX2(templ->bind & PIPE_BIND_RENDER_TARGET ? 16 : 8, w);

			if (l == 0) {
				rsc->tex_lw = util_logbase2(w);
				rsc->tex_lh = util_logbase2(h);
			}
			rsc->level_off[l] = (uint32_t)off;
			rsc->level_stride[l] = pitch * bpp;
			off = align64(off + (uint64_t)pitch * bpp * h, 128);
		}
		rsc->stride = rsc->level_stride[0];
		size = off;
	} else {
		unsigned bpp = util_format_get_blocksize(templ->format);

		rsc->stride = align(templ->width0, MBX_TILE) * bpp;
		size = (uint64_t)rsc->stride * align(templ->height0, MBX_TILE);
	}
	rsc->size = (uint32_t)size;
	rsc->bo = mbx_bo_create(screen, align64(size ? size : 1, 4096));
	if (!rsc->bo) {
		FREE(rsc);
		return NULL;
	}
	return &rsc->base;
}

static struct pipe_resource *mbx_resource_create_with_modifiers(struct pipe_screen *pscreen,
								 const struct pipe_resource *templ,
								 const uint64_t *modifiers, int count)
{
	/* linear is all there is */
	for (int i = 0; i < count; i++)
		if (modifiers[i] == DRM_FORMAT_MOD_LINEAR || modifiers[i] == DRM_FORMAT_MOD_INVALID)
			return mbx_resource_create(pscreen, templ);
	return NULL;
}

/* A buffer from another device (a window system's, or EGL image import) */
static struct pipe_resource *mbx_resource_from_handle(struct pipe_screen *pscreen,
						      const struct pipe_resource *templ,
						      struct winsys_handle *handle, unsigned usage)
{
	struct mbx_screen *screen = mbx_screen(pscreen);
	struct mbx_resource *rsc;

	if (handle->type != WINSYS_HANDLE_TYPE_FD ||
	    (handle->modifier != DRM_FORMAT_MOD_LINEAR && handle->modifier != DRM_FORMAT_MOD_INVALID))
		return NULL;
	rsc = CALLOC_STRUCT(mbx_resource);
	if (!rsc)
		return NULL;
	rsc->base = *templ;
	rsc->base.screen = pscreen;
	pipe_reference_init(&rsc->base.reference, 1);
	rsc->bo = mbx_bo_import(screen, (int)handle->handle);
	if (!rsc->bo) {
		FREE(rsc);
		return NULL;
	}
	rsc->stride = handle->stride;
	rsc->size = (uint32_t)rsc->bo->size;
	return &rsc->base;
}

static bool mbx_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *pctx,
				    struct pipe_resource *prsc, struct winsys_handle *handle, unsigned usage)
{
	struct mbx_screen *screen = mbx_screen(pscreen);
	struct mbx_resource *rsc = mbx_resource(prsc);

	handle->modifier = DRM_FORMAT_MOD_LINEAR;
	if (handle->type == WINSYS_HANDLE_TYPE_KMS && screen->ro && rsc->scanout)
		return renderonly_get_handle(rsc->scanout, handle);
	if (handle->type != WINSYS_HANDLE_TYPE_FD && handle->type != WINSYS_HANDLE_TYPE_SHARED)
		return false;
	{
		int fd;

		if (drmPrimeHandleToFD(screen->fd, rsc->bo->handle, DRM_CLOEXEC | DRM_RDWR, &fd))
			return false;
		handle->handle = (unsigned)fd;
	}
	handle->stride = rsc->stride;
	handle->offset = 0;
	return true;
}

static bool mbx_resource_get_param(struct pipe_screen *pscreen, struct pipe_context *pctx,
				   struct pipe_resource *prsc, unsigned plane, unsigned layer, unsigned level,
				   enum pipe_resource_param param, unsigned handle_usage, uint64_t *value)
{
	struct mbx_resource *rsc = mbx_resource(prsc);

	switch (param) {
	case PIPE_RESOURCE_PARAM_STRIDE:
		*value = rsc->stride;
		return true;
	case PIPE_RESOURCE_PARAM_OFFSET:
		*value = 0;
		return true;
	case PIPE_RESOURCE_PARAM_MODIFIER:
		*value = DRM_FORMAT_MOD_LINEAR;
		return true;
	case PIPE_RESOURCE_PARAM_NPLANES:
		*value = 1;
		return true;
	default:
		return false;
	}
}

static void mbx_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
	struct mbx_resource *rsc = mbx_resource(prsc);

	if (rsc->scanout)
		renderonly_scanout_destroy(rsc->scanout, mbx_screen(pscreen)->ro);
	mbx_fallback_resource_destroy(rsc);
	mbx_bo_unref(rsc->bo);
	FREE(rsc);
}

/* --- CPU access: the buffers are mapped write-combined, so a map is a pointer --- */

struct mbx_transfer {
	struct pipe_transfer base;
	uint8_t *staging;	/* a twiddled texture's box in linear order, until the unmap */
};

/* The index of texel (u, v) in a twiddled 2^lw x 2^lh level: v's bits even, u's odd, the longer side's extra bits on top */
static uint32_t twiddle_index(uint32_t u, uint32_t v, int lw, int lh)
{
	int lmin = lw < lh ? lw : lh, b;
	uint32_t i = 0;

	for (b = 0; b < lmin; b++)
		i |= (v >> b & 1) << (2 * b) | (u >> b & 1) << (2 * b + 1);
	return i | (lw > lh ? u >> lmin : v >> lmin) << (2 * lmin);
}

void mbx_tex_twiddle(struct mbx_resource *rsc, unsigned level, const struct pipe_box *box, uint8_t *lin,
		     unsigned lin_stride, bool store)
{
	unsigned bpp = util_format_get_blocksize(rsc->base.format), x, y;
	unsigned w = u_minify(rsc->base.width0, level), h = u_minify(rsc->base.height0, level);
	int lw = util_logbase2(w), lh = util_logbase2(h);
	uint8_t *base = mbx_bo_map(rsc->bo);

	if (!base)
		return;
	base += rsc->level_off[level];
	for (y = 0; y < (unsigned)box->height; y++)
		for (x = 0; x < (unsigned)box->width; x++) {
			uint8_t *t = base + (uint64_t)twiddle_index(box->x + x, box->y + y, lw, lh) * bpp;
			uint8_t *l = lin + (uint64_t)y * lin_stride + (uint64_t)x * bpp;

			if (store)
				memcpy(t, l, bpp);
			else
				memcpy(l, t, bpp);
		}
}

static void *mbx_transfer_map(struct pipe_context *pctx, struct pipe_resource *prsc, unsigned level,
			      unsigned usage, const struct pipe_box *box, struct pipe_transfer **ptransfer)
{
	struct mbx_resource *rsc = mbx_resource(prsc);
	struct mbx_transfer *trans = CALLOC_STRUCT(mbx_transfer);
	uint8_t *base;

	if (!trans)
		return NULL;
	/* what is recorded against this resource has to be rendered first */
	mbx_flush_for_resource(mbx_context(pctx), rsc);
	mbx_resource_wait(mbx_screen(pctx->screen), rsc);
	base = mbx_bo_map(rsc->bo);
	if (!base) {
		FREE(trans);
		return NULL;
	}
	pipe_resource_reference(&trans->base.resource, prsc);
	trans->base.level = level;
	trans->base.usage = usage;
	trans->base.box = *box;
	if (prsc->target == PIPE_BUFFER) {
		trans->base.stride = 0;
		trans->base.layer_stride = 0;
		*ptransfer = &trans->base;
		return base + box->x;
	}
	if (rsc->twiddled) {
		unsigned bpp = util_format_get_blocksize(prsc->format);

		trans->base.stride = box->width * bpp;
		trans->base.layer_stride = trans->base.stride * box->height;
		trans->staging = malloc((size_t)trans->base.layer_stride);
		if (!trans->staging) {
			pipe_resource_reference(&trans->base.resource, NULL);
			FREE(trans);
			return NULL;
		}
		if (usage & PIPE_MAP_READ)
			mbx_tex_twiddle(rsc, level, box, trans->staging, trans->base.stride, false);
		*ptransfer = &trans->base;
		return trans->staging;
	}
	if (rsc->tex) {
		trans->base.stride = rsc->level_stride[level];
		trans->base.layer_stride = rsc->size;
		*ptransfer = &trans->base;
		return base + rsc->level_off[level] + (uint64_t)box->y * rsc->level_stride[level] +
		       (uint64_t)box->x * util_format_get_blocksize(prsc->format);
	}
	trans->base.stride = rsc->stride;
	trans->base.layer_stride = rsc->size;
	*ptransfer = &trans->base;
	return base + (uint64_t)box->y * rsc->stride +
	       (uint64_t)(box->x / util_format_get_blockwidth(prsc->format)) * util_format_get_blocksize(prsc->format);
}

/*
 * The chip does not filter next to a texel whose R, G and B are all zero (README assumption 39), so a black texel
 * is written as colour 1 where that is invisible: ARGB8888 (blue 1) and LA88 (luminance 1), 1/255 off. (The
 * 16-bit formats would be 3 to 6 per cent off, which shows on black, so they keep their zeros.)
 */
static void nudge_black(enum pipe_format format, uint8_t *p, unsigned stride, unsigned w, unsigned h)
{
	unsigned x, y;

	if (format == PIPE_FORMAT_B8G8R8A8_UNORM) {
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				uint8_t *t = p + (uint64_t)y * stride + (uint64_t)x * 4;

				if (!(t[0] | t[1] | t[2]))
					t[0] = 1;
			}
	} else if (format == PIPE_FORMAT_L8A8_UNORM) {
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				uint8_t *t = p + (uint64_t)y * stride + (uint64_t)x * 2;

				if (!t[0])
					t[0] = 1;
			}
	}
}

/* The texture's padding repeats its edge texels, so clamping to the padded size is clamping to the edge (NPOT) */
static void tex_fill_padding(struct mbx_resource *rsc, struct pipe_transfer *t)
{
	unsigned bpp = util_format_get_blocksize(rsc->base.format), l = t->level;
	unsigned w = u_minify(rsc->base.width0, l), h = u_minify(rsc->base.height0, l);
	unsigned pw = l == 0 ? 1u << rsc->tex_lw : MAX2(8, util_next_power_of_two(w)), ph, x, y;
	uint8_t *base = mbx_bo_map(rsc->bo);

	ph = l == 0 ? 1u << rsc->tex_lh : MAX2(8, util_next_power_of_two(h));
	if (!base || !w || !h)
		return;
	base += rsc->level_off[l];
	if (t->box.x + t->box.width == (int)w && pw > w)
		for (y = t->box.y; y < (unsigned)(t->box.y + t->box.height) && y < h; y++)
			for (x = w; x < pw; x++)
				memcpy(base + (uint64_t)y * rsc->level_stride[l] + (uint64_t)x * bpp,
				       base + (uint64_t)y * rsc->level_stride[l] + (uint64_t)(w - 1) * bpp, bpp);
	if (t->box.y + t->box.height == (int)h && ph > h)
		for (y = h; y < ph; y++)
			memcpy(base + (uint64_t)y * rsc->level_stride[l], base + (uint64_t)(h - 1) * rsc->level_stride[l],
			       (uint64_t)pw * bpp);
}

static void mbx_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
	struct mbx_resource *rsc = mbx_resource(ptrans->resource);

	struct mbx_transfer *mt = (struct mbx_transfer *)ptrans;

	if (mt->staging) {
		if (ptrans->usage & PIPE_MAP_WRITE) {
			nudge_black(rsc->base.format, mt->staging, ptrans->stride, ptrans->box.width, ptrans->box.height);
			mbx_tex_twiddle(rsc, ptrans->level, &ptrans->box, mt->staging, ptrans->stride, true);
		}
		free(mt->staging);
	}
	if (ptrans->usage & PIPE_MAP_WRITE)
		rsc->sp_dirty = true;
	if (rsc->tex && !rsc->twiddled && (ptrans->usage & PIPE_MAP_WRITE)) {
		uint8_t *bp = mbx_bo_map(rsc->bo);

		if (bp)
			nudge_black(rsc->base.format, bp + rsc->level_off[ptrans->level] +
				    (uint64_t)ptrans->box.y * rsc->level_stride[ptrans->level] +
				    (uint64_t)ptrans->box.x * util_format_get_blocksize(rsc->base.format),
				    rsc->level_stride[ptrans->level], ptrans->box.width, ptrans->box.height);
		tex_fill_padding(rsc, ptrans);
	}
	pipe_resource_reference(&ptrans->resource, NULL);
	FREE(ptrans);
}

/* The window system is about to use a resource (a swap): what was drawn into it is rendered first */
static void mbx_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
	mbx_flush_for_resource(mbx_context(pctx), mbx_resource(prsc));
}

/* Same-format, unscaled copies only, through the CPU; the rest is not done yet */
static void mbx_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
	if (!util_try_blit_via_copy_region(pctx, info, false) && !mbx_fallback_blit(mbx_context(pctx), info))
		mesa_loge("mbx: a blit that is not a plain copy (scaling, conversion, masks) could not be done");
}

void mbx_resource_screen_init(struct pipe_screen *pscreen)
{
	pscreen->resource_create = mbx_resource_create;
	pscreen->resource_create_with_modifiers = mbx_resource_create_with_modifiers;
	pscreen->resource_from_handle = mbx_resource_from_handle;
	pscreen->resource_get_handle = mbx_resource_get_handle;
	pscreen->resource_get_param = mbx_resource_get_param;
	pscreen->resource_destroy = mbx_resource_destroy;
}

void mbx_resource_context_init(struct pipe_context *pctx)
{
	pctx->buffer_map = mbx_transfer_map;
	pctx->texture_map = mbx_transfer_map;
	pctx->buffer_unmap = mbx_transfer_unmap;
	pctx->texture_unmap = mbx_transfer_unmap;
	pctx->transfer_flush_region = u_default_transfer_flush_region;
	pctx->buffer_subdata = u_default_buffer_subdata;
	pctx->texture_subdata = u_default_texture_subdata;
	pctx->resource_copy_region = util_resource_copy_region;
	pctx->blit = mbx_blit;
	pctx->flush_resource = mbx_flush_resource;
}
