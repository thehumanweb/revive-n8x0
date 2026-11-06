/* SPDX-License-Identifier: MIT */
#ifndef MBX_SCREEN_H
#define MBX_SCREEN_H

#include <stdint.h>

#include "pipe/p_screen.h"
#include "renderonly/renderonly.h"

#include <stdio.h>
#include <stdlib.h>

/* MBX_DEBUG=1 in the environment traces the driver's decisions on stderr */
#define mbx_dbg(...) do { if (getenv("MBX_DEBUG")) { fprintf(stderr, "mbx: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

struct mbx_screen {
	struct pipe_screen base;
	int fd;
	struct renderonly *ro;
	uint32_t core_id, revision;
};

static inline struct mbx_screen *mbx_screen(struct pipe_screen *p)
{
	return (struct mbx_screen *)p;
}

struct pipe_screen *mbx_screen_create(int fd, const struct pipe_screen_config *config,
				      struct renderonly *ro);

struct pipe_context *mbx_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

#endif
