/* SPDX-License-Identifier: MIT */
/* Buffers of the mbx render node: a GEM object mapped into the file's MBX address space */
#ifndef MBX_BO_H
#define MBX_BO_H

#include <stdint.h>

struct mbx_screen;

struct mbx_bo {
	int refcount;
	struct mbx_screen *screen;
	uint32_t handle;
	uint32_t va;		/* the MBX's address of its first byte */
	uint64_t size;
	uint64_t offset;	/* mmap offset on the DRM fd */
	void *map;		/* CPU view (write-combined), made on first use */
};

struct mbx_bo *mbx_bo_create(struct mbx_screen *screen, uint64_t size);
/* mapped at a 64 KB aligned address (the TA tail pointers need it) */
struct mbx_bo *mbx_bo_create_align64k(struct mbx_screen *screen, uint64_t size);
/* a dma-buf from another device (omapdrm's scanout buffers) */
struct mbx_bo *mbx_bo_import(struct mbx_screen *screen, int prime_fd);
void *mbx_bo_map(struct mbx_bo *bo);
struct mbx_bo *mbx_bo_ref(struct mbx_bo *bo);
void mbx_bo_unref(struct mbx_bo *bo);

#endif
