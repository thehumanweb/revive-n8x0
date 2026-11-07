// SPDX-License-Identifier: GPL-2.0-only
/*
 * A frame: set up the TA, start it, write the stream to its ports, wait for
 * the end word's TA_COMPLETE, set up the render, kick it, wait for
 * RENDER_COMPLETE. The ioctl only queues it: an ordered workqueue runs the jobs one
 * at a time, in submission order, each with a fence (a number; they complete in order)
 * that is also attached to the buffers it writes, so other users of those buffers (a
 * display flip) wait for it. The registers and what they mean are mbx/README.md's and
 * the model's.
 */
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mbx_drv.h"

#define MBX_MAX_STREAM_WORDS	(256u << 10)
#define MBX_DEFAULT_TIMEOUT_MS	2000
#define MBX_MAX_TIMEOUT_MS	10000

void mbx_job_irq(struct mbx_device *m, u32 status)
{
	unsigned long flags;

	spin_lock_irqsave(&m->irq_lock, flags);
	m->pending |= status;
	spin_unlock_irqrestore(&m->irq_lock, flags);
	wake_up(&m->wq);
}

/* Wait for one of bits; the TA's overflow and stream errors end the wait too */
static int mbx_wait(struct mbx_device *m, u32 bits, unsigned long ms)
{
	const u32 bad = MBX_INT_TA_OFLOW | MBX_INT_TA_STREAM;
	unsigned long flags;
	u32 got;

	if (!wait_event_timeout(m->wq, READ_ONCE(m->pending) & (bits | bad), msecs_to_jiffies(ms)))
		return -ETIMEDOUT;
	spin_lock_irqsave(&m->irq_lock, flags);
	got = m->pending;
	m->pending &= ~(bits | bad);
	spin_unlock_irqrestore(&m->irq_lock, flags);
	if (got & MBX_INT_TA_STREAM)
		return -EINVAL;
	if (got & MBX_INT_TA_OFLOW)
		return -ENOMEM;
	return 0;
}

/* Walk the chunks: every one whole and of a known type, or the stream is refused */
static int mbx_check_stream(const u32 *w, u32 n)
{
	u32 i = 0;

	while (i < n) {
		u32 type = w[i] >> 28, count = w[i] & 0xffff;

		if ((type != MBX_CHUNK_TA && type != MBX_CHUNK_CTL) || (w[i] & 0x0fff0000) ||
		    count > n - i - 1)
			return -EINVAL;
		i += 1 + count;
	}
	return 0;
}

static void mbx_feed(struct mbx_device *m, const u32 *w, u32 n)
{
	u32 i = 0;

	while (i < n) {
		u32 type = w[i] >> 28, count = w[i] & 0xffff, k;
		u32 port = type == MBX_CHUNK_TA ? MBX_TA_PORT : MBX_TACTL_PORT;

		for (k = 0; k < count; k++) {
			if (unlikely(mbx_trace))
				printk(KERN_DEBUG "mbx: %s %08x\n", type == MBX_CHUNK_TA ? "T" : "C", w[i + 1 + k]);
			writel(w[i + 1 + k], m->regs + port);
		}
		i += 1 + count;
	}
}

/* debug: 1 resets the cores sixteen times, interrupts off, 0x85c = 0x80, as the stock daemon's bring-up does */
static int reset16;
unsigned int ta_reset = 2;	/* 0: as before; 1: reset the TA context before each scene (0x81c); 2: and alternate the context id */
module_param(ta_reset, uint, 0644);
MODULE_PARM_DESC(ta_reset, "debug: 1 = reset the TA context (0x81c) before each scene, 2 = and alternate the hardware context id");
unsigned int kick_delay_us;
module_param(kick_delay_us, uint, 0644);
MODULE_PARM_DESC(kick_delay_us, "debug: wait this long after TA_COMPLETE before the render (the stock daemon waits about 2500)");
bool pb_every;
module_param(pb_every, bool, 0644);
bool ctx_every = true;	/* the stock daemon zeroes the EVM context before every scene (found during device testing) */
module_param(ctx_every, bool, 0644);
unsigned int tails_zero = 0x10000;
module_param(tails_zero, uint, 0644);
module_param(reset16, int, 0644);
MODULE_PARM_DESC(reset16, "debug: 1 resets the cores sixteen times as the stock daemon does");

