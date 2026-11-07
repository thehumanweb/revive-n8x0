// SPDX-License-Identifier: MIT
/*
 * draw_vbo: the vertices go into the TA stream as the CPU reads them (the TA
 * takes vertex data inline, there is no vertex fetch), after an object header
 * and, when the vertex shader changed, the VGP program and its constants.
 */
#include <string.h>

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/log.h"
#include "util/u_prim.h"

#include "mbx_context.h"
#include "mbx_fallback.h"
#include "mbx_program.h"
#include "mbx_vgp.h"

#include <math.h>

/* The ISP/TSP state word (mbx/emu.h, render.c): the bits the model reads */
#define ST_NODEPTHWRITE	(1u << 7)
#define ST_SRC_ONE	(1u << 29)
#define ST_BLEND	(1u << 13)
#define ST_ALPHATEST	(1u << 14)
#define ST_GOURAUD	(1u << 22)
#define ST_SATURATE	(1u << 5)
#define ST_ALPHANEVER	(1u << 8)

#define PRIM_CULL_POS	(1u << 18)	/* culls triangles of positive area in surface pixels (y down) */
#define PRIM_CULL_NEG	(1u << 19)	/* and negative */

/*
 * A blend factor as the state word's 3-bit code (mbx/render.c, factor8): 0
 * zero, 1 one, 2 the other side's colour, 3 one minus it, 4 source alpha, 5
 * one minus, 6 destination alpha, 7 one minus. "The other side" means the
 * source factor takes the destination colour and the destination factor the
 * source colour, so SRC_COLOR as a source factor (and DST_COLOR as a
 * destination factor) has no code. Returns -1 for what the chip can't do.
 */
static int hw_factor(unsigned f, bool dst_side, bool *saturate)
{
	switch (f) {
	case PIPE_BLENDFACTOR_ZERO: return 0;
	case PIPE_BLENDFACTOR_ONE: return 1;
	case PIPE_BLENDFACTOR_DST_COLOR: return dst_side ? -1 : 2;
	case PIPE_BLENDFACTOR_SRC_COLOR: return dst_side ? 2 : -1;
	case PIPE_BLENDFACTOR_INV_DST_COLOR: return dst_side ? -1 : 3;
	case PIPE_BLENDFACTOR_INV_SRC_COLOR: return dst_side ? 3 : -1;
	case PIPE_BLENDFACTOR_SRC_ALPHA: return 4;
	case PIPE_BLENDFACTOR_INV_SRC_ALPHA: return 5;
	case PIPE_BLENDFACTOR_DST_ALPHA: return 6;
	case PIPE_BLENDFACTOR_INV_DST_ALPHA: return 7;
	case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
		if (dst_side)
			return -1;
		*saturate = true;
		return 0;
	default: return -1;
	}
}

/* The blend and colour-mask bits of the state word, from the bound blend state */
static uint32_t hw_blend_bits(struct mbx_context *ctx)
{
	const struct pipe_rt_blend_state *rt = ctx->blend ? &ctx->blend->rt[0] : NULL;
	uint32_t st = 0;
	bool sat = false;
	int sf, df;

	if (!rt)
		return ST_SRC_ONE;
	if (!rt->colormask)
		return ST_ALPHANEVER;	/* no colour written: the depth still is */
	if (rt->colormask != 0xf)
		mesa_logw_once("mbx: a partial colour mask is not supported yet");
	if (!rt->blend_enable)
		return ST_SRC_ONE;	/* (ONE, ZERO) is no blend; the zero code would write black */
	sf = hw_factor(rt->rgb_src_factor, false, &sat);
	df = hw_factor(rt->rgb_dst_factor, true, &sat);
	if (rt->rgb_func != PIPE_BLEND_ADD || rt->alpha_func != PIPE_BLEND_ADD ||
	    rt->rgb_src_factor != rt->alpha_src_factor || rt->rgb_dst_factor != rt->alpha_dst_factor ||
	    sf < 0 || df < 0) {
		mesa_logw_once("mbx: this blend state is not supported yet (separate or non-add blending, or a constant colour)");
		return ST_SRC_ONE;
	}
	st = ST_BLEND | (uint32_t)sf << 29 | (uint32_t)df << 26;
	if (sat)
		st |= ST_SATURATE;
	return st;
}

/* State the chip cannot do: the CPU fallback draws it (mbx_fallback.c) */
static bool state_needs_fallback(struct mbx_context *ctx)
{
	const struct pipe_rt_blend_state *rt = ctx->blend ? &ctx->blend->rt[0] : NULL;
	bool sat;

	if (rt) {
		if (rt->colormask && rt->colormask != 0xf)
			return true;		/* a partial colour mask: the chip writes all of the colour or none */
		if (rt->colormask && rt->blend_enable &&
		    (rt->rgb_func != PIPE_BLEND_ADD || rt->alpha_func != PIPE_BLEND_ADD ||
		     rt->rgb_src_factor != rt->alpha_src_factor || rt->rgb_dst_factor != rt->alpha_dst_factor ||
		     hw_factor(rt->rgb_src_factor, false, &sat) < 0 || hw_factor(rt->rgb_dst_factor, true, &sat) < 0))
			return true;		/* separate or non-add blending, a constant colour */
	}
	if (ctx->rast && ctx->rast->offset_tri && (ctx->rast->offset_units != 0 || ctx->rast->offset_scale != 0))
		return true;			/* polygon offset (the primitive word has a bias; not used yet) */
	if (ctx->dsa && ctx->fb.zsbuf.texture && (ctx->dsa->stencil[0].enabled || ctx->dsa->stencil[1].enabled) &&
	    !mbx_stencil_chip_ok(ctx))
		return true;			/* no stencil on the chip: only a test is baked into the depth (mbx_stencil.c) */
	if (ctx->dsa && ctx->dsa->alpha_enabled)
		return true;
	for (unsigned u = 0; u < MBX_MAX_SAMPLERS; u++)
		if (ctx->views[u] && ctx->views[u]->texture && ctx->views[u]->texture->target == PIPE_TEXTURE_CUBE)
			return true;		/* cube maps: softpipe samples them */
	if (ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture && mbx_resource(ctx->fb.cbufs[0].texture)->twiddled)
		return true;			/* a mipmapped texture is twiddled: the chip cannot render into it */
	if (ctx->fsi && (*ctx->fsi).kind == MBX_FS_MATCHED)
		for (unsigned u = 0; u < MBX_MAX_TEX; u++)
			if (((*ctx->fsi).mc >> u & 1) && ctx->views[u] && ctx->views[u]->texture &&
			    ctx->views[u]->texture->format == PIPE_FORMAT_A8_UNORM)
				return true;	/* an alpha-only texture's colour is zero, the chip returns its byte in every channel */
	return false;
}

