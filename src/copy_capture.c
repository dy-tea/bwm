#include "copy_capture.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "server.h"
#include "toplevel.h"
#include "types.h"
#include "xwayland.h"
#include <assert.h>
#include <drm_fourcc.h>
#include <wayland-server-protocol.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

typedef struct image_copy_source_t {
	struct wlr_ext_image_capture_source_v1 base;
	struct wlr_output *output;
	struct wlr_buffer *last_buffer;
	struct wl_listener output_commit;
	struct wl_listener output_destroy;
	struct wl_listener base_destroy;
} image_copy_source_t;

static const struct wlr_ext_image_capture_source_v1_interface source_impl;

static image_copy_source_t *source_from_base(struct wlr_ext_image_capture_source_v1 *base) {
	return (image_copy_source_t *)((char *)base - offsetof(image_copy_source_t, base));
}

static void output_source_destroy_internal(image_copy_source_t *src) {
	if (!src)
		return;

	wlr_ext_image_capture_source_v1_finish(&src->base);
	wl_list_remove(&src->output_commit.link);
	wl_list_remove(&src->output_destroy.link);
	wl_list_remove(&src->base_destroy.link);

	if (src->last_buffer)
		wlr_buffer_unlock(src->last_buffer);

	free(src->base.shm_formats);
	free(src);
}

static void output_source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	(void)base;
	(void)with_cursors;
}

static void output_source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	(void)base;
}

static void output_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	(void)base;
	(void)schedule_frame;
}


#define MAX_BLOCKED_WINDOWS 128

struct blocked_node_state {
	struct wlr_scene_node *node;
	bool was_enabled;
};

static int disable_blocked_windows(struct blocked_node_state *states, int max_states) {
	int count = 0;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->node || !tl->node->client)
			continue;

		client_t *c = tl->node->client;
		if (!c->flags.block_out_from_screenshare)
			continue;
		if (!c->flags.shown && c->state != STATE_FULLSCREEN)
			continue;

		if (count >= max_states)
			break;
		wlr_log(WLR_DEBUG, "ext-copy-capture: disabling toplevel scene_tree=%p"
			" app_id=%s", (void *)&tl->scene_tree->node, c->app_id);

		states[count].node = &tl->scene_tree->node;
		states[count].was_enabled = tl->scene_tree->node.enabled;
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		count++;
	}

	xwayland_toplevel_t *xw;
	wl_list_for_each(xw, &server.xwayland.views, link) {
		if (!xw->node || !xw->node->client)
			continue;

		client_t *c = xw->node->client;
		if (!c->flags.block_out_from_screenshare)
			continue;
		if (!c->flags.shown && c->state != STATE_FULLSCREEN)
			continue;

		if (count >= max_states)
			break;
		wlr_log(WLR_DEBUG, "ext-copy-capture: disabling xwayland scene_tree=%p"
			" title=%s", (void *)&xw->scene_tree->node, c->title);

		states[count].node = &xw->scene_tree->node;
		states[count].was_enabled = xw->scene_tree->node.enabled;
		wlr_scene_node_set_enabled(&xw->scene_tree->node, false);
		count++;
	}

	return count;
}

static void restore_blocked_windows(struct blocked_node_state *states, int count) {
	for (int i = 0; i < count; i++)
		wlr_scene_node_set_enabled(states[i].node, states[i].was_enabled);
}

static void output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *dst_frame,
		struct wlr_ext_image_capture_source_v1_frame_event *frame_event) {
	(void)base;
	(void)dst_frame;
	(void)frame_event;
}

static struct wlr_ext_image_capture_source_v1_cursor *output_source_get_pointer_cursor(struct
		wlr_ext_image_capture_source_v1 *base, struct wlr_seat *seat) {
	(void)base;
	(void)seat;
	return NULL;
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = output_source_start,
	.stop = output_source_stop,
	.request_frame = output_source_request_frame,
	.copy_frame = output_source_copy_frame,
	.get_pointer_cursor = output_source_get_pointer_cursor,
};

static void source_handle_output_commit(struct wl_listener *listener, void *data) {
	image_copy_source_t *src = wl_container_of(listener, src, output_commit);
	struct wlr_output_event_commit *event = data;
	struct wlr_output *output = src->output;

	if ((event->state->committed & WLR_OUTPUT_STATE_ENABLED) && !output->enabled)
		return;
	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER))
		return;

	if (src->last_buffer)
		wlr_buffer_unlock(src->last_buffer);
	src->last_buffer = event->state->buffer;
	wlr_buffer_lock(src->last_buffer);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_copy(&damage, &event->state->damage);

	struct wlr_ext_image_capture_source_v1_frame_event frame_event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&src->base.events.frame, &frame_event);

	pixman_region32_fini(&damage);
}

