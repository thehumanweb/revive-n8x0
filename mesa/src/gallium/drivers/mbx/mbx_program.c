// SPDX-License-Identifier: MIT
/*
 * Shaders: the NIR the state tracker gives us, kept until a draw, when the
 * vertex shader is compiled to VGP and the fragment shader matched onto the
 * fixed pipe (mbx_compile.c).
 */
#include "compiler/nir/nir.h"
#include "nir/nir_serialize.h"
#include "util/log.h"
#include "util/ralloc.h"
#include "util/u_memory.h"

#include "mbx_context.h"
#include "mbx_fallback.h"
#include "mbx_program.h"

/*
 * A texture combiner word C (README assumption 38, 41): colour in bits 15-31 and alpha in 0-14, each A * B + C * D with
 * C = 1 - 1 = 0. Sources: A {konst, previous, texel, one}, B and D {primary, previous, texel, konst}, C {primary,
 * previous, texel, one}; the stock replace word is 0xd6887610, the model's decoding of the rest is texture.c.
 * unit 0: the texel times the primary colour (modulate), the texel alone (replace), or the primary colour (no texel here);
 * unit 1: the previous result times its texel, or the previous result.
 */
static uint32_t comb_word(unsigned has_texel, bool has_prim, unsigned unit)
{
	const uint32_t tail = 3u << 25 | 2u << 22 | 1u << 19;	/* C = one, complemented: zero; D = texel (unused) */

	if (unit == 0)
		return has_texel ? (has_prim ? 2u << 30 : 3u << 30 | 2u << 27) | tail : 3u << 30 | tail;
	return has_texel ? 1u << 30 | 2u << 27 | tail : 3u << 30 | 1u << 27 | tail;
}

static uint32_t comb_word_alpha(unsigned has_texel, bool has_prim, unsigned unit)
{
	const uint32_t tail = 3u << 9 | 1u << 4;

	if (unit == 0)
		return has_texel ? (has_prim ? 2u << 13 : 3u << 13 | 2u << 11) | tail : 3u << 13 | tail;
	return has_texel ? 1u << 13 | 2u << 11 | tail : 3u << 13 | 1u << 11 | tail;
}

/*
 * The combiner words of the texture units for a shader; l8: the units whose bound texture is L8, whose texel alpha is 1
 * where the chip returns the byte (so that lane takes the primary colour's alpha, as if the shader had no texel there).
 */
void mbx_tex_combiners(const struct mbx_fs_info *fi, unsigned ntex, unsigned l8, uint32_t out[MBX_MAX_TEX])
{
	for (unsigned u = 0; u < ntex; u++)
		out[u] = comb_word(fi->mc >> u & 1, fi->pass_c, u) |
			 comb_word_alpha((fi->ma & ~l8) >> u & 1, fi->pass_a, u);
}

static void *mbx_shader_create(struct pipe_context *pctx, const struct pipe_shader_state *templ)
{
	struct mbx_shader *so = CALLOC_STRUCT(mbx_shader);

	if (!so)
		return NULL;
	if (templ->type != PIPE_SHADER_IR_NIR) {
		FREE(so);
		return NULL;
	}
	so->nir_orig = nir_shader_clone(NULL, templ->ir.nir);	/* the fallback's, before our lowering */
	so->nir = templ->ir.nir;	/* ours now */
	so->stage = so->nir->info.stage;
	mbx_nir_prepare(so->nir);
	if (so->stage == MESA_SHADER_FRAGMENT)
		mbx_fs_match(so->nir, &so->fs_info);
	return so;
}

static void mbx_shader_delete(struct pipe_context *pctx, void *hwcso)
{
	struct mbx_shader *so = hwcso;

	mbx_fallback_shader_delete(mbx_context(pctx), so);
	ralloc_free(so->nir);
	FREE(so);
}

static void mbx_vs_bind(struct pipe_context *pctx, void *hwcso)
{
	mbx_context(pctx)->vs = hwcso;
}

static void mbx_fs_bind(struct pipe_context *pctx, void *hwcso)
{
	mbx_context(pctx)->fs = hwcso;
}

void mbx_program_init(struct pipe_context *pctx)
{
	pctx->create_vs_state = mbx_shader_create;
	pctx->delete_vs_state = mbx_shader_delete;
	pctx->bind_vs_state = mbx_vs_bind;
	pctx->create_fs_state = mbx_shader_create;
	pctx->delete_fs_state = mbx_shader_delete;
	pctx->bind_fs_state = mbx_fs_bind;
}

void mbx_program_destroy(struct mbx_vgp_program *p)
{
	FREE(p);
}

/* The vertex and fragment shader bound, linked: compiled once for the pair and kept in the context */
struct mbx_vgp_program *mbx_program_get(struct mbx_context *ctx)
{
	struct mbx_vgp_program *p;

	bool scissor = ctx->rast && ctx->rast->scissor;

	for (p = ctx->programs; p; p = p->next)
		if (p->vs == ctx->vs && p->fs == ctx->fs && p->scissor == scissor)
			return p->ninsn ? p : NULL;
	p = CALLOC_STRUCT(mbx_vgp_program);
	if (!p)
		return NULL;
	p->vs = ctx->vs;
	p->fs = ctx->fs;
	p->scissor = scissor;
	p->next = ctx->programs;
	ctx->programs = p;	/* kept even when it fails (ninsn 0), so it is not tried again */
	if (ctx->fs->fs_info.kind != MBX_FS_MATCHED) {
		mbx_dbg("this fragment shader does not reduce to the fixed pipe: the CPU fallback draws it");
		return NULL;
	}
	p->gouraud = !ctx->fs->fs_info.flat;
	p->tex_scale_slot[0] = p->tex_scale_slot[1] = -1;
	if (ctx->fs->fs_info.tex) {
		const struct mbx_fs_info *fi = &ctx->fs->fs_info;

		p->tex = true;
		p->units = fi->units;
		p->ntex = fi->units & 2 ? 2 : 1;	/* unit 1 needs unit 0's block before it */
		mbx_tex_combiners(fi, p->ntex, 0, p->tex_c);
		/* state bits 16-18: one texture unit is mode 2, two are 5 (found on the chip: 1 draws nothing, 3, 4, 6, 7 hang) */
		p->st_extra |= (p->ntex == 2 ? 5u : 2u) << 16;
	}
	p->sections = 0xf0020000 | 0x4 | (scissor ? 0xfu << 20 : 0);	/* the VGP section the program is loaded in (bit 2); user clip planes 0-3 on for the scissor; bit 17 is needed on the chip (observed on an N810), the model ignores it */
	if (ctx->fs->fs_info.has_off)
		p->st_extra |= 1u << 23;	/* the offset colour (o2) is added after the texture units (found on the chip) */
	if (!mbx_compile_vs(ctx->vs->nir, &ctx->fs->fs_info, p)) {
		p->ninsn = 0;
		return NULL;
	}
	return p;
}
