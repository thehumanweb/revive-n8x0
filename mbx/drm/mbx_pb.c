// SPDX-License-Identifier: GPL-2.0-only
/*
 * The parameter buffer: memory the TA bins triangles into, and the free list
 * that hands its pages out (mbx/README.md assumptions 22 and 26). The object is
 * the free list's table followed by the pages, mapped at fixed addresses in
 * every address space (mbx_mmu.c); the list is built through whichever space
 * is active, once after each reset.
 */
#include <linux/jiffies.h>
#include <linux/sizes.h>

#include <drm/drm_gem_shmem_helper.h>

#include "mbx_drv.h"

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
