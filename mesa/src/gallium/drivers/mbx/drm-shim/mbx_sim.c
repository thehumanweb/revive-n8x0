// SPDX-License-Identifier: MIT
/*
 * A drm-shim backend for the mbx driver: the DRM render node's ioctls (the
 * uapi of mbx/drm/mbx_drm.h) implemented in userspace on top of the emulator
 * core, so the same DRM winsys and Mesa driver run on the host.
 *
 * It does what the kernel driver does: an address space a DRM file through
 * eight page tables of physical page addresses, buffers mapped into it,
 * and a submit that sets up the TA, feeds the stream, kicks the render. Physical
 * memory is the shim's memfd (a buffer's mem_addr is its physical address).
 * The emulator is synchronous, so a submit is done when it returns.
 *
 * Use: LD_PRELOAD=.../libmbx_sim_drm_shim.so; see DRM_SHIM_DEBUG=1.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "drm-uapi/mbx_drm.h"
#include "drm-shim/drm_shim.h"
#include "util/log.h"
#include "util/vma.h"

#include "emu/mbxemu.h"
#include "emu/mbx_contract.h"

bool drm_shim_driver_prefers_first_render_node = true;

/* registers (mbx/emu.h) */
#define R_SOFT_RESET	0x080
#define R_INT_STATUS	0x12c
#define R_INT_ENABLE	0x130
#define R_INT_CLEAR	0x134
#define R_REGIONS	0x608
#define R_RENDER_CTL	0x610
#define R_SAMPLING	0x61c
#define R_FOG_COLOUR	0x63c
#define R_FORMAT	0x650
#define R_WIDTH		0x654
#define R_HEIGHT	0x658
#define R_ADDR		0x65c
#define R_STRIDE	0x660
#define R_ZBUF		0x674
#define R_KICK		0x680
#define R_6DC		0x6dc
#define R_HW_CONTEXT	0x6ec
#define R_TA_START	0x800
#define R_TA_CONTEXT_ID	0x810
#define R_PB_BASE	0x83c
#define R_TA_TAILS	0x840
#define R_TA_REGIONS	0x844
#define R_TA_WIDTH	0x84c
#define R_TA_HEIGHT	0x850
#define R_TA_CONFIG	0x85c
#define R_MMU_TABLE(i)	(0x1000 + 4 * (i))
#define R_MMU_CTRL	0x1020

#define I_RENDER	0x4
#define I_TA_DONE	0x10
#define I_TA_OFLOW	0x20
#define I_TA_STREAM	0x200
#define I_MASTER	0x8000

#define R_FL_TABLE	0x824
#define R_FL_FIRST	0x828
#define R_FL_LAST	0x82c
#define R_FL_BUILD	0x838
#define I_EVM_DALLOC	0x40

#define VM_START	0x10000u
#define VM_SIZE		(32u << 20)
#define PAGE		4096u

/* The parameter buffer, as the kernel driver lays it out (mbx/drm/mbx_drv.h): the free list's
 * table then 1024 pages at the top of the address space, the same pages in every space */
#define PB_PAGES	1024u
#define PB_LAST_PAGE	((VM_SIZE >> 12) - 1)
#define PB_FIRST_PAGE	(PB_LAST_PAGE + 1 - PB_PAGES)
#define PB_TABLE_SIZE	((VM_SIZE >> 12) * 2)
#define PB_VA		(PB_FIRST_PAGE << 12)
#define PB_TABLE_VA	(PB_VA - PB_TABLE_SIZE)
#define CTX_SIZE	0x8000u			/* the TA and EVM contexts, padded: 16 KB each */
#define CTX_VA		(PB_TABLE_VA - CTX_SIZE)

struct sim_map {
	struct sim_map *next;
	struct shim_bo *bo;
	uint32_t va, pages;
};

struct sim_fd {
	struct sim_fd *next;
	struct shim_fd *shim_fd;
	struct shim_bo *pt[8];
	struct util_vma_heap heap;
	struct sim_map *maps;
};

static struct {
	struct mbxemu *emu;
	uint8_t *mem;		/* the shim's memfd: physical memory */
	struct shim_bo dummy;
	struct shim_bo pb;	/* the free list's table, then the parameter buffer's pages */
	struct shim_bo ctx;	/* the TA and EVM contexts */
	uint64_t seqno;		/* the last fence handed out: jobs run to completion inside the ioctl */
	bool pb_built;
	struct sim_fd *fds;
	mtx_t lock;
	struct sim_fd *active;
} sim = { .lock = { 0 } };