static void mbx_reset_cores(struct mbx_device *m)
{
	int i;

	if (reset16) {
		mbx_write(m, MBX_REG_INT_ENABLE, 0);
		mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);
		mbx_write(m, MBX_REG_TA_CONFIG, 0x80);
		for (i = 0; i < 16; i++) {
			mbx_write(m, MBX_REG_SOFT_RESET, 0x111);
			mbx_write(m, MBX_REG_SOFT_RESET, 0);
		}
		mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);
		mbx_write(m, MBX_REG_INT_ENABLE, MBX_INT_MASTER | MBX_INT_RENDER | MBX_INT_TA_DONE | MBX_INT_TA_OFLOW |
			  MBX_INT_TA_STREAM | MBX_INT_EVM_DALLOC);
	}
	mbx_write(m, MBX_REG_SOFT_RESET, 0x111);
	mbx_write(m, MBX_REG_SOFT_RESET, 0);
	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);
	m->active_vm = NULL;
	m->pb_built = false;	/* a reset empties the free list */
	m->ctx_loaded = false;	/* and the contexts */
}


#define MBX_MAX_BOS	64

struct mbx_job {
	struct work_struct work;
	struct mbx_device *m;
	struct mbx_vm *vm;			/* a reference: the job outlives its file */
	struct drm_mbx_submit a;
	u32 *stream;
	unsigned long ms;
	struct drm_gem_object **bos;		/* references, put when the job is done */
	u32 nbos;
	struct dma_fence **deps;		/* what to wait for before running */
	u32 ndeps;
	struct dma_fence *done;
};

static const char *mbx_fence_driver_name(struct dma_fence *f)
{
	return "mbx";
}

static const char *mbx_fence_timeline_name(struct dma_fence *f)
{
	return "mbx-render";
}

static const struct dma_fence_ops mbx_fence_ops = {
	.get_driver_name = mbx_fence_driver_name,
	.get_timeline_name = mbx_fence_timeline_name,
};

/* debug: the render registers of unknown meaning and the format, changeable at run time (defaults: the stock values) */
static unsigned int r60c, r614 = 0x1e3ce508, r618 = 0x1e3ce508, r644, r648 = 1, r64c, r6d0 = 1;
static int r650_override = -1;
module_param(r60c, uint, 0644);
module_param(r614, uint, 0644);
module_param(r618, uint, 0644);
module_param(r644, uint, 0644);
module_param(r648, uint, 0644);
module_param(r64c, uint, 0644);
module_param(r6d0, uint, 0644);
module_param(r650_override, int, 0644);
MODULE_PARM_DESC(r650_override, "debug: -1 keeps the target's format in 0x650, else this value");

/* debug: bits ORed into 0x85c (the stock stack sets bit 18, 0x40000, when the target has no external Z) */
static unsigned int ta_extra;
module_param(ta_extra, uint, 0644);
MODULE_PARM_DESC(ta_extra, "debug: bits ORed into TA_CONFIG (0x85c)");

static bool prof;
module_param(prof, bool, 0644);
MODULE_PARM_DESC(prof, "print the average time of each phase of a job every two seconds");

/* Time spent in each phase of the jobs since the last print (the prof parameter) */
static struct {
	u64 n, build, ta, bit30, render, started;
	u64 vm, ctx, bg, region;	/* the build phase's parts */
} pstat;

static void mbx_prof_add(u64 build, u64 ta, u64 bit30, u64 render, u64 vm, u64 ctx, u64 bg, u64 region)
{
	u64 now = ktime_get_ns();

	if (!pstat.started)
		pstat.started = now;
	pstat.n++;
	pstat.build += build;
	pstat.ta += ta;
	pstat.bit30 += bit30;
	pstat.render += render;
	pstat.vm += vm;
	pstat.ctx += ctx;
	pstat.bg += bg;
	pstat.region += region;
	if (now - pstat.started >= 2 * NSEC_PER_SEC) {
		u64 n = pstat.n;

		pr_info("mbx prof: %llu jobs in 2 s; per job: build+region init %llu us (address space %llu, free list+contexts %llu, background %llu, region array and tails %llu), TA (feed and wait) %llu us, bit 30 pass %llu us, render %llu us\n",
			n, div64_u64(pstat.build, n * 1000), div64_u64(pstat.vm, n * 1000), div64_u64(pstat.ctx, n * 1000),
			div64_u64(pstat.bg, n * 1000), div64_u64(pstat.region, n * 1000), div64_u64(pstat.ta, n * 1000),
			div64_u64(pstat.bit30, n * 1000), div64_u64(pstat.render, n * 1000));
		memset(&pstat, 0, sizeof(pstat));
	}
}

