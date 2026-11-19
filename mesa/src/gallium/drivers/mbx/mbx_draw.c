// SPDX-License-Identifier: MIT
/*
 * draw_vbo: the vertices go into the TA stream as the CPU reads them (the TA
 * takes vertex data inline, there is no vertex fetch), after an object header
 * and, when the vertex shader changed, the VGP program and its constants.
 */
#include <poll.h>
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
	mbx_texture_nudge(ctx, tr);
	if (ctx->fsi && ctx->fsi->swap_rb)
		return mbx_texture_swapped(ctx, tr);
	return tr;
}

/*
 * Mirrored repeat: texture word A bit 30 mirrors s, bit 28 mirrors t, the clamp bits clear (found during device testing with the
 * MBX_TEX_A_OR probe: gles2_feat wrap_probe, nearest and linear equal GL's picture). Only a texture whose storage is its size (a power of
 * two) can mirror: an NPOT one is read with the coordinates scaled into padded storage, and a mirror would fold at the padding.
 */
static uint32_t tex_mirror_bits(const struct mbx_resource *tr, const struct pipe_sampler_state *smp)
{
	bool pot = util_is_power_of_two_or_zero(tr->base.width0) && util_is_power_of_two_or_zero(tr->base.height0);

	return pot ? (smp->wrap_s == PIPE_TEX_WRAP_MIRROR_REPEAT ? 1u << 30 : 0) | (smp->wrap_t == PIPE_TEX_WRAP_MIRROR_REPEAT ? 1u << 28 : 0) : 0;
}

/* An experiment: bits ORed into the texture word A (MBX_TEX_A_OR, hex), to find the chip's mirror bits (the unexplored bits 30 and 28) */
static uint32_t tex_a_or(void)
{
	static int init;
	static uint32_t v;

	if (!init) {
		const char *e = getenv("MBX_TEX_A_OR");

		v = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
		init = 1;
	}
	return v;
}

