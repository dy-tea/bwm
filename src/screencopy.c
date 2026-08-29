#include "once.h"
#include "output.h"
#include "screencopy.h"
#include "server.h"
#include "toplevel.h"
#include "types.h"
#include "wlr-screencopy-unstable-v1-protocol.h"
#include "xwayland.h"
#include <assert.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

#define SCREENCOPY_MANAGER_VERSION 3

typedef struct screencopy_frame_t {
	struct wl_resource *resource;
	struct wl_list link;

	struct wlr_output *output;
	struct wlr_box box;

	bool overlay_cursor;
	bool cursor_locked;
	bool with_damage;

	enum wlr_buffer_cap buffer_cap;
	struct wlr_buffer *buffer;

	uint32_t shm_format;
	uint32_t dmabuf_format;
	int shm_stride;

	struct wl_listener output_commit;
	struct wl_listener output_destroy;

	struct screencopy_client_t *client;
	struct screencopy_mgr_t *manager;
} screencopy_frame_t;

typedef struct screencopy_mgr_t {
	struct wl_global *global;
	struct wl_list frames;

	struct {
		struct wl_listener display_destroy;
	};
} screencopy_mgr_t;

typedef struct screencopy_damage_t {
	struct wl_list link;
	struct wlr_output *output;
	pixman_region32_t damage;
	struct wl_listener output_precommit;
	struct wl_listener output_destroy;
} screencopy_damage_t;

typedef struct screencopy_client_t {
	int ref;
	struct wl_list damages;
	struct screencopy_mgr_t *manager;
} screencopy_client_t;

static const struct zwlr_screencopy_frame_v1_interface frame_impl;

static void client_unref(screencopy_client_t *client);

static screencopy_frame_t *frame_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwlr_screencopy_frame_v1_interface, &frame_impl));
	return wl_resource_get_user_data(resource);
}

static void frame_destroy(screencopy_frame_t *frame) {
	if (frame == NULL)
		return;

	if (frame->output != NULL && frame->buffer != NULL) {
		wlr_output_lock_attach_render(frame->output, false);
		if (frame->cursor_locked)
			wlr_output_lock_software_cursors(frame->output, false);
	}

	wl_list_remove(&frame->link);
	wl_list_remove(&frame->output_commit.link);
	wl_list_remove(&frame->output_destroy.link);
	wl_resource_set_user_data(frame->resource, NULL);

	if (frame->buffer)
		wlr_buffer_unlock(frame->buffer);

	if (frame->client)
		client_unref(frame->client);

	free(frame);
}

static void frame_send_ready(screencopy_frame_t *frame, struct timespec *when) {
	time_t tv_sec = when->tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? tv_sec >> 32 : 0;
	uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
	zwlr_screencopy_frame_v1_send_ready(frame->resource, tv_sec_hi, tv_sec_lo, when->tv_nsec);
}

static void screencopy_damage_destroy(screencopy_damage_t *damage) {
	wl_list_remove(&damage->output_destroy.link);
	wl_list_remove(&damage->output_precommit.link);
	wl_list_remove(&damage->link);
	pixman_region32_fini(&damage->damage);
	free(damage);
}

static void screencopy_damage_handle_output_precommit(struct wl_listener *listener, void *data) {
	screencopy_damage_t *damage = wl_container_of(listener, damage, output_precommit);
	const struct wlr_output_event_precommit *event = data;

	if (event->state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		pixman_region32_union(&damage->damage, &damage->damage, &event->state->damage);
		pixman_region32_intersect_rect(&damage->damage, &damage->damage, 0, 0, damage->output->width,
			damage->output->height);
	} else if (event->state->committed & WLR_OUTPUT_STATE_BUFFER) {
		pixman_region32_union_rect(&damage->damage, &damage->damage, 0, 0, damage->output->width,
			damage->output->height);
	}
}

static void screencopy_damage_handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	screencopy_damage_t *damage = wl_container_of(listener, damage, output_destroy);
	screencopy_damage_destroy(damage);
}