/* The frame, on the hardware: with the job lock held, in the job's address space */
static int mbx_job_run(struct mbx_job *job)
{
	struct mbx_device *m = job->m;
	struct drm_mbx_submit *a = &job->a;
	unsigned long ms = job->ms;
	ktime_t t_ta = 0, t_render = 0;
	u64 p0 = 0, p1 = 0, p2 = 0, p3 = 0, pv = 0, pc = 0, pb = 0, pr = 0;
	u32 ctx, i;
	int ret;

	mutex_lock(&m->job_lock);
	if (prof)
		p0 = ktime_get_ns();
	if (m->active_vm != job->vm || job->vm->dirty)	/* the MMU takes the address space again when it changed (another client, new or removed mappings) */
		mbx_vm_activate(job->vm);
	if (prof)
		pv = ktime_get_ns();
	spin_lock_irq(&m->irq_lock);
	m->pending = 0;
	spin_unlock_irq(&m->irq_lock);
	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);

	ret = mbx_pb_build(m, ms);
	if (ret) {
		dev_err(m->dev, "free list build failed (%d)\n", ret);
		goto reset;
	}
	mbx_ctx_load(m);
	if (prof)
		pc = ktime_get_ns();
	mbx_pb_set_background(m, a);
	if (prof)
		pb = ktime_get_ns();
	ret = mbx_job_prepare_buffers(job->vm, a);
	if (ret)
		goto reset;
	mbx_write(m, MBX_REG_TA_854, 0x3727c5ac);
	mbx_write(m, MBX_REG_TA_858, 0x3727c5ac);
	mbx_write(m, MBX_REG_TA_CONFIG, a->ta_config | 0xe1 | ta_extra);	/* the stock stack's base bits */
	mbx_write(m, MBX_REG_TA_WIDTH, a->ta_width);
	mbx_write(m, MBX_REG_TA_HEIGHT, a->ta_height);
	mbx_write(m, MBX_REG_TA_REGIONS, a->ta_regions);
	mbx_write(m, MBX_REG_TA_TAILS, a->ta_tails);
	ctx = ta_reset == 2 ? (m->ctx_seq++ & 1) : a->ta_context;
	mbx_write(m, MBX_REG_TA_CONTEXT_ID, ctx);
	if (ta_reset) {
		/* the stock services' reset_context: 0x810 = the context id, 0x81c = 1, wait for TA_CONTEXT, clear it */
		mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_TA_CONTEXT);
		mbx_write(m, MBX_REG_TA_RESET, 1);
		for (i = 0; i < 100 && !(mbx_read(m, MBX_REG_INT_STATUS) & MBX_INT_TA_CONTEXT); i++)
			udelay(3);	/* a few microseconds: look before sleeping */
		for (i = 0; i < 500; i++) {
			if (mbx_read(m, MBX_REG_INT_STATUS) & MBX_INT_TA_CONTEXT)
				break;
			usleep_range(100, 200);
		}
		mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_TA_CONTEXT);
		if (unlikely(mbx_trace))
			printk(KERN_DEBUG "mbx: S after the TA context reset (%u polls) %08x\n", i, mbx_read(m, MBX_REG_INT_STATUS));
	}
	if (prof) {
		p1 = ktime_get_ns();
		pr = p1 - pb;
	}
	t_ta = ktime_get();
	mbx_write(m, MBX_REG_TA_START, 1);
	mbx_feed(m, job->stream, a->stream_words);
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: S after the stream %08x\n", mbx_read(m, MBX_REG_INT_STATUS));
	ret = mbx_wait(m, MBX_INT_TA_DONE, ms);
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: S after TA_DONE %08x (%lld us after the TA start)\n", mbx_read(m, MBX_REG_INT_STATUS),
		       ktime_to_us(ktime_sub(ktime_get(), t_ta)));
	if (ret)
		goto reset;
	if (kick_delay_us)
		usleep_range(kick_delay_us, kick_delay_us + 200);

	if (prof)
		p2 = ktime_get_ns();
	ret = mbx_job_region_bit30(job->vm, a, a->flags & MBX_SUBMIT_ZLOAD ? 0x20000000 : 0);
	if (ret)
		goto reset;
	if (prof)
		p3 = ktime_get_ns();
	{
		u32 tx, ty;

		mbx_grid(a, &tx, &ty);
		/* the render registers whose meaning is not known (README assumption 31), as the stock stack writes them */
		mbx_write(m, 0x60c, r60c);
		mbx_write(m, 0x614, r614);
		mbx_write(m, 0x618, r618);
		mbx_write(m, 0x644, r644);
		mbx_write(m, 0x648, r648);
		mbx_write(m, 0x64c, r64c);
		mbx_write(m, 0x6d0, r6d0);
		mbx_write(m, MBX_REG_TILES, (ty - 1) << 16 | (tx - 1));	/* the tile grid again, as the TA has it */
	}
	mbx_write(m, MBX_REG_R_REGIONS, a->r_regions);
	mbx_write(m, MBX_REG_R_CTL, a->r_ctl);
	mbx_write(m, MBX_REG_R_SAMPLING, a->r_sampling);
	mbx_write(m, MBX_REG_R_FOG, a->r_fog);
	mbx_write(m, MBX_REG_R_FORMAT, r650_override >= 0 ? (u32)r650_override : a->r_format);
	mbx_write(m, MBX_REG_R_WIDTH, a->r_width);
	mbx_write(m, MBX_REG_R_HEIGHT, a->r_height);
	mbx_write(m, MBX_REG_R_ADDR, a->r_addr);
	mbx_write(m, MBX_REG_R_STRIDE, a->r_stride);
	mbx_write(m, MBX_REG_R_ZBUF, a->r_zbuf);
	mbx_write(m, MBX_REG_PB_BASE, MBX_PB_VA);
	mbx_write(m, MBX_REG_R_6DC, a->r_6dc);
	mbx_write(m, MBX_REG_R_HW_CONTEXT, ctx);
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: R before the kick: 0860 %08x 0614 %08x 0618 %08x 0854 %08x 085c %08x 0610 %08x\n",
		       mbx_read(m, MBX_REG_TILES), mbx_read(m, 0x614), mbx_read(m, 0x618), mbx_read(m, MBX_REG_TA_854),
		       mbx_read(m, MBX_REG_TA_CONFIG), mbx_read(m, MBX_REG_R_CTL));
	t_render = ktime_get();
	mbx_write(m, MBX_REG_R_KICK, 1);
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: S after the kick %08x\n", mbx_read(m, MBX_REG_INT_STATUS));
	ret = mbx_wait(m, MBX_INT_RENDER, ms);
	if (unlikely(mbx_trace))
		printk(KERN_DEBUG "mbx: S after RENDER %08x (%lld us after the kick)\n", mbx_read(m, MBX_REG_INT_STATUS),
		       ktime_to_us(ktime_sub(ktime_get(), t_render)));
	if (ret)
		goto reset;
	if (prof)
		mbx_prof_add(p1 - p0, p2 - p1, p3 - p2, ktime_get_ns() - p3, pv - p0, pc - pv, pb - pc, pr);
	mutex_unlock(&m->job_lock);
	return 0;