static void source_handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	image_copy_source_t *src = wl_container_of(listener, src, output_destroy);
	wl_list_remove(&src->output_commit.link);
	wl_list_remove(&src->output_destroy.link);
	if (src->last_buffer) {
		wlr_buffer_unlock(src->last_buffer);
		src->last_buffer = NULL;
	}
	src->output = NULL;
}

static void source_handle_base_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	image_copy_source_t *src = wl_container_of(listener, src, base_destroy);
	src->base_destroy.notify = NULL;
	output_source_destroy_internal(src);
}

static image_copy_source_t *create_output_source(struct wlr_output *wlr_output) {
	image_copy_source_t *src = calloc(1, sizeof(*src));
	if (!src)
		return NULL;

	wlr_ext_image_capture_source_v1_init(&src->base, &source_impl);

	int ow, oh;
	wlr_output_transformed_resolution(wlr_output, &ow, &oh);
	src->base.width = (uint32_t)ow;
	src->base.height = (uint32_t)oh;

	static const uint32_t shm_formats[] = {DRM_FORMAT_ARGB8888};
	src->base.shm_formats = calloc(1, sizeof(shm_formats));
	if (src->base.shm_formats)
		memcpy(src->base.shm_formats, shm_formats, sizeof(shm_formats));
	src->base.shm_formats_len = 1;

	src->base.dmabuf_device = 0;

	src->output = wlr_output;

	src->output_commit.notify = source_handle_output_commit;
	wl_signal_add(&wlr_output->events.commit, &src->output_commit);
	src->output_destroy.notify = source_handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &src->output_destroy);

	src->base_destroy.notify = source_handle_base_destroy;
	wl_signal_add(&src->base.events.destroy, &src->base_destroy);

	return src;
}

typedef struct output_capture_mgr_t {
	struct wl_global *global;
	struct wl_listener display_destroy;
} output_capture_mgr_t;

static void output_mgr_handle_create_source(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id, struct wl_resource *output_resource) {
	(void)mgr_resource;
	struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
	if (!wlr_output)
		return;

	image_copy_source_t *src = create_output_source(wlr_output);
	if (!src) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	if (!wlr_ext_image_capture_source_v1_create_resource(&src->base, wl_client, id)) {
		output_source_destroy_internal(src);
		wl_client_post_no_memory(wl_client);
		return;
	}
}

