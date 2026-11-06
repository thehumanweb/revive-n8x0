// SPDX-License-Identifier: GPL-2.0-only
/*
 * The MBX's MMU: eight page tables of 1024 bare physical page addresses map
 * 32 MB of device addresses (mbx/README.md, assumption 8). One address space
 * a DRM file; the MMU holds one at a time, switched at each job. Pages with no
 * buffer point at a scratch page so a stray access never reaches physical 0.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <drm/drm_gem_shmem_helper.h>

#include "mbx_drv.h"

/* An object's pages at fixed device addresses, from va for npages (the parameter buffer, the contexts) */
static int mbx_fixed_map(struct mbx_vm *vm, struct drm_gem_object *obj, u32 va, u32 npages)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);
	struct sg_dma_page_iter it;
	u32 pg = va >> 12, end = pg + npages;

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

static int mbx_pb_map(struct mbx_vm *vm, struct mbx_device *m)
{
	int ret = mbx_fixed_map(vm, m->pb, MBX_PB_TABLE_VA, MBX_PB_TABLE_SIZE / MBX_PAGE + MBX_PB_PAGES);

	return ret ? ret : mbx_fixed_map(vm, m->ctx, MBX_CTX_VA, MBX_CTX_SIZE / MBX_PAGE);
}

struct mbx_vm *mbx_vm_create(struct mbx_device *m)
{
	struct mbx_vm *vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	int i, j;

