/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MBX_DRV_H
#define MBX_DRV_H

#include <linux/clk.h>
#include <linux/io.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>

#define MBX_ABI_VERSION		1

struct mbx_device {
	struct drm_device drm;
	struct device *dev;
	void __iomem *regs;	/* the 64 KB window */
	void __iomem *prcm;	/* GFX domain's PRCM registers */
	struct clk *clks[3];	/* ick, 2d_fck, 3d_fck */
	int irq;
	u32 core_id, revision;
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

#endif
