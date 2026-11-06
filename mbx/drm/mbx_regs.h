/* SPDX-License-Identifier: GPL-2.0-only */
/* PowerVR MBX registers, from the model in mbx/ (see mbx/emu.h) */
#ifndef MBX_REGS_H
#define MBX_REGS_H

#define MBX_REG_SOFT_RESET	0x080
#define MBX_REG_INT_STATUS	0x12c	/* a write sets bits: the doorbell */
#define MBX_REG_INT_ENABLE	0x130
#define MBX_REG_INT_CLEAR	0x134
#define MBX_REG_CORE_ID		0xf00
#define MBX_REG_REVISION	0xf10

#define MBX_INT_RENDER		BIT(2)
#define MBX_INT_ISP		BIT(3)
#define MBX_INT_TA_DONE		BIT(4)
#define MBX_INT_TA_OFLOW	BIT(5)
#define MBX_INT_EVM_DALLOC	BIT(6)
#define MBX_INT_TA_CONTEXT	BIT(8)
#define MBX_INT_TA_STREAM	BIT(9)
#define MBX_INT_2D		BIT(10)
#define MBX_INT_MASTER		BIT(15)
#define MBX_INT_MASK		0x7fff

/* Slave ports, offsets in the 64 KB window */
#define MBX_TA_PORT		0x8000
#define MBX_2D_PORT		0xa000
#define MBX_TACTL_PORT		0xc000

/* PRCM, GFX domain (the OMAP2420 has no hwmod for it) */
#define OMAP24XX_PRCM_BASE		0x48008000
#define GFX_PRCM_CLKCFG_CTRL		0x080	/* VALID_CONFIG */
#define GFX_PRCM_CLKCFG_STATUS		0x084
#define GFX_RM_RSTCTRL			0x350
#define GFX_RM_RSTST			0x358
#define GFX_RM_RSTST_RST		0x10
#define GFX_PM_PWSTCTRL			0x3e0

#endif