static void sim_mem_read(void *o, uint32_t phys, void *buf, uint32_t len)
{
	memcpy(buf, sim.mem + phys, len);
}

static void sim_mem_write(void *o, uint32_t phys, const void *buf, uint32_t len)
{
	memcpy(sim.mem + phys, buf, len);
}

static void sim_irq(void *o, int level) { }

static void sim_log(void *o, const char *line)
{
	mesa_loge("mbxemu: %s", line);
}

static void sim_init(void)
{
	struct mbxemu_host host = { .mem_read = sim_mem_read, .mem_write = sim_mem_write,
				    .irq = sim_irq, .log = sim_log };

	if (sim.emu)
		return;
	mtx_init(&sim.lock, mtx_plain);
	/* the whole memfd, sparse: a buffer's mem_addr is its physical address */
	sim.mem = mmap(NULL, 4ull << 30, PROT_READ | PROT_WRITE, MAP_SHARED, shim_device.mem_fd, 0);
	if (sim.mem == MAP_FAILED) {
		mesa_loge("mbx sim: cannot map the shim memory");
		abort();
	}
	drm_shim_bo_init(&sim.dummy, PAGE);
	drm_shim_bo_init(&sim.pb, PB_TABLE_SIZE + (size_t)PB_PAGES * PAGE);
	drm_shim_bo_init(&sim.ctx, CTX_SIZE);
	sim.emu = mbxemu_new(&host);
	if (getenv("MBX_SIM_TRACE"))
		mbxemu_set_trace(sim.emu, atoi(getenv("MBX_SIM_TRACE")));
}

static struct sim_fd *sim_fd_get(int fd)
{
	struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
	struct sim_fd *f;
	int i, j;

	for (f = sim.fds; f; f = f->next)
		if (f->shim_fd == shim_fd)
			return f;
	f = calloc(1, sizeof(*f));
	f->shim_fd = shim_fd;
	util_vma_heap_init(&f->heap, VM_START, CTX_VA - VM_START);
	for (i = 0; i < 8; i++) {
		uint32_t *pt;

		f->pt[i] = calloc(1, sizeof(*f->pt[i]));
		drm_shim_bo_init(f->pt[i], PAGE);
		pt = (uint32_t *)(sim.mem + f->pt[i]->mem_addr);
		for (j = 0; j < 1024; j++)
			pt[j] = (uint32_t)sim.dummy.mem_addr;
	}
	/* the parameter buffer at its fixed addresses */
	for (j = 0; j < (int)(PB_TABLE_SIZE / PAGE + PB_PAGES); j++) {
		uint32_t pg = (PB_TABLE_VA >> 12) + j;

		((uint32_t *)(sim.mem + f->pt[pg >> 10]->mem_addr))[pg & 1023] = (uint32_t)(sim.pb.mem_addr + (uint64_t)j * PAGE);
	}
	/* the contexts, just below the free list's table */
	for (j = 0; j < (int)(CTX_SIZE / PAGE); j++) {
		uint32_t pg = (CTX_VA >> 12) + j;

		((uint32_t *)(sim.mem + f->pt[pg >> 10]->mem_addr))[pg & 1023] = (uint32_t)(sim.ctx.mem_addr + (uint64_t)j * PAGE);
	}
	f->next = sim.fds;
	sim.fds = f;
	return f;
}

static int ioctl_get_param(int fd, unsigned long request, void *arg)
{
	struct drm_mbx_get_param *p = arg;

	switch (p->param) {
	case MBX_PARAM_CORE_ID: p->value = mbxemu_reg_read(sim.emu, 0xf00); return 0;
	case MBX_PARAM_REVISION: p->value = mbxemu_reg_read(sim.emu, 0xf10); return 0;
	case MBX_PARAM_ABI: p->value = MBX_ABI_VERSION; return 0;
	}
	return -EINVAL;
}

static int ioctl_bo_create(int fd, unsigned long request, void *arg)
{
	struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
	struct drm_mbx_bo_create *c = arg;
	struct shim_bo *bo;

	if (c->flags || !c->size)
		return -EINVAL;
	bo = calloc(1, sizeof(*bo));
	if (drm_shim_bo_init(bo, (c->size + PAGE - 1) & ~(uint64_t)(PAGE - 1))) {
		free(bo);
		return -ENOMEM;
	}
	c->handle = drm_shim_bo_get_handle(shim_fd, bo);
	drm_shim_bo_put(bo);
	return 0;
}

