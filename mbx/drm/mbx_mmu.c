// SPDX-License-Identifier: GPL-2.0-only
/*
 * The MBX's MMU: eight page tables of 1024 bare physical page addresses map
 * 32 MB of device addresses (mbx/README.md, assumption 8). One address space
 * a DRM file; the MMU holds one at a time, switched at each job. Pages with no
 * buffer point at a scratch page so a stray access never reaches physical 0.
 */
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <drm/drm_gem_shmem_helper.h>

#include "mbx_drv.h"

/* The parameter buffer's object (the free list's table, then its pages) at its fixed addresses */
static int mbx_pb_map(struct mbx_vm *vm, struct mbx_device *m)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->pb);
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);
	struct sg_dma_page_iter it;
	u32 pg = MBX_PB_TABLE_VA >> 12, end = (MBX_PB_VA >> 12) + MBX_PB_PAGES;

	if (IS_ERR(sgt))
		return PTR_ERR(sgt);
	for_each_sgtable_dma_page(sgt, &it, 0) {
		if (pg >= end)
			break;
		vm->pt[pg >> 10][pg & 1023] = lower_32_bits(sg_page_iter_dma_address(&it));
		pg++;
	}
	return 0;
}

struct mbx_vm *mbx_vm_create(struct mbx_device *m)
{
	struct mbx_vm *vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	int i, j;

	if (!vm)
		return ERR_PTR(-ENOMEM);
	vm->m = m;
	mutex_init(&vm->lock);
	INIT_LIST_HEAD(&vm->maps);
	/* buffers go below the free list's table; the parameter buffer owns the top */
	drm_mm_init(&vm->mm, MBX_VM_START, MBX_PB_TABLE_VA - MBX_VM_START);
	for (i = 0; i < MBX_MMU_TABLES; i++) {
		vm->pt[i] = dma_alloc_coherent(m->dev, MBX_PAGE, &vm->pt_dma[i], GFP_KERNEL);
		if (!vm->pt[i])
			goto err;
		for (j = 0; j < MBX_PAGE / 4; j++)
			vm->pt[i][j] = lower_32_bits(m->dummy_dma);
	}
	if (mbx_pb_map(vm, m)) {
		i = MBX_MMU_TABLES;
		goto err;
	}
	return vm;
err:
	while (i--)
		dma_free_coherent(m->dev, MBX_PAGE, vm->pt[i], vm->pt_dma[i]);
	drm_mm_takedown(&vm->mm);
	kfree(vm);
	return ERR_PTR(-ENOMEM);
}

static void unmap_locked(struct mbx_vm *vm, struct mbx_mapping *map)
{
	u32 pg = map->node.start >> 12, n = map->node.size >> 12, k;

	for (k = 0; k < n; k++, pg++)
		vm->pt[pg >> 10][pg & 1023] = lower_32_bits(vm->m->dummy_dma);
	list_del(&map->link);
	drm_mm_remove_node(&map->node);
	drm_gem_object_put(map->obj);
	kfree(map);
}

void mbx_vm_destroy(struct mbx_vm *vm)
{
	struct mbx_device *m = vm->m;
	struct mbx_mapping *map, *tmp;
	int i;

	mutex_lock(&m->job_lock);
	if (m->active_vm == vm) {
		mbx_write(m, MBX_REG_MMU_CTRL, 0);
		m->active_vm = NULL;
	}
	mutex_unlock(&m->job_lock);
	mutex_lock(&vm->lock);
	list_for_each_entry_safe(map, tmp, &vm->maps, link)
		unmap_locked(vm, map);
	mutex_unlock(&vm->lock);
	drm_mm_takedown(&vm->mm);
	for (i = 0; i < MBX_MMU_TABLES; i++)
		dma_free_coherent(m->dev, MBX_PAGE, vm->pt[i], vm->pt_dma[i]);
	kfree(vm);
}

static struct mbx_mapping *find_locked(struct mbx_vm *vm, struct drm_gem_object *obj)
{
	struct mbx_mapping *map;

	list_for_each_entry(map, &vm->maps, link)
		if (map->obj == obj)
			return map;
	return NULL;
}

/* Map a buffer (once): its pages' DMA addresses into the page tables */
struct mbx_mapping *mbx_vm_map(struct mbx_vm *vm, struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	struct mbx_mapping *map;
	struct sg_table *sgt;
	struct sg_dma_page_iter it;
	u32 pg, end;
	int ret;

	mutex_lock(&vm->lock);
	map = find_locked(vm, obj);
	if (map)
		goto out;
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt)) {
		map = ERR_CAST(sgt);
		goto out;
	}
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		map = ERR_PTR(-ENOMEM);
		goto out;
	}
	ret = drm_mm_insert_node(&vm->mm, &map->node, ALIGN(obj->size, MBX_PAGE));
	if (ret) {
		kfree(map);
		map = ERR_PTR(ret);
		goto out;
	}
	pg = map->node.start >> 12;
	end = pg + (map->node.size >> 12);
	for_each_sgtable_dma_page(sgt, &it, 0) {
		if (pg >= end)
			break;
		vm->pt[pg >> 10][pg & 1023] = lower_32_bits(sg_page_iter_dma_address(&it));
		pg++;
	}
	drm_gem_object_get(obj);
	map->obj = obj;
	list_add(&map->link, &vm->maps);
out:
	mutex_unlock(&vm->lock);
	return map;
}

int mbx_vm_unmap(struct mbx_vm *vm, struct drm_gem_object *obj)
{
	struct mbx_mapping *map;
	int ret = -ENOENT;

	mutex_lock(&vm->lock);
	map = find_locked(vm, obj);
	if (map) {
		unmap_locked(vm, map);
		ret = 0;
	}
	mutex_unlock(&vm->lock);
	return ret;
}

/* Make vm the MMU's address space; the TLB is flushed whenever it is written */
void mbx_vm_activate(struct mbx_vm *vm)
{
	struct mbx_device *m = vm->m;
	int i;

	for (i = 0; i < MBX_MMU_TABLES; i++)
		mbx_write(m, MBX_REG_MMU_TABLE(i), lower_32_bits(vm->pt_dma[i]));
	mbx_write(m, MBX_REG_MMU_CTRL, 1);
	m->active_vm = vm;
}
