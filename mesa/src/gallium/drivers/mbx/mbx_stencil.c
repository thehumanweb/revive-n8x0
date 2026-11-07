/* SPDX-License-Identifier: MIT */
/*
 * The stencil test on the chip (README assumption 53). The chip has no stencil; the stencil plane lives on the CPU (softpipe's combined
 * depth-stencil twin). A draw that only TESTS stencil (every stencil operation KEEP) is run on the chip with the test baked into the
 * external Z buffer the render loads (README assumption 7): the CPU works out which pixels pass from the plane, writes a depth that
 * fails the draw's depth test into the others (0.0 for LESS and LEQUAL, 2.0 for GREATER, GEQUAL and EQUAL; the chip ignores the sign of
 * a loaded depth, so there is no -1), runs the job, and puts the depth of the rejected pixels back. With the depth test off the draw is
 * run as LESS without depth writes, 2.0 where the stencil passes and 0.0 where it does not, and the whole buffer is put back.
 */
#include <string.h>
#include <stdlib.h>

#include "util/u_inlines.h"
#include "util/u_math.h"

#include "mbx_context.h"
#include "mbx_fallback.h"

#define Z_PASS_ALL	0x40000001u	/* 2.0, low bit set as the chip stores it */
#define Z_FAIL_LESS	0x00000001u	/* about 0.0 */

static bool stencil_on(const struct mbx_context *ctx)
{
	return ctx->dsa && ctx->fb.zsbuf.texture &&
	       (ctx->dsa->stencil[0].enabled || ctx->dsa->stencil[1].enabled);
}

/* True when this draw's stencil can be done on the chip: only a test, the operations all KEEP, one test for both faces, a depth test the mask can express */
bool mbx_stencil_chip_ok(struct mbx_context *ctx)
{
	const struct pipe_stencil_state *s0, *s1;

	if (!stencil_on(ctx) || getenv("MBX_CPU_STENCIL"))
		return false;
	s0 = &ctx->dsa->stencil[0];
	s1 = &ctx->dsa->stencil[1];
	if (!s0->enabled || s0->fail_op != PIPE_STENCIL_OP_KEEP || s0->zfail_op != PIPE_STENCIL_OP_KEEP ||
	    s0->zpass_op != PIPE_STENCIL_OP_KEEP)
		return false;
	if (s1->enabled && (s1->func != s0->func || s1->valuemask != s0->valuemask || s1->fail_op != PIPE_STENCIL_OP_KEEP ||
			    s1->zfail_op != PIPE_STENCIL_OP_KEEP || s1->zpass_op != PIPE_STENCIL_OP_KEEP ||
			    ctx->stencil_ref.ref_value[1] != ctx->stencil_ref.ref_value[0]))
		return false;
	if (ctx->dsa->depth_enabled)
		switch (ctx->dsa->depth_func) {
		case PIPE_FUNC_LESS: case PIPE_FUNC_LEQUAL: case PIPE_FUNC_GREATER: case PIPE_FUNC_GEQUAL: case PIPE_FUNC_EQUAL:
			break;
		default:
			return false;	/* ALWAYS and NOTEQUAL cannot be made to reject, NEVER draws nothing */
		}
	return true;
}

static bool stencil_pass(unsigned func, unsigned ref, unsigned st)
{
	switch (func) {
	case PIPE_FUNC_NEVER: return false;
	case PIPE_FUNC_LESS: return ref < st;
	case PIPE_FUNC_EQUAL: return ref == st;
	case PIPE_FUNC_LEQUAL: return ref <= st;
	case PIPE_FUNC_GREATER: return ref > st;
	case PIPE_FUNC_NOTEQUAL: return ref != st;
	case PIPE_FUNC_GEQUAL: return ref >= st;
	default: return true;
	}
}

/* Before the chip draw: finish what was recorded (keeping its depth), write the mask into the Z memory, remember what to put back */
bool mbx_stencil_begin(struct mbx_context *ctx)
{
	struct mbx_resource *zr = mbx_resource(ctx->fb.zsbuf.texture);
	const struct pipe_stencil_state *s0 = &ctx->dsa->stencil[0];
	unsigned w = ctx->fb.width, h = ctx->fb.height, stride_px, func = s0->func, ref = ctx->stencil_ref.ref_value[0] & s0->valuemask;
	bool pass[256], depth_on = ctx->dsa->depth_enabled;
	uint32_t reject = ctx->dsa->depth_func == PIPE_FUNC_LESS || ctx->dsa->depth_func == PIPE_FUNC_LEQUAL ? Z_FAIL_LESS : Z_PASS_ALL;
	uint8_t *st;
	uint32_t *mem;

	ctx->zkeep = true;
	mbx_job_flush(ctx);			/* what was drawn ends, its depth is stored */
	ctx->zkeep = false;
	if (!mbx_z_ensure(ctx, zr))
		return false;
	mbx_resource_wait(ctx->screen, zr);
	mbx_z_fill_if_stale(ctx, zr);
	st = malloc((size_t)w * h);
	if (!st)
		return false;
	if (!mbx_fallback_read_stencil(ctx, st)) {
		free(st);
		return false;
	}
	mem = mbx_bo_map(zr->zbo);
	if (!mem) {
		free(st);
		return false;
	}
	stride_px = align(zr->zw, 16);
	if (!depth_on)
		reject = Z_FAIL_LESS;
	for (unsigned i = 0; i < 256; i++)
		pass[i] = stencil_pass(func, ref, i & s0->valuemask);
	free(ctx->smask.backup);
	ctx->smask.backup = malloc((size_t)stride_px * h * 4);
	if (!ctx->smask.backup) {
		free(st);
		return false;
	}
	memcpy(ctx->smask.backup, mem, (size_t)stride_px * h * 4);
	free(ctx->smask.fail);
	ctx->smask.fail = malloc((size_t)w * h);
	if (!ctx->smask.fail) {
		free(st);
		return false;
	}
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			bool p = pass[st[(size_t)y * w + x]];

			ctx->smask.fail[(size_t)y * w + x] = !p;
			if (!p)
				mem[(size_t)y * stride_px + x] = reject;
			else if (!depth_on)
				mem[(size_t)y * stride_px + x] = Z_PASS_ALL;
		}
	free(st);
	ctx->smask.active = true;
	ctx->smask.override = !depth_on;
	ctx->smask.zr = zr;
	ctx->smask.w = w;
	ctx->smask.h = h;
	zr->zvalid = true;			/* the job loads the masked memory */
	return true;
}

/* After the chip draw: render it, keeping the depth, and put the depth of the rejected pixels (all of it when the depth test was off) back */
void mbx_stencil_end(struct mbx_context *ctx)
{
	struct mbx_resource *zr = ctx->smask.zr;
	uint32_t *mem;
	unsigned stride_px;

	if (!ctx->smask.active)
		return;
	ctx->zkeep = true;
	mbx_job_flush(ctx);
	ctx->zkeep = false;
	mbx_resource_wait(ctx->screen, zr);
	mem = mbx_bo_map(zr->zbo);
	stride_px = align(zr->zw, 16);
	if (mem)
		for (unsigned y = 0; y < ctx->smask.h; y++)
			for (unsigned x = 0; x < ctx->smask.w; x++)
				if (ctx->smask.override || ctx->smask.fail[(size_t)y * ctx->smask.w + x])
					mem[(size_t)y * stride_px + x] = ctx->smask.backup[(size_t)y * stride_px + x];
	zr->zvalid = true;
	ctx->smask.active = false;
}
