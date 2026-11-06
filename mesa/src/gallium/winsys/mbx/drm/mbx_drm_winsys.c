// SPDX-License-Identifier: MIT
/* The mbx driver's entry from the DRM loaders: the render node itself, or through kmsro */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util/os_file.h"
#include "util/u_screen.h"

#include "renderonly/renderonly.h"
#include "kmsro/drm/kmsro_drm_public.h"
#include "mbx_drm_public.h"
#include "mbx/mbx_screen.h"
#include "drm-uapi/mbx_drm.h"

static bool
mbx_is_node(int fd)
{
	struct drm_mbx_get_param p = { .param = MBX_PARAM_ABI };

	return ioctl(fd, DRM_IOCTL_MBX_GET_PARAM, &p) == 0 && p.value == MBX_ABI_VERSION;
}

struct pipe_screen *
mbx_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
	if (mbx_is_node(fd))
		return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, NULL, mbx_screen_create);

#ifdef GALLIUM_KMSRO
	/* a display-only device: find the MBX and scan out of this one's buffers */
	return kmsro_drm_screen_create(fd, config);
#endif
	return NULL;
}

struct pipe_screen *
mbx_drm_screen_create_renderonly(int fd, struct renderonly *ro, const struct pipe_screen_config *config)
{
	return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, ro, mbx_screen_create);
}