reset:
	dev_err(m->dev, "job failed (%d): resetting the cores\n", ret);
	mbx_reset_cores(m);
	mutex_unlock(&m->job_lock);
	return ret;
}

static void mbx_job_free(struct mbx_job *job)
{
	u32 i;

	for (i = 0; i < job->nbos; i++)
		drm_gem_object_put(job->bos[i]);
	for (i = 0; i < job->ndeps; i++)
		dma_fence_put(job->deps[i]);
	kfree(job->bos);
	kfree(job->deps);
	kvfree(job->stream);
	dma_fence_put(job->done);
	mbx_vm_put(job->vm);	/* last: the space may go with it */
	kfree(job);
}

static void mbx_job_work(struct work_struct *work)
{
	struct mbx_job *job = container_of(work, struct mbx_job, work);
	struct mbx_device *m = job->m;
	u64 seqno = job->done->seqno;
	unsigned long flags;
	u32 i;
	int ret;

	/* what others did to the buffers must be finished first */
	for (i = 0; i < job->ndeps; i++)
		dma_fence_wait(job->deps[i], false);
	ret = mbx_job_run(job);
	if (ret)
		dma_fence_set_error(job->done, ret);
	spin_lock_irqsave(&m->fence_lock, flags);
	m->status[seqno % ARRAY_SIZE(m->status)] = ret ? 1 : 0;
	spin_unlock_irqrestore(&m->fence_lock, flags);
	atomic64_set(&m->completed, seqno);
	dma_fence_signal(job->done);
	wake_up_all(&m->fence_wq);
	mbx_job_free(job);
}