static void output_mgr_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *mgr_resource) {
	(void)wl_client;
	wl_resource_destroy(mgr_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_mgr_impl = {
	.create_source = output_mgr_handle_create_source,
	.destroy = output_mgr_handle_destroy,
};

static void output_mgr_bind(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id) {
	output_capture_mgr_t *mgr = data;
	struct wl_resource *resource = wl_resource_create(wl_client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &output_mgr_impl, mgr, NULL);
}

static void output_mgr_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	output_capture_mgr_t *mgr = wl_container_of(listener, mgr, display_destroy);
	wl_list_remove(&mgr->display_destroy.link);
	wl_global_destroy(mgr->global);
	free(mgr);
}

typedef struct copy_mgr_t {
	struct wl_global *global;
	struct wl_listener display_destroy;
} copy_mgr_t;

typedef struct copy_frame_t copy_frame_t;

typedef struct copy_session_t {
	struct wl_resource *resource;
	struct wlr_ext_image_capture_source_v1 *source;
	copy_frame_t *frame;
	struct wl_listener source_destroy;
} copy_session_t;

typedef struct copy_frame_t {
	struct wl_resource *resource;
	struct wlr_buffer *buffer;
	bool capturing;
	pixman_region32_t buffer_damage;
	copy_session_t *session;
} copy_frame_t;

copy_session_t *session_from_resource(struct wl_resource *resource);
copy_frame_t *frame_from_resource(struct wl_resource *resource);

static void frame_destroy(copy_frame_t *frame) {
	if (!frame)
		return;

	wl_resource_set_user_data(frame->resource, NULL);
	if (frame->buffer)
		wlr_buffer_unlock(frame->buffer);

	pixman_region32_fini(&frame->buffer_damage);
	free(frame);
}

static void frame_handle_resource_destroy(struct wl_resource *resource) {
	copy_frame_t *frame = frame_from_resource(resource);
	if (frame) {
		frame->session->frame = NULL;
		frame_destroy(frame);
	}
}

static void frame_handle_destroy(struct wl_client *wl_client, struct wl_resource *frame_resource) {
	(void)wl_client;
	wl_resource_destroy(frame_resource);
}

static void frame_handle_attach_buffer(struct wl_client *wl_client,
		struct wl_resource *frame_resource, struct wl_resource *buffer_resource) {
	(void)wl_client;
	copy_frame_t *frame = frame_from_resource(frame_resource);
	if (!frame)
		return;

	struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
	if (!buffer) {
		wl_resource_post_error(frame->resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			"invalid buffer");
		return;
	}

	struct wlr_dmabuf_attributes dmabuf_attribs;
	struct wlr_shm_attributes shm_attribs;
	if (wlr_buffer_get_dmabuf(buffer, &dmabuf_attribs)) {
		wlr_log(WLR_INFO, "ext-copy-capture: attached buffer is DMA-BUF "
			"%dx%d fmt=0x%x n_planes=%d", buffer->width, buffer->height, dmabuf_attribs.format,
				dmabuf_attribs.n_planes);
	} else if (wlr_buffer_get_shm(buffer, &shm_attribs)) {
		wlr_log(WLR_INFO, "ext-copy-capture: attached buffer is SHM "
			"%dx%d fmt=0x%x stride=%d", buffer->width, buffer->height, shm_attribs.format,
				shm_attribs.stride);
	} else {
		wlr_log(WLR_INFO, "ext-copy-capture: attached buffer type is "
			"UNKNOWN %dx%d", buffer->width, buffer->height);
	}

	if (frame->buffer)
		wlr_buffer_unlock(frame->buffer);
	frame->buffer = buffer;
	wlr_buffer_lock(frame->buffer);
}

static void frame_handle_damage_buffer(struct wl_client *wl_client,
		struct wl_resource *frame_resource, int32_t x, int32_t y, int32_t width, int32_t height) {
	(void)wl_client;
	copy_frame_t *frame = frame_from_resource(frame_resource);
	if (!frame)
		return;

	if (x < 0 || y < 0 || width <= 0 || height <= 0) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE, "invalid buffer damage");
		return;
	}

	pixman_region32_union_rect(&frame->buffer_damage, &frame->buffer_damage, x, y, width, height);
}

