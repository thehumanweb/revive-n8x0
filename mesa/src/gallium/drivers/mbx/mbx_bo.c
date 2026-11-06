// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "drm-uapi/mbx_drm.h"
#include "util/u_atomic.h"
#include "util/log.h"

#include "mbx_bo.h"
#include "mbx_screen.h"

/* GEM handle to a mapped, usable buffer: asks where it lives in the MBX's space */
static struct mbx_bo *bo_from_handle(struct mbx_screen *screen, uint32_t handle)
{
	struct drm_mbx_bo_info info = { .handle = handle };
	struct mbx_bo *bo;

	if (drmIoctl(screen->fd, DRM_IOCTL_MBX_BO_INFO, &info)) {
		mesa_loge("mbx: BO_INFO failed: %s", strerror(errno));
		drmCloseBufferHandle(screen->fd, handle);
		return NULL;
	}
	bo = calloc(1, sizeof(*bo));
	if (!bo) {
		drmCloseBufferHandle(screen->fd, handle);
		return NULL;
	}
	bo->refcount = 1;
	bo->screen = screen;
	bo->handle = handle;
	bo->va = info.va;
	bo->size = info.size;
	bo->offset = info.offset;
	return bo;
}

struct mbx_bo *mbx_bo_create(struct mbx_screen *screen, uint64_t size)
{
	struct drm_mbx_bo_create create = { .size = size };

	if (drmIoctl(screen->fd, DRM_IOCTL_MBX_BO_CREATE, &create)) {
		mesa_loge("mbx: BO_CREATE of %llu bytes failed: %s", (unsigned long long)size, strerror(errno));
		return NULL;
	}
	return bo_from_handle(screen, create.handle);
}

struct mbx_bo *mbx_bo_import(struct mbx_screen *screen, int prime_fd)
{
	uint32_t handle;

	if (drmPrimeFDToHandle(screen->fd, prime_fd, &handle))
		return NULL;
	return bo_from_handle(screen, handle);
}

void *mbx_bo_map(struct mbx_bo *bo)
{
	if (!bo->map) {
		void *p = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->screen->fd,
			       (off_t)bo->offset);

		if (p == MAP_FAILED) {
			mesa_loge("mbx: mmap of a buffer failed: %s", strerror(errno));
			return NULL;
		}
		bo->map = p;
	}
	return bo->map;
}

struct mbx_bo *mbx_bo_ref(struct mbx_bo *bo)
{
	if (bo)
		p_atomic_inc(&bo->refcount);
	return bo;
}

void mbx_bo_unref(struct mbx_bo *bo)
{
	struct drm_mbx_bo_unmap unmap;

	if (!bo || !p_atomic_dec_zero(&bo->refcount))
		return;
	if (bo->map)
		munmap(bo->map, bo->size);
	unmap.handle = bo->handle;
	unmap.pad = 0;
	drmIoctl(bo->screen->fd, DRM_IOCTL_MBX_BO_UNMAP, &unmap);
	drmCloseBufferHandle(bo->screen->fd, bo->handle);
	free(bo);
}