static bool tex_words(struct mbx_context *ctx, unsigned unit, bool trilinear, uint32_t *a, uint32_t *b)
{
	struct pipe_sampler_view *view = ctx->views[unit];
	const struct pipe_sampler_state *smp = ctx->samplers[unit];
	struct mbx_resource *tr;
	unsigned stride_px, fmt;
	bool clamp_s, clamp_t, bilinear;
	uint32_t mirror;

	if (mbx_debug_level())
		mbx_dbg("tex_words unit %u: view %p smp %p texture %p tex %d imported %d", unit, (void *)view, (const void *)smp,
			view ? (void *)view->texture : NULL, view && view->texture ? mbx_resource(view->texture)->tex : -1,
			view && view->texture ? mbx_resource(view->texture)->imported : -1);
	if (!view || !smp || !view->texture || !mbx_resource(view->texture)->tex)
		return false;
	tr = unit_texture(ctx, unit);
	if (!tr)
		return false;
	if (tr->imported && mbx_debug_level()) {
		const uint32_t *px;
		unsigned nz = 0, nff = 0;
		struct pollfd pfd = { .fd = tr->dmabuf_fd, .events = POLLIN };

		if (tr->dmabuf_fd > 0)
			poll(&pfd, 1, 1000);	/* the writer's fence (the client's job) */
		px = mbx_bo_map(tr->bo);
		if (px)
			for (unsigned y = 0; y < tr->base.height0; y++)
				for (unsigned x = 0; x < tr->base.width0; x++) {
					uint32_t v = px[y * (tr->level_stride[0] / 4) + x];

					nz += v != 0;
					nff += (v >> 24) == 0xff;
				}
		mbx_dbg("imported texture (inode %lu) %ux%u fmt %s stride %u va %#llx: %u of %u texels non-zero, %u with a 0xff top byte; centre %08x",
			tr->dmabuf_ino, tr->base.width0, tr->base.height0, util_format_name(tr->base.format), tr->level_stride[0],
			(unsigned long long)tr->bo->va, nz, tr->base.width0 * tr->base.height0, nff,
			px ? px[(tr->base.height0 / 2) * (tr->level_stride[0] / 4) + tr->base.width0 / 2] : 0);
	}
	fmt = mbx_chip_tex_format(tr->base.format);
	if (tr->twiddled) {
		/* a twiddled chain: no stride; mipmapped when the sampler asks (word B bit 29, with the level
		 * bias field 9: found on the chip, round to the nearest level); the chip's trilinear mode (bit 30) is not
		 * understood yet, so the *_MIPMAP_LINEAR filters take the nearest level too */
		bool mip = smp->min_mip_filter != PIPE_TEX_MIPFILTER_NONE;

		clamp_s = smp->wrap_s != PIPE_TEX_WRAP_REPEAT && smp->wrap_s != PIPE_TEX_WRAP_MIRROR_REPEAT;
		clamp_t = smp->wrap_t != PIPE_TEX_WRAP_REPEAT && smp->wrap_t != PIPE_TEX_WRAP_MIRROR_REPEAT;
		mirror = tex_mirror_bits(tr, smp);
		if (ctx->tex_mag[unit] == 1) {		/* magnified: level 0 with the magnification filter */
			mip = false;
			trilinear = false;
		}
		bilinear = (mip || ctx->tex_mag[unit] == 0) ? smp->min_img_filter == PIPE_TEX_FILTER_LINEAR : smp->mag_img_filter == PIPE_TEX_FILTER_LINEAR;
		*a = (clamp_s ? 1u << 31 : 0) | (clamp_t ? 1u << 29 : 0) | (uint32_t)(tr->tex_lw - 3) << 24 |
		     (uint32_t)(tr->tex_lh - 3) << 20 | fmt << 14 |
		     (trilinear ? 0xbu << 9 : mip ? (uint32_t)tr->base.last_level << 9 : 0);	/* trilinear: the whole chain (0xb) */
		*b = (trilinear ? 0x4e000000u : mip ? 0x32000000u : 0x0e000000u) | (bilinear ? 1u << 31 : 0) | tr->bo->va >> 7;
		*a |= mirror | tex_a_or();
		return true;
	}
	stride_px = tr->level_stride[0] / util_format_get_blocksize(tr->base.format);
	/* NPOT textures are read with the coordinates scaled (the VGP does it), so only clamping them is right */
	clamp_s = smp->wrap_s != PIPE_TEX_WRAP_REPEAT && smp->wrap_s != PIPE_TEX_WRAP_MIRROR_REPEAT;
	clamp_t = smp->wrap_t != PIPE_TEX_WRAP_REPEAT && smp->wrap_t != PIPE_TEX_WRAP_MIRROR_REPEAT;
	mirror = tex_mirror_bits(tr, smp);
	bilinear = (ctx->tex_mag[unit] == 0 ? smp->min_img_filter : smp->mag_img_filter) == PIPE_TEX_FILTER_LINEAR;
	*a = (clamp_s ? 1u << 31 : 0) | (clamp_t ? 1u << 29 : 0) | (uint32_t)(tr->tex_lw - 3) << 24 |
	     (uint32_t)(tr->tex_lh - 3) << 20 | fmt << 14 | ((stride_px / 8) & 1);
	/* 0x0e000000 as the background object has it; the stride in 16 texels at bit 18, the address / 128 */
	*b = 0x0e000000 | (bilinear ? 1u << 31 : 0) | (uint32_t)((stride_px / 8) >> 1) << 18 | tr->bo->va >> 7;
	*a |= mirror | tex_a_or();
	return true;
}

/* The shift of the position's x (in clip units of w) that makes up for the header's x offset being its x scale */
/* The constants a program that reads gl_FragCoord carries: (x scale, x translate, y scale, y translate), then (z scale, z translate, 0, 0) */
static float fragcoord_const(const struct pipe_viewport_state *vp, int reg, unsigned lane)
{
	if (reg == 0)
		return lane == 0 ? vp->scale[0] : lane == 1 ? vp->translate[0] : lane == 2 ? vp->scale[1] : vp->translate[1];
	return lane == 0 ? vp->scale[2] : lane == 1 ? vp->translate[2] : 0.0f;
}

static float vp_dx(const struct pipe_viewport_state *vp)
{
	return vp->scale[0] != 0.0f ? (vp->translate[0] - vp->scale[0]) / vp->scale[0] : 0.0f;
}

