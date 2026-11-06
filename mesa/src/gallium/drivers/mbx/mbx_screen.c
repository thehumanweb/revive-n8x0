// SPDX-License-Identifier: MIT
/*
 * The PowerVR MBX (OMAP2420) Gallium screen: vertex work on the VGP, the pixel
 * side fixed-function (texture units, fog, blending), so fragment shaders
 * are matched onto it or fall back to the CPU. See JOURNEY.md.
 */
#include <sys/ioctl.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/mbx_drm.h"
#include "compiler/nir/nir.h"
#include "util/format/u_format.h"
#include "util/os_misc.h"
#include "util/ralloc.h"
#include "util/u_screen.h"

#include "mbx_context.h"
#include "mbx_program.h"
#include "mbx_screen.h"

static const char *mbx_screen_get_name(struct pipe_screen *pscreen)
{
	return "PowerVR MBX";
}

static const char *mbx_screen_get_vendor(struct pipe_screen *pscreen)
{
	return "Imagination Technologies";
}

static int mbx_screen_get_fd(struct pipe_screen *pscreen)
{
	return mbx_screen(pscreen)->fd;
}

static void mbx_screen_destroy(struct pipe_screen *pscreen)
{
	struct mbx_screen *screen = mbx_screen(pscreen);

	if (screen->ro)
		screen->ro->destroy(screen->ro);
	close(screen->fd);
	ralloc_free(pscreen);
}

static bool mbx_screen_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
					   enum pipe_texture_target target, unsigned sample_count,
					   unsigned storage_sample_count, unsigned usage)
{
	if (sample_count > 1 || storage_sample_count > 1)
		return false;
	if (target >= PIPE_MAX_TEXTURE_TYPES)
		return false;

	if (usage & PIPE_BIND_VERTEX_BUFFER) {
		switch (format) {
		case PIPE_FORMAT_R32_FLOAT:
		case PIPE_FORMAT_R32G32_FLOAT:
		case PIPE_FORMAT_R32G32B32_FLOAT:
		case PIPE_FORMAT_R32G32B32A32_FLOAT:
		case PIPE_FORMAT_R8G8B8A8_UNORM:
		case PIPE_FORMAT_R8G8B8A8_USCALED:
		case PIPE_FORMAT_R16G16_SNORM:
		case PIPE_FORMAT_R16G16B16A16_SNORM:
		case PIPE_FORMAT_R16G16_UNORM:
		case PIPE_FORMAT_R16G16B16A16_UNORM:
		case PIPE_FORMAT_R8_UNORM:
		case PIPE_FORMAT_R8G8_UNORM:
		case PIPE_FORMAT_R8G8B8_UNORM:
			break;
		default:
			return false;
		}
		usage &= ~PIPE_BIND_VERTEX_BUFFER;
	}
	if (usage & PIPE_BIND_INDEX_BUFFER)
		usage &= ~PIPE_BIND_INDEX_BUFFER;

	if (usage & PIPE_BIND_DEPTH_STENCIL) {
		/* depth is kept on chip for a render: there is no memory behind it */
		if (format != PIPE_FORMAT_Z16_UNORM && format != PIPE_FORMAT_Z24X8_UNORM)
			return false;
		usage &= ~PIPE_BIND_DEPTH_STENCIL;
	}
	if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) {
		if (format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM &&
		    format != PIPE_FORMAT_B5G6R5_UNORM)
			return false;
		usage &= ~(PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET);
	}
	if (usage & PIPE_BIND_SAMPLER_VIEW) {
		switch (format) {
		case PIPE_FORMAT_B8G8R8A8_UNORM:
		case PIPE_FORMAT_B8G8R8X8_UNORM:
		case PIPE_FORMAT_B5G6R5_UNORM:
		case PIPE_FORMAT_R8G8B8A8_UNORM:
		case PIPE_FORMAT_R8G8B8X8_UNORM:
		case PIPE_FORMAT_A8_UNORM:
		case PIPE_FORMAT_L8_UNORM:
		case PIPE_FORMAT_L8A8_UNORM:
			break;
		default:
			return false;
		}
		usage &= ~PIPE_BIND_SAMPLER_VIEW;
	}
	return usage == 0 || usage == PIPE_BIND_CONSTANT_BUFFER || usage == PIPE_BIND_SHADER_BUFFER;
}

static void mbx_screen_query_dmabuf_modifiers(struct pipe_screen *pscreen, enum pipe_format format, int max,
					      uint64_t *modifiers, unsigned int *external_only, int *count)
{
	if (!modifiers || max < 1) {
		*count = 1;
		return;
	}
	modifiers[0] = DRM_FORMAT_MOD_LINEAR;
	if (external_only)
		external_only[0] = false;
	*count = 1;
}

