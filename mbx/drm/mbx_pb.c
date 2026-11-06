// SPDX-License-Identifier: GPL-2.0-only
/*
 * The parameter buffer: memory the TA bins triangles into, and the free list
 * that hands its pages out (mbx/README.md assumptions 22 and 26). The object is
 * the free list's table followed by the pages, mapped at fixed addresses in
 * every address space (mbx_mmu.c); the list is built through whichever space
 * is active, once after each reset.
 */
#include <linux/capability.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/jiffies.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

#include <drm/drm_gem_shmem_helper.h>

#include "mbx_drv.h"
#include <drm/mbx_drm.h>

int mbx_pb_create(struct mbx_device *m)
{
	struct drm_gem_shmem_object *shmem;

	/* the table and the pages are contiguous in device addresses, so one object holds both */
	shmem = drm_gem_shmem_create(&m->drm, MBX_PB_TABLE_SIZE + (size_t)MBX_PB_PAGES * MBX_PAGE);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	shmem->map_wc = true;
	m->pb = &shmem->base;
	return 0;
}

void mbx_pb_destroy(struct mbx_device *m)
{
	if (m->pb)
		drm_gem_object_put(m->pb);
	m->pb = NULL;
}

/* Build the free list (the table, the first and last page, then the build) and wait for EVM_DALLOC */
int mbx_pb_build(struct mbx_device *m, unsigned long ms)
{
	unsigned long flags;

	if (m->pb_built)
		return 0;
	spin_lock_irqsave(&m->irq_lock, flags);
	m->pending &= ~MBX_INT_EVM_DALLOC;
	spin_unlock_irqrestore(&m->irq_lock, flags);
	mbx_write(m, MBX_REG_FL_TABLE, MBX_PB_TABLE_VA);
	mbx_write(m, MBX_REG_FL_FIRST, MBX_PB_FIRST_PAGE);
	mbx_write(m, MBX_REG_FL_LAST, MBX_PB_LAST_PAGE);
	mbx_write(m, MBX_REG_FL_BUILD, 1);
	if (!wait_event_timeout(m->wq, READ_ONCE(m->pending) & MBX_INT_EVM_DALLOC, msecs_to_jiffies(ms)))
		return -ETIMEDOUT;
	spin_lock_irqsave(&m->irq_lock, flags);
	m->pending &= ~MBX_INT_EVM_DALLOC;
	spin_unlock_irqrestore(&m->irq_lock, flags);
	m->pb_built = true;
	return 0;
}

/* DRM_IOCTL_MBX_PB_DUMP: a window of the parameter buffer, for comparing it with the model's */
int mbx_ioctl_pb_dump(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_device *m = to_mbx(dev);
	struct drm_mbx_pb_dump *d = data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->pb);
	size_t total = MBX_PB_TABLE_SIZE + (size_t)MBX_PB_PAGES * MBX_PAGE, done = 0;
	struct sg_table *sgt;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (d->offset > total || d->size > total - d->offset)
		return -EINVAL;
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);
	/* the chip wrote these pages; the CPU's lines for them are stale */
	mutex_lock(&m->job_lock);
	dma_sync_sgtable_for_cpu(m->dev, sgt, DMA_FROM_DEVICE);
	while (done < d->size) {
		size_t pos = d->offset + done, in_page = pos & (PAGE_SIZE - 1);
		size_t n = min_t(size_t, d->size - done, PAGE_SIZE - in_page);
		void *p = kmap_local_page(shmem->pages[pos >> PAGE_SHIFT]);
		unsigned long left = copy_to_user(u64_to_user_ptr(d->ptr) + done, p + in_page, n);

		kunmap_local(p);
		if (left) {
			mutex_unlock(&m->job_lock);
			return -EFAULT;
		}
		done += n;
	}
	mutex_unlock(&m->job_lock);
	return 0;
}

/*
 * The background object: the first words of the parameter buffer, which the render
 * starts every tile from (README assumption 6): word 1 and 2 texture words A and B,
 * here a linear texture of the colour target itself, so a tile with nothing in it
 * keeps the pixels the target already holds. Without a valid one the chip flushes
 * whatever its tile memory held.
 */
void mbx_pb_set_background(struct mbx_device *m, const struct drm_mbx_submit *a)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->pb);
	u32 w = (a->r_width >> 16 & 0x7ff) + 1, h = (a->r_height >> 16 & 0x7ff) + 1;
	u32 fmt = a->r_format == 9 ? 1 : 6;			/* RGB565, else ARGB8888 */
	u32 stride = a->r_stride * (fmt == 1 ? 2 : 1);		/* texels a row */
	u32 lw = max_t(u32, order_base_2(w), 3) - 3, lh = max_t(u32, order_base_2(h), 3) - 3;
	u32 field = DIV_ROUND_UP(stride, 8);			/* the stride in eights of texels */
	u32 word_a = lw << 24 | lh << 20 | fmt << 14 | (field & 1);
	u32 word_b = (field >> 1) << 18 | (a->r_addr >> 7);
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);
	u32 *p;

	if (IS_ERR(sgt))
		return;
	p = kmap_local_page(shmem->pages[MBX_PB_TABLE_SIZE >> PAGE_SHIFT]);
	p[0] = 0;
	p[1] = word_a;
	p[2] = word_b;
	kunmap_local(p);
	dma_sync_single_range_for_device(m->dev, sg_dma_address(sgt->sgl), MBX_PB_TABLE_SIZE, 16, DMA_TO_DEVICE);
}
