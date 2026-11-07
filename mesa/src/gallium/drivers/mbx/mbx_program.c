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
		/* texture word C: colour and alpha each A * B + C * D with C = 1 - 1 = 0. Replace is the stock
		 * stack's own word (the background object's); modulate has the texel in A and the primary colour in B */
		p->tex = true;
		p->tex_c[0] = ctx->fs->fs_info.modulate ? 0x86884610 : 0xd6887610;
		p->ntex = 1;
		if (ctx->fs->fs_info.units & 2) {
			/* the second unit multiplies the first's result by its own texel (found on the chip: its picture is
			 * the model's): colour and alpha A = previous, B = texel, C = 1 - 1 */
			p->tex_c[1] = 0x56883610;
			p->ntex = 2;
		}
		/* state bits 16-18: one texture unit is mode 2, two are 5 (found on the chip: 1 draws nothing, 3, 4, 6, 7 hang) */
		p->st_extra |= (p->ntex == 2 ? 5u : 2u) << 16;
	}
	p->sections = 0xf0020000 | 0x4 | (scissor ? 0xfu << 20 : 0);	/* user clip planes 0-3 on for the scissor */	/* the VGP section the program is loaded in (bit 2); bit 17 is needed on the chip (observed on an N810), the model ignores it */
	if (!mbx_compile_vs(ctx->vs->nir, &ctx->fs->fs_info, p)) {
		p->ninsn = 0;
		return NULL;
	}
	return p;
}