static bool want_trilinear(struct mbx_context *ctx, const struct mbx_vgp_program *prog);

static uint32_t hw_state_word(struct mbx_context *ctx, const struct mbx_vgp_program *prog)
{
	uint32_t st = prog->st_extra;
	const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
	unsigned func = 7;	/* always */

	st |= 1u << 25;		/* with the alpha word ALWAYS (0xe0000000): the colour is exact; without bit 25 it is ANDed with the target */
	if (prog->gouraud)
		st |= ST_GOURAUD;
	if (ctx->smask.active && ctx->smask.override) {
		func = PIPE_FUNC_LESS;	/* the stencil mask is in the depth: 2.0 where it passes, 0.0 where it does not */
		st |= ST_NODEPTHWRITE;
	} else if (dsa && dsa->depth_enabled) {
		func = dsa->depth_func;	/* PIPE_FUNC_* has the hardware's numbering */
		if (!dsa->depth_writemask)
			st |= ST_NODEPTHWRITE;
	} else {
		st |= ST_NODEPTHWRITE;
	}
	st |= func << 9;
	if (want_trilinear(ctx, prog))
		st = (st & ~(7u << 16)) | 4u << 16;	/* two-pass trilinear */
	st |= hw_blend_bits(ctx);
	if (ctx->fsi && (*ctx->fsi).kind == MBX_FS_MATCHED && (*ctx->fsi).alpha_word)
		st |= ST_ALPHATEST;		/* the alpha function takes effect only with bit 14 (found during device testing, README assumption 45) */
	return st;
}

static uint32_t hw_cull_bits(struct mbx_context *ctx)
{
	const struct pipe_rasterizer_state *r = ctx->rast;
	bool front_neg;

	if (!r || r->cull_face == PIPE_FACE_NONE)
		return 0;
	/* front_ccw is the winding in surface pixels with y = 0 at the top (the state tracker has already
	 * accounted for the viewport's flip and for render-to-texture); a triangle that looks counter-clockwise
	 * there has a negative area in the numbers the TA computes */
	front_neg = r->front_ccw;
	if (r->cull_face == PIPE_FACE_BACK)
		return front_neg ? PRIM_CULL_POS : PRIM_CULL_NEG;
	return front_neg ? PRIM_CULL_NEG : PRIM_CULL_POS;
}

/*
 * The vertex layout: input register k is vertex element k (the state tracker
 * orders the vertex shader's inputs densely). A float format sends its
 * components; four bytes unorm go as one packed word, which the VGP reads as
 * x, y, z, w in memory order; anything else is converted to floats here.
 */
static uint32_t vertex_layout(struct mbx_context *ctx, const struct mbx_vgp_program *prog,
			      unsigned comps[MBX_MAX_ATTRS])
{
	uint32_t layout = 0;

	for (unsigned k = 0; k < MBX_MAX_ATTRS; k++) {
		comps[k] = 0;
		if (k >= ctx->velems->count || !prog->attr_used[k])
			continue;
		if (ctx->velems->e[k].src_format == PIPE_FORMAT_R8G8B8A8_UNORM)
			comps[k] = 5;
		else
			comps[k] = util_format_get_nr_components(ctx->velems->e[k].src_format);
		layout |= comps[k] << (4 * k);
	}
	return layout;
}

/* Texture words A and B of unit 0 from the bound view and sampler: a linear power-of-two texture with a stride */
/* Trilinear: the chip's two-pass mode (state bits 16-18 = 4, word B 0x4e000000, README assumption 47) takes one texture unit and a twiddled chain */
static bool want_trilinear(struct mbx_context *ctx, const struct mbx_vgp_program *prog)
{
	const struct pipe_sampler_state *smp = ctx->samplers[0];
	struct pipe_sampler_view *view = ctx->views[0];

	return prog->ntex == 1 && smp && view && view->texture && mbx_resource(view->texture)->twiddled &&
	       view->texture->last_level > 0 && smp->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR;
}

/* The texture a unit samples: the bound one, or for a shader that swaps red and blue its copy with them swapped */
static struct mbx_resource *unit_texture(struct mbx_context *ctx, unsigned unit)
{
	struct pipe_sampler_view *view = ctx->views[unit];
	struct mbx_resource *tr;

	if (!view || !view->texture)
		return NULL;
	tr = mbx_resource(view->texture);
	if (ctx->fsi && ctx->fsi->swap_rb)
		return mbx_texture_swapped(ctx, tr);
	return tr;
}

