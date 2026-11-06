// SPDX-License-Identifier: GPL-2.0-only
/* Buffers: shmem GEM objects, write-combined for the CPU, mapped into the MBX's address space */
#include <linux/sizes.h>

#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vma_manager.h>

#include "mbx_drv.h"

#define MBX_BO_MAX	(16u << 20)

int mbx_ioctl_bo_create(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_mbx_bo_create *a = data;
	struct drm_gem_shmem_object *shmem;
	int ret;

	if (a->flags || !a->size || a->size > MBX_BO_MAX)
		return -EINVAL;
	shmem = drm_gem_shmem_create(dev, PAGE_ALIGN(a->size));
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	shmem->map_wc = true;
	ret = drm_gem_handle_create(file, &shmem->base, &a->handle);
	drm_gem_object_put(&shmem->base);
	return ret;
}

int mbx_ioctl_bo_info(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_file *f = file->driver_priv;
	struct drm_mbx_bo_info *a = data;
	struct drm_gem_object *obj;
	struct mbx_mapping *map;
	int ret;

	if (a->flags & ~MBX_BO_INFO_ALIGN_64K)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, a->handle);
	if (!obj)
		return -ENOENT;
	map = mbx_vm_map(f->vm, obj, a->flags & MBX_BO_INFO_ALIGN_64K ? SZ_64K : 0);
	if (IS_ERR(map)) {
		ret = PTR_ERR(map);
		goto out;
	}
	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto out;
	a->size = obj->size;
	a->offset = drm_vma_node_offset_addr(&obj->vma_node);
	a->va = (u32)map->node.start;
out:
	drm_gem_object_put(obj);
	return ret;
}

int mbx_ioctl_bo_unmap(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_file *f = file->driver_priv;
	struct drm_mbx_bo_unmap *a = data;
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file, a->handle);
	if (!obj)
		return -ENOENT;
	mbx_flush_jobs(to_mbx(dev));	/* a queued job may be using the mapping */
	ret = mbx_vm_unmap(f->vm, obj);
	drm_gem_object_put(obj);
	return ret;
}