static bool perform_output_capture(copy_frame_t *frame, image_copy_source_t *src) {
	struct wlr_output *output = src->output;

	struct blocked_node_state blocked_states[MAX_BLOCKED_WINDOWS];
	int nblocked = disable_blocked_windows(blocked_states, MAX_BLOCKED_WINDOWS);
	bool ok = false;

	wlr_log(WLR_DEBUG, "ext-copy-capture: blocked windows found=%d", nblocked);

	if (nblocked > 0 && src->last_buffer) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: invalidating cached buffer");
		wlr_buffer_unlock(src->last_buffer);
		src->last_buffer = NULL;
	}

	if (!src->last_buffer) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: no buffer, rendering fresh frame");
		struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene, output);
		if (!scene_output) {
			wlr_log(WLR_DEBUG, "ext-copy-capture: no scene output");
			goto out;
		}
		struct wlr_output_state tmp_state;
		wlr_output_state_init(&tmp_state);
		wlr_output_state_set_enabled(&tmp_state, true);
		if (!wlr_scene_output_build_state(scene_output, &tmp_state, NULL)) {
			wlr_log(WLR_DEBUG, "ext-copy-capture: scene build failed");
			wlr_output_state_finish(&tmp_state);
			goto out;
		}
		src->last_buffer = tmp_state.buffer;
		wlr_buffer_lock(src->last_buffer);
		wlr_output_state_finish(&tmp_state);
		wlr_log(WLR_DEBUG, "ext-copy-capture: fresh render OK, buffer=%p (%dx%d)",
			(void *)src->last_buffer, src->last_buffer->width, src->last_buffer->height);
	}

	int phys_w = src->last_buffer->width;
	int phys_h = src->last_buffer->height;

	struct wlr_texture *texture = wlr_texture_from_buffer(output->renderer, src->last_buffer);
	if (!texture) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: failed to create texture");
		goto out;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: texture=%p phys=%dx%d logical=%dx%d", (void *)texture, phys_w,
		phys_h, src->base.width, src->base.height);

	// try rendering directly to client buffer
	const pixman_region32_t *clip = !pixman_region32_empty(&frame->buffer_damage) ?
		&frame->buffer_damage : NULL;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(output->renderer, frame->buffer,
		NULL);
	if (pass) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: direct render pass started");
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.clip = clip,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			.dst_box = {
				.width = src->base.width,
				.height = src->base.height,
			},
			.src_box = {
				.width = phys_w,
				.height = phys_h,
			},
		});

		wlr_render_pass_submit(pass);
		wlr_texture_destroy(texture);
		wlr_log(WLR_DEBUG, "ext-copy-capture: direct render completed OK");
		ok = true;
		goto out;
	}

	wlr_log(WLR_INFO, "ext-copy-capture: direct render FAILED, "
		"falling back to SHM read-pixels path");

	uint32_t dst_format;
	size_t dst_stride;
	void *dst_data;

	if (!wlr_buffer_begin_data_ptr_access(frame->buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &dst_data,
			&dst_format, &dst_stride)) {
		wlr_texture_destroy(texture);
		wlr_log(WLR_DEBUG, "ext-copy-capture: client buffer not writable");
		goto out;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: client fmt=0x%x stride=%zu "
		"size=%dx%d", dst_format, dst_stride, phys_w, phys_h);

	if (!wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
		.data = dst_data,
		.format = dst_format,
		.stride = (uint32_t)dst_stride,
		.src_box = {
			.width = phys_w,
			.height = phys_h
		},
	})) {
		wlr_buffer_end_data_ptr_access(frame->buffer);
		wlr_texture_destroy(texture);
		wlr_log(WLR_DEBUG, "ext-copy-capture: wlr_texture_read_pixels failed");
		goto out;
	}

	wlr_texture_destroy(texture);

	wlr_log(WLR_DEBUG, "ext-copy-capture: SHM path completed OK");
	wlr_buffer_end_data_ptr_access(frame->buffer);
	ok = true;

out:
	restore_blocked_windows(blocked_states, nblocked);
	if (!ok)
		return false;

	ext_image_copy_capture_frame_v1_send_transform(frame->resource, WL_OUTPUT_TRANSFORM_NORMAL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	time_t tv_sec = now.tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? tv_sec >> 32 : 0;
	uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
	ext_image_copy_capture_frame_v1_send_presentation_time(frame->resource, tv_sec_hi, tv_sec_lo,
		now.tv_nsec);

	ext_image_copy_capture_frame_v1_send_ready(frame->resource);
	return true;
}

typedef struct capture_renderer_t {
	struct wlr_backend backend;
	struct wlr_output output;
	struct wlr_scene_output *scene_output;
} capture_renderer_t;

static size_t last_capture_output_num = 0;

static bool capture_output_test(struct wlr_output *output, const struct wlr_output_state *state) {
	(void)output;
	uint32_t supported = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
		WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;
	return (state->committed & ~supported) == 0;
}

