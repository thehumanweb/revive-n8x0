/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MBX_DRV_H
#define MBX_DRV_H

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/workqueue.h>
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

/* An address space: 32 MB of 4 KB pages through eight page tables */
#define MBX_VM_SIZE		(32u << 20)
#define MBX_VM_START		0x810000	/* clients' buffers go in the second arena, above the table and contexts at 0x800000 */
#define MBX_PAGE		4096

/*
 * The parameter buffer: the TA bins its triangles into pages it takes off a free list the driver
 * builds. Page p is at device address p << 12. As the stock stack has it (found during device
 * testing), the buffer is at device address 0 (its first page the background object's), in the first
 * arena (0-8 MB); the free list's table and the contexts are at the start of the second (0x800000),
 * clients' buffers after them. A list item's pointer is a word offset from the buffer's base (0x83c),
 * the region entries' list addresses absolute: at base 0 the two coincide. The same pages are mapped
 * in every file's space. One buffer serves all jobs because they run one at a time and a render
 * gives its pages back.
 */
#define MBX_PB_PAGES		768			/* 3 MB */
#define MBX_PB_FIRST_PAGE	0
#define MBX_PB_LAST_PAGE	(MBX_PB_FIRST_PAGE + MBX_PB_PAGES - 1)
#define MBX_PB_VA		(MBX_PB_FIRST_PAGE << 12)
#define MBX_PB_TABLE_SIZE	0x4000			/* 16 KB, 16 KB aligned */
#define MBX_PB_TABLE_VA		0x800000
/*
 * The TA and EVM contexts, zeroed and padded (the chip writes past their nominal sizes: a page,
 * 520 bytes): 16 KB each, in one object mapped in every space after the free list's table.
 */
#define MBX_CTX_SIZE		0x8000u
#define MBX_CTX_VA		(MBX_PB_TABLE_VA + MBX_PB_TABLE_SIZE)

struct mbx_device;

struct mbx_mapping {
	struct list_head link;
	struct drm_mm_node node;
	struct drm_gem_object *obj;	/* held while mapped */
};

struct mbx_vm {
	struct kref ref;		/* the file's, and each queued job's */
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

	struct drm_gem_object *pb;	/* the parameter buffer's pages */
	struct drm_gem_object *tbl;	/* the free list's table */
	struct drm_gem_object *ctx;	/* the TA and EVM contexts */
	bool pb_built;		/* the free list is built (not after a reset) */
	bool ctx_loaded;	/* the contexts are zeroed and loaded (not after a reset) */
	void *dummy;		/* what unmapped pages point at */
	dma_addr_t dummy_dma;

	struct mutex job_lock;	/* one frame at a time; the MMU's current space */
	struct mbx_vm *active_vm;
	spinlock_t irq_lock;
	wait_queue_head_t wq;
	u32 pending;		/* interrupt bits seen, not yet consumed */

	/* queued jobs: one at a time, in order; each has a fence (its number) */
	struct workqueue_struct *job_wq;
	struct mutex submit_lock;	/* numbering and queueing in the same order */
	spinlock_t fence_lock;
	u64 fence_ctx;
	atomic64_t next_seqno, completed;
	wait_queue_head_t fence_wq;
	u8 status[64];		/* by fence number: 1 if the job failed (the last 64) */
};

#define to_mbx(d) container_of(d, struct mbx_device, drm)

static inline u32 mbx_read(struct mbx_device *m, u32 reg)
{
	return readl(m->regs + reg);
}

extern bool mbx_trace;	/* debug: log every register write and TA word (KERN_DEBUG, in dmesg) */

static inline void mbx_write(struct mbx_device *m, u32 reg, u32 v)
{
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: W %04x %08x\n", reg, v);
	writel(v, m->regs + reg);
}

/* mbx_mmu.c */
struct mbx_vm *mbx_vm_create(struct mbx_device *m);
struct mbx_vm *mbx_vm_get(struct mbx_vm *vm);
void mbx_vm_put(struct mbx_vm *vm);
struct mbx_mapping *mbx_vm_map(struct mbx_vm *vm, struct drm_gem_object *obj, u32 align);
/* bytes from device address va to the end of the buffer mapped there: 0 if none */
u32 mbx_vm_room(struct mbx_vm *vm, u32 va);
/* the CPU's access to len bytes at va in a buffer of the space, in runs of whole words */
int mbx_vm_cpu(struct mbx_vm *vm, u32 va, u32 len, void (*fn)(void *priv, u32 off, u32 *w, u32 n), void *priv);
int mbx_vm_unmap(struct mbx_vm *vm, struct drm_gem_object *obj);
void mbx_vm_activate(struct mbx_vm *vm);	/* with job_lock held */
int mbx_pb_create(struct mbx_device *m);
void mbx_pb_destroy(struct mbx_device *m);
void mbx_ctx_load(struct mbx_device *m);	/* with the job's space active */
int mbx_pb_build(struct mbx_device *m, unsigned long ms);	/* with the job's space active */
void mbx_pb_set_background(struct mbx_device *m, const struct drm_mbx_submit *a);
u32 mbx_region_bytes(const struct drm_mbx_submit *a);
void mbx_grid(const struct drm_mbx_submit *a, u32 *tx, u32 *ty);	/* the tile grid from TA_CONFIG */
int mbx_job_prepare_buffers(struct mbx_vm *vm, const struct drm_mbx_submit *a);	/* the region array and the tails */
int mbx_job_region_bit30(struct mbx_vm *vm, const struct drm_mbx_submit *a);
int mbx_ioctl_pb_dump(struct drm_device *dev, void *data, struct drm_file *file);

/* mbx_gem.c */
int mbx_ioctl_bo_create(struct drm_device *dev, void *data, struct drm_file *file);
int mbx_ioctl_bo_info(struct drm_device *dev, void *data, struct drm_file *file);
int mbx_ioctl_bo_unmap(struct drm_device *dev, void *data, struct drm_file *file);

/* mbx_job.c */
int mbx_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);
int mbx_ioctl_wait_fence(struct drm_device *dev, void *data, struct drm_file *file);
int mbx_job_init(struct mbx_device *m);
void mbx_job_fini(struct mbx_device *m);
void mbx_flush_jobs(struct mbx_device *m);	/* before something a queued job may use goes away */
void mbx_job_irq(struct mbx_device *m, u32 status);

#endif