/* The fences of what others did to a buffer: writers, and readers too if this job writes */
static int mbx_job_add_deps(struct mbx_job *job, struct drm_gem_object *obj, bool write)
{
	struct dma_fence **fences;
	unsigned int n, i;
	int ret;

	ret = dma_resv_get_fences(obj->resv, write ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE, &n, &fences);
	if (ret)
		return ret;
	if (n) {
		struct dma_fence **d = krealloc_array(job->deps, job->ndeps + n, sizeof(*d), GFP_KERNEL);

		if (!d) {
			for (i = 0; i < n; i++)
				dma_fence_put(fences[i]);
			kfree(fences);
			return -ENOMEM;
		}
		job->deps = d;
		for (i = 0; i < n; i++)
			job->deps[job->ndeps++] = fences[i];	/* the references move */
	}
	kfree(fences);
	return 0;
}

int mbx_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_device *m = to_mbx(dev);
	struct mbx_file *f = file->driver_priv;
	struct drm_mbx_submit *a = data;
	struct drm_mbx_submit_bo *ubos = NULL;
	struct mbx_job *job;
	struct dma_fence *fence;
	u32 i;
	int ret;

	if ((a->flags & ~(MBX_SUBMIT_FLAT_BG | MBX_SUBMIT_ZLOAD)) || a->stream_words > MBX_MAX_STREAM_WORDS || a->n_bos > MBX_MAX_BOS)
		return -EINVAL;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	INIT_WORK(&job->work, mbx_job_work);
	job->m = m;
	job->a = *a;
	job->ms = a->timeout_ms ? min_t(u32, a->timeout_ms, MBX_MAX_TIMEOUT_MS) : MBX_DEFAULT_TIMEOUT_MS;
	job->vm = mbx_vm_get(f->vm);

	job->stream = vmemdup_user(u64_to_user_ptr(a->stream), (size_t)a->stream_words * 4);
	if (IS_ERR(job->stream)) {
		ret = PTR_ERR(job->stream);
		job->stream = NULL;
		goto err;
	}
	if (unlikely(mbx_trace)) {
		u32 k;

		printk(KERN_DEBUG "mbx: submit: ta cfg %08x w %08x h %08x regions %08x tails %08x ctx %u | r regions %08x ctl %08x samp %08x fog %08x fmt %08x w %08x h %08x addr %08x stride %u zbuf %08x 6dc %08x | words %u bos %u\n",
		       a->ta_config, a->ta_width, a->ta_height, a->ta_regions, a->ta_tails, a->ta_context, a->r_regions, a->r_ctl,
		       a->r_sampling, a->r_fog, a->r_format, a->r_width, a->r_height, a->r_addr, a->r_stride, a->r_zbuf, a->r_6dc,
		       a->stream_words, a->n_bos);
		for (k = 0; k < a->stream_words; k += 8)
			printk(KERN_DEBUG "mbx: stream[%u]: %08x %08x %08x %08x %08x %08x %08x %08x\n", k,
			       job->stream[k], k + 1 < a->stream_words ? job->stream[k + 1] : 0, k + 2 < a->stream_words ? job->stream[k + 2] : 0,
			       k + 3 < a->stream_words ? job->stream[k + 3] : 0, k + 4 < a->stream_words ? job->stream[k + 4] : 0,
			       k + 5 < a->stream_words ? job->stream[k + 5] : 0, k + 6 < a->stream_words ? job->stream[k + 6] : 0,
			       k + 7 < a->stream_words ? job->stream[k + 7] : 0);
	}
	ret = mbx_check_stream(job->stream, a->stream_words);
	if (ret)
		goto err;
	/* the buffers the contract needs (mbx/README.md assumption 31): the tail pointers 64 KB aligned and
	 * 64 KB, room for the region array, a target row the background texture can address */
	if ((a->ta_tails & 0xffff) || mbx_vm_room(f->vm, a->ta_tails) < tails_zero ||
	    mbx_vm_room(f->vm, a->ta_regions) < mbx_region_bytes(a) ||
	    ((a->r_stride * (a->r_format == 9 ? 2 : 1)) & 15)) {
		ret = -EINVAL;
		goto err;
	}

	if (a->n_bos) {
		ubos = memdup_user(u64_to_user_ptr(a->bos), (size_t)a->n_bos * sizeof(*ubos));
		if (IS_ERR(ubos)) {
			ret = PTR_ERR(ubos);
			ubos = NULL;
			goto err;
		}
		job->bos = kcalloc(a->n_bos, sizeof(*job->bos), GFP_KERNEL);
		if (!job->bos) {
			ret = -ENOMEM;
			goto err;
		}
		for (i = 0; i < a->n_bos; i++) {
			job->bos[i] = drm_gem_object_lookup(file, ubos[i].handle);
			if (!job->bos[i]) {
				ret = -ENOENT;
				goto err;
			}
			job->nbos = i + 1;
			ret = mbx_job_add_deps(job, job->bos[i], ubos[i].flags & MBX_SUBMIT_BO_WRITE);
			if (ret)
				goto err;
		}
	}

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		ret = -ENOMEM;
		goto err;
	}
	mutex_lock(&m->submit_lock);
	dma_fence_init(fence, &mbx_fence_ops, &m->fence_lock, m->fence_ctx, atomic64_inc_return(&m->next_seqno));
	job->done = fence;
	/* the fence goes on the buffers before the job can run: later users wait for it */
	for (i = 0; i < job->nbos; i++) {
		struct dma_resv *resv = job->bos[i]->resv;

		dma_resv_lock(resv, NULL);
		if (!dma_resv_reserve_fences(resv, 1))
			dma_resv_add_fence(resv, fence, (ubos[i].flags & MBX_SUBMIT_BO_WRITE) ?
					   DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
		dma_resv_unlock(resv);
	}
	a->fence = fence->seqno;
	queue_work(m->job_wq, &job->work);
	mutex_unlock(&m->submit_lock);
	kfree(ubos);
	return 0;
err:
	kfree(ubos);
	mbx_job_free(job);
	return ret;
}

