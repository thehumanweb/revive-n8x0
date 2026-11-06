/* SPDX-License-Identifier: GPL-2.0-only */
/* PowerVR MBX registers, from the model in mbx/ (see mbx/emu.h) */
#ifndef MBX_REGS_H
#define MBX_REGS_H

#define MBX_REG_SOFT_RESET	0x080	/* bits 0, 4, 8: the cores */
#define MBX_REG_INT_STATUS	0x12c	/* a write sets bits: the doorbell */
#define MBX_REG_INT_ENABLE	0x130
#define MBX_REG_INT_CLEAR	0x134
#define MBX_REG_CORE_ID		0xf00
#define MBX_REG_REVISION	0xf10

/* the render */
#define MBX_REG_R_REGIONS	0x608
#define MBX_REG_R_CTL		0x610
#define MBX_REG_R_SAMPLING	0x61c
#define MBX_REG_R_FOG		0x63c
#define MBX_REG_R_FORMAT	0x650
#define MBX_REG_R_WIDTH		0x654
#define MBX_REG_R_HEIGHT	0x658
#define MBX_REG_R_ADDR		0x65c
#define MBX_REG_R_STRIDE	0x660
#define MBX_REG_R_ZBUF		0x674
#define MBX_REG_R_KICK		0x680
#define MBX_REG_R_6DC		0x6dc
#define MBX_REG_R_HW_CONTEXT	0x6ec

/* the TA */
#define MBX_REG_TA_START	0x800
#define MBX_REG_TA_CONTEXT_ID	0x810
#define MBX_REG_PB_BASE		0x83c
#define MBX_REG_TA_TAILS	0x840
#define MBX_REG_TA_REGIONS	0x844
#define MBX_REG_TA_WIDTH	0x84c
#define MBX_REG_TA_HEIGHT	0x850
#define MBX_REG_TA_CONFIG	0x85c

/* the parameter buffer's free list (mbx/README.md assumptions 22 and 26): a table of 16-bit
 * entries in device memory, one a page, page p at device address p << 12 */
#define MBX_REG_FL_TABLE	0x824
#define MBX_REG_FL_FIRST	0x828
#define MBX_REG_FL_LAST		0x82c
#define MBX_REG_FL_BUILD	0x838

/* the MMU: eight page tables of 1024 bare physical page addresses (32 MB) */
#define MBX_REG_MMU_TABLE(i)	(0x1000 + 4 * (i))
#define MBX_MMU_TABLES		8
#define MBX_REG_MMU_CTRL	0x1020	/* bit 0 enable; a write otherwise flushes the TLB */

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