static bool capture_output_commit(struct wlr_output *output, const struct wlr_output_state *state) {
	(void)output;
	if ((state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled)
		return true;

	if (!(state->committed & WLR_OUTPUT_STATE_BUFFER)) {
		wlr_log(WLR_DEBUG, "capture output commit: missing buffer");
		return false;
	}

	return true;
}

static const struct wlr_output_impl capture_output_impl = {
	.test = capture_output_test,
	.commit = capture_output_commit,
};

static const struct wlr_backend_impl capture_backend_impl = {0};

static bool capture_renderer_init(capture_renderer_t *r, struct wlr_scene *scene,
		struct wl_event_loop *loop, struct wlr_allocator *allocator, struct wlr_renderer *renderer) {
	wlr_backend_init(&r->backend, &capture_backend_impl);
	r->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

	wlr_output_init(&r->output, &r->backend, &capture_output_impl, loop, NULL);

	size_t n = ++last_capture_output_num;
	char name[64];
	snprintf(name, sizeof(name), "DOORS-CAPTURE-%zu", n);
	wlr_output_set_name(&r->output, name);

	if (!wlr_output_init_render(&r->output, allocator, renderer)) {
		wlr_log(WLR_ERROR, "capture_renderer: failed to init render");
		wlr_output_finish(&r->output);
		wlr_backend_finish(&r->backend);
		return false;
	}

	r->scene_output = wlr_scene_output_create(scene, &r->output);
	if (!r->scene_output) {
		wlr_log(WLR_ERROR, "capture_renderer: failed to create scene output");
		wlr_output_finish(&r->output);
		wlr_backend_finish(&r->backend);
		return false;
	}

	return true;
}

static void capture_renderer_finish(capture_renderer_t *r) {
	if (r->scene_output) {
		wlr_scene_output_destroy(r->scene_output);
		r->scene_output = NULL;
	}
	wlr_output_finish(&r->output);
	wlr_backend_finish(&r->backend);
}

void capture_renderer_destroy(void *raw) {
	capture_renderer_t *r = raw;
	if (!r)
		return;
	capture_renderer_finish(r);
	free(r);
}

static bool capture_renderer_render(capture_renderer_t *r, int width, int height,
		struct wlr_buffer **out_buffer) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, width, height, 0);

	if (!wlr_scene_output_build_state(r->scene_output, &state, NULL)) {
		wlr_log(WLR_DEBUG, "capture_renderer: scene build failed");
		wlr_output_state_finish(&state);
		return false;
	}

	if (!wlr_output_commit_state(&r->output, &state)) {
		wlr_log(WLR_DEBUG, "capture_renderer: output commit failed");
		wlr_output_state_finish(&state);
		return false;
	}

	struct wlr_buffer *buf = state.buffer;
	wlr_buffer_lock(buf);
	wlr_output_state_finish(&state);
	*out_buffer = buf;
	return true;
}

static bool copy_dmabuf_to_frame(struct wlr_buffer *dst, struct wlr_buffer *src,
		struct wlr_renderer *renderer, const pixman_region32_t *clip) {
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, src);
	if (!texture)
		return false;

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, dst, NULL);
	if (!pass) {
		wlr_texture_destroy(texture);
		return false;
	}

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.clip = clip,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});

	bool ok = wlr_render_pass_submit(pass);
	wlr_texture_destroy(texture);
	return ok;
}

static bool copy_shm_to_frame(struct wlr_buffer *dst, struct wlr_buffer *src,
		struct wlr_renderer *renderer) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format,
		&stride))
		return false;

	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, src);
	if (!texture) {
		wlr_buffer_end_data_ptr_access(dst);
		return false;
	}

	bool ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = format,
		.stride = (uint32_t)stride,
	});

	wlr_texture_destroy(texture);
	wlr_buffer_end_data_ptr_access(dst);
	return ok;
}

static bool copy_buffer_to_frame(copy_frame_t *frame, struct wlr_buffer *src,
		struct wlr_renderer *renderer) {
	struct wlr_buffer *dst = frame->buffer;

	if (src->width != dst->width || src->height != dst->height)
		return false;

	const pixman_region32_t *clip = !pixman_region32_empty(&frame->buffer_damage) ?
		&frame->buffer_damage : NULL;

	struct wlr_dmabuf_attributes dmabuf;
	if (wlr_buffer_get_dmabuf(dst, &dmabuf))
		return copy_dmabuf_to_frame(dst, src, renderer, clip);

	return copy_shm_to_frame(dst, src, renderer);
}

static bool perform_scene_node_capture(copy_frame_t *frame,
		struct wlr_ext_image_capture_source_v1 *source, struct wlr_scene *capture_scene, bool block_out,
		void **capture_renderer_ptr) {
	if (!capture_scene) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: no capture scene");
		return false;
	}

	capture_renderer_t *r = *capture_renderer_ptr;
	if (!r) {
		r = calloc(1, sizeof(*r));
		if (!r || !capture_renderer_init(r, capture_scene, wl_display_get_event_loop(server.wl_display),
				server.allocator, server.renderer)) {
			free(r);
			wlr_log(WLR_DEBUG, "ext-copy-capture: failed to init renderer");
			return false;
		}
		*capture_renderer_ptr = r;
	}

	struct blocked_node_state blocked_states[MAX_BLOCKED_WINDOWS];
	int nblocked = disable_blocked_windows(blocked_states, MAX_BLOCKED_WINDOWS);

	if (block_out) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: blocking out toplevel");
	}

	struct wlr_buffer *source_buf = NULL;
	if (!capture_renderer_render(r, (int)source->width, (int)source->height, &source_buf)) {
		restore_blocked_windows(blocked_states, nblocked);
		return false;
	}

	bool ok = copy_buffer_to_frame(frame, source_buf, server.renderer);
	wlr_buffer_unlock(source_buf);

	restore_blocked_windows(blocked_states, nblocked);

	if (!ok)
		return false;

	ext_image_copy_capture_frame_v1_send_transform(frame->resource, WL_OUTPUT_TRANSFORM_NORMAL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	time_t tv_sec = now.tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? tv_sec >> 32 : 0;
	uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
	ext_image_copy_capture_frame_v1_send_presentation_time(frame->resource, tv_sec_hi, tv_sec_lo,
		now.tv_nsec);

	ext_image_copy_capture_frame_v1_send_ready(frame->resource);
	return true;
}

