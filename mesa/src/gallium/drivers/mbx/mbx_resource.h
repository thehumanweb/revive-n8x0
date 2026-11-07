/* SPDX-License-Identifier: MIT */
#ifndef MBX_RESOURCE_H
#define MBX_RESOURCE_H

#include "pipe/p_state.h"
#include "renderonly/renderonly.h"

#include "mbx_bo.h"

#define MBX_MAX_LEVELS 12

struct mbx_screen;

struct mbx_context;

struct mbx_resource {
	struct pipe_resource base;
	struct mbx_bo *bo;
	uint32_t stride;	/* bytes a row; 0 for buffers */
	uint32_t size;		/* bytes the layout takes */
	bool on_chip;		/* a depth buffer: the MBX keeps depth on chip, there is no memory */
	uint64_t last_fence;	/* the last job that wrote it: the CPU waits for it before touching the memory */
	struct renderonly_scanout *scanout;	/* the KMS device's buffer this is, when it is scanned out */
	/* a sampled 2D texture: linear (the chip reads linear textures with a stride), every level's
	 * width and height padded to a power of two of at least 8, the edge texels repeated into the padding */
	bool tex;
	bool twiddled;				/* the levels are one chain, each in the chip's twiddled order (power-of-two textures with mipmaps) */
	bool bind_rt;				/* made with a render target bind (softpipe then shares its memory) */
	struct pipe_resource *sp_twin;		/* the CPU fallback's view of it (mbx_fallback.c) */
	bool sp_fresh;				/* a depth twin not yet cleared */
	bool sp_dirty;				/* a sampled texture the CPU wrote since the twin was filled */
	uint8_t tex_lw, tex_lh;			/* log2 of level 0's padded width and height */
	uint32_t level_off[MBX_MAX_LEVELS];	/* bytes */
	uint32_t level_stride[MBX_MAX_LEVELS];	/* bytes a row */
};

/* Copy a box of one level between a linear buffer and a twiddled texture (store: linear to texture) */
void mbx_tex_twiddle(struct mbx_resource *rsc, unsigned level, const struct pipe_box *box, uint8_t *lin,
		     unsigned lin_stride, bool store);

/* The chip's texel format (texture word A bits 14-16) for a gallium format, or -1 */
static inline int mbx_chip_tex_format(enum pipe_format f)
{
	switch (f) {
	case PIPE_FORMAT_B8G8R8A8_UNORM: return 6;	/* ARGB8888 */
	case PIPE_FORMAT_B5G6R5_UNORM: return 1;	/* RGB565 */
	case PIPE_FORMAT_B4G4R4A4_UNORM: return 2;	/* ARGB4444 */
	case PIPE_FORMAT_B5G5R5A1_UNORM: return 0;	/* ARGB1555 */
	case PIPE_FORMAT_L8A8_UNORM: return 4;		/* LA88 */
	case PIPE_FORMAT_A8_UNORM:
	case PIPE_FORMAT_L8_UNORM: return 5;		/* one byte in every channel, alpha too (the combiner picks) */
	default: return -1;
	}
}

static inline struct mbx_resource *mbx_resource(struct pipe_resource *p)
{
	return (struct mbx_resource *)p;
}

void mbx_resource_screen_init(struct pipe_screen *pscreen);
void mbx_resource_context_init(struct pipe_context *pctx);

/* context.c: render what is recorded for rsc, before the CPU or another user reads it */
void mbx_flush_for_resource(struct mbx_context *ctx, struct mbx_resource *rsc);
/* and wait for the job that wrote it, before the CPU reads or writes its memory */
void mbx_resource_wait(struct mbx_screen *screen, struct mbx_resource *rsc);

#endif
