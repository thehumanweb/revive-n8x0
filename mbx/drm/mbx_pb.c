// SPDX-License-Identifier: GPL-2.0-only
/*
 * The parameter buffer: memory the TA bins triangles into, and the free list
 * that hands its pages out (mbx/README.md assumptions 22 and 26). The object is
 * the free list's table followed by the pages, mapped at fixed addresses in
 * every address space (mbx_mmu.c); the list is built through whichever space
 * is active, once after each reset.
 */
#include <linux/capability.h>
#include <linux/module.h>
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
	shmem = drm_gem_shmem_create(&m->drm, MBX_CTX_SIZE);
	if (IS_ERR(shmem)) {
		drm_gem_object_put(m->pb);
		m->pb = NULL;
		return PTR_ERR(shmem);
	}
	m->ctx = &shmem->base;
	return 0;
}

void mbx_pb_destroy(struct mbx_device *m)
{
	if (m->ctx)
		drm_gem_object_put(m->ctx);
	m->ctx = NULL;
	if (m->pb)
		drm_gem_object_put(m->pb);
	m->pb = NULL;
}

/* The parameter buffer's pages, filled with end words before the free list is built (found during device testing) */
static void mbx_pb_fill(struct mbx_device *m)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->pb);
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);
	u32 i, j, first = MBX_PB_TABLE_SIZE >> PAGE_SHIFT;

	if (IS_ERR(sgt))
		return;
	for (i = 0; i < MBX_PB_PAGES; i++) {
		u32 *p = kmap_local_page(shmem->pages[first + i]);

		for (j = 0; j < PAGE_SIZE / 4; j++)
			p[j] = 0xf0000000;
		kunmap_local(p);
	}
	dma_sync_sgtable_for_device(m->dev, sgt, DMA_TO_DEVICE);
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
	mbx_pb_fill(m);
	/* the buffer's base page holds the background object: the list starts on the next */
	mbx_write(m, MBX_REG_FL_TABLE, MBX_PB_TABLE_VA);
	mbx_write(m, MBX_REG_FL_FIRST, MBX_PB_FIRST_PAGE + 1);
	mbx_write(m, MBX_REG_FL_LAST, MBX_PB_LAST_PAGE);
	mbx_write(m, MBX_REG_PB_BASE, MBX_PB_VA);
	mbx_write(m, MBX_REG_R_6D8, 0x09000000);
	mbx_write(m, MBX_REG_CE0, 1);
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

/* debug: bytes added to the background texture's address (to see that the chip reads it) */
static int bg_shift;
module_param(bg_shift, int, 0644);
MODULE_PARM_DESC(bg_shift, "debug: bytes added to the background texture's address");

/* The TA's tile grid and macro tile, from TA_CONFIG (the same as Mesa's and the model's) */
static u32 mt_size(u32 tiles)
{
	return tiles <= 32 ? 4 : tiles <= 64 ? 8 : 16;
}

static void grid(const struct drm_mbx_submit *a, u32 *tx, u32 *ty)
{
	*tx = (a->ta_config >> 20 & 0x3ff) + 1;
	*ty = (a->ta_config >> 10 & 0x3ff) + 1;
}

/* An IEEE single from a small unsigned integer (no floating point in the kernel) */
static u32 f32_from_u32(u32 v)
{
	u32 e;

	if (!v)
		return 0;
	e = fls(v) - 1;
	return (e + 127) << 23 | ((v << (23 - e)) & 0x7fffff);
}

/*
 * The TA and EVM contexts: zeroed, then loaded as the stock stack loads them before a
 * client's first TA (0x820 and 0x864 the two 16 KB halves of the object, 0x810 = 0,
 * 0x818 = 1; mbx/README.md assumption 31). Once after each reset.
 */
void mbx_ctx_load(struct mbx_device *m)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->ctx);
	struct sg_table *sgt;
	u32 i;

	if (m->ctx_loaded)
		return;
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt))
		return;
	for (i = 0; i < MBX_CTX_SIZE / PAGE_SIZE; i++) {
		void *p = kmap_local_page(shmem->pages[i]);

		memset(p, 0, PAGE_SIZE);
		kunmap_local(p);
	}
	dma_sync_sgtable_for_device(m->dev, sgt, DMA_TO_DEVICE);
	mbx_write(m, MBX_REG_TA_CTX, MBX_CTX_VA);
	mbx_write(m, MBX_REG_EVM_CTX, MBX_CTX_VA + 0x4000);
	mbx_write(m, MBX_REG_TA_CONTEXT_ID, 0);
	mbx_write(m, MBX_REG_TA_STORE, 1);
	m->ctx_loaded = true;
}

/*
 * The background object, the first 26 words of the parameter buffer (the chip reserves 132 for
 * it): a triangle twice the padded size, textured with the render target itself, 2048 x 2048
 * and clamped, so a tile starts from the pixels the target holds (README assumption 31).
 */