static screencopy_damage_t *screencopy_damage_create(screencopy_client_t *client,
		struct wlr_output *output) {
	screencopy_damage_t *damage = calloc(1, sizeof(*damage));
	if (!damage)
		return NULL;

	damage->output = output;
	pixman_region32_init_rect(&damage->damage, 0, 0, output->width, output->height);
	wl_list_insert(&client->damages, &damage->link);

	damage->output_precommit.notify = screencopy_damage_handle_output_precommit;
	wl_signal_add(&output->events.precommit, &damage->output_precommit);

	damage->output_destroy.notify = screencopy_damage_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &damage->output_destroy);

	return damage;
}

static screencopy_damage_t *screencopy_damage_find(screencopy_client_t *client,
		struct wlr_output *output) {
	screencopy_damage_t *damage;
	wl_list_for_each(damage, &client->damages, link) {
		if (damage->output == output)
			return damage;
	}
	return NULL;
}

static screencopy_damage_t *screencopy_damage_get_or_create(screencopy_client_t *client,
		struct wlr_output *output) {
	screencopy_damage_t *damage = screencopy_damage_find(client, output);
	return damage ? damage : screencopy_damage_create(client, output);
}

static void client_unref(screencopy_client_t *client) {
	assert(client->ref > 0);

	if (--client->ref != 0)
		return;

	screencopy_damage_t *damage, *tmp;
	wl_list_for_each_safe(damage, tmp, &client->damages, link)
		screencopy_damage_destroy(damage);

	free(client);
}

static void block_out_window(toplevel_t *tl, struct wlr_render_pass *pass,
		struct wlr_output *output) {
	if (!tl->node || !tl->node->client) {
		wlr_log(WLR_DEBUG, "block_out: no node/client for toplevel %p", (void *)tl);
		return;
	}

	client_t *c = tl->node->client;
	if (!c->flags.block_out_from_screenshare) {
		wlr_log(WLR_DEBUG, "block_out: %s not blocked", c->title);
		return;
	}
	if (!c->flags.shown && c->state != STATE_FULLSCREEN) {
		wlr_log(WLR_DEBUG, "block_out: %s not shown (state=%d)", c->title, c->state);
		return;
	}

	output_t *o = output_from_wlr_output(output);
	if (!o) {
		wlr_log(WLR_DEBUG, "block_out: no output_t for wlr_output %p", (void *)output);
		return;
	}

	struct wlr_box win_rect = {0};
	switch (c->state) {
	case STATE_TILED:
	case STATE_PSEUDO_TILED:
		win_rect = c->tiled_rectangle;
		break;
	case STATE_FLOATING:
		win_rect = c->floating_rectangle;
		break;
	case STATE_FULLSCREEN:
		win_rect = o->rectangle;
		break;
	}

	if (win_rect.width == 0 || win_rect.height == 0)
		return;

	int abs_x, abs_y;
	wlr_scene_node_coords(&tl->scene_tree->node, &abs_x, &abs_y);

	int bw = (int)settings.border_width;
	float scale = o->wlr_output->scale;
	int buf_x = (abs_x - o->rectangle.x - bw) * scale;
	int buf_y = (abs_y - o->rectangle.y - bw) * scale;
	int buf_w = (win_rect.width + 2 * bw) * scale;
	int buf_h = (win_rect.height + 2 * bw) * scale;

	wlr_log(WLR_DEBUG, "block_out_window: x=%d y=%d w=%d h=%d"
		" bw=%d scale=%f buf_x=%d buf_y=%d buf_w=%d buf_h=%d", win_rect.x, win_rect.y, win_rect.width,
			win_rect.height, bw, scale, buf_x, buf_y, buf_w, buf_h);

	struct wlr_box block_box = {
		.x = buf_x,
		.y = buf_y,
		.width = buf_w,
		.height = buf_h,
	};

	enum wl_output_transform transform = wlr_output_transform_invert(output->transform);
	if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		(void)transform;
		wlr_log(WLR_DEBUG, "screencopy block-out: skipped window on transformed output");
		return;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = block_box,
		.color = {0, 0, 0, 1},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
}