static bool tex_words(struct mbx_context *ctx, unsigned unit, bool trilinear, uint32_t *a, uint32_t *b)
{
	struct pipe_sampler_view *view = ctx->views[unit];
	const struct pipe_sampler_state *smp = ctx->samplers[unit];
	struct mbx_resource *tr;
	unsigned stride_px, fmt;
	bool clamp_s, clamp_t, bilinear;

	if (!view || !smp || !view->texture || !mbx_resource(view->texture)->tex)
		return false;
	tr = unit_texture(ctx, unit);
	if (!tr)
		return false;
	if (tr->imported && getenv("MBX_DEBUG")) {
		const uint32_t *px = mbx_bo_map(tr->bo);

		mbx_dbg("imported texture %ux%u fmt %s stride %u lw %u lh %u va %#llx fence %llu; texels: first %08x centre %08x last row %08x",
			tr->base.width0, tr->base.height0, util_format_name(tr->base.format), tr->level_stride[0], tr->tex_lw, tr->tex_lh,
			(unsigned long long)tr->bo->va, (unsigned long long)tr->last_fence, px ? px[0] : 0,
			px ? px[(tr->base.height0 / 2) * (tr->level_stride[0] / 4) + tr->base.width0 / 2] : 0,
			px ? px[(tr->base.height0 - 1) * (tr->level_stride[0] / 4) + 5] : 0);
	}
	fmt = tr->base.format == PIPE_FORMAT_B8G8R8X8_UNORM ? 6 : mbx_chip_tex_format(tr->base.format);	/* an imported XRGB buffer: its unused byte is read as alpha */
	if (tr->twiddled) {
		/* a twiddled chain: no stride; mipmapped when the sampler asks (word B bit 29, with the level
		 * bias field 9: found on the chip, round to the nearest level); the chip's trilinear mode (bit 30) is not
		 * understood yet, so the *_MIPMAP_LINEAR filters take the nearest level too */
		bool mip = smp->min_mip_filter != PIPE_TEX_MIPFILTER_NONE;

		clamp_s = smp->wrap_s != PIPE_TEX_WRAP_REPEAT && smp->wrap_s != PIPE_TEX_WRAP_MIRROR_REPEAT;
		clamp_t = smp->wrap_t != PIPE_TEX_WRAP_REPEAT && smp->wrap_t != PIPE_TEX_WRAP_MIRROR_REPEAT;
		bilinear = mip ? smp->min_img_filter == PIPE_TEX_FILTER_LINEAR : smp->mag_img_filter == PIPE_TEX_FILTER_LINEAR;
		*a = (clamp_s ? 1u << 31 : 0) | (clamp_t ? 1u << 29 : 0) | (uint32_t)(tr->tex_lw - 3) << 24 |
		     (uint32_t)(tr->tex_lh - 3) << 20 | fmt << 14 |
		     (trilinear ? 0xbu << 9 : mip ? (uint32_t)tr->base.last_level << 9 : 0);	/* trilinear: the whole chain (0xb) */
		*b = (trilinear ? 0x4e000000u : mip ? 0x32000000u : 0x0e000000u) | (bilinear ? 1u << 31 : 0) | tr->bo->va >> 7;
		return true;
	}
	stride_px = tr->level_stride[0] / util_format_get_blocksize(tr->base.format);
	/* NPOT textures are read with the coordinates scaled (the VGP does it), so only clamping them is right */
	clamp_s = smp->wrap_s != PIPE_TEX_WRAP_REPEAT && smp->wrap_s != PIPE_TEX_WRAP_MIRROR_REPEAT;
	clamp_t = smp->wrap_t != PIPE_TEX_WRAP_REPEAT && smp->wrap_t != PIPE_TEX_WRAP_MIRROR_REPEAT;
	bilinear = smp->mag_img_filter == PIPE_TEX_FILTER_LINEAR;
	*a = (clamp_s ? 1u << 31 : 0) | (clamp_t ? 1u << 29 : 0) | (uint32_t)(tr->tex_lw - 3) << 24 |
	     (uint32_t)(tr->tex_lh - 3) << 20 | fmt << 14 | ((stride_px / 8) & 1);
	/* 0x0e000000 as the background object has it; the stride in 16 texels at bit 18, the address / 128 */
	*b = 0x0e000000 | (bilinear ? 1u << 31 : 0) | (uint32_t)((stride_px / 8) >> 1) << 18 | tr->bo->va >> 7;
	return true;
}

static void emit_header(struct mbx_context *ctx, const struct mbx_vgp_program *prog, uint32_t layout)
{
	const struct pipe_viewport_state *vp = &ctx->viewport;
	uint32_t ta[MBX_MAX_TEX] = { 0 }, tb[MBX_MAX_TEX] = { 0 }, tc[MBX_MAX_TEX];
	unsigned u, l8 = 0;

	memcpy(tc, prog->tex_c, sizeof(tc));

	for (u = 0; u < prog->ntex; u++) {
		struct pipe_sampler_view *vw = ctx->views[u];

		if (vw && vw->texture && vw->texture->format == PIPE_FORMAT_L8_UNORM)
			l8 |= 1u << u;
	}
	if (l8)
		mbx_tex_combiners(prog->fsi, prog->ntex, l8, tc);
	for (u = 0; u < prog->ntex; u++) {
		if (u == 0 && !(prog->units & 1))
			continue;	/* unit 1 alone: unit 0's block is a copy of unit 1's (below) */
		if (!tex_words(ctx, u, u == 0 && want_trilinear(ctx, prog), &ta[u], &tb[u]))
			ta[u] = tb[u] = 0;
	}
	if (!(prog->units & 1) && prog->ntex == 2) {
		ta[0] = ta[1];
		tb[0] = tb[1];
	}
	mbx_emit(ctx, MBX_CH_TA, 0x10000000 | 0x1 | 0x2 | (prog->ntex > 0 ? 0x4 : 0) | (prog->ntex > 1 ? 0x8 : 0) | 0x20 | 0x80 | 0x100);
	mbx_emit(ctx, MBX_CH_TA, hw_state_word(ctx, prog));
	mbx_emit(ctx, MBX_CH_TA, ctx->fsi && (*ctx->fsi).kind == MBX_FS_MATCHED && (*ctx->fsi).alpha_word ?
		 (*ctx->fsi).alpha_word : 0xe0000000);	/* alpha function ALWAYS (0 is NEVER, which rejects every pixel with bit 25 set), or the shader's discard on alpha */
	for (u = 0; u < prog->ntex; u++) {
		mbx_emit(ctx, MBX_CH_TA, ta[u]);
		mbx_emit(ctx, MBX_CH_TA, tb[u]);
		mbx_emit(ctx, MBX_CH_TA, tc[u]);
	}
	mbx_emit(ctx, MBX_CH_TA, layout);
	mbx_emit_f(ctx, MBX_CH_TA, vp->scale[0]);	/* x scale, x offset, y offset, y scale */
	mbx_emit_f(ctx, MBX_CH_TA, vp->translate[0]);
	mbx_emit_f(ctx, MBX_CH_TA, vp->translate[1]);
	mbx_emit_f(ctx, MBX_CH_TA, vp->scale[1]);
	mbx_emit_f(ctx, MBX_CH_TA, vp->scale[2]);	/* depth: scale, offset, the unit of the offset */
	mbx_emit_f(ctx, MBX_CH_TA, vp->translate[2]);
	mbx_emit_f(ctx, MBX_CH_TA, 1e-5f);
}

