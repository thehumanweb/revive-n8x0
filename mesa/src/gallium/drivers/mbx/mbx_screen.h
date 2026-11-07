/* SPDX-License-Identifier: MIT */
#ifndef MBX_SCREEN_H
#define MBX_SCREEN_H

#include <stdint.h>

#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/simple_mtx.h"
#include "renderonly/renderonly.h"

#include <stdio.h>
#include <stdlib.h>

/* MBX_DEBUG=1 in the environment traces the driver's decisions on stderr */
#define mbx_dbg(...) do { if (getenv("MBX_DEBUG")) { fprintf(stderr, "mbx: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* A job's fence: jobs complete in order, so a number says it all */
struct pipe_fence_handle {
	struct pipe_reference reference;
	uint64_t seqno;
};

struct sw_winsys;

struct mbx_screen {
	struct pipe_screen base;
	bool no_omap_damage;		/* the KMS device has no DRM_IOCTL_OMAP_GEM_DAMAGE (or it was turned off) */
	struct pipe_screen *sp_screen;	/* the CPU fallback's softpipe, made at the first use (mbx_fallback.c) */
	struct sw_winsys *sp_ws;
	uint64_t fence_done;	/* the highest fence known to be done */
	int fd;
	struct renderonly *ro;
	uint32_t core_id, revision;
	simple_mtx_t bo_lock;		/* guards the list of imported buffers */
	struct mbx_bo *imports;
};

static inline struct mbx_screen *mbx_screen(struct pipe_screen *p)
{
	return (struct mbx_screen *)p;
}

/* Wait for a job's fence (ns; false on a timeout); a job that failed still counts as done */
bool mbx_fence_wait(struct mbx_screen *screen, uint64_t seqno, uint64_t timeout_ns);

struct pipe_screen *mbx_screen_create(int fd, const struct pipe_screen_config *config,
				      struct renderonly *ro);

struct pipe_context *mbx_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

#endif