static struct sim_map *find_map(struct sim_fd *f, struct shim_bo *bo)
{
	struct sim_map *m;

	for (m = f->maps; m; m = m->next)
		if (m->bo == bo)
			return m;
	return NULL;
}

static int ioctl_bo_info(int fd, unsigned long request, void *arg)
{
	struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
	struct drm_mbx_bo_info *i = arg;
	struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, i->handle);
	struct sim_fd *f;
	struct sim_map *m;
	int ret = 0;

	if (!bo)
		return -ENOENT;
	if (i->flags & ~MBX_BO_INFO_ALIGN_64K) {
		drm_shim_bo_put(bo);
		return -EINVAL;
	}
	mtx_lock(&sim.lock);
	f = sim_fd_get(fd);
	m = find_map(f, bo);
	if (!m) {
		uint32_t pg, k;
		uint64_t va = util_vma_heap_alloc(&f->heap, bo->size, i->flags & MBX_BO_INFO_ALIGN_64K ? 0x10000 : PAGE);

		if (!va) {
			ret = -ENOSPC;
			goto out;
		}
		m = calloc(1, sizeof(*m));
		m->bo = bo;
		m->va = (uint32_t)va;
		m->pages = bo->size / PAGE;
		pg = m->va >> 12;
		for (k = 0; k < m->pages; k++, pg++)
			((uint32_t *)(sim.mem + f->pt[pg >> 10]->mem_addr))[pg & 1023] =
				(uint32_t)(bo->mem_addr + (uint64_t)k * PAGE);
		drm_shim_bo_get(bo);	/* held while mapped */
		m->next = f->maps;
		f->maps = m;
	}
	i->size = bo->size;
	i->offset = drm_shim_bo_get_mmap_offset(shim_fd, bo);
	i->va = m->va;
out:
	mtx_unlock(&sim.lock);
	drm_shim_bo_put(bo);
	return ret;
}

static int ioctl_bo_unmap(int fd, unsigned long request, void *arg)
{
	struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
	struct drm_mbx_bo_unmap *u = arg;
	struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, u->handle);
	struct sim_fd *f;
	struct sim_map **pp, *m;
	int ret = -ENOENT;

	if (!bo)
		return -ENOENT;
	mtx_lock(&sim.lock);
	f = sim_fd_get(fd);
	for (pp = &f->maps; (m = *pp); pp = &m->next) {
		if (m->bo == bo) {
			uint32_t pg = m->va >> 12, k;

			for (k = 0; k < m->pages; k++, pg++)
				((uint32_t *)(sim.mem + f->pt[pg >> 10]->mem_addr))[pg & 1023] =
					(uint32_t)sim.dummy.mem_addr;
			util_vma_heap_free(&f->heap, m->va, (uint64_t)m->pages * PAGE);
			*pp = m->next;
			drm_shim_bo_put(m->bo);
			free(m);
			ret = 0;
			break;
		}
	}
	mtx_unlock(&sim.lock);
	drm_shim_bo_put(bo);
	return ret;
}

static void rw(uint32_t reg, uint32_t v) { mbxemu_reg_write(sim.emu, reg, v); }

/* device memory through the active file's page tables, for the contract's steps (mbx_contract.h) */
static uint32_t *sim_dev_ptr(uint32_t va)
{
	uint32_t pg = va >> 12;
	uint32_t phys = ((uint32_t *)(sim.mem + sim.active->pt[pg >> 10]->mem_addr))[pg & 1023];

	return (uint32_t *)(sim.mem + phys + (va & (PAGE - 1)));
}
static void sim_c_wr32(void *o, uint32_t va, uint32_t v) { *sim_dev_ptr(va) = v; }
static uint32_t sim_c_rd32(void *o, uint32_t va) { return *sim_dev_ptr(va); }
static void sim_c_reg(void *o, uint32_t reg, uint32_t v) { rw(reg, v); }
static const struct mbx_io SIM_IO = { .wr32 = sim_c_wr32, .rd32 = sim_c_rd32, .reg = sim_c_reg };

/* the mapping a device address lies in, and how far it goes: what a client's buffer holds */
static uint32_t sim_map_room(struct sim_fd *f, uint32_t va)
{
	struct sim_map *m;

	for (m = f->maps; m; m = m->next)
		if (va >= m->va && va < m->va + m->pages * PAGE)
			return m->va + m->pages * PAGE - va;
	return 0;
}