static void block_out_xwayland_window(xwayland_toplevel_t *view, struct wlr_render_pass *pass,
		struct wlr_output *output) {
	if (!view->node || !view->node->client)
		return;

	client_t *c = view->node->client;
	if (!c->flags.block_out_from_screenshare)
		return;
	if (!c->flags.shown && c->state != STATE_FULLSCREEN)
		return;

	output_t *o = output_from_wlr_output(output);
	if (!o)
		return;

	struct wlr_box win_rect = {0};
	switch (c->state) {
	case STATE_TILED:
	case STATE_PSEUDO_TILED:
		win_rect = c->tiled_rectangle;
		break;
	case STATE_FLOATING:
		win_rect = c->floating_rectangle;
		break;
	case STATE_FULLSCREEN:
		win_rect = o->rectangle;
		break;
	}

	if (win_rect.width == 0 || win_rect.height == 0)
		return;

	int abs_x, abs_y;
	wlr_scene_node_coords(&view->scene_tree->node, &abs_x, &abs_y);

	int bw = (int)settings.border_width;
	int buf_x = (abs_x - o->rectangle.x - bw) * output->scale;
	int buf_y = (abs_y - o->rectangle.y - bw) * output->scale;
	int buf_w = (win_rect.width + 2 * bw) * output->scale;
	int buf_h = (win_rect.height + 2 * bw) * output->scale;

	struct wlr_box block_box = {
		.x = buf_x,
		.y = buf_y,
		.width = buf_w,
		.height = buf_h,
	};

	enum wl_output_transform transform = wlr_output_transform_invert(output->transform);
	if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		(void)transform;
		wlr_log(WLR_DEBUG, "screencopy block-out: skipped xwayland window on transformed output");
		return;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = block_box,
		.color = {0, 0, 0, 1},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
}

static void block_out_windows(struct wlr_render_pass *pass, struct wlr_output *output) {
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		block_out_window(tl, pass, output);
	}

	xwayland_toplevel_t *xw;
	wl_list_for_each(xw, &server.xwayland.views, link) {
		block_out_xwayland_window(xw, pass, output);
	}
}

static void frame_handle_output_commit(struct wl_listener *listener, void *data) {
	screencopy_frame_t *frame = wl_container_of(listener, frame, output_commit);
	struct wlr_output_event_commit *event = data;
	struct wlr_output *output = frame->output;

	if ((event->state->committed & WLR_OUTPUT_STATE_ENABLED) && !output->enabled)
		goto err;
	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER))
		return;
	if (!frame->buffer)
		return;

	if (frame->with_damage) {
		screencopy_damage_t *damage = screencopy_damage_get_or_create(frame->client, output);
		if (damage && pixman_region32_empty(&damage->damage))
			return;
	}

	wl_list_remove(&frame->output_commit.link);
	wl_list_init(&frame->output_commit.link);

	struct wlr_buffer *src_buffer = event->state->buffer;
	if (frame->box.x < 0 || frame->box.y < 0 || frame->box.x + frame->box.width > src_buffer->width ||
			frame->box.y + frame->box.height > src_buffer->height) {
		goto err;
	}

	struct wlr_texture *texture = wlr_texture_from_buffer(output->renderer, src_buffer);
	if (!texture) {
		wlr_log(WLR_DEBUG, "Failed to create texture from source buffer");
		goto err;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(output->renderer, frame->buffer,
		NULL);
	if (!pass) {
		wlr_texture_destroy(texture);
		goto err;
	}

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		.dst_box = {
			.width = frame->box.width,
			.height = frame->box.height,
		},
		.src_box = {
			.x = frame->box.x,
			.y = frame->box.y,
			.width = frame->box.width,
			.height = frame->box.height,
		},
	});

	wlr_log(WLR_DEBUG, "screencopy: calling block_out_windows before submit");
	block_out_windows(pass, output);

	bool ok = wlr_render_pass_submit(pass);
	wlr_texture_destroy(texture);

	wlr_log(WLR_DEBUG, "screencopy: output commit render pass ok=%d", ok);

	if (!ok) {
		wlr_log(WLR_DEBUG, "Failed to render to destination buffer");
		goto err;
	}

	zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);

	if (frame->with_damage) {
		screencopy_damage_t *damage = screencopy_damage_get_or_create(frame->client, output);
		if (damage) {
			int n_boxes;
			const pixman_box32_t *boxes = pixman_region32_rectangles(&damage->damage, &n_boxes);
			for (int i = 0; i < n_boxes; i++) {
				const pixman_box32_t *box = &boxes[i];
				zwlr_screencopy_frame_v1_send_damage(frame->resource, box->x1, box->y1, box->x2 - box->x1,
					box->y2 - box->y1);
			}
			pixman_region32_clear(&damage->damage);
		}
	}

	frame_send_ready(frame, &event->when);
	frame_destroy(frame);
	return;