static void frame_handle_capture(struct wl_client *wl_client, struct wl_resource *frame_resource) {
	(void)wl_client;
	copy_frame_t *frame = frame_from_resource(frame_resource);
	if (!frame)
		return;

	if (!frame->buffer) {
		wl_resource_post_error(frame->resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			"capture without buffer");
		return;
	}
	if (frame->capturing) {
		wl_resource_post_error(frame->resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
			"already captured");
		return;
	}

	frame->capturing = true;

	copy_session_t *session = frame->session;
	struct wlr_ext_image_capture_source_v1 *source = session->source;

	wlr_log(WLR_DEBUG, "ext-copy-capture: frame_handle_capture source=%p impl=%p", (void *)source,
		source ? (void *)source->impl : NULL);

	if (!source) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: source is NULL, sending stopped");
		ext_image_copy_capture_frame_v1_send_failed(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		session->frame = NULL;
		frame_destroy(frame);
		return;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: checking output source path");
	if (wlr_output_try_from_ext_image_capture_source_v1(source) || source->impl == &source_impl) {
		wlr_log(WLR_DEBUG, "ext-copy-capture: source is output-backed, calling perform_output_capture");
		image_copy_source_t *img_src = source_from_base(source);
		if (perform_output_capture(frame, img_src)) {
			session->frame = NULL;
			frame_destroy(frame);
			return;
		}
		wlr_log(WLR_DEBUG, "ext-copy-capture: perform_output_capture failed");
	} else {
		wlr_log(WLR_DEBUG, "ext-copy-capture: source is NOT output-backed");
	}

	// scene-node source
	wlr_log(WLR_DEBUG, "ext-copy-capture: checking toplevel sources");
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->image_capture_source != source)
			continue;

		wlr_log(WLR_DEBUG, "ext-copy-capture: found matching toplevel %p, "
			"calling perform_scene_node_capture", (void *)tl);
		bool block_out = tl->node && tl->node->client &&
			tl->node->client->flags.block_out_from_screenshare;

		if (perform_scene_node_capture(frame, source, tl->image_capture, block_out,
				&tl->capture_renderer)) {
			session->frame = NULL;
			frame_destroy(frame);
			return;
		}

		wlr_log(WLR_DEBUG, "ext-copy-capture: perform_scene_node_capture failed");
		break;
	}

	xwayland_toplevel_t *xw;
	wl_list_for_each(xw, &server.xwayland.views, link) {
		if (xw->image_capture_source != source)
			continue;

		wlr_log(WLR_DEBUG, "ext-copy-capture: found matching xwayland view %p, "
			"calling perform_scene_node_capture", (void *)xw);
		bool block_out = xw->node && xw->node->client &&
			xw->node->client->flags.block_out_from_screenshare;

		if (perform_scene_node_capture(frame, source, xw->image_capture, block_out,
				&xw->capture_renderer)) {
			session->frame = NULL;
			frame_destroy(frame);
			return;
		}
		wlr_log(WLR_DEBUG, "ext-copy-capture: perform_scene_node_capture failed");
		break;
	}

	wlr_log(WLR_DEBUG, "ext-copy-capture: no matching source found, sending failed");
	ext_image_copy_capture_frame_v1_send_failed(frame->resource,
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
	session->frame = NULL;
	frame_destroy(frame);
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
	.destroy = frame_handle_destroy,
	.attach_buffer = frame_handle_attach_buffer,
	.damage_buffer = frame_handle_damage_buffer,
	.capture = frame_handle_capture,
};

copy_frame_t *frame_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &ext_image_copy_capture_frame_v1_interface, &frame_impl));
	return wl_resource_get_user_data(resource);
}

