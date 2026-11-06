/* SPDX-License-Identifier: MIT */
/*
 * mbxemu: a functional model of the N810's PowerVR MBX1 (OMAP2420), built
 * from observing the chip. It is not timing-accurate: a TA stream is decoded
 * as it arrives, a render kick rasterizes the scene at once, a blit runs
 * when its words are complete.
 *
 * No QEMU dependency: a host (the QEMU device in qemu/) gives physical
 * memory, the interrupt line and a log through struct mbxemu_host, and calls
 * in with register and slave port accesses. README.md lists what the model
 * assumes where the chip's behaviour hasn't been observed; it is not a
 * reference for what the chip does there.
 */
#ifndef MBXEMU_H
#define MBXEMU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The MBX on the OMAP2420: registers, then three 8 KB slave ports */
#define MBXEMU_BASE		0x50000000
#define MBXEMU_SIZE		0x10000		/* the whole window, ports included */
#define MBXEMU_REGS		0x0000		/* offsets in the window */
#define MBXEMU_REGS_SIZE	0x4000
#define MBXEMU_TA_PORT		0x8000		/* the TA's input FIFO */
#define MBXEMU_2D_PORT		0xa000		/* the 2D core's (the same FIFO) */
#define MBXEMU_TACTL_PORT	0xc000		/* TA control words */
#define MBXEMU_PORT_SIZE	0x2000
#define MBXEMU_INTC_LINE	21		/* its OMAP2420 INTC line */

struct mbxemu;

struct mbxemu_host {
	void *opaque;
	/* the MBX as bus master: physical memory, after its MMU */
	void (*mem_read)(void *opaque, uint32_t phys, void *buf, uint32_t len);
	void (*mem_write)(void *opaque, uint32_t phys, const void *buf, uint32_t len);
	/* the interrupt line's level, called when it changes */
	void (*irq)(void *opaque, int level);
	/* diagnostics (may be NULL): one line, no newline */
	void (*log)(void *opaque, const char *line);
};

struct mbxemu *mbxemu_new(const struct mbxemu_host *host);
void mbxemu_free(struct mbxemu *e);
/* power-on state: the registers, VGP memory, scenes, the MMU off */
void mbxemu_reset(struct mbxemu *e);

/* 32-bit accesses at an offset in the 64 KB window (MBXEMU_REGS etc.) */
uint32_t mbxemu_read(struct mbxemu *e, uint32_t off);
void mbxemu_write(struct mbxemu *e, uint32_t off, uint32_t v);

/* The same, by block */
uint32_t mbxemu_reg_read(struct mbxemu *e, uint32_t reg);
void mbxemu_reg_write(struct mbxemu *e, uint32_t reg, uint32_t v);
void mbxemu_ta_write(struct mbxemu *e, uint32_t off, uint32_t v);	/* off: the lane is off / 4 & 3 */
void mbxemu_ta_ctl_write(struct mbxemu *e, uint32_t v);
void mbxemu_2d_write(struct mbxemu *e, uint32_t v);

/* 1 (the default): a scene the chip would not take comes out as garbage; 0: the model forgives it */
void mbxemu_set_strict(struct mbxemu *e, int strict);

/* 1: log every TA word, register access and render; 0: only problems */
void mbxemu_set_trace(struct mbxemu *e, int level);

struct mbxemu_stats {
	unsigned int scenes;		/* TA scenes started */
	unsigned int objects, prims;	/* TA objects, triangles kept */
	unsigned int renders, tiles;	/* render kicks, tiles rendered */
	unsigned int blits;
	unsigned int stream_errors;	/* TA words it couldn't parse */
	unsigned int ta_hangs;		/* sections words without bit 17: the TA took nothing */
	unsigned int violations;	/* contract violations counted (emu.h, V_*) */
	unsigned int violation_mask;	/* which kinds have happened */
};
void mbxemu_stats(const struct mbxemu *e, struct mbxemu_stats *s);

#ifdef __cplusplus
}
#endif

#endif