err:
	zwlr_screencopy_frame_v1_send_failed(frame->resource);
	frame_destroy(frame);
}

static void frame_handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	screencopy_frame_t *frame = wl_container_of(listener, frame, output_destroy);
	zwlr_screencopy_frame_v1_send_failed(frame->resource);
	frame_destroy(frame);
}

static void frame_handle_copy(struct wl_client *wl_client, struct wl_resource *frame_resource,
		struct wl_resource *buffer_resource) {
	(void)wl_client;
	screencopy_frame_t *frame = frame_from_resource(frame_resource);
	if (frame == NULL)
		return;

	struct wlr_output *output = frame->output;

	if (!output->enabled) {
		zwlr_screencopy_frame_v1_send_failed(frame->resource);
		frame_destroy(frame);
		return;
	}

	struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
	if (buffer == NULL) {
		wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"invalid buffer");
		return;
	}

	if (buffer->width != frame->box.width || buffer->height != frame->box.height) {
		wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"invalid buffer dimensions");
		return;
	}

	if (frame->buffer != NULL) {
		wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
			"frame already used");
		return;
	}

	enum wlr_buffer_cap cap;
	struct wlr_dmabuf_attributes dmabuf;
	void *data;
	uint32_t format;
	size_t stride;
	if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		cap = WLR_BUFFER_CAP_DMABUF;
		if (dmabuf.format != frame->dmabuf_format) {
			wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"invalid buffer format");
			return;
		}
	} else if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data,
			&format, &stride)) {
		wlr_buffer_end_data_ptr_access(buffer);
		cap = WLR_BUFFER_CAP_DATA_PTR;
		if (format != frame->shm_format) {
			wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"invalid buffer format");
			return;
		}
		if (stride != (size_t)frame->shm_stride) {
			wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
				"invalid buffer stride");
			return;
		}
	} else {
		wl_resource_post_error(frame->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
			"unsupported buffer type");
		return;
	}

	frame->buffer = buffer;
	frame->buffer_cap = cap;

	wl_signal_add(&output->events.commit, &frame->output_commit);
	frame->output_commit.notify = frame_handle_output_commit;

	wlr_output_update_needs_frame(output);

	wlr_output_lock_attach_render(output, true);
	if (frame->overlay_cursor) {
		wlr_output_lock_software_cursors(output, true);
		frame->cursor_locked = true;
	}
}

static void frame_handle_copy_with_damage(struct wl_client *wl_client,
		struct wl_resource *frame_resource, struct wl_resource *buffer_resource) {
	screencopy_frame_t *frame = frame_from_resource(frame_resource);
	if (frame == NULL)
		return;
	frame->with_damage = true;
	frame_handle_copy(wl_client, frame_resource, buffer_resource);
}