void mbx_pb_set_background(struct mbx_device *m, const struct drm_mbx_submit *a)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->pb);
	u32 tx, ty, mx, my, pad_w, pad_h, stride_px, i, *p;
	bool rgb565 = a->r_format == 9;
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);
	u32 o[26] = {
		0xe0000000, 0xa8800000, 0x0e000000, 0xd6887610, 0x22220e80,
		0, 0, 0, 0, 0, 0,
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
	};

	if (IS_ERR(sgt))
		return;
	grid(a, &tx, &ty);
	mx = mt_size(tx);
	my = mt_size(ty);
	pad_w = DIV_ROUND_UP(tx, mx) * mx * 16;
	pad_h = DIV_ROUND_UP(ty, my) * my * 16;
	stride_px = a->r_stride * (rgb565 ? 2 : 1);
	o[1] |= rgb565 ? 0x4000 : 0x18000;
	o[2] |= (stride_px / 16) << 18 | (a->r_addr + bg_shift) >> 7;
	o[7] = f32_from_u32(2 * pad_w);				/* the far corners */
	o[10] = f32_from_u32(2 * pad_h);
	o[21] = f32_from_u32(pad_w) - (10u << 23);		/* the texture coordinates: pad / 1024 */
	o[25] = f32_from_u32(pad_h) - (10u << 23);
	p = kmap_local_page(shmem->pages[MBX_PB_TABLE_SIZE >> PAGE_SHIFT]);
	for (i = 0; i < ARRAY_SIZE(o); i++)
		p[i] = o[i];
	kunmap_local(p);
	dma_sync_single_range_for_device(m->dev, sg_dma_address(sgt->sgl), MBX_PB_TABLE_SIZE, sizeof(o), DMA_TO_DEVICE);
}

/* The region array before the TA: 0x22000000 | the macro tile's origin, word 1 clear (assumption 31) */
struct region_init {
	u32 tx, ty, mx, my, nx;
};

static void region_init_fn(void *priv, u32 off, u32 *w, u32 n)
{
	struct region_init *r = priv;
	u32 k;

	for (k = 0; k < n; k++) {
		u32 word = off / 4 + k, entry = word / 2, macro = entry / (r->mx * r->my);

		w[k] = word & 1 ? 0 : 0x22000000 | ((macro / r->nx) * r->my) << 8 | (macro % r->nx) * r->mx;
	}
}

static void or_bit30_fn(void *priv, u32 off, u32 *w, u32 n)
{
	u32 k;

	for (k = 0; k < n; k++)
		if (!((off / 4 + k) & 1))
			w[k] |= 0x40000000;
}

static void zero_fn(void *priv, u32 off, u32 *w, u32 n)
{
	memset(w, 0, n * 4);
}

/* The region array's size in bytes for the job's tile grid: every entry of the padded grid, 8 bytes each */
u32 mbx_region_bytes(const struct drm_mbx_submit *a)
{
	u32 tx, ty, mx, my;

	grid(a, &tx, &ty);
	mx = mt_size(tx);
	my = mt_size(ty);
	return DIV_ROUND_UP(tx, mx) * mx * DIV_ROUND_UP(ty, my) * my * 8;
}

/* 0x860, tried in several encodings (module parameter tiles_mode) while the chip's is not known */
u32 mbx_tiles_value(const struct drm_mbx_submit *a, int mode)
{
	u32 tx, ty, mx, my, pad;

	grid(a, &tx, &ty);
	mx = mt_size(tx);
	my = mt_size(ty);
	pad = DIV_ROUND_UP(tx, mx) * mx * DIV_ROUND_UP(ty, my) * my;
	switch (mode) {
	case 1: return tx * ty;
	case 2: return pad;
	case 3: return tx * ty - 1;
	case 4: return a->ta_config & 0x3ffffc00;
	case 5: return pad - 1;
	case 6: return tx | ty << 16;
	case 7: return (tx - 1) | (ty - 1) << 16;
	}
	return 0;
}

/* The region array initialised and the tails zeroed: before the TA */
int mbx_job_prepare_buffers(struct mbx_vm *vm, const struct drm_mbx_submit *a)
{
	struct region_init r;
	int ret;

	grid(a, &r.tx, &r.ty);
	r.mx = mt_size(r.tx);
	r.my = mt_size(r.ty);
	r.nx = DIV_ROUND_UP(r.tx, r.mx);
	ret = mbx_vm_cpu(vm, a->ta_regions, mbx_region_bytes(a), region_init_fn, &r);
	if (ret)
		return ret;
	return mbx_vm_cpu(vm, a->ta_tails, 0x10000, zero_fn, NULL);
}

/* Bit 30 on every region entry: after the TA, before the render */
int mbx_job_region_bit30(struct mbx_vm *vm, const struct drm_mbx_submit *a)
{
	return mbx_vm_cpu(vm, a->ta_regions, mbx_region_bytes(a), or_bit30_fn, NULL);
}