/* The VGP program (section bit 2) and the constants, when they differ from what the job has loaded */
static void emit_program(struct mbx_context *ctx, const struct mbx_vgp_program *prog)
{
	struct mbx_job *job = &ctx->job;
	const float *uniforms = NULL, *fs_uniforms = NULL;
	unsigned i, nu = prog->nuniform;

	if (job->loaded_prog != prog) {
		mbx_emit(ctx, MBX_CH_TA, 0x80000000 | 0x4);
		mbx_emit(ctx, MBX_CH_TA, ((prog->ninsn - 1) & 0xff) << 8);	/* flags 0, first 0 */
		mbx_emit(ctx, MBX_CH_TA, 0x90000000 | (prog->ninsn - 1));
		for (i = 0; i < prog->ninsn; i++) {
			mbx_emit(ctx, MBX_CH_TA, prog->code[i][0]);
			mbx_emit(ctx, MBX_CH_TA, prog->code[i][1]);
		}
		job->loaded_prog = prog;
		job->loaded_const_serial = ~0u;
	}
	if (job->loaded_const_serial == ctx->const_serial)
		return;
	if (ctx->cb[0].user_buffer)
		uniforms = (const float *)((const uint8_t *)ctx->cb[0].user_buffer + ctx->cb[0].buffer_offset);
	else if (ctx->cb[0].buffer)
		uniforms = (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[0].buffer)->bo) +
					   ctx->cb[0].buffer_offset);
	if (ctx->cb[1].user_buffer)
		fs_uniforms = (const float *)((const uint8_t *)ctx->cb[1].user_buffer + ctx->cb[1].buffer_offset);
	else if (ctx->cb[1].buffer)
		fs_uniforms = (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[1].buffer)->bo) +
					      ctx->cb[1].buffer_offset);
	if (getenv("MBX_DEBUG") && atoi(getenv("MBX_DEBUG")) > 1)
		for (i = 0; i < nu; i++)
			mbx_dbg("  uniform c%u = %g %g %g %g (buffer %p, user %p, size %u)", i,
				uniforms ? uniforms[i * 4] : -1.0f, uniforms ? uniforms[i * 4 + 1] : -1.0f,
				uniforms ? uniforms[i * 4 + 2] : -1.0f, uniforms ? uniforms[i * 4 + 3] : -1.0f,
				(void *)ctx->cb[0].buffer, ctx->cb[0].user_buffer, ctx->cb[0].buffer_size);
	if (prog->nconst) {
		mbx_emit(ctx, MBX_CH_TA, 0xa0000000 | (prog->nconst - 1));
		for (i = 0; i < prog->nconst; i++) {
			for (unsigned c = 0; c < 4; c++) {
				if (prog->scissor && (int)i >= prog->scissor_slot && (int)i < prog->scissor_slot + 4) {
					/* plane k of the window-space rectangle: a * clip x (or y) + b * clip w >= 0, with
					 * x_win = x / w * scale + translate (the viewport the header sends) */
					const struct pipe_viewport_state *vp = &ctx->viewport;
					unsigned k = i - prog->scissor_slot;
					float sc = k < 2 ? vp->scale[0] : vp->scale[1], tr = k < 2 ? vp->translate[0] : vp->translate[1];
					float lo = k < 2 ? ctx->scissor.minx : ctx->scissor.miny, hi = k < 2 ? ctx->scissor.maxx : ctx->scissor.maxy;
					float v[4] = { 0, 0, 0, 0 };

					if (k & 1) {		/* the high edge: hi - x_win >= 0 */
						v[k < 2 ? 0 : 1] = -sc;
						v[3] = hi - tr;
					} else {		/* the low edge: x_win - lo >= 0 */
						v[k < 2 ? 0 : 1] = sc;
						v[3] = tr - lo;
					}
					mbx_emit_f(ctx, MBX_CH_TA, v[c]);
				} else if (prog->tex && ((int)i == prog->tex_scale_slot[0] || (int)i == prog->tex_scale_slot[1])) {
					float s = 1.0f;
					unsigned un = (int)i == prog->tex_scale_slot[0] ? 0 : 1;

					if (ctx->views[un] && c < 2) {
						struct mbx_resource *tr = mbx_resource(ctx->views[un]->texture);

						s = c == 0 ? (float)tr->base.width0 / (float)(1u << tr->tex_lw) :
							     (float)tr->base.height0 / (float)(1u << tr->tex_lh);
					} else if (c >= 2) {
						s = 0.0f;
					}
					mbx_emit_f(ctx, MBX_CH_TA, s);
				} else if (prog->nfsu && (int)i >= prog->fsu_base && (int)i < prog->fsu_base + (int)prog->nfsu) {
					/* a fragment uniform the vertex program multiplies into the colour */
					unsigned idx = prog->fsu_vec[i - prog->fsu_base] * 4 + c;

					mbx_emit_f(ctx, MBX_CH_TA, fs_uniforms && idx * 4 < ctx->cb[1].buffer_size ? fs_uniforms[idx] : 0.0f);
				} else if (i < nu) {
					mbx_emit_f(ctx, MBX_CH_TA, uniforms ? uniforms[i * 4 + c] : 0.0f);
				} else {
					mbx_emit_f(ctx, MBX_CH_TA, prog->consts[i][c]);
				}
			}
		}
	}
	job->loaded_const_serial = ctx->const_serial;
}