static void frame_handle_destroy(struct wl_client *wl_client, struct wl_resource *frame_resource) {
	(void)wl_client;
	wl_resource_destroy(frame_resource);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
	.copy = frame_handle_copy,
	.destroy = frame_handle_destroy,
	.copy_with_damage = frame_handle_copy_with_damage,
};

static void frame_handle_resource_destroy(struct wl_resource *frame_resource) {
	screencopy_frame_t *frame = frame_from_resource(frame_resource);
	frame_destroy(frame);
}

static void capture_output(struct wl_client *wl_client, screencopy_client_t *client,
		uint32_t version, uint32_t id, int32_t overlay_cursor, struct wlr_output *output,
		const struct wlr_box *box) {
	screencopy_frame_t *frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	frame->output = output;
	frame->overlay_cursor = !!overlay_cursor;

	frame->resource = wl_resource_create(wl_client, &zwlr_screencopy_frame_v1_interface, version, id);
	if (frame->resource == NULL) {
		free(frame);
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(frame->resource, &frame_impl, frame, frame_handle_resource_destroy);

	if (output == NULL || !output->enabled) {
		wl_resource_set_user_data(frame->resource, NULL);
		zwlr_screencopy_frame_v1_send_failed(frame->resource);
		free(frame);
		return;
	}

	screencopy_frame_t *existing;
	wl_list_for_each(existing, &client->manager->frames, link) {
		if (existing->client == client && existing->output == output) {
			wl_resource_set_user_data(frame->resource, NULL);
			zwlr_screencopy_frame_v1_send_failed(frame->resource);
			free(frame);
			return;
		}
	}

	frame->client = client;
	client->ref++;
	frame->manager = client->manager;
	wl_list_insert(&client->manager->frames, &frame->link);

	wl_list_init(&frame->output_commit.link);

	wl_signal_add(&output->events.destroy, &frame->output_destroy);
	frame->output_destroy.notify = frame_handle_output_destroy;

	struct wlr_renderer *renderer = output->renderer;
	assert(renderer);

	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain))
		goto error;

	struct wlr_buffer *buf = wlr_swapchain_acquire(output->swapchain);
	if (buf == NULL)
		goto error;

	struct wlr_texture *tex = wlr_texture_from_buffer(renderer, buf);
	wlr_buffer_unlock(buf);
	if (!tex)
		goto error;

	frame->shm_format = wlr_texture_preferred_read_format(tex);
	wlr_texture_destroy(tex);

	if (frame->shm_format == DRM_FORMAT_INVALID) {
		wlr_log(WLR_ERROR, "Failed to capture output: no read format supported by renderer");
		goto error;
	}

	if (output->allocator && (output->allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF))
		frame->dmabuf_format = output->render_format;
	else
		frame->dmabuf_format = DRM_FORMAT_INVALID;

	struct wlr_box buffer_box = {0};
	if (box == NULL) {
		buffer_box.width = output->width;
		buffer_box.height = output->height;
	} else {
		int ow, oh;
		wlr_output_effective_resolution(output, &ow, &oh);
		buffer_box = *box;
		wlr_box_transform(&buffer_box, &buffer_box, wlr_output_transform_invert(output->transform), ow,
			oh);
		buffer_box.x *= output->scale;
		buffer_box.y *= output->scale;
		buffer_box.width *= output->scale;
		buffer_box.height *= output->scale;
	}

	frame->box = buffer_box;
	frame->shm_stride = buffer_box.width * 4;

	uint32_t shm_format;
	switch (frame->shm_format) {
	case DRM_FORMAT_ARGB8888:
		shm_format = WL_SHM_FORMAT_ARGB8888;
		break;
	case DRM_FORMAT_XRGB8888:
		shm_format = WL_SHM_FORMAT_XRGB8888;
		break;
	case DRM_FORMAT_ABGR8888:
		shm_format = WL_SHM_FORMAT_ABGR8888;
		break;
	case DRM_FORMAT_XBGR8888:
		shm_format = WL_SHM_FORMAT_XBGR8888;
		break;
	default:
		shm_format = WL_SHM_FORMAT_XRGB8888;
		break;
	}

	zwlr_screencopy_frame_v1_send_buffer(frame->resource, shm_format, buffer_box.width,
		buffer_box.height, frame->shm_stride);

	if (version >= 3) {
		if (frame->dmabuf_format != DRM_FORMAT_INVALID) {
			zwlr_screencopy_frame_v1_send_linux_dmabuf(frame->resource, frame->dmabuf_format,
				buffer_box.width, buffer_box.height);
		}
		zwlr_screencopy_frame_v1_send_buffer_done(frame->resource);
	}

	return;