	if (!vm)
		return ERR_PTR(-ENOMEM);
	kref_init(&vm->ref);
	vm->m = m;
	mutex_init(&vm->lock);
	INIT_LIST_HEAD(&vm->maps);
	/* buffers go below the contexts; the parameter buffer owns the top */
	drm_mm_init(&vm->mm, MBX_VM_START, MBX_CTX_VA - MBX_VM_START);
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

static void mbx_vm_release(struct kref *ref)
{
	struct mbx_vm *vm = container_of(ref, struct mbx_vm, ref);
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

struct mbx_vm *mbx_vm_get(struct mbx_vm *vm)
{
	kref_get(&vm->ref);
	return vm;
}

void mbx_vm_put(struct mbx_vm *vm)
{
	kref_put(&vm->ref, mbx_vm_release);
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
struct mbx_mapping *mbx_vm_map(struct mbx_vm *vm, struct drm_gem_object *obj, u32 align)
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
	ret = drm_mm_insert_node_generic(&vm->mm, &map->node, ALIGN(obj->size, MBX_PAGE), align, 0, 0);
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

/*
 * The stock daemon's MMU protocol (found during device testing), tried through module parameters:
 * mmu_seq 1: the enable handshake (clear bit 0 and wait for bit 16 to clear, set it and wait for bit 16);
 * tlb_mode 1: a TLB load (0x1024 the device page, 0x1028 the physical page, bit 8 of 0x1020, poll bit 24
 * clear) for every page of the client's buffers, the contexts and the free list's table and first pages;
 * 2: and for all the parameter buffer's pages.
 */
static int mmu_seq;
module_param(mmu_seq, int, 0644);
MODULE_PARM_DESC(mmu_seq, "debug: 1 does the stock daemon's MMU enable handshake");
static int tlb_mode;
module_param(tlb_mode, int, 0644);
MODULE_PARM_DESC(tlb_mode, "debug: 1 loads the TLB for the client's pages, 2 also for the whole parameter buffer");

static void mbx_tlb_load(struct mbx_vm *vm, u32 pg)
{
	struct mbx_device *m = vm->m;
	int i;

	mbx_write(m, MBX_REG_MMU_TLB_PAGE, pg);
	mbx_write(m, MBX_REG_MMU_TLB_PHYS, vm->pt[pg >> 10][pg & 1023] >> 12);
	mbx_write(m, MBX_REG_MMU_CTRL, BIT(8) | BIT(0));
	for (i = 0; i < 1000 && (mbx_read(m, MBX_REG_MMU_CTRL) & BIT(24)); i++)
		udelay(1);
}

static void mbx_tlb_load_range(struct mbx_vm *vm, u32 va, u32 bytes)
{
	u32 pg;

	for (pg = va >> 12; pg < (va + bytes) >> 12; pg++)
		mbx_tlb_load(vm, pg);
}

/* Make vm the MMU's address space; the TLB is flushed whenever it is written */
void mbx_vm_activate(struct mbx_vm *vm)
{
	struct mbx_device *m = vm->m;
	int i;

	for (i = 0; i < MBX_MMU_TABLES; i++)
		mbx_write(m, MBX_REG_MMU_TABLE(i), lower_32_bits(vm->pt_dma[i]));
	if (mmu_seq) {
		int k;

		mbx_write(m, MBX_REG_MMU_CTRL, 0);
		for (k = 0; k < 1000 && (mbx_read(m, MBX_REG_MMU_CTRL) & BIT(16)); k++)
			udelay(1);
		mbx_write(m, MBX_REG_MMU_CTRL, 1);
		for (k = 0; k < 1000 && !(mbx_read(m, MBX_REG_MMU_CTRL) & BIT(16)); k++)
			udelay(1);
	} else {
		mbx_write(m, MBX_REG_MMU_CTRL, 1);
	}
	if (tlb_mode) {
		struct mbx_mapping *map;

		mutex_lock(&vm->lock);
		list_for_each_entry(map, &vm->maps, link)
			mbx_tlb_load_range(vm, map->node.start, map->node.size);
		mutex_unlock(&vm->lock);
		mbx_tlb_load_range(vm, MBX_CTX_VA, MBX_CTX_SIZE);
		mbx_tlb_load_range(vm, MBX_PB_TABLE_VA, MBX_PB_TABLE_SIZE);
		mbx_tlb_load_range(vm, MBX_PB_VA, tlb_mode > 1 ? MBX_PB_PAGES * MBX_PAGE : 16 * MBX_PAGE);
	}
	m->active_vm = vm;
}

/* The buffer mapped where va is, with the mapping's lock held */
static struct mbx_mapping *containing_locked(struct mbx_vm *vm, u32 va)
{
	struct mbx_mapping *map;

	list_for_each_entry(map, &vm->maps, link)
		if (va >= map->node.start && va < map->node.start + map->node.size)
			return map;
	return NULL;
}

u32 mbx_vm_room(struct mbx_vm *vm, u32 va)
{
	struct mbx_mapping *map;
	u32 room = 0;

	mutex_lock(&vm->lock);
	map = containing_locked(vm, va);
	if (map)
		room = map->node.start + map->node.size - va;
	mutex_unlock(&vm->lock);
	return room;
}

int mbx_vm_cpu(struct mbx_vm *vm, u32 va, u32 len, void (*fn)(void *priv, u32 off, u32 *w, u32 n), void *priv)
{
	struct mbx_device *m = vm->m;
	struct mbx_mapping *map;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	u32 done = 0;
	int ret = 0;

	if ((va | len) & 3)
		return -EINVAL;
	mutex_lock(&vm->lock);
	map = containing_locked(vm, va);
	if (!map || len > map->node.start + map->node.size - va) {
		ret = -EINVAL;
		goto out;
	}
	shmem = to_drm_gem_shmem_obj(map->obj);
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto out;
	}
	dma_sync_sgtable_for_cpu(m->dev, sgt, DMA_BIDIRECTIONAL);
	while (done < len) {
		u32 pos = va - map->node.start + done, in_page = pos & (PAGE_SIZE - 1);
		u32 n = min_t(u32, len - done, PAGE_SIZE - in_page);
		void *p = kmap_local_page(shmem->pages[pos >> PAGE_SHIFT]);

		fn(priv, done, p + in_page, n / 4);
		kunmap_local(p);
		done += n;
	}
	dma_sync_sgtable_for_device(m->dev, sgt, DMA_BIDIRECTIONAL);
out:
	mutex_unlock(&vm->lock);
	return ret;
}