static void session_source_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	copy_session_t *session = wl_container_of(listener, session, source_destroy);
	session->source = NULL;
	wl_list_remove(&session->source_destroy.link);
	wl_list_init(&session->source_destroy.link);
}

static void session_destroy(copy_session_t *session) {
	if (!session)
		return;

	wl_list_remove(&session->source_destroy.link);
	if (session->source)
		session->source = NULL;

	wl_resource_set_user_data(session->resource, NULL);
	free(session);
}

static void session_handle_resource_destroy(struct wl_resource *resource) {
	copy_session_t *session = session_from_resource(resource);
	if (session)
		session_destroy(session);
}

static void session_handle_create_frame(struct wl_client *wl_client,
		struct wl_resource *session_resource, uint32_t id) {
	copy_session_t *session = session_from_resource(session_resource);
	if (!session)
		return;

	if (session->frame) {
		wl_resource_post_error(session->resource, EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
			"duplicate frame");
		return;
	}

	copy_frame_t *frame = calloc(1, sizeof(*frame));
	if (!frame) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	frame->resource = wl_resource_create(wl_client, &ext_image_copy_capture_frame_v1_interface,
		wl_resource_get_version(session_resource), id);
	if (!frame->resource) {
		free(frame);
		wl_client_post_no_memory(wl_client);
		return;
	}

	frame->session = session;
	session->frame = frame;
	pixman_region32_init(&frame->buffer_damage);

	wl_resource_set_implementation(frame->resource, &frame_impl, frame, frame_handle_resource_destroy);
}

static void session_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *session_resource) {
	(void)wl_client;
	wl_resource_destroy(session_resource);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
	.create_frame = session_handle_create_frame,
	.destroy = session_handle_destroy,
};

copy_session_t *session_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &ext_image_copy_capture_session_v1_interface,
		&session_impl));
	return wl_resource_get_user_data(resource);
}

static uint32_t drm_format_to_wl_shm(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_ABGR8888:
		return WL_SHM_FORMAT_ABGR8888;
	default:
		return fmt;
	}
}

static void send_buffer_constraints(copy_session_t *session,
		struct wlr_ext_image_capture_source_v1 *source) {
	if (!source)
		return;

	ext_image_copy_capture_session_v1_send_buffer_size(session->resource, source->width,
		source->height);

	if (source->shm_formats_len) {
		for (size_t i = 0; i < source->shm_formats_len; i++)
			ext_image_copy_capture_session_v1_send_shm_format(session->resource,
				drm_format_to_wl_shm(source->shm_formats[i]));
	} else {
		ext_image_copy_capture_session_v1_send_shm_format(session->resource, WL_SHM_FORMAT_ARGB8888);
	}

	if (source->impl == &source_impl && source->dmabuf_formats.len > 0) {
		struct wl_array dev_id_array = {
			.data = &source->dmabuf_device,
			.size = sizeof(source->dmabuf_device),
		};
		ext_image_copy_capture_session_v1_send_dmabuf_device(session->resource, &dev_id_array);

		for (size_t i = 0; i < source->dmabuf_formats.len; i++) {
			const struct wlr_drm_format *fmt = &source->dmabuf_formats.formats[i];
			struct wl_array modifiers_array = {
				.data = fmt->modifiers,
				.size = fmt->len * sizeof(fmt->modifiers[0]),
			};
			ext_image_copy_capture_session_v1_send_dmabuf_format(session->resource, fmt->format,
				&modifiers_array);
		}
	}

	ext_image_copy_capture_session_v1_send_done(session->resource);
}