error:
	zwlr_screencopy_frame_v1_send_failed(frame->resource);
	frame_destroy(frame);
}

static void manager_handle_capture_output(struct wl_client *wl_client,
		struct wl_resource *manager_resource, uint32_t id, int32_t overlay_cursor,
		struct wl_resource *output_resource) {
	uint32_t version = wl_resource_get_version(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	screencopy_client_t *client = wl_resource_get_user_data(manager_resource);

	capture_output(wl_client, client, version, id, overlay_cursor, output, NULL);
}

static void manager_handle_capture_output_region(struct wl_client *wl_client,
		struct wl_resource *manager_resource, uint32_t id, int32_t overlay_cursor,
		struct wl_resource *output_resource, int32_t x, int32_t y, int32_t width, int32_t height) {
	uint32_t version = wl_resource_get_version(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);

	struct wlr_box box = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};

	screencopy_client_t *client = wl_resource_get_user_data(manager_resource);

	capture_output(wl_client, client, version, id, overlay_cursor, output, &box);
}

static void manager_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *manager_resource) {
	(void)wl_client;
	wl_resource_destroy(manager_resource);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
	.capture_output = manager_handle_capture_output,
	.capture_output_region = manager_handle_capture_output_region,
	.destroy = manager_handle_destroy,
};

static void manager_handle_resource_destroy(struct wl_resource *resource) {
	screencopy_client_t *client = wl_resource_get_user_data(resource);
	client_unref(client);
}

static void manager_bind(struct wl_client *wl_client, void *data, uint32_t version, uint32_t id) {
	screencopy_mgr_t *manager = data;

	screencopy_client_t *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	struct wl_resource *resource = wl_resource_create(wl_client, &zwlr_screencopy_manager_v1_interface,
		version, id);
	if (resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}

	client->ref = 1;
	client->manager = manager;
	wl_list_init(&client->damages);

	wl_resource_set_implementation(resource, &manager_impl, client, manager_handle_resource_destroy);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	screencopy_mgr_t *manager = wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

static screencopy_mgr_t *screencopy_manager = NULL;

struct wl_global *screencopy_get_global(void) {
	return screencopy_manager ? screencopy_manager->global : NULL;
}

void screencopy_init(void) {
	ONCE();
	screencopy_manager = calloc(1, sizeof(*screencopy_manager));
	if (!screencopy_manager)
		return;

	screencopy_manager->global = wl_global_create(server.wl_display,
		&zwlr_screencopy_manager_v1_interface, SCREENCOPY_MANAGER_VERSION, screencopy_manager,
		manager_bind);
	if (!screencopy_manager->global) {
		free(screencopy_manager);
		screencopy_manager = NULL;
		return;
	}
	wl_list_init(&screencopy_manager->frames);

	screencopy_manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server.wl_display, &screencopy_manager->display_destroy);

	wlr_log(WLR_INFO, "Custom screencopy manager initialized (block-out supported)");
}

void screencopy_fini(void) {
	ONCE();
	if (!screencopy_manager)
		return;

	screencopy_frame_t *frame, *tmp;
	wl_list_for_each_safe(frame, tmp, &screencopy_manager->frames, link)
		frame_destroy(frame);

	wl_list_remove(&screencopy_manager->display_destroy.link);
	wl_global_destroy(screencopy_manager->global);
	free(screencopy_manager);
	screencopy_manager = NULL;
}