int mbx_ioctl_wait_fence(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_device *m = to_mbx(dev);
	struct drm_mbx_wait_fence *a = data;
	long ret;

	if (a->fence > (u64)atomic64_read(&m->next_seqno))
		return -EINVAL;
	ret = wait_event_interruptible_timeout(m->fence_wq, (u64)atomic64_read(&m->completed) >= a->fence,
					       nsecs_to_jiffies(min_t(u64, a->timeout_ns, 60ull * NSEC_PER_SEC)));
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;
	/* the last 64 jobs' outcomes are kept */
	if ((u64)atomic64_read(&m->completed) - a->fence < ARRAY_SIZE(m->status) &&
	    m->status[a->fence % ARRAY_SIZE(m->status)])
		return -EIO;
	return 0;
}

void mbx_flush_jobs(struct mbx_device *m)
{
	if (m->job_wq)
		flush_workqueue(m->job_wq);
}

void mbx_job_fini(struct mbx_device *m)
{
	if (m->job_wq) {
		destroy_workqueue(m->job_wq);	/* runs what is queued first */
		m->job_wq = NULL;
	}
}

static void mbx_job_fini_action(void *data)
{
	mbx_job_fini(data);
}

int mbx_job_init(struct mbx_device *m)
{
	mutex_init(&m->submit_lock);
	spin_lock_init(&m->fence_lock);
	init_waitqueue_head(&m->fence_wq);
	atomic64_set(&m->next_seqno, 0);
	atomic64_set(&m->completed, 0);
	m->fence_ctx = dma_fence_context_alloc(1);
	m->job_wq = alloc_ordered_workqueue("mbx", WQ_MEM_RECLAIM);
	if (!m->job_wq)
		return -ENOMEM;
	return devm_add_action_or_reset(m->dev, mbx_job_fini_action, m);
}
