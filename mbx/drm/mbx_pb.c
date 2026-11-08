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
#include <linux/delay.h>
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
	shmem = drm_gem_shmem_create(&m->drm, (size_t)MBX_PB_PAGES * MBX_PAGE);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	shmem->map_wc = true;
	m->pb = &shmem->base;
	shmem = drm_gem_shmem_create(&m->drm, MBX_PB_TABLE_SIZE);
	if (IS_ERR(shmem)) {
		drm_gem_object_put(m->pb);
		m->pb = NULL;
		return PTR_ERR(shmem);
	}
	m->tbl = &shmem->base;
	shmem = drm_gem_shmem_create(&m->drm, MBX_CTX_SIZE);
	if (IS_ERR(shmem)) {
		drm_gem_object_put(m->tbl);
		drm_gem_object_put(m->pb);
		m->tbl = m->pb = NULL;
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
	if (m->tbl)
		drm_gem_object_put(m->tbl);
	m->tbl = NULL;
	if (m->pb)
		drm_gem_object_put(m->pb);
	m->pb = NULL;
}

/* The parameter buffer's pages, filled with end words before the free list is built (found during device testing) */
static void mbx_pb_fill(struct mbx_device *m)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(m->pb);
	struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);
	u32 i, j, first = 0;

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

	/*
	 * Built once, and again every pb_rebuild jobs (and after a reset): the model gives back all but the shared page of a
	 * scene, a page a job; the chip is not known to lose any, but a rebuild between scenes (each starts with fresh
	 * contexts) costs a few milliseconds and ends any such leak.
	 */
	if (m->pb_built && !pb_every && !(pb_rebuild && ++m->pb_jobs >= pb_rebuild))
		return 0;
	m->pb_jobs = 0;
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

/* DRM_IOCTL_MBX_PB_DUMP: a window of the table (16 KB) and then the parameter buffer's pages, for comparing them with the model's */
int mbx_ioctl_pb_dump(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_device *m = to_mbx(dev);
	struct drm_mbx_pb_dump *d = data;
	size_t total = MBX_PB_TABLE_SIZE + (size_t)MBX_PB_PAGES * MBX_PAGE, done = 0;
	struct sg_table *sgt_t, *sgt_p;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (d->offset > total || d->size > total - d->offset)
		return -EINVAL;
	sgt_t = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(m->tbl));
	sgt_p = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(m->pb));
	if (IS_ERR(sgt_t) || IS_ERR(sgt_p))
		return -ENOMEM;
	/* the chip wrote these pages; the CPU's lines for them are stale */
	mutex_lock(&m->job_lock);
	dma_sync_sgtable_for_cpu(m->dev, sgt_t, DMA_FROM_DEVICE);
	dma_sync_sgtable_for_cpu(m->dev, sgt_p, DMA_FROM_DEVICE);
	while (done < d->size) {
		size_t pos = d->offset + done, in_page = pos & (PAGE_SIZE - 1);
		size_t n = min_t(size_t, d->size - done, PAGE_SIZE - in_page);
		struct drm_gem_shmem_object *shmem = pos < MBX_PB_TABLE_SIZE ? to_drm_gem_shmem_obj(m->tbl) :
						     to_drm_gem_shmem_obj(m->pb);
		size_t idx = (pos < MBX_PB_TABLE_SIZE ? pos : pos - MBX_PB_TABLE_SIZE) >> PAGE_SHIFT;
		void *p = kmap_local_page(shmem->pages[idx]);
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

/* debug: 1 flips the background triangle's winding (v1 and v2 swapped, with their texture coordinates) */
static int bg_mode;
module_param(bg_mode, int, 0644);
MODULE_PARM_DESC(bg_mode, "debug: 1 flips the background triangle's winding");

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

	if (m->ctx_loaded && !ctx_every)
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
	/* as the stock daemon does: clear TA_CONTEXT, store, poll for it up to 50 ms, clear it again */
	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_TA_CONTEXT);
	mbx_write(m, MBX_REG_TA_STORE, 1);
	/* the store is done in a few microseconds: look for it before sleeping (a sleep costs 100 us and its wake-up) */
	for (i = 0; i < 100 && !(mbx_read(m, MBX_REG_INT_STATUS) & MBX_INT_TA_CONTEXT); i++)
		udelay(3);
	for (i = 0; i < 500; i++) {
		if (mbx_read(m, MBX_REG_INT_STATUS) & MBX_INT_TA_CONTEXT)
			break;
		usleep_range(100, 200);
	}
	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_TA_CONTEXT);
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: S after the context store (%u polls) %08x\n", i, mbx_read(m, MBX_REG_INT_STATUS));
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
	if (bg_mode == 1) {
		/* v1 = (0, 2 pad_h), v2 = (2 pad_w, 0): positions at words 7-10, u at 20-22, v at 23-25 */
		o[7] = 0;
		o[8] = f32_from_u32(2 * pad_h);
		o[9] = f32_from_u32(2 * pad_w);
		o[10] = 0;
		o[21] = 0;
		o[22] = f32_from_u32(pad_w) - (10u << 23);
		o[24] = f32_from_u32(pad_h) - (10u << 23);
		o[25] = 0;
	}
	p = kmap_local_page(shmem->pages[0]);
	if (a->flags & MBX_SUBMIT_FLAT_BG) {
		/* the frame starts with a clear: the texture is 16 x 8 texels of the clear colour in the page's
		 * unused part (the buffer's first page holds only the background object), 8 x 8 read with clamping, and
		 * every texture coordinate is zero (README assumption 44) */
		u32 *tex = p + MBX_FLAT_BG_OFFSET / 4;

		o[1] = 0xa0000000 | (rgb565 ? 0x4000 : 0x18000);
		o[2] = 0x0e000000 | (16 / 16) << 18 | (MBX_PB_VA + MBX_FLAT_BG_OFFSET) >> 7;
		o[21] = 0;
		o[25] = 0;
		for (i = 0; i < (rgb565 ? 16 * 8 / 2 : 16 * 8); i++)
			tex[i] = rgb565 ? (a->bg_colour & 0xffff) * 0x10001u : a->bg_colour;
	}
	for (i = 0; i < ARRAY_SIZE(o); i++)
		p[i] = o[i];
	kunmap_local(p);
	dma_sync_single_range_for_device(m->dev, sg_dma_address(sgt->sgl), 0, MBX_FLAT_BG_OFFSET + 512, DMA_TO_DEVICE);
}

