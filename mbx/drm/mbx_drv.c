// SPDX-License-Identifier: GPL-2.0-only
/*
 * PowerVR MBX (OMAP2420) DRM render node: platform driver, power and reset,
 * interrupt. GEM, the MBX's address spaces and job submission follow.
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>

#include <drm/mbx_drm.h>
#include "mbx_drv.h"
#include "mbx_regs.h"

static const char *const mbx_clk_names[] = { "ick", "2d_fck", "3d_fck" };

/*
 * Power up as TI's EnableSystemClocks() does. Order matters: an access to
 * the MBX before its clocks run and its reset is pulsed leaves the
 * interconnect answering every later access with an external abort until
 * the next boot.
 */
static int mbx_power_on(struct mbx_device *m)
{
	struct clk *l3;
	u32 v;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(m->clks); i++) {
		ret = clk_prepare_enable(m->clks[i]);
		if (ret) {
			dev_err(m->dev, "clock %s: %d\n", mbx_clk_names[i], ret);
			goto err;
		}
	}
	/* 2D and 3D cores at L3 / 2, as TI's and Nokia's settings */
	l3 = clk_get(NULL, "core_l3_ck");
	if (!IS_ERR(l3)) {
		for (i = 1; i < ARRAY_SIZE(m->clks); i++)
			clk_set_rate(m->clks[i], clk_get_rate(l3) / 2);
		clk_put(l3);
	}
	if (!(readl(m->prcm + GFX_PRCM_CLKCFG_STATUS) & 1))
		writel(1, m->prcm + GFX_PRCM_CLKCFG_CTRL);
	/* pm24xx forces the GFX domain off: power on, pulse the reset */
	writel(0, m->prcm + GFX_PM_PWSTCTRL);
	writel(GFX_RM_RSTST_RST, m->prcm + GFX_RM_RSTST);
	writel(1, m->prcm + GFX_RM_RSTCTRL);
	writel(0, m->prcm + GFX_RM_RSTCTRL);
	ret = readl_poll_timeout(m->prcm + GFX_RM_RSTST, v, v & GFX_RM_RSTST_RST, 10, 10000);
	if (ret) {
		dev_err(m->dev, "GFX reset did not complete\n");
		goto err;
	}
	writel(GFX_RM_RSTST_RST, m->prcm + GFX_RM_RSTST);
	return 0;
err:
	while (i--)
		clk_disable_unprepare(m->clks[i]);
	return ret;
}

static void mbx_power_off(struct mbx_device *m)
{
	int i;

	writel(1, m->prcm + GFX_RM_RSTCTRL);
	for (i = ARRAY_SIZE(m->clks) - 1; i >= 0; i--)
		clk_disable_unprepare(m->clks[i]);
}

static irqreturn_t mbx_irq(int irq, void *data)
{
	struct mbx_device *m = data;
	u32 st = mbx_read(m, MBX_REG_INT_STATUS) & mbx_read(m, MBX_REG_INT_ENABLE) & MBX_INT_MASK;

	if (!st)
		return IRQ_NONE;
	mbx_write(m, MBX_REG_INT_CLEAR, st);
	dev_dbg(m->dev, "irq %#x\n", st);
	return IRQ_HANDLED;
}

static int mbx_ioctl_get_param(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct mbx_device *m = to_mbx(drm);
	struct drm_mbx_get_param *p = data;

	switch (p->param) {
	case MBX_PARAM_CORE_ID:
		p->value = m->core_id;
		return 0;
	case MBX_PARAM_REVISION:
		p->value = m->revision;
		return 0;
	case MBX_PARAM_ABI:
		p->value = MBX_ABI_VERSION;
		return 0;
	}
	return -EINVAL;
}

static const struct drm_ioctl_desc mbx_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MBX_GET_PARAM, mbx_ioctl_get_param, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(mbx_fops);

static const struct drm_driver mbx_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.ioctls = mbx_ioctls,
	.num_ioctls = ARRAY_SIZE(mbx_ioctls),
	.fops = &mbx_fops,
	.name = "mbx",
	.desc = "PowerVR MBX (OMAP2420)",
	.major = 1,
	.minor = 0,
};

static int mbx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mbx_device *m;
	int i, ret;

	m = devm_drm_dev_alloc(dev, &mbx_driver, struct mbx_device, drm);
	if (IS_ERR(m))
		return PTR_ERR(m);
	m->dev = dev;
	platform_set_drvdata(pdev, m);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	m->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(m->regs))
		return PTR_ERR(m->regs);
	m->prcm = devm_ioremap(dev, OMAP24XX_PRCM_BASE, 0x1000);
	if (!m->prcm)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(m->clks); i++) {
		m->clks[i] = devm_clk_get(dev, mbx_clk_names[i]);
		if (IS_ERR(m->clks[i]))
			return dev_err_probe(dev, PTR_ERR(m->clks[i]), "clock %s\n", mbx_clk_names[i]);
	}
	m->irq = platform_get_irq(pdev, 0);
	if (m->irq < 0)
		return m->irq;

	ret = mbx_power_on(m);
	if (ret)
		return ret;
	m->core_id = mbx_read(m, MBX_REG_CORE_ID);
	m->revision = mbx_read(m, MBX_REG_REVISION);
	dev_info(dev, "MBX core id %#010x, revision %#x\n", m->core_id, m->revision);

	mbx_write(m, MBX_REG_INT_CLEAR, MBX_INT_MASK);
	ret = devm_request_irq(dev, m->irq, mbx_irq, 0, "mbx", m);
	if (ret)
		goto off;
	mbx_write(m, MBX_REG_INT_ENABLE, MBX_INT_MASTER | MBX_INT_RENDER | MBX_INT_TA_DONE |
		  MBX_INT_TA_OFLOW | MBX_INT_TA_STREAM);

	ret = drm_dev_register(&m->drm, 0);
	if (ret)
		goto off;
	return 0;
off:
	mbx_power_off(m);
	return ret;
}

static void mbx_remove(struct platform_device *pdev)
{
	struct mbx_device *m = platform_get_drvdata(pdev);

	drm_dev_unplug(&m->drm);
	mbx_write(m, MBX_REG_INT_ENABLE, 0);
	mbx_power_off(m);
}

static const struct of_device_id mbx_of_match[] = {
	{ .compatible = "ti,omap2420-mbx" },
	{ }
};
MODULE_DEVICE_TABLE(of, mbx_of_match);

static struct platform_driver mbx_platform_driver = {
	.probe = mbx_probe,
	.remove = mbx_remove,
	.driver = {
		.name = "mbx",
		.of_match_table = mbx_of_match,
	},
};
module_platform_driver(mbx_platform_driver);

MODULE_DESCRIPTION("PowerVR MBX (OMAP2420) DRM render node");
MODULE_LICENSE("GPL");
