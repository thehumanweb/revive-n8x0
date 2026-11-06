/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/* uapi of the PowerVR MBX DRM render node */
#ifndef MBX_DRM_H
#define MBX_DRM_H

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define MBX_PARAM_CORE_ID	0	/* CORE_ID register, 0x01010003 on the N810 */
#define MBX_PARAM_REVISION	1
#define MBX_PARAM_ABI		2	/* bumped on incompatible changes */

struct drm_mbx_get_param {
	__u32 param;		/* in: MBX_PARAM_* */
	__u32 pad;
	__u64 value;		/* out */
};

#define DRM_MBX_GET_PARAM	0x00

#define DRM_IOCTL_MBX_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_MBX_GET_PARAM, struct drm_mbx_get_param)

#if defined(__cplusplus)
}
#endif

#endif