static bool mbx_screen_is_dmabuf_modifier_supported(struct pipe_screen *pscreen, uint64_t modifier,
						    enum pipe_format format, bool *external_only)
{
	if (external_only)
		*external_only = false;
	return modifier == DRM_FORMAT_MOD_LINEAR;
}

static unsigned mbx_screen_get_dmabuf_modifier_planes(struct pipe_screen *pscreen, uint64_t modifier,
						      enum pipe_format format)
{
	return 1;
}

static void mbx_init_shader_caps(struct mbx_screen *screen)
{
	for (unsigned i = 0; i <= MESA_SHADER_FRAGMENT; i++) {
		struct pipe_shader_caps *caps = (struct pipe_shader_caps *)&screen->base.shader_caps[i];

		if (i != MESA_SHADER_VERTEX && i != MESA_SHADER_FRAGMENT)
			continue;
		/* the VGP: 256 instructions, 192 constants, 14 temporaries, 8 inputs, no branches */
		caps->max_instructions = caps->max_alu_instructions = 256;
		caps->max_control_flow_depth = 0;
		caps->max_inputs = 8;
		caps->max_outputs = i == MESA_SHADER_FRAGMENT ? 1 : 8;
		caps->max_temps = 14;
		caps->max_const_buffer0_size = 192 * 4 * sizeof(float);
		caps->max_const_buffers = 1;
		caps->max_texture_samplers = caps->max_sampler_views = 2;
		caps->supported_irs = 1 << PIPE_SHADER_IR_NIR;
	}
}

static void mbx_init_screen_caps(struct mbx_screen *screen)
{
	struct pipe_caps *caps = (struct pipe_caps *)&screen->base.caps;

	u_init_pipe_screen_caps(&screen->base, 1);
	caps->npot_textures = true;
	caps->blend_equation_separate = true;
	caps->vertex_color_unclamped = true;
	caps->fragment_color_clamped = true;
	caps->mixed_framebuffer_sizes = true;
	caps->mixed_color_depth_bits = true;
	caps->alpha_test = false;
	caps->two_sided_color = false;
	caps->texrect = false;
	caps->fs_coord_origin_upper_left = true;
	caps->fs_coord_pixel_center_half_integer = true;
	caps->uma = true;
	caps->graphics = true;
	caps->max_texture_2d_size = 2048;
	caps->max_texture_cube_levels = 1;
	caps->max_texture_3d_levels = 0;
	caps->max_varyings = 8;
	caps->clip_planes = 8;
	caps->supported_prim_modes = BITFIELD_BIT(MESA_PRIM_POINTS) | BITFIELD_BIT(MESA_PRIM_LINES) |
				     BITFIELD_BIT(MESA_PRIM_LINE_STRIP) | BITFIELD_BIT(MESA_PRIM_TRIANGLES) |
				     BITFIELD_BIT(MESA_PRIM_TRIANGLE_STRIP) |
				     BITFIELD_BIT(MESA_PRIM_TRIANGLE_FAN);
	caps->min_line_width = caps->min_point_size = 1;
	caps->max_line_width = caps->max_point_size = 32;
}

struct pipe_screen *mbx_screen_create(int fd, const struct pipe_screen_config *config,
				      struct renderonly *ro)
{
	struct mbx_screen *screen = rzalloc(NULL, struct mbx_screen);
	struct pipe_screen *pscreen = &screen->base;
	struct drm_mbx_get_param p = { .param = MBX_PARAM_CORE_ID };

	if (!screen)
		return NULL;
	screen->fd = fd;
	screen->ro = ro;
	if (!ioctl(fd, DRM_IOCTL_MBX_GET_PARAM, &p))
		screen->core_id = (uint32_t)p.value;
	p.param = MBX_PARAM_REVISION;
	if (!ioctl(fd, DRM_IOCTL_MBX_GET_PARAM, &p))
		screen->revision = (uint32_t)p.value;

	pscreen->destroy = mbx_screen_destroy;
	pscreen->get_screen_fd = mbx_screen_get_fd;
	pscreen->get_name = mbx_screen_get_name;
	pscreen->get_vendor = mbx_screen_get_vendor;
	pscreen->get_device_vendor = mbx_screen_get_vendor;
	pscreen->context_create = mbx_context_create;
	pscreen->is_format_supported = mbx_screen_is_format_supported;
	pscreen->query_dmabuf_modifiers = mbx_screen_query_dmabuf_modifiers;
	pscreen->is_dmabuf_modifier_supported = mbx_screen_is_dmabuf_modifier_supported;
	pscreen->get_dmabuf_modifier_planes = mbx_screen_get_dmabuf_modifier_planes;

	for (unsigned i = 0; i <= MESA_SHADER_COMPUTE; i++)
		pscreen->nir_options[i] = &mbx_nir_options;
	mbx_resource_screen_init(pscreen);
	mbx_init_shader_caps(screen);
	mbx_init_screen_caps(screen);
	return pscreen;
}