static int ioctl_submit(int fd, unsigned long request, void *arg)
{
	struct drm_mbx_submit *a = arg;
	const uint32_t *w = (const uint32_t *)(uintptr_t)a->stream;
	struct sim_fd *f;
	uint32_t i, st;
	int ret = 0;

	if (a->flags || a->stream_words > (256u << 10) || a->n_bos > 64)
		return -EINVAL;
	/* chunks whole and of a known type, as the kernel checks them */
	for (i = 0; i < a->stream_words;) {
		uint32_t type = w[i] >> 28, count = w[i] & 0xffff;

		if ((type != MBX_CHUNK_TA && type != MBX_CHUNK_CTL) || (w[i] & 0x0fff0000) ||
		    count > a->stream_words - i - 1)
			return -EINVAL;
		i += 1 + count;
	}
	mtx_lock(&sim.lock);
	f = sim_fd_get(fd);
	/* the contract's buffers: the tails 64 KB aligned and 64 KB, the regions big enough */
	{
		uint32_t tx = (a->ta_config >> 20 & 0x3ff) + 1, ty = (a->ta_config >> 10 & 0x3ff) + 1;
		uint32_t stride_px = a->r_stride * (a->r_format == 9 ? 2 : 1);

		if ((a->ta_tails & 0xffff) || sim_map_room(f, a->ta_tails) < 0x10000 ||
		    sim_map_room(f, a->ta_regions) < mbx_region_entries(tx, ty) * 8 || (stride_px & 15)) {
			mtx_unlock(&sim.lock);
			return -EINVAL;
		}
	}
	for (i = 0; i < 8; i++)
		rw(R_MMU_TABLE(i), (uint32_t)f->pt[i]->mem_addr);
	rw(R_MMU_CTRL, 1);
	sim.active = f;
	rw(R_INT_ENABLE, I_MASTER | I_RENDER | I_TA_DONE | I_TA_OFLOW | I_TA_STREAM);
	rw(R_INT_CLEAR, 0x7fff);

	if (!sim.pb_built) {
		mbx_contract_pb_fill(&SIM_IO, PB_VA, PB_PAGES);
		mbx_contract_freelist(&SIM_IO, PB_TABLE_VA, PB_VA, PB_LAST_PAGE);
		rw(R_FL_BUILD, 1);
		if (!(mbxemu_reg_read(sim.emu, R_INT_STATUS) & I_EVM_DALLOC)) {
			ret = -ETIMEDOUT;
			goto reset;
		}
		rw(R_INT_CLEAR, I_EVM_DALLOC);
		mbx_contract_contexts(&SIM_IO, CTX_VA);	/* once, before the first TA after a reset */
		sim.pb_built = true;
	}
	{
		uint32_t tx = (a->ta_config >> 20 & 0x3ff) + 1, ty = (a->ta_config >> 10 & 0x3ff) + 1;

		/* the background object, the region array and the tails, for this frame */
		mbx_contract_background(&SIM_IO, PB_VA, a->r_addr, a->r_stride * (a->r_format == 9 ? 2 : 1), tx, ty,
					a->r_format == 9);
		mbx_contract_regions(&SIM_IO, a->ta_regions, tx, ty);
		mbx_contract_tails(&SIM_IO, a->ta_tails, 0x10000);
	}
	mbx_contract_ta_setup(&SIM_IO);
	rw(R_TA_CONFIG, a->ta_config | 0xe1);	/* the stock stack's base bits, as the kernel driver */
	rw(R_TA_WIDTH, a->ta_width);
	rw(R_TA_HEIGHT, a->ta_height);
	rw(R_TA_REGIONS, a->ta_regions);
	rw(R_TA_TAILS, a->ta_tails);
	rw(R_TA_CONTEXT_ID, a->ta_context);
	rw(R_TA_START, 1);
	for (i = 0; i < a->stream_words;) {
		uint32_t type = w[i] >> 28, count = w[i] & 0xffff, k;

		for (k = 0; k < count; k++) {
			if (type == MBX_CHUNK_CTL)
				mbxemu_ta_ctl_write(sim.emu, w[i + 1 + k]);
			else
				mbxemu_ta_write(sim.emu, 0, w[i + 1 + k]);
		}
		i += 1 + count;
	}
	st = mbxemu_reg_read(sim.emu, R_INT_STATUS);
	if (st & I_TA_STREAM) { ret = -EINVAL; goto reset; }
	if (st & I_TA_OFLOW) { ret = -ENOMEM; goto reset; }
	if (!(st & I_TA_DONE)) { ret = -ETIMEDOUT; goto reset; }

	rw(R_REGIONS, a->r_regions);
	rw(R_RENDER_CTL, a->r_ctl);
	rw(R_SAMPLING, a->r_sampling);
	rw(R_FOG_COLOUR, a->r_fog);
	rw(R_FORMAT, a->r_format);
	rw(R_WIDTH, a->r_width);
	rw(R_HEIGHT, a->r_height);
	rw(R_ADDR, a->r_addr);
	rw(R_STRIDE, a->r_stride);
	rw(R_ZBUF, a->r_zbuf);
	rw(R_PB_BASE, PB_VA);
	{
		uint32_t tx = (a->ta_config >> 20 & 0x3ff) + 1, ty = (a->ta_config >> 10 & 0x3ff) + 1;

		mbx_contract_render_regs(&SIM_IO, tx, ty);
		mbx_contract_prerender(&SIM_IO, a->ta_regions, tx, ty);	/* bit 30 on every region entry */
	}
	rw(R_6DC, a->r_6dc);
	rw(R_HW_CONTEXT, a->ta_context);
	rw(R_KICK, 1);
	st = mbxemu_reg_read(sim.emu, R_INT_STATUS);
	if (!(st & I_RENDER)) { ret = -ETIMEDOUT; goto reset; }
	goto out;
reset:
	mesa_loge("mbx sim: job failed (%d), resetting", ret);
	rw(R_SOFT_RESET, 0x111);
	rw(R_SOFT_RESET, 0);
	sim.pb_built = false;
out:
	a->fence = ++sim.seqno;
	mtx_unlock(&sim.lock);
	return ret;
}