/* The region array before the TA: 0x22000000 | the macro tile's origin, word 1 clear (assumption 31) */
struct region_init {
	u32 tx, ty, mx, my, nx;
	u32 shift;		/* log2 of the entries in a macro tile: 16, 64 or 256 */
	u32 macro_last, word0;	/* the macro tile whose entry word was last worked out (its divisions are not repeated for each of its entries) */
};

static void region_init_fn(void *priv, u32 off, u32 *w, u32 n)
{
	struct region_init *r = priv;
	u32 k;

	for (k = 0; k < n; k++) {
		u32 word = off / 4 + k, macro = (word >> 1) >> r->shift;

		if (word & 1) {
			w[k] = 0;
			continue;
		}
		if (macro != r->macro_last) {	/* the ARM11 has no divide instruction: once a macro tile, not once a word */
			r->macro_last = macro;
			r->word0 = 0x22000000 | ((macro / r->nx) * r->my) << 8 | (macro % r->nx) * r->mx;
		}
		w[k] = r->word0;
	}
}

struct bit30_arg {
	u32 extra;
	u32 damage;	/* packed tile rectangle (mbx_drm.h), 0: all */
};

static void or_bit30_fn(void *priv, u32 off, u32 *w, u32 n)
{
	const struct bit30_arg *a = priv;
	u32 k;

	for (k = 0; k < n; k++) {
		if ((off / 4 + k) & 1)
			continue;
		w[k] |= 0x40000000 | a->extra;
		if (a->damage) {
			u32 tx = w[k] & 0xff, ty = w[k] >> 8 & 0xff;

			/* a tile outside the damage is not rendered and keeps its pixels: bit 25 on an entry that has a list drops it
			 * (measured, README assumption 50; the entry that ends the array is rendered whatever its bits) */
			if (tx < (a->damage & 0xff) || ty < (a->damage >> 8 & 0xff) || tx >= (a->damage >> 16 & 0xff) ||
			    ty >= (a->damage >> 24 & 0xff))
				w[k] |= 0x02000000;
		}
	}
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

void mbx_grid(const struct drm_mbx_submit *a, u32 *tx, u32 *ty)
{
	grid(a, tx, ty);
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
	r.macro_last = ~0u;
	r.shift = __ffs(r.mx * r.my);
	ret = mbx_vm_cpu(vm, a->ta_regions, mbx_region_bytes(a), false, region_init_fn, &r);
	if (ret)
		return ret;
	return mbx_vm_cpu(vm, a->ta_tails, tails_zero, false, zero_fn, NULL);
}

/* Bit 30 on every region entry: after the TA, before the render */
int mbx_job_region_bit30(struct mbx_vm *vm, const struct drm_mbx_submit *a, u32 extra)
{
	struct bit30_arg arg = { .extra = extra, .damage = a->damage };

	return mbx_vm_cpu(vm, a->ta_regions, mbx_region_bytes(a), true, or_bit30_fn, &arg);
}
