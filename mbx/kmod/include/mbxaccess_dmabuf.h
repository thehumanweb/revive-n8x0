/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * mbxaccess: dma-bufs (omapdrm scanout buffers) for the MBX to render into.
 * Attachments are kept until the /dev/mbx file is closed.
 */
#ifndef MBXACCESS_DMABUF_H
#define MBXACCESS_DMABUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct mbxaccess_dmabuf {
	__s32 fd;		/* in: dma-buf */
	__u32 size;		/* out: bytes */
	__u32 phys;		/* out: physical (DMA) address of the first byte */
	__u32 contiguous;	/* out: 1 if one physically contiguous range */
};

#define MBXDmaBufPhys	_IOWR('M', 9, struct mbxaccess_dmabuf)
/*
 * Back the next services allocation of the dma-buf's size (mbxdaemon's
 * VirtualAllocateReserve for PVRSRVAllocDeviceMem) with the dma-buf:
 * the MBX renders into it, with the allocation's own sync object.
 */
#define MBXDmaBufArm	_IOWR('M', 10, struct mbxaccess_dmabuf)

#endif