/* the sim runs a job inside SUBMIT, so every fence is done */
static int ioctl_wait_fence(int fd, unsigned long request, void *arg)
{
	struct drm_mbx_wait_fence *w = arg;

	return w->fence > sim.seqno ? -EINVAL : 0;
}

/* the parameter buffer's bytes, as the kernel driver's ioctl gives them */
static int ioctl_pb_dump(int fd, unsigned long request, void *arg)
{
	struct drm_mbx_pb_dump *d = arg;
	size_t total = PB_TABLE_SIZE + (size_t)PB_PAGES * PAGE;

	if (d->offset > total || d->size > total - d->offset)
		return -EINVAL;
	memcpy((void *)(uintptr_t)d->ptr, sim.mem + sim.pb.mem_addr + d->offset, d->size);
	return 0;
}

/* the emulator's registers, as the driver's ioctl gives the chip's */
static int ioctl_reg_dump(int fd, unsigned long request, void *arg)
{
	struct drm_mbx_reg_dump *d = arg;
	uint32_t i, v;

	if ((d->offset | d->size) & 3 || d->offset > 0x4000 || d->size > 0x4000 - d->offset)
		return -EINVAL;
	for (i = 0; i < d->size; i += 4) {
		v = mbxemu_reg_read(sim.emu, d->offset + i);
		memcpy((uint8_t *)(uintptr_t)d->ptr + i, &v, 4);
	}
	return 0;
}

static ioctl_fn_t driver_ioctls[] = {
	[DRM_MBX_GET_PARAM] = ioctl_get_param,
	[DRM_MBX_BO_CREATE] = ioctl_bo_create,
	[DRM_MBX_BO_INFO] = ioctl_bo_info,
	[DRM_MBX_BO_UNMAP] = ioctl_bo_unmap,
	[DRM_MBX_SUBMIT] = ioctl_submit,
	[DRM_MBX_WAIT_FENCE] = ioctl_wait_fence,
	[DRM_MBX_PB_DUMP] = ioctl_pb_dump,
	[DRM_MBX_REG_DUMP] = ioctl_reg_dump,
};

void drm_shim_driver_init(void)
{
	shim_device.bus_type = DRM_BUS_PLATFORM;
	shim_device.driver_name = "mbx";
	shim_device.driver_ioctls = driver_ioctls;
	shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);
	sim_init();

	drm_shim_override_file("OF_FULLNAME=/ocp/gpu@50000000\n"
			       "OF_COMPATIBLE_N=1\n"
			       "OF_COMPATIBLE_0=ti,omap2420-mbx\n",
			       "/sys/dev/char/%d:%d/device/uevent", DRM_MAJOR, render_node_minor);
}