static void emit_header(struct mbx_context *ctx, const struct mbx_vgp_program *prog, uint32_t layout)
{
	const struct pipe_viewport_state *vp = &ctx->viewport;
	uint32_t ta[MBX_MAX_TEX] = { 0 }, tb[MBX_MAX_TEX] = { 0 }, tc[MBX_MAX_TEX];
	unsigned u, l8 = 0;

	memcpy(tc, prog->tex_c, sizeof(tc));

	for (u = 0; u < prog->ntex; u++) {
		struct pipe_sampler_view *vw = ctx->views[u];

		/* L8 and XRGB: the texel's alpha is 1, where the chip returns the byte (the X byte of a texture that was rendered or cleared
		 * holds the alpha written there) */
		if (vw && vw->texture && (vw->texture->format == PIPE_FORMAT_L8_UNORM || vw->texture->format == PIPE_FORMAT_B8G8R8X8_UNORM ||
					  mbx_resource(vw->texture)->alpha_one))
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
	mbx_emit_f(ctx, MBX_CH_TA, vp->scale[0]);	/* the x offset is the x scale (the program moves the position by the difference: vp_slot) */
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
	if (mbx_debug_level() > 1)
		for (i = 0; i < nu; i++)
			mbx_dbg("  uniform c%u = %g %g %g %g (buffer %p, user %p, size %u)", i,
				uniforms ? uniforms[i * 4] : -1.0f, uniforms ? uniforms[i * 4 + 1] : -1.0f,
				uniforms ? uniforms[i * 4 + 2] : -1.0f, uniforms ? uniforms[i * 4 + 3] : -1.0f,
				(void *)ctx->cb[0].buffer, ctx->cb[0].user_buffer, ctx->cb[0].buffer_size);
	if (prog->nconst) {
		mbx_emit(ctx, MBX_CH_TA, 0xa0000000 | (prog->nconst - 1));
		for (i = 0; i < prog->nconst; i++) {
			for (unsigned c = 0; c < 4; c++) {
				if ((int)i == prog->vp_slot) {
					mbx_emit_f(ctx, MBX_CH_TA, c == 0 ? vp_dx(&ctx->viewport) : 0.0f);
				} else if (prog->fc_slot >= 0 && ((int)i == prog->fc_slot || (int)i == prog->fc_slot + 1)) {
					mbx_emit_f(ctx, MBX_CH_TA, fragcoord_const(&ctx->viewport, (int)i - prog->fc_slot, c));
				} else if (prog->scissor && (int)i >= prog->scissor_slot && (int)i < prog->scissor_slot + 4) {
					/* plane k of the window-space rectangle: a * clip x (or y) + b * clip w >= 0, with
					 * x_win = x / w * scale + translate (the viewport the header sends) */
					const struct pipe_viewport_state *vp = &ctx->viewport;
					unsigned k = i - prog->scissor_slot;
					float sc = k < 2 ? vp->scale[0] : vp->scale[1], tr = k < 2 ? vp->scale[0] : vp->translate[1];	/* x: the position is already shifted */
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
			if ((int)i == prog->vp_slot)
				k[i][c] = c == 0 ? vp_dx(&ctx->viewport) : 0.0f;
			else if (prog->fc_slot >= 0 && ((int)i == prog->fc_slot || (int)i == prog->fc_slot + 1))
				k[i][c] = fragcoord_const(&ctx->viewport, (int)i - prog->fc_slot, c);
			else if (prog->nfsu && (int)i >= prog->fsu_base && (int)i < prog->fsu_base + (int)prog->nfsu)
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

/*
 * The chip clamps a vertex's colour to 0..1 before it interpolates: where the colour leaves 0..1 inside a triangle the picture differs from
 * the per-pixel clamp (dEQP: a sum of two varyings, which the linker may have moved into the vertex shader). A small draw's vertices are run on
 * the CPU and the draw takes the grid mode if one of them has a colour out of range.
 */
#define OOR_MAX_VERTICES 192

static bool colour_out_of_range(struct mbx_context *ctx, const struct mbx_vgp_program *prog, const struct pipe_draw_info *info,
				const struct pipe_draw_start_count_bias *dr, const void *indices, const unsigned *comps, uint8_t **vb_ptr)
{
	static float k[MBX_MAX_CONSTS][4];
	float va[MBX_MAX_ATTRS][4];

	if (prog->tex || prog->grid || !prog->gouraud || dr->count > OOR_MAX_VERTICES || getenv("MBX_NO_GRID_SUMS"))
		return false;
	cpu_constants(ctx, prog, k);
	mbx_cpuvs_oor_begin(prog->code, prog->ninsn, (const float (*)[4])k, prog->nconst);
	for (unsigned i = 0; i < dr->count; i++) {
		unsigned ix = dr->start + i;

		if (info->index_size == 1)
			ix = ((const uint8_t *)indices)[ix];
		else if (info->index_size == 2)
			ix = ((const uint16_t *)indices)[ix];
		else if (info->index_size == 4)
			ix = ((const uint32_t *)indices)[ix];
		if (info->index_size)
			ix += dr->index_bias;
		fetch_vertex(ctx, comps, ix, info->start_instance, vb_ptr, va);
		if (mbx_cpuvs_oor_vertex(prog->ninsn, (const float (*)[4])va))
			return true;
	}
	return false;
}

/*
 * The chip has one filter bit a texture unit, GL has a minification and a magnification filter (and mipmaps for the first only): which one a
 * draw wants depends on how many texels a pixel covers (rho). The first triangle of the draw is run through the vertex program on the CPU for its
 * screen positions and texture coordinates; rho <= 1 magnifies. Not known (-1): behind the eye, degenerate, or no coordinates in the outputs.
 */
static void estimate_magnification(struct mbx_context *ctx, const struct mbx_vgp_program *prog, const struct pipe_draw_info *info,
				   const struct pipe_draw_start_count_bias *dr, const void *indices, const unsigned *comps, uint8_t **vb_ptr)
{
	static float k[MBX_MAX_CONSTS][4];
	float va[3][MBX_MAX_ATTRS][4], o[3][8][4];
	unsigned ix[3];
	bool done = false;

	for (unsigned u = 0; u < MBX_MAX_TEX; u++)
		ctx->tex_mag[u] = -1;
	if (!prog->ntex || dr->count < 3 || (info->mode != MESA_PRIM_TRIANGLES && info->mode != MESA_PRIM_TRIANGLE_STRIP &&
					      info->mode != MESA_PRIM_TRIANGLE_FAN))
		return;
	for (unsigned u = 0; u < prog->ntex && u < MBX_MAX_TEX; u++) {
		const struct pipe_sampler_state *smp = ctx->samplers[u];
		const struct mbx_resource *tr;
		float x[3], y[3], uu[3], vv[3], dx1, dy1, dx2, dy2, det, dudx, dudy, dvdx, dvdy, rho;
		bool ok = true;

		if (!(prog->units >> u & 1) || !smp || !ctx->views[u] || !ctx->views[u]->texture || !mbx_resource(ctx->views[u]->texture)->tex)
			continue;
		if (smp->min_img_filter == smp->mag_img_filter && smp->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
			continue;
		tr = mbx_resource(ctx->views[u]->texture);
		if (!done) {
			cpu_constants(ctx, prog, k);
			for (unsigned t = 0; t < MBX_MAX_TEX; t++) {		/* the textures' scale constants are made at the emit */
				int sl = prog->tex_scale_slot[t];

				if (sl > 0 && (unsigned)sl < prog->nconst && ctx->views[t] && ctx->views[t]->texture) {
					const struct mbx_resource *r2 = mbx_resource(ctx->views[t]->texture);

					k[sl][0] = (float)r2->base.width0 / (float)(1u << r2->tex_lw);
					k[sl][1] = (float)r2->base.height0 / (float)(1u << r2->tex_lh);
					k[sl][2] = k[sl][3] = 0.0f;
				}
			}
			for (unsigned q = 0; q < 3; q++) {
				unsigned s = info->mode == MESA_PRIM_TRIANGLES ? q : info->mode == MESA_PRIM_TRIANGLE_STRIP ? q : (q == 0 ? 0 : q);

				ix[q] = dr->start + s;
				if (info->index_size == 1)
					ix[q] = ((const uint8_t *)indices)[ix[q]];
				else if (info->index_size == 2)
					ix[q] = ((const uint16_t *)indices)[ix[q]];
				else if (info->index_size == 4)
					ix[q] = ((const uint32_t *)indices)[ix[q]];
				if (info->index_size)
					ix[q] += dr->index_bias;
				fetch_vertex(ctx, comps, ix[q], info->start_instance, vb_ptr, va[q]);
				mbx_cpuvs_outputs(prog->code, prog->ninsn, (const float (*)[4])k, prog->nconst, (const float (*)[4])va[q], o[q]);
			}
			done = true;
		}
		for (unsigned q = 0; q < 3; q++) {
			float w = o[q][0][3];

			if (!(w > 1e-4f)) {
				ok = false;
				break;
			}
			x[q] = o[q][0][0] / w * ctx->viewport.scale[0];
			y[q] = o[q][0][1] / w * ctx->viewport.scale[1];
			uu[q] = o[q][3 + u][0] * (float)(1u << tr->tex_lw);
			vv[q] = o[q][3 + u][1] * (float)(1u << tr->tex_lh);
		}
		if (!ok)
			continue;
		dx1 = x[1] - x[0];
		dy1 = y[1] - y[0];
		dx2 = x[2] - x[0];
		dy2 = y[2] - y[0];
		det = dx1 * dy2 - dx2 * dy1;
		if (fabsf(det) < 1e-3f)
			continue;
		dudx = ((uu[1] - uu[0]) * dy2 - (uu[2] - uu[0]) * dy1) / det;
		dudy = (dx1 * (uu[2] - uu[0]) - dx2 * (uu[1] - uu[0])) / det;
		dvdx = ((vv[1] - vv[0]) * dy2 - (vv[2] - vv[0]) * dy1) / det;
		dvdy = (dx1 * (vv[2] - vv[0]) - dx2 * (vv[1] - vv[0])) / det;
		rho = MAX2(hypotf(dudx, dvdx), hypotf(dudy, dvdy));
		ctx->tex_mag[u] = rho <= 1.0f;
		mbx_dbg("texture unit %u: %g texels a pixel: %s", u, rho, rho <= 1.0f ? "magnified" : "minified");
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

/*
 * The kernel takes at most 256 K words of stream a job (and the parameter buffer holds a few hundred pages): a job that has
 * grown past this is rendered, and the next object starts a new one (its background is what the target holds by then, as after
 * any other flush). Checked between objects; one object (a draw's own vertices) is not split.
 */
#define JOB_SOFT_WORDS (160u * 1024)

static bool split_job(struct mbx_context *ctx)
{
	if (ctx->job.active && ctx->job.n > JOB_SOFT_WORDS) {
		mbx_dbg("the job's stream has %u words: rendered, a new job starts", ctx->job.n);
		mbx_job_flush(ctx);
		return mbx_job_begin(ctx);
	}
	return true;
}

/* The words that open an object: its primitive word, half the width or size of a line or point, the sections word */
static void emit_prim_header(struct mbx_context *ctx, const struct mbx_vgp_program *prog, uint32_t prim)
{
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
				u = (const float *)((uint8_t *)mbx_bo_map(mbx_resource(ctx->cb[0].buffer)->bo) + ctx->cb[0].buffer_offset);
			if (u)
				w = u[prog->psize_slot * 4 + prog->psize_lane];
		}
		mbx_emit_f(ctx, MBX_CH_TA, CLAMP(w, 1.0f, 32.0f) * 0.5f);
	}
	mbx_emit(ctx, MBX_CH_TA, prog->sections);
}

/*
 * A draw whose stream has grown past the soft limit (dEQP's shader tests draw a grid of 50000 vertices, 600 K words; the kernel takes 256 K
 * and the parameter buffer a few hundred pages) is cut between two primitives of a list: the object ends, the job is rendered (its depth
 * kept) and the next one opens the same object again. false: no job could be opened.
 */
static bool split_draw(struct mbx_context *ctx, const struct mbx_vgp_program *prog, uint32_t layout, uint32_t prim)
{
	mbx_emit(ctx, MBX_CH_CTL, 1);
	ctx->job.ndraws++;
	ctx->zkeep = true;
	mbx_job_flush(ctx);
	ctx->zkeep = false;
	if (!mbx_job_begin(ctx))
		return false;
	for (unsigned un = 0; un < prog->ntex; un++)
		if ((prog->units >> un & 1) && ctx->views[un] && ctx->views[un]->texture)
			mbx_job_use_texture(ctx, unit_texture(ctx, un));
	emit_header(ctx, prog, layout);
	emit_program(ctx, prog);
	emit_prim_header(ctx, prog, prim);
	return true;
}

static void mbx_draw_vbo_impl(struct pipe_context *pctx, const struct pipe_draw_info *info,
			 unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
			 const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
	struct mbx_context *ctx = mbx_context(pctx);
	const struct mbx_vgp_program *prog;
	uint8_t *vb_ptr[PIPE_MAX_ATTRIBS] = { 0 };
	uint32_t prim = hw_prim_word(info->mode);
	const void *indices = NULL;
	unsigned comps[MBX_MAX_ATTRS], d, list_unit = 0;
	bool can_split = false;
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
	if (ctx->fb.nr_cbufs && ctx->fb.cbufs[0].texture) {
		struct mbx_resource *cr = mbx_resource(ctx->fb.cbufs[0].texture);

		cr->wver++;		/* a texture rendered into: its swapped copy is stale */
		cr->gpu_written = true;
	}
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
	if (mbx_debug_level() > 1) {
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
	if (mbx_debug_level() > 1) {
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

		estimate_magnification(ctx, prog, info, &draws[d], indices, comps, vb_ptr);

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
				if (!split_job(ctx))
					return;
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
		if (!split_job(ctx))
			return;
		mbx_dbg("draw %u: header", d);
		for (unsigned un = 0; un < prog->ntex; un++)
			if ((prog->units >> un & 1) && ctx->views[un] && ctx->views[un]->texture)
				mbx_job_use_texture(ctx, unit_texture(ctx, un));
		emit_header(ctx, prog, layout);
		mbx_dbg("draw %u: program", d);
		emit_program(ctx, prog);
		if ((info->mode == MESA_PRIM_TRIANGLES || info->mode == MESA_PRIM_TRIANGLE_STRIP || info->mode == MESA_PRIM_TRIANGLE_FAN) &&
		    draws[d].count >= 3 && (prog->grid || colour_out_of_range(ctx, prog, info, &draws[d], indices, comps, vb_ptr))) {
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
		emit_prim_header(ctx, prog, prim);
		{
			/* lists can be cut after any whole primitive; strips, fans and loops are not cut; a stencil-tested draw is one job */
			list_unit = info->mode == MESA_PRIM_TRIANGLES ? 3 : info->mode == MESA_PRIM_LINES ? 2 : info->mode == MESA_PRIM_POINTS ? 1 : 0;
			can_split = list_unit && !(ctx->dsa && ctx->fb.zsbuf.texture && (ctx->dsa->stencil[0].enabled || ctx->dsa->stencil[1].enabled));
		}
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
			if (can_split && (i + 1) % list_unit == 0 && i + 1 < draws[d].count && ctx->job.n > JOB_SOFT_WORDS &&
			    !split_draw(ctx, prog, layout, prim))
				return;
		}
		mbx_emit(ctx, MBX_CH_CTL, 1);		/* the object ends */
		mbx_dbg("draw %u: done, stream %u words", d, ctx->job.n);
		ctx->job.ndraws++;
	}
	mbx_stencil_end(ctx);	/* a stencil-tested draw is rendered now and the depth of the rejected pixels put back */
}

static void mbx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
			 unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
			 const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
	uint64_t t0 = mbx_prof_level() ? prof_now_us() : 0;

	mbx_draw_vbo_impl(pctx, info, drawid_offset, indirect, draws, num_draws);
	if (t0)
		mbx_prof_note(MBX_PROF_DRAW, prof_now_us() - t0);
}

void mbx_draw_init(struct pipe_context *pctx)
{
	pctx->draw_vbo = mbx_draw_vbo;
}
