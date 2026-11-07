// SPDX-License-Identifier: MIT
/* State objects: kept as the state tracker gives them, read when a draw is recorded */
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_helpers.h"
#include "util/u_transfer.h"

#include "mbx_context.h"

static void mbx_set_blend_color(struct pipe_context *pctx, const struct pipe_blend_color *color)
{
	mbx_context(pctx)->blend_color = *color;
}

static void mbx_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref) { }
static void mbx_set_sample_mask(struct pipe_context *pctx, unsigned mask) { }
static void mbx_set_polygon_stipple(struct pipe_context *pctx, const struct pipe_poly_stipple *s) { }

static void mbx_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader, uint index,
				    const struct pipe_constant_buffer *cb)
{
	struct mbx_context *ctx = mbx_context(pctx);
	int slot = shader == MESA_SHADER_VERTEX ? 0 : shader == MESA_SHADER_FRAGMENT ? 1 : -1;

	if (slot < 0 || index != 0)
		return;
	if (cb && (cb->buffer || cb->user_buffer)) {
		pipe_resource_reference(&ctx->cb[slot].buffer, cb->buffer);
		ctx->cb[slot].user_buffer = cb->user_buffer;
		ctx->cb[slot].buffer_offset = cb->buffer_offset;
		ctx->cb[slot].buffer_size = cb->buffer_size;
	} else {
		pipe_resource_reference(&ctx->cb[slot].buffer, NULL);
		ctx->cb[slot].user_buffer = NULL;
		ctx->cb[slot].buffer_size = 0;
	}
	ctx->const_serial++;
}

static void mbx_set_framebuffer_state(struct pipe_context *pctx, const struct pipe_framebuffer_state *fb)
{
	struct mbx_context *ctx = mbx_context(pctx);

	/* what is recorded belongs to the old target */
	mbx_job_flush(ctx);
	util_copy_framebuffer_state(&ctx->fb, fb);
}

static void mbx_set_scissor_states(struct pipe_context *pctx, unsigned start, unsigned count,
				   const struct pipe_scissor_state *s)
{
	if (count) {
		mbx_context(pctx)->scissor = s[0];
		mbx_context(pctx)->const_serial++;	/* the scissor's clip planes are program constants */
	}
}

static void mbx_set_viewport_states(struct pipe_context *pctx, unsigned start, unsigned count,
				    const struct pipe_viewport_state *v)
{
	if (count) {
		mbx_context(pctx)->viewport = v[0];
		mbx_context(pctx)->const_serial++;
	}
}

static void mbx_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
				   const struct pipe_vertex_buffer *vb)
{
	struct mbx_context *ctx = mbx_context(pctx);

	util_set_vertex_buffers_count(ctx->vb, &ctx->num_vb, vb, count);
}

/* A state object that is just a copy of its template */
#define CSO(name, type)							\
static void *mbx_create_##name(struct pipe_context *pctx, const type *t) \
{									\
	type *s = MALLOC(sizeof(*s));					\
	if (s)								\
		*s = *t;						\
	return s;							\
}

CSO(blend, struct pipe_blend_state)
CSO(rast, struct pipe_rasterizer_state)
CSO(dsa, struct pipe_depth_stencil_alpha_state)
CSO(sampler, struct pipe_sampler_state)

static void mbx_delete_cso(struct pipe_context *pctx, void *s)
{
	FREE(s);
}

static void mbx_bind_blend(struct pipe_context *pctx, void *s) { mbx_context(pctx)->blend = s; }
static void mbx_bind_rast(struct pipe_context *pctx, void *s) { mbx_context(pctx)->rast = s; }
static void mbx_bind_dsa(struct pipe_context *pctx, void *s) { mbx_context(pctx)->dsa = s; }

static void *mbx_create_vertex_elements(struct pipe_context *pctx, unsigned count,
					const struct pipe_vertex_element *e)
{
	struct mbx_vertex_elements *v = CALLOC_STRUCT(mbx_vertex_elements);

	if (!v)
		return NULL;
	v->count = count;
	memcpy(v->e, e, count * sizeof(*e));
	return v;
}