/* One vertex's words in the layout the program names */
static void emit_vertex(struct mbx_context *ctx, const unsigned *attr_comps, unsigned index,
			unsigned instance, uint8_t **vb_ptr)
{
	for (unsigned k = 0; k < ctx->velems->count; k++) {
		const struct pipe_vertex_element *e = &ctx->velems->e[k];
		unsigned comps = k < MBX_MAX_ATTRS ? attr_comps[k] : 0;
		const uint8_t *p;

		if (!comps)
			continue;
		p = vb_ptr[e->vertex_buffer_index] + e->src_offset +
		    (uint64_t)(e->instance_divisor ? instance / e->instance_divisor : index) * e->src_stride;
		if (comps == 5) {
			uint32_t w;

			memcpy(&w, p, 4);
			mbx_emit(ctx, MBX_CH_TA, w);
		} else {
			float rgba[4] = { 0, 0, 0, 1 };

			if (e->src_format == PIPE_FORMAT_R32_FLOAT || e->src_format == PIPE_FORMAT_R32G32_FLOAT ||
			    e->src_format == PIPE_FORMAT_R32G32B32_FLOAT ||
			    e->src_format == PIPE_FORMAT_R32G32B32A32_FLOAT) {
				memcpy(rgba, p, util_format_get_blocksize(e->src_format));
			} else {
				util_format_unpack_rgba(e->src_format, rgba, p, 1);
			}
			for (unsigned c = 0; c < comps; c++)
				mbx_emit_f(ctx, MBX_CH_TA, rgba[c]);
		}
	}
}

/*
 * Grid mode (README assumption 48): a fragment shader that is smooth but not affine in its varyings is evaluated by the vertex
 * program at the corners of a fine grid (about 8 pixels), and the chip interpolates the colour between them. A triangle is cut
 * into N x N small ones from its attributes (linear in object space, so exact for a vertex shader that is affine in them); N
 * follows from where its corners land on the screen (the vertex program run on the CPU).
 */
#define GRID_PX 8.0f
#define GRID_MAX_N 16
#define GRID_BUDGET 12000	/* small triangles a draw may become */

static void fetch_vertex(struct mbx_context *ctx, const unsigned *attr_comps, unsigned index, unsigned instance,
			 uint8_t **vb_ptr, float out[MBX_MAX_ATTRS][4])
{
	for (unsigned k = 0; k < MBX_MAX_ATTRS; k++) {
		out[k][0] = out[k][1] = out[k][2] = 0.0f;
		out[k][3] = 1.0f;
	}
	for (unsigned k = 0; k < ctx->velems->count && k < MBX_MAX_ATTRS; k++) {
		const struct pipe_vertex_element *e = &ctx->velems->e[k];
		const uint8_t *p;

		if (!attr_comps[k])
			continue;
		p = vb_ptr[e->vertex_buffer_index] + e->src_offset +
		    (uint64_t)(e->instance_divisor ? instance / e->instance_divisor : index) * e->src_stride;
		if (e->src_format == PIPE_FORMAT_R32_FLOAT || e->src_format == PIPE_FORMAT_R32G32_FLOAT ||
		    e->src_format == PIPE_FORMAT_R32G32B32_FLOAT || e->src_format == PIPE_FORMAT_R32G32B32A32_FLOAT)
			memcpy(out[k], p, util_format_get_blocksize(e->src_format));
		else
			util_format_unpack_rgba(e->src_format, out[k], p, 1);
	}
}

static void emit_values(struct mbx_context *ctx, const unsigned *attr_comps, float v[MBX_MAX_ATTRS][4])
{
	for (unsigned k = 0; k < MBX_MAX_ATTRS; k++) {
		if (!attr_comps[k])
			continue;
		if (attr_comps[k] == 5) {	/* four unorm bytes in one word */
			uint32_t w = 0;

			for (unsigned c = 0; c < 4; c++)
				w |= (uint32_t)(CLAMP(v[k][c], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * c);
			mbx_emit(ctx, MBX_CH_TA, w);
		} else {
			for (unsigned c = 0; c < attr_comps[k]; c++)
				mbx_emit_f(ctx, MBX_CH_TA, v[k][c]);
		}
	}
}

/* The constants the program reads, as the header would send them (without the scissor planes and texture scales) */
static void cpu_constants(struct mbx_context *ctx, const struct mbx_vgp_program *prog, float k[MBX_MAX_CONSTS][4])
{
	const float *uniforms = NULL, *fs_uniforms = NULL;

	if (ctx->cb[0].user_buffer)
		uniforms = (const float *)((const uint8_t *)ctx->cb[0].user_buffer + ctx->cb[0].buffer_offset);
	else if (ctx->cb[0].buffer)
		uniforms = (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[0].buffer)->bo) + ctx->cb[0].buffer_offset);
	if (ctx->cb[1].user_buffer)
		fs_uniforms = (const float *)((const uint8_t *)ctx->cb[1].user_buffer + ctx->cb[1].buffer_offset);
	else if (ctx->cb[1].buffer)
		fs_uniforms = (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[1].buffer)->bo) + ctx->cb[1].buffer_offset);
	for (unsigned i = 0; i < prog->nconst && i < MBX_MAX_CONSTS; i++)
		for (unsigned c = 0; c < 4; c++) {
			if (prog->nfsu && (int)i >= prog->fsu_base && (int)i < prog->fsu_base + (int)prog->nfsu)
				k[i][c] = fs_uniforms ? fs_uniforms[prog->fsu_vec[i - prog->fsu_base] * 4 + c] : 0.0f;
			else if (i < prog->nuniform)
				k[i][c] = uniforms ? uniforms[i * 4 + c] : 0.0f;
			else
				k[i][c] = prog->consts[i][c];
		}
}

