/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/* uapi of the PowerVR MBX DRM render node */
#ifndef MBX_DRM_H
#define MBX_DRM_H

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Bumped on incompatible changes; DRM_MBX_GET_PARAM MBX_PARAM_ABI returns it */
#define MBX_ABI_VERSION		3

#define MBX_PARAM_CORE_ID	0	/* CORE_ID register, 0x01010003 on the N810 */
#define MBX_PARAM_REVISION	1
#define MBX_PARAM_ABI		2

struct drm_mbx_get_param {
	__u32 param;		/* in: MBX_PARAM_* */
	__u32 pad;
	__u64 value;		/* out */
};

/* A buffer: shmem pages, mapped write-combined for the CPU */
struct drm_mbx_bo_create {
	__u64 size;		/* in: bytes, rounded up to pages */
	__u32 flags;		/* in: 0 */
	__u32 handle;		/* out */
};

/*
 * Where a buffer is: the first call for a handle maps it into the file's
 * MBX address space (32 MB, 4 KB pages; what the MBX sees), also for
 * dma-bufs imported by PRIME (omapdrm's scanout buffers).
 */
struct drm_mbx_bo_info {
	__u32 handle;		/* in */
	__u32 pad;
	__u64 size;		/* out: bytes */
	__u64 offset;		/* out: mmap offset on the DRM fd */
	__u32 va;		/* out: MBX (device) address */
	__u32 pad2;
};

struct drm_mbx_bo_unmap {
	__u32 handle;		/* in: drop its mapping in the MBX address space */
	__u32 pad;
};

/*
 * The TA stream is chunks: a header word, type << 28 | count, then count
 * words. TA words go to the TA slave port (a vertex, a header, a VGP
 * program, the end word), CTL words to the TA control port (1 ends an
 * object, 0 restarts a strip).
 */
#define MBX_CHUNK_TA		0
#define MBX_CHUNK_CTL		1
#define MBX_CHUNK(type, count)	((__u32)(type) << 28 | (count))

/* A buffer a job touches; the kernel waits for what others did to it and marks it with the job's fence */
struct drm_mbx_submit_bo {
	__u32 handle;
	__u32 flags;		/* MBX_SUBMIT_BO_* */
};
#define MBX_SUBMIT_BO_WRITE	(1u << 0)	/* the job writes it (a render target): a later reader waits */

/*
 * One frame: the TA is set up and started, the stream goes in, then the render is
 * set up and kicked. Every value is a register's, as in mbx/README.md; addresses are
 * MBX addresses from DRM_MBX_BO_INFO. The ioctl queues the job and returns at once with
 * its fence (a number: jobs complete in order); DRM_IOCTL_MBX_WAIT_FENCE waits for it.
 * The buffers named in bos are synchronised implicitly (reservation objects): the
 * job waits for earlier writers (and, if it writes, readers), and its fence is attached
 * to them, so a display flip of a render target waits for the render without userspace
 * doing anything. Memory the job uses that is not named (the region array, tails,
 * background object) is the caller's to keep until the fence is done. A job that fails
 * (the TA overflows, a timeout) resets the cores and its fence reports -EIO.
 */
struct drm_mbx_submit {
	__u64 stream;		/* in: user pointer to the chunks */
	__u32 stream_words;
	__u32 timeout_ms;	/* 0: 2000; for each wait the job makes */
	/* the TA */
	__u32 ta_config;	/* 0x85c */
	__u32 ta_width;		/* 0x84c */
	__u32 ta_height;	/* 0x850 */
	__u32 ta_regions;	/* 0x844 */
	__u32 ta_tails;		/* 0x840 */
	__u32 ta_context;	/* 0x810 */
	/* the render */
	__u32 r_regions;	/* 0x608 */
	__u32 r_ctl;		/* 0x610 */
	__u32 r_sampling;	/* 0x61c */
	__u32 r_fog;		/* 0x63c */
	__u32 r_format;		/* 0x650 */
	__u32 r_width;		/* 0x654 */
	__u32 r_height;		/* 0x658 */
	__u32 r_addr;		/* 0x65c */
	__u32 r_stride;		/* 0x660 */
	__u32 r_zbuf;		/* 0x674 */
	__u32 r_pb;		/* 0x83c */
	__u32 r_6dc;		/* 0x6dc */
	__u32 flags;		/* 0 */
	__u32 pad;
	__u64 bos;		/* in: user pointer to struct drm_mbx_submit_bo[n_bos] */
	__u32 n_bos;		/* at most 64 */
	__u32 pad2;
	__u64 fence;		/* out: the job's fence */
};

/* Wait until the job with this fence has completed (or the relative timeout passes) */
struct drm_mbx_wait_fence {
	__u64 fence;
	__u64 timeout_ns;
};

#define DRM_MBX_GET_PARAM	0x00
#define DRM_MBX_BO_CREATE	0x01
#define DRM_MBX_BO_INFO		0x02
#define DRM_MBX_BO_UNMAP	0x03
#define DRM_MBX_SUBMIT		0x04
#define DRM_MBX_WAIT_FENCE	0x05

#define DRM_IOCTL_MBX_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MBX_GET_PARAM, struct drm_mbx_get_param)
#define DRM_IOCTL_MBX_BO_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MBX_BO_CREATE, struct drm_mbx_bo_create)
#define DRM_IOCTL_MBX_BO_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MBX_BO_INFO, struct drm_mbx_bo_info)
#define DRM_IOCTL_MBX_BO_UNMAP \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MBX_BO_UNMAP, struct drm_mbx_bo_unmap)
#define DRM_IOCTL_MBX_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MBX_SUBMIT, struct drm_mbx_submit)
#define DRM_IOCTL_MBX_WAIT_FENCE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_MBX_WAIT_FENCE, struct drm_mbx_wait_fence)

#if defined(__cplusplus)
}
#endif

#endif