static void mbx_bind_vertex_elements(struct pipe_context *pctx, void *s)
{
	mbx_context(pctx)->velems = s;
}

static void mbx_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader, unsigned start,
				    unsigned count, void **states)
{
	struct mbx_context *ctx = mbx_context(pctx);

	if (shader != MESA_SHADER_FRAGMENT)
		return;
	for (unsigned i = 0; i < count; i++)
		if (start + i < MBX_MAX_SAMPLERS)
			ctx->samplers[start + i] = states ? states[i] : NULL;
}

static struct pipe_sampler_view *mbx_create_sampler_view(struct pipe_context *pctx,
							 struct pipe_resource *prsc,
							 const struct pipe_sampler_view *templ)
{
	struct pipe_sampler_view *so = CALLOC_STRUCT(pipe_sampler_view);

	if (!so)
		return NULL;
	*so = *templ;
	so->texture = NULL;
	pipe_resource_reference(&so->texture, prsc);
	pipe_reference_init(&so->reference, 1);
	so->context = pctx;
	return so;
}

static void mbx_sampler_view_destroy(struct pipe_context *pctx, struct pipe_sampler_view *view)
{
	pipe_resource_reference(&view->texture, NULL);
	FREE(view);
}

static void mbx_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader, unsigned start,
				  unsigned count, unsigned unbind_num_trailing_slots,
				  struct pipe_sampler_view **views)
{
	struct mbx_context *ctx = mbx_context(pctx);

	if (shader != MESA_SHADER_FRAGMENT)
		return;
	for (unsigned i = 0; i < count; i++)
		if (start + i < MBX_MAX_SAMPLERS)
			pipe_sampler_view_reference(&ctx->views[start + i], views ? views[i] : NULL);
	for (unsigned i = 0; i < unbind_num_trailing_slots; i++)
		if (start + count + i < MBX_MAX_SAMPLERS)
			pipe_sampler_view_reference(&ctx->views[start + count + i], NULL);
	ctx->const_serial++;	/* the texture scale (a program constant) follows the bound texture */
}

void mbx_state_init(struct pipe_context *pctx)
{
	pctx->set_blend_color = mbx_set_blend_color;
	pctx->set_stencil_ref = mbx_set_stencil_ref;
	pctx->set_sample_mask = mbx_set_sample_mask;
	pctx->set_constant_buffer = mbx_set_constant_buffer;
	pctx->set_framebuffer_state = mbx_set_framebuffer_state;
	pctx->set_polygon_stipple = mbx_set_polygon_stipple;
	pctx->set_scissor_states = mbx_set_scissor_states;
	pctx->set_viewport_states = mbx_set_viewport_states;
	pctx->set_vertex_buffers = mbx_set_vertex_buffers;

	pctx->create_blend_state = mbx_create_blend;
	pctx->bind_blend_state = mbx_bind_blend;
	pctx->delete_blend_state = mbx_delete_cso;
	pctx->create_rasterizer_state = mbx_create_rast;
	pctx->bind_rasterizer_state = mbx_bind_rast;
	pctx->delete_rasterizer_state = mbx_delete_cso;
	pctx->create_depth_stencil_alpha_state = mbx_create_dsa;
	pctx->bind_depth_stencil_alpha_state = mbx_bind_dsa;
	pctx->delete_depth_stencil_alpha_state = mbx_delete_cso;
	pctx->create_vertex_elements_state = mbx_create_vertex_elements;
	pctx->bind_vertex_elements_state = mbx_bind_vertex_elements;
	pctx->delete_vertex_elements_state = mbx_delete_cso;
	pctx->create_sampler_state = mbx_create_sampler;
	pctx->bind_sampler_states = mbx_bind_sampler_states;
	pctx->delete_sampler_state = mbx_delete_cso;
	pctx->create_sampler_view = mbx_create_sampler_view;
	pctx->sampler_view_destroy = mbx_sampler_view_destroy;
	pctx->sampler_view_release = u_default_sampler_view_release;
	pctx->resource_release = u_default_resource_release;
	pctx->set_sampler_views = mbx_set_sampler_views;
}