static void mgr_handle_create_session(struct wl_client *wl_client, struct wl_resource *mgr_resource,
		uint32_t id, struct wl_resource *source_resource, uint32_t options) {
	(void)options;
	(void)mgr_resource;

	struct wlr_ext_image_capture_source_v1 *wlr_source =
		wlr_ext_image_capture_source_v1_from_resource(source_resource);
	if (!wlr_source)
		return;

	copy_session_t *session = calloc(1, sizeof(*session));
	if (!session) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	session->resource = wl_resource_create(wl_client, &ext_image_copy_capture_session_v1_interface,
		wl_resource_get_version(source_resource), id);
	if (!session->resource) {
		free(session);
		wl_client_post_no_memory(wl_client);
		return;
	}

	session->source = wlr_source;
	wl_resource_set_implementation(session->resource, &session_impl, session,
		session_handle_resource_destroy);

	session->source_destroy.notify = session_source_destroy;
	wl_signal_add(&wlr_source->events.destroy, &session->source_destroy);

	wlr_log(WLR_DEBUG, "ext-copy-capture: create_session source=%p impl=%p "
		"width=%d height=%d shm_fmts=%zu", (void *)wlr_source, (void *)wlr_source->impl, wlr_source->width,
			wlr_source->height, wlr_source->shm_formats_len);

	if (wlr_source->impl->start) {
		wlr_source->impl->start(wlr_source,
			options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
	}

	send_buffer_constraints(session, wlr_source);
}

static void mgr_handle_create_pointer_cursor_session(struct wl_client *wl_client,
		struct wl_resource *mgr_resource, uint32_t id, struct wl_resource *source_resource,
		struct wl_resource *pointer_resource) {
	(void)mgr_resource;
	(void)source_resource;
	(void)pointer_resource;
	copy_session_t *session = calloc(1, sizeof(*session));
	if (!session) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	session->resource = wl_resource_create(wl_client, &ext_image_copy_capture_session_v1_interface, 1,
		id);
	if (!session->resource) {
		free(session);
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(session->resource, &session_impl, session,
		session_handle_resource_destroy);

	ext_image_copy_capture_session_v1_send_stopped(session->resource);
}

static void mgr_handle_destroy(struct wl_client *wl_client, struct wl_resource *mgr_resource) {
	(void)wl_client;
	wl_resource_destroy(mgr_resource);
}

static const struct ext_image_copy_capture_manager_v1_interface copy_mgr_impl = {
	.create_session = mgr_handle_create_session,
	.create_pointer_cursor_session = mgr_handle_create_pointer_cursor_session,
	.destroy = mgr_handle_destroy,
};

static void copy_mgr_bind(struct wl_client *wl_client, void *data, uint32_t version, uint32_t id) {
	copy_mgr_t *mgr = data;
	struct wl_resource *resource = wl_resource_create(wl_client,
		&ext_image_copy_capture_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &copy_mgr_impl, mgr, NULL);
}

static void copy_mgr_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	copy_mgr_t *mgr = wl_container_of(listener, mgr, display_destroy);
	wl_list_remove(&mgr->display_destroy.link);
	wl_global_destroy(mgr->global);
	free(mgr);
}

static output_capture_mgr_t *output_mgr = NULL;
static copy_mgr_t *copy_mgr = NULL;

void image_copy_capture_init(void) {
	output_mgr = calloc(1, sizeof(*output_mgr));
	if (!output_mgr)
		goto err_out;

	output_mgr->global = wl_global_create(server.wl_display,
		&ext_output_image_capture_source_manager_v1_interface, 1, output_mgr, output_mgr_bind);
	if (!output_mgr->global)
		goto err_out;

	output_mgr->display_destroy.notify = output_mgr_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &output_mgr->display_destroy);

	copy_mgr = calloc(1, sizeof(*copy_mgr));
	if (!copy_mgr)
		goto err_out;

	copy_mgr->global = wl_global_create(server.wl_display, &ext_image_copy_capture_manager_v1_interface,
		1, copy_mgr, copy_mgr_bind);
	if (!copy_mgr->global)
		goto err_out;

	copy_mgr->display_destroy.notify = copy_mgr_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &copy_mgr->display_destroy);

	wlr_log(WLR_INFO, "ext-image-copy-capture initialized");
	return;

err_out:
	wlr_log(WLR_ERROR, "Failed to initialize ext-image-copy-capture");
	image_copy_capture_fini();
}

void image_copy_capture_fini(void) {
	if (output_mgr) {
		wl_list_remove(&output_mgr->display_destroy.link);
		if (output_mgr->global)
			wl_global_destroy(output_mgr->global);
		free(output_mgr);
		output_mgr = NULL;
	}
	if (copy_mgr) {
		wl_list_remove(&copy_mgr->display_destroy.link);
		if (copy_mgr->global)
			wl_global_destroy(copy_mgr->global);
		free(copy_mgr);
		copy_mgr = NULL;
	}
}

struct wl_global *image_copy_capture_get_global(void) {
	return copy_mgr ? copy_mgr->global : NULL;
}

struct wl_global *image_capture_source_get_global(void) {
	return output_mgr ? output_mgr->global : NULL;
}
