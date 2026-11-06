/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mbxdma: a test of feeding the MBX's TA FIFO (the slave port) by the
 * OMAP2420's system DMA instead of CPU stores. One ioctl on /dev/mbxdma
 * copies words from the caller and moves them to the TA port by one sDMA
 * channel, paced by a DMA request line (S_DMA_4, request 5, is the GFX
 * one), and says how far it got.
 */
#ifndef MBXDMA_H
#define MBXDMA_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct mbxdma_xfer {
	/* in */
	__u64 src;		/* the words, in the caller's memory */
	__u32 words;		/* how many (at most 16384) */
	__u32 dst_off;		/* the first word's byte offset in the TA port;
				 * MBXDMA_TO_RAM: the driver's own buffer instead
				 * (a test of the channel itself) */
	__u32 req;		/* DMA request line (1-63); 0: unsynchronised */
	__u32 sync;		/* 0 element, 1 frame, 2 block, 3 packet */
	__u32 frame;		/* elements a frame (CEN); the frames are words / frame */
	__u32 dst_amode;	/* 0 constant, 1 post-increment, 2 single, 3 double index */
	__s32 dst_ei, dst_fi;	/* element and frame index (CDEI, CDFI) */
	__u32 timeout_us;
	__u32 flags;		/* MBXDMA_ASYNC: start it and return; MBXDMA_WAIT ends it */
	/* out */
	__u32 done;		/* 1: the block finished */
	__u32 csr;		/* the channel's status at the end */
	__u32 moved;		/* words written (from the current destination address) */
	__u32 ccen, ccfn;	/* the current element and frame */
	__u32 us;		/* from the enable to the end (or the timeout) */
	__u32 cdac;		/* the current destination address */
	__u32 ram_ok;		/* MBXDMA_TO_RAM: words that arrived intact */
};

/* A physical range filled with one word by the sDMA's constant fill
 * (whole pages); the call sleeps until it is done */
struct mbxdma_fill {
	__u32 phys, bytes, value, timeout_us;
	__u32 done, us;		/* out */
};

#define MBXDMA_XFER	_IOWR('M', 0x40, struct mbxdma_xfer)
#define MBXDMA_FILL	_IOWR('M', 0x42, struct mbxdma_fill)
#define MBXDMA_WAIT	_IOWR('M', 0x41, struct mbxdma_xfer)
#define MBXDMA_TO_RAM	0xffffffffu
#define MBXDMA_ASYNC	1

#endif