/* The number of cuts along a triangle's longest screen side (1: not cut) */
static unsigned grid_cuts(struct mbx_context *ctx, const struct mbx_vgp_program *prog, const float k[MBX_MAX_CONSTS][4],
			  float va[MBX_MAX_ATTRS][4], float vb[MBX_MAX_ATTRS][4], float vc[MBX_MAX_ATTRS][4])
{
	float (*v[3])[4] = { va, vb, vc };
	float x[3], y[3], ext = 0.0f;
	const struct pipe_viewport_state *vp = &ctx->viewport;

	for (unsigned i = 0; i < 3; i++) {
		float pos[4];

		mbx_cpuvs_run(prog->code, prog->ninsn, k, prog->nconst, (const float (*)[4])v[i], pos);
		if (!(pos[3] > 1e-4f))
			return 1;			/* behind the eye: the TA clips it, the grid has nothing to measure */
		x[i] = pos[0] / pos[3] * vp->scale[0];
		y[i] = pos[1] / pos[3] * vp->scale[1];
	}
	for (unsigned i = 0; i < 3; i++) {
		float dx = fabsf(x[i] - x[(i + 1) % 3]), dy = fabsf(y[i] - y[(i + 1) % 3]);

		ext = MAX3(ext, dx, dy);
	}
	return (unsigned)CLAMP(ceilf(ext / GRID_PX), 1.0f, (float)GRID_MAX_N);
}

static void grid_lerp(float out[MBX_MAX_ATTRS][4], float a[MBX_MAX_ATTRS][4], float b[MBX_MAX_ATTRS][4],
		      float c[MBX_MAX_ATTRS][4], float wa, float wb, float wc)
{
	for (unsigned k = 0; k < MBX_MAX_ATTRS; k++)
		for (unsigned i = 0; i < 4; i++)
			out[k][i] = a[k][i] * wa + b[k][i] * wb + c[k][i] * wc;
}

/* One triangle as N x N small ones, in a triangle list */
static void grid_triangle(struct mbx_context *ctx, const unsigned *comps, float va[MBX_MAX_ATTRS][4],
			  float vb[MBX_MAX_ATTRS][4], float vc[MBX_MAX_ATTRS][4], unsigned n)
{
	float p[GRID_MAX_N + 1][GRID_MAX_N + 1][MBX_MAX_ATTRS][4];
	unsigned i, j;

	for (i = 0; i <= n; i++)
		for (j = 0; i + j <= n; j++)
			grid_lerp(p[i][j], va, vb, vc, 1.0f - (float)(i + j) / (float)n, (float)i / (float)n, (float)j / (float)n);
	for (i = 0; i < n; i++)
		for (j = 0; i + j < n; j++) {
			emit_values(ctx, comps, p[i][j]);
			emit_values(ctx, comps, p[i + 1][j]);
			emit_values(ctx, comps, p[i][j + 1]);
			if (i + j + 1 < n) {
				emit_values(ctx, comps, p[i + 1][j]);
				emit_values(ctx, comps, p[i + 1][j + 1]);
				emit_values(ctx, comps, p[i][j + 1]);
			}
		}
}

static uint32_t hw_prim_word(unsigned mode)
{
	switch (mode) {
	case MESA_PRIM_TRIANGLES: return 0x70000000;
	case MESA_PRIM_TRIANGLE_STRIP: return 0x40000000;
	case MESA_PRIM_TRIANGLE_FAN: return 0x50000000;
	case MESA_PRIM_LINES:			/* a line strip restarted after each segment (control word 0) */
	case MESA_PRIM_LINE_STRIP: return 0x60000000;
	case MESA_PRIM_POINTS: return 0xe0000000;
	default: return 0;
	}
}

