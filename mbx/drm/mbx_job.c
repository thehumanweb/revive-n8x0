// SPDX-License-Identifier: GPL-2.0-only
/*
 * A frame: set up the TA, start it, write the stream to its ports, wait for
 * the end word's TA_COMPLETE, set up the render, kick it, wait for
 * RENDER_COMPLETE. One at a time; the ioctl blocks until the render is done
 * (a fence and an asynchronous queue come later). The registers and what they
 * mean are mbx/README.md's and the model's.
 */
#include <linux/jiffies.h>
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

int mbx_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mbx_device *m = to_mbx(dev);
	struct mbx_file *f = file->driver_priv;
	struct drm_mbx_submit *a = data;
	unsigned long ms;
	u32 *w;
	int ret;

	if (a->flags || a->stream_words > MBX_MAX_STREAM_WORDS)
		return -EINVAL;
	w = vmemdup_user(u64_to_user_ptr(a->stream), (size_t)a->stream_words * 4);
	if (IS_ERR(w))
		return PTR_ERR(w);
	ret = mbx_check_stream(w, a->stream_words);
	if (ret)
		goto out;
	ms = a->timeout_ms ? min_t(u32, a->timeout_ms, MBX_MAX_TIMEOUT_MS) : MBX_DEFAULT_TIMEOUT_MS;

	mutex_lock(&m->job_lock);
	mbx_vm_activate(f->vm);
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
	mbx_feed(m, w, a->stream_words);
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
	mbx_write(m, MBX_REG_PB_BASE, a->r_pb);
	mbx_write(m, MBX_REG_R_6DC, a->r_6dc);
	mbx_write(m, MBX_REG_R_HW_CONTEXT, a->ta_context);
	mbx_write(m, MBX_REG_R_KICK, 1);
	ret = mbx_wait(m, MBX_INT_RENDER, ms);
	if (ret)
		goto reset;
	mutex_unlock(&m->job_lock);
	goto out;
reset:
	dev_err(m->dev, "job failed (%d): resetting the cores\n", ret);
	mbx_reset_cores(m);
	mutex_unlock(&m->job_lock);
out:
	kvfree(w);
	return ret;
}
