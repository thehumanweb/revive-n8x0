/* SPDX-License-Identifier: MIT */
#ifndef MBX_RESOURCE_H
#define MBX_RESOURCE_H

#include "pipe/p_state.h"
#include "renderonly/renderonly.h"

#include "mbx_bo.h"

struct mbx_context;

struct mbx_resource {
	struct pipe_resource base;
	struct mbx_bo *bo;
	uint32_t stride;	/* bytes a row; 0 for buffers */
	uint32_t size;		/* bytes the layout takes */
	bool on_chip;		/* a depth buffer: the MBX keeps depth on chip, there is no memory */
	struct renderonly_scanout *scanout;	/* the KMS device's buffer this is, when it is scanned out */
};

static inline struct mbx_resource *mbx_resource(struct pipe_resource *p)
{
	return (struct mbx_resource *)p;
}

void mbx_resource_screen_init(struct pipe_screen *pscreen);
void mbx_resource_context_init(struct pipe_context *pctx);

/* context.c: render what is recorded for rsc, before the CPU or another user reads it */
void mbx_flush_for_resource(struct mbx_context *ctx, struct mbx_resource *rsc);

#endif
