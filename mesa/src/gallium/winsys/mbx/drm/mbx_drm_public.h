/* SPDX-License-Identifier: MIT */
#ifndef MBX_DRM_PUBLIC_H
#define MBX_DRM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

struct pipe_screen *mbx_drm_screen_create(int fd, const struct pipe_screen_config *config);
struct pipe_screen *mbx_drm_screen_create_renderonly(int fd, struct renderonly *ro,
						      const struct pipe_screen_config *config);

#endif
