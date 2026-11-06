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

		for (k = 0; k < count; k++)
			writel(w[i + 1 + k], m->regs + port);
		i += 1 + count;
	}
}

static void mbx_reset_cores(struct mbx_device *m)
{
	mbx_write(m, MBX_REG_SOFT_RESET, 0x111);
	mbx_write(m, MBX_REG_SOFT_RESET, 0);
	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);
	m->active_vm = NULL;
	m->pb_built = false;	/* a reset empties the free list */
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

/* The frame, on the hardware: with the job lock held, in the job's address space */
static int mbx_job_run(struct mbx_job *job)
{
	struct mbx_device *m = job->m;
	struct drm_mbx_submit *a = &job->a;
	unsigned long ms = job->ms;
	int ret;

	mutex_lock(&m->job_lock);
	mbx_vm_activate(job->vm);
	spin_lock_irq(&m->irq_lock);
	m->pending = 0;
	spin_unlock_irq(&m->irq_lock);
	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);

	ret = mbx_pb_build(m, ms);
	if (ret) {
		dev_err(m->dev, "free list build failed (%d)\n", ret);
		goto reset;
	}
	mbx_write(m, MBX_REG_TA_CONFIG, a->ta_config);
	mbx_write(m, MBX_REG_TA_WIDTH, a->ta_width);
	mbx_write(m, MBX_REG_TA_HEIGHT, a->ta_height);
	mbx_write(m, MBX_REG_TA_REGIONS, a->ta_regions);
	mbx_write(m, MBX_REG_TA_TAILS, a->ta_tails);
	mbx_write(m, MBX_REG_TA_CONTEXT_ID, a->ta_context);
	mbx_write(m, MBX_REG_TA_START, 1);
	mbx_feed(m, job->stream, a->stream_words);
	ret = mbx_wait(m, MBX_INT_TA_DONE, ms);
	if (ret)
		goto reset;

	mbx_write(m, MBX_REG_R_REGIONS, a->r_regions);
	mbx_write(m, MBX_REG_R_CTL, a->r_ctl);
	mbx_write(m, MBX_REG_R_SAMPLING, a->r_sampling);
	mbx_write(m, MBX_REG_R_FOG, a->r_fog);
	mbx_write(m, MBX_REG_R_FORMAT, a->r_format);
	mbx_write(m, MBX_REG_R_WIDTH, a->r_width);
	mbx_write(m, MBX_REG_R_HEIGHT, a->r_height);
	mbx_write(m, MBX_REG_R_ADDR, a->r_addr);
	mbx_write(m, MBX_REG_R_STRIDE, a->r_stride);
	mbx_write(m, MBX_REG_R_ZBUF, a->r_zbuf);
	if (a->flags & MBX_SUBMIT_BG_TARGET) {
		mbx_pb_set_background(m, a);
		mbx_write(m, MBX_REG_PB_BASE, MBX_PB_VA);
	} else {
		mbx_write(m, MBX_REG_PB_BASE, a->r_pb);
	}
	mbx_write(m, MBX_REG_R_6DC, a->r_6dc);
	mbx_write(m, MBX_REG_R_HW_CONTEXT, a->ta_context);
	mbx_write(m, MBX_REG_R_KICK, 1);
	ret = mbx_wait(m, MBX_INT_RENDER, ms);
	if (ret)
		goto reset;
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

	if ((a->flags & ~MBX_SUBMIT_BG_TARGET) || a->stream_words > MBX_MAX_STREAM_WORDS || a->n_bos > MBX_MAX_BOS)
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
	ret = mbx_check_stream(job->stream, a->stream_words);
	if (ret)
		goto err;

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