static void mbx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
			 unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
			 const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
	struct mbx_context *ctx = mbx_context(pctx);
	const struct mbx_vgp_program *prog;
	uint8_t *vb_ptr[PIPE_MAX_ATTRIBS] = { 0 };
	uint32_t prim = hw_prim_word(info->mode);
	const void *indices = NULL;
	unsigned comps[MBX_MAX_ATTRS], d;
	uint32_t layout;

	if (indirect && indirect->buffer) {
		mesa_loge("mbx: indirect draws are not supported");
		return;
	}
	if (!prim) {
		mesa_loge("mbx: primitive mode %d reached the driver (not in the caps)", info->mode);
		return;
	}
	if (ctx->fs)
		ctx->fsi = mbx_fs_specialize(ctx, &ctx->fsv);
	mbx_dbg("draw mode %d, %u draws, vs %p fs %p velems %p viewport scale %g", info->mode, num_draws,
		(void *)ctx->vs, (void *)ctx->fs, (void *)ctx->velems, ctx->viewport.scale[0]);
	if (!ctx->vs || !ctx->fs || !ctx->velems || !ctx->viewport.scale[0])
		return;
	if (ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture)
		mbx_resource(ctx->fb.cbufs[0].texture)->wver++;	/* a texture rendered into: its swapped copy is stale */
	for (unsigned u = 0; u < MBX_MAX_SAMPLERS; u++)
		if (ctx->clear.pending && ctx->views[u] && ctx->views[u]->texture == &ctx->clear.rsc->base)
			mbx_clear_settle(ctx);	/* sampling a texture whose clear is still pending */
	if (state_needs_fallback(ctx)) {
		if (!mbx_fallback_draw(ctx, info, drawid_offset, draws, num_draws))
			mesa_loge("mbx: this state needs the CPU fallback and it could not draw; draw skipped");
		return;
	}
	if (!(prog = mbx_program_get(ctx))) {
		/* the pair does not reduce to the VGP and the fixed pipe: softpipe draws it */
		if (!mbx_fallback_draw(ctx, info, drawid_offset, draws, num_draws))
			mesa_loge("mbx: no program for this vertex and fragment shader pair, and no fallback; draw skipped");
		return;
	}
	{
		bool any = false;

		for (unsigned k = 0; k < MBX_MAX_ATTRS; k++)
			any |= prog->attr_used[k];
		if (!any) {	/* the TA has no vertex layout to name: softpipe draws it */
			if (!mbx_fallback_draw(ctx, info, drawid_offset, draws, num_draws))
				mesa_loge("mbx: a draw without vertex attributes could not be drawn; skipped");
			return;
		}
	}
	if (prog->fsi && prog->fsi->swap_rb) {
		bool ok = true;

		for (unsigned un = 0; un < prog->ntex; un++)
			if ((prog->units >> un & 1) && !unit_texture(ctx, un))
				ok = false;
		if (!ok) {	/* a texture that cannot be copied with red and blue exchanged: softpipe draws it */
			if (!mbx_fallback_draw(ctx, info, drawid_offset, draws, num_draws))
				mesa_loge("mbx: a draw with a red-blue swizzle could not be drawn; skipped");
			return;
		}
	}
	if (ctx->dsa && ctx->fb.zsbuf.texture && (ctx->dsa->stencil[0].enabled || ctx->dsa->stencil[1].enabled) &&
	    !mbx_stencil_begin(ctx)) {
		mbx_dbg("the stencil mask could not be made: the draw goes to the CPU");
		if (!mbx_fallback_draw(ctx, info, drawid_offset, draws, num_draws))
			mesa_loge("mbx: a draw with stencil could not be drawn; skipped");
		return;
	}
	if (!mbx_job_begin(ctx)) {
		mbx_dbg("no job: framebuffer has %u colour buffers", ctx->fb.nr_cbufs);
		mbx_stencil_end(ctx);
		return;
	}
	if ((ctx->dsa && ctx->dsa->depth_enabled) || ctx->smask.active)
		ctx->job.zuse = true;
	mbx_dbg("program: %u instructions, %u uniform slots, %u constants", prog->ninsn, prog->nuniform, prog->nconst);
	layout = vertex_layout(ctx, prog, comps);
	if (getenv("MBX_DEBUG") && atoi(getenv("MBX_DEBUG")) > 1) {
		char text[128];

		for (unsigned i = 0; i < prog->ninsn; i++) {
			mbx_vgp_disasm(text, sizeof(text), prog->code[i]);
			mbx_dbg("  %2u: %08x %08x  %s", i, prog->code[i][0], prog->code[i][1], text);
		}
		for (unsigned i = 0; i < prog->nconst; i++)
			mbx_dbg("  c%u = %g %g %g %g%s", i, prog->consts[i][0], prog->consts[i][1], prog->consts[i][2],
				prog->consts[i][3], i < prog->nuniform ? " (uniform)" : "");
		mbx_dbg("  layout %08x, comps %u %u %u %u, velems %u, viewport scale %g %g %g offset %g %g %g",
			layout, comps[0], comps[1], comps[2], comps[3], ctx->velems->count, ctx->viewport.scale[0],
			ctx->viewport.scale[1], ctx->viewport.scale[2], ctx->viewport.translate[0],
			ctx->viewport.translate[1], ctx->viewport.translate[2]);
	}
	for (unsigned k = 0; k < ctx->velems->count; k++) {
		unsigned b = ctx->velems->e[k].vertex_buffer_index;

		if (vb_ptr[b])
			continue;
		if (ctx->vb[b].is_user_buffer)
			vb_ptr[b] = (uint8_t *)ctx->vb[b].buffer.user;
		else if (ctx->vb[b].buffer.resource)
			vb_ptr[b] = (uint8_t *)mbx_bo_map(mbx_resource(ctx->vb[b].buffer.resource)->bo) +
				    ctx->vb[b].buffer_offset;
	}
	if (getenv("MBX_DEBUG") && atoi(getenv("MBX_DEBUG")) > 1) {
		for (unsigned k = 0; k < ctx->velems->count; k++) {
			const struct pipe_vertex_element *e = &ctx->velems->e[k];
			const struct pipe_vertex_buffer *vb = &ctx->vb[e->vertex_buffer_index];

			mbx_dbg("  elem %u: buffer %u offset %u stride %u format %s; vb: user %d resource %p user ptr %p offset %u; mapped %p",
				k, e->vertex_buffer_index, e->src_offset, e->src_stride, util_format_name(e->src_format),
				vb->is_user_buffer, (void *)vb->buffer.resource, vb->buffer.user, vb->buffer_offset,
				(void *)vb_ptr[e->vertex_buffer_index]);
			if (vb_ptr[e->vertex_buffer_index]) {
				const float *f = (const float *)(vb_ptr[e->vertex_buffer_index] + e->src_offset);

				mbx_dbg("    first floats: %g %g %g %g", f[0], f[1], f[2], f[3]);
			}
		}
		mbx_dbg("  num_vb %u, draws: start %u count %u bias %d, index_size %u",
			ctx->num_vb, draws[0].start, draws[0].count, draws[0].index_bias, info->index_size);
	}
	if (info->index_size) {
		if (info->has_user_indices)
			indices = info->index.user;
		else
			indices = mbx_bo_map(mbx_resource(info->index.resource)->bo);
	}

	for (d = 0; d < num_draws; d++) {
		unsigned i;
		bool per_point = prim >> 28 == 0xe && prog->psize_kind == MBX_PSIZE_INPUT;

		if (per_point) {
			/* gl_PointSize from a vertex attribute: the chip takes one size per object, so each point is an object */
			const struct pipe_vertex_element *e = &ctx->velems->e[MIN2(prog->psize_slot, ctx->velems->count - 1)];

			for (i = 0; i < draws[d].count; i++) {
				unsigned idx = draws[d].start + i;
				float rgba[4] = { 0, 0, 0, 1 };
				const uint8_t *pp;

				if (info->index_size == 1)
					idx = ((const uint8_t *)indices)[idx];
				else if (info->index_size == 2)
					idx = ((const uint16_t *)indices)[idx];
				else if (info->index_size == 4)
					idx = ((const uint32_t *)indices)[idx];
				if (info->index_size)
					idx += draws[d].index_bias;
				pp = vb_ptr[e->vertex_buffer_index] + e->src_offset + (uint64_t)idx * e->src_stride;
				util_format_unpack_rgba(e->src_format, rgba, pp, 1);
				for (unsigned un = 0; un < prog->ntex; un++)
					if ((prog->units >> un & 1) && ctx->views[un] && ctx->views[un]->texture)
						mbx_job_use_texture(ctx, unit_texture(ctx, un));
				emit_header(ctx, prog, layout);
				emit_program(ctx, prog);
				mbx_emit(ctx, MBX_CH_TA, prim | hw_cull_bits(ctx));
				mbx_emit_f(ctx, MBX_CH_TA, CLAMP(rgba[prog->psize_lane], 1.0f, 32.0f) * 0.5f);
				mbx_emit(ctx, MBX_CH_TA, prog->sections);
				emit_vertex(ctx, comps, idx, info->start_instance, vb_ptr);
				mbx_emit(ctx, MBX_CH_CTL, 1);
				ctx->job.ndraws++;
			}
			continue;
		}
		mbx_dbg("draw %u: header", d);
		for (unsigned un = 0; un < prog->ntex; un++)
			if ((prog->units >> un & 1) && ctx->views[un] && ctx->views[un]->texture)
				mbx_job_use_texture(ctx, unit_texture(ctx, un));
		emit_header(ctx, prog, layout);
		mbx_dbg("draw %u: program", d);
		emit_program(ctx, prog);
		if (prog->grid && (info->mode == MESA_PRIM_TRIANGLES || info->mode == MESA_PRIM_TRIANGLE_STRIP ||
				   info->mode == MESA_PRIM_TRIANGLE_FAN) && draws[d].count >= 3) {
			static float k[MBX_MAX_CONSTS][4];
			float va[MBX_MAX_ATTRS][4], vb[MBX_MAX_ATTRS][4], vc[MBX_MAX_ATTRS][4];
			unsigned ntri = info->mode == MESA_PRIM_TRIANGLES ? draws[d].count / 3 : draws[d].count - 2, t, used = 0;

			cpu_constants(ctx, prog, k);
			mbx_emit(ctx, MBX_CH_TA, 0x70000000 | hw_cull_bits(ctx));	/* a triangle list, whatever the draw's mode was */
			mbx_emit(ctx, MBX_CH_TA, prog->sections);
			for (t = 0; t < ntri; t++) {
				unsigned ix[3], q, n;

				for (q = 0; q < 3; q++) {
					unsigned s = info->mode == MESA_PRIM_TRIANGLES ? 3 * t + q :
						     info->mode == MESA_PRIM_TRIANGLE_STRIP ? t + (q ^ ((t & 1) && q < 2 ? 1 : 0)) :
						     (q == 0 ? 0 : t + q);

					ix[q] = draws[d].start + s;
					if (info->index_size == 1)
						ix[q] = ((const uint8_t *)indices)[ix[q]];
					else if (info->index_size == 2)
						ix[q] = ((const uint16_t *)indices)[ix[q]];
					else if (info->index_size == 4)
						ix[q] = ((const uint32_t *)indices)[ix[q]];
					if (info->index_size)
						ix[q] += draws[d].index_bias;
				}
				fetch_vertex(ctx, comps, ix[0], info->start_instance, vb_ptr, va);
				fetch_vertex(ctx, comps, ix[1], info->start_instance, vb_ptr, vb);
				fetch_vertex(ctx, comps, ix[2], info->start_instance, vb_ptr, vc);
				n = used < GRID_BUDGET ? grid_cuts(ctx, prog, (const float (*)[4])k, va, vb, vc) : 1;
				used += n * n;
				grid_triangle(ctx, comps, va, vb, vc, n);
			}
			mbx_emit(ctx, MBX_CH_CTL, 1);
			ctx->job.ndraws++;
			continue;
		}
		mbx_dbg("draw %u: %u vertices", d, draws[d].count);
		mbx_dbg("cull: face %d front_ccw %d scale1 %g -> bits %08x", ctx->rast ? ctx->rast->cull_face : -1,
			ctx->rast ? ctx->rast->front_ccw : -1, ctx->viewport.scale[1], hw_cull_bits(ctx));
		mbx_emit(ctx, MBX_CH_TA, prim | hw_cull_bits(ctx));
		if (prim >> 28 == 0x6 || prim >> 28 == 0xe) {
			/* lines and points carry half their width or size (one for the object) */
			float w = ctx->rast ? (prim >> 28 == 0xe ? ctx->rast->point_size : ctx->rast->line_width) : 1.0f;

			if (prim >> 28 == 0xe && prog->psize_kind == MBX_PSIZE_IMM) {
				w = prog->psize;
			} else if (prim >> 28 == 0xe && prog->psize_kind == MBX_PSIZE_UNIFORM) {
				const float *u = NULL;

				if (ctx->cb[0].user_buffer)
					u = (const float *)((const uint8_t *)ctx->cb[0].user_buffer + ctx->cb[0].buffer_offset);
				else if (ctx->cb[0].buffer)
					u = (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[0].buffer)->bo) +
							    ctx->cb[0].buffer_offset);
				if (u)
					w = u[prog->psize_slot * 4 + prog->psize_lane];
			}
			mbx_emit_f(ctx, MBX_CH_TA, CLAMP(w, 1.0f, 32.0f) * 0.5f);
		}
		mbx_emit(ctx, MBX_CH_TA, prog->sections);
		for (i = 0; i < draws[d].count; i++) {
			unsigned idx = draws[d].start + i;

			if (info->index_size == 1)
				idx = ((const uint8_t *)indices)[idx];
			else if (info->index_size == 2)
				idx = ((const uint16_t *)indices)[idx];
			else if (info->index_size == 4)
				idx = ((const uint32_t *)indices)[idx];
			if (info->index_size)
				idx += draws[d].index_bias;	/* undefined for a draw without indices */
			emit_vertex(ctx, comps, idx, info->start_instance, vb_ptr);
			if (info->mode == MESA_PRIM_LINES && (i & 1) && i + 1 < draws[d].count)
				mbx_emit(ctx, MBX_CH_CTL, 0);	/* the next segment starts a new strip */
		}
		mbx_emit(ctx, MBX_CH_CTL, 1);		/* the object ends */
		mbx_dbg("draw %u: done, stream %u words", d, ctx->job.n);
		ctx->job.ndraws++;
	}
	mbx_stencil_end(ctx);	/* a stencil-tested draw is rendered now and the depth of the rejected pixels put back */
}

void mbx_draw_init(struct pipe_context *pctx)
{
	pctx->draw_vbo = mbx_draw_vbo;
}
