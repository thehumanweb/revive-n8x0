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
static struct mbx_bo *bo_from_handle(struct mbx_screen *screen, uint32_t handle, uint32_t flags)
{
	struct drm_mbx_bo_info info = { .handle = handle, .flags = flags };
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
	/* every buffer is listed by handle: an import of a dma-buf this process exported (or imported before) returns the same handle */
	simple_mtx_lock(&screen->bo_lock);
	bo->next_import = screen->imports;
	screen->imports = bo;
	simple_mtx_unlock(&screen->bo_lock);
	return bo;
}

static struct mbx_bo *bo_create(struct mbx_screen *screen, uint64_t size, uint32_t info_flags)
{
	struct drm_mbx_bo_create create = { .size = size };

	if (drmIoctl(screen->fd, DRM_IOCTL_MBX_BO_CREATE, &create)) {
		mesa_loge("mbx: BO_CREATE of %llu bytes failed: %s", (unsigned long long)size, strerror(errno));
		return NULL;
	}
	return bo_from_handle(screen, create.handle, info_flags);
}

struct mbx_bo *mbx_bo_create(struct mbx_screen *screen, uint64_t size)
{
	return bo_create(screen, size, 0);
}

struct mbx_bo *mbx_bo_create_align64k(struct mbx_screen *screen, uint64_t size)
{
	return bo_create(screen, size, MBX_BO_INFO_ALIGN_64K);
}

/*
 * Importing a dma-buf this file already has (it imported it before, or it made and exported the buffer itself) returns the
 * same GEM handle, not a new one: the second user must share the first's mbx_bo, or the first release closes the handle
 * under the second (jobs then fail to find it, the mapping loses its permission). Every buffer is on a list on the
 * screen, by handle.
 */
struct mbx_bo *mbx_bo_import(struct mbx_screen *screen, int prime_fd)
{
	uint32_t handle;
	struct mbx_bo *bo;

	if (drmPrimeFDToHandle(screen->fd, prime_fd, &handle))
		return NULL;
	simple_mtx_lock(&screen->bo_lock);
	for (bo = screen->imports; bo; bo = bo->next_import)
		if (bo->handle == handle) {
			if (p_atomic_inc_return(&bo->refcount) > 1) {
				simple_mtx_unlock(&screen->bo_lock);
				return bo;
			}
			p_atomic_dec(&bo->refcount);	/* it is being released: make a new one */
			break;
		}
	simple_mtx_unlock(&screen->bo_lock);
	return bo_from_handle(screen, handle, 0);
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
	struct mbx_bo **pp;

	if (!bo)
		return;
	simple_mtx_lock(&bo->screen->bo_lock);
	if (!p_atomic_dec_zero(&bo->refcount)) {
		simple_mtx_unlock(&bo->screen->bo_lock);
		return;
	}
	for (pp = &bo->screen->imports; *pp; pp = &(*pp)->next_import)
		if (*pp == bo) {
			*pp = bo->next_import;
			break;
		}
	simple_mtx_unlock(&bo->screen->bo_lock);
	if (bo->map)
		munmap(bo->map, bo->size);
	unmap.handle = bo->handle;
	unmap.pad = 0;
	drmIoctl(bo->screen->fd, DRM_IOCTL_MBX_BO_UNMAP, &unmap);
	drmCloseBufferHandle(bo->screen->fd, bo->handle);
	free(bo);
}
