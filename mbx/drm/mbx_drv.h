/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MBX_DRV_H
#define MBX_DRV_H

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_mm.h>
#include <drm/mbx_drm.h>

#include "mbx_regs.h"

#define MBX_ABI_VERSION		2

/* An address space: 32 MB of 4 KB pages through eight page tables */
#define MBX_VM_SIZE		(32u << 20)
#define MBX_VM_START		0x10000		/* the first 64 KB stay unmapped */
#define MBX_PAGE		4096

struct mbx_device;

struct mbx_mapping {
	struct list_head link;
	struct drm_mm_node node;
	struct drm_gem_object *obj;	/* held while mapped */
};

struct mbx_vm {
	struct mbx_device *m;
	struct drm_mm mm;
	struct mutex lock;		/* mm, maps, the page tables' entries */
	u32 *pt[MBX_MMU_TABLES];
	dma_addr_t pt_dma[MBX_MMU_TABLES];
	struct list_head maps;
};

struct mbx_file {
	struct mbx_vm *vm;
};

struct mbx_device {
	struct drm_device drm;
	struct device *dev;
	void __iomem *regs;	/* the 64 KB window */
	void __iomem *prcm;	/* GFX domain's PRCM registers */
	struct clk *clks[3];	/* ick, 2d_fck, 3d_fck */
	int irq;
	u32 core_id, revision;

	void *dummy;		/* what unmapped pages point at */
	dma_addr_t dummy_dma;

	struct mutex job_lock;	/* one frame at a time; the MMU's current space */
	struct mbx_vm *active_vm;
	spinlock_t irq_lock;
	wait_queue_head_t wq;
	u32 pending;		/* interrupt bits seen, not yet consumed */
};

#define to_mbx(d) container_of(d, struct mbx_device, drm)

static inline u32 mbx_read(struct mbx_device *m, u32 reg)
{
	return readl(m->regs + reg);
}

static inline void mbx_write(struct mbx_device *m, u32 reg, u32 v)
{
	writel(v, m->regs + reg);
}

/* mbx_mmu.c */
struct mbx_vm *mbx_vm_create(struct mbx_device *m);
void mbx_vm_destroy(struct mbx_vm *vm);
struct mbx_mapping *mbx_vm_map(struct mbx_vm *vm, struct drm_gem_object *obj);
int mbx_vm_unmap(struct mbx_vm *vm, struct drm_gem_object *obj);
void mbx_vm_activate(struct mbx_vm *vm);	/* with job_lock held */

/* mbx_gem.c */
int mbx_ioctl_bo_create(struct drm_device *dev, void *data, struct drm_file *file);
int mbx_ioctl_bo_info(struct drm_device *dev, void *data, struct drm_file *file);
int mbx_ioctl_bo_unmap(struct drm_device *dev, void *data, struct drm_file *file);

/* mbx_job.c */
int mbx_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);
void mbx_job_irq(struct mbx_device *m, u32 status);

#endif
