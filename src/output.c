#include "animation.h"
#include "effects.h"
#include "idle.h"
#include "ipc.h"
#include "layer.h"
#include "lock.h"
#include "output.h"
#include "output_config.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

static void handle_output_destroy(struct wl_listener *listener, void *data);

bool output_supports_hdr(output_t *output, const char **unsupported_reason_ptr) {
	const char *unsupported_reason = NULL;
	if (!(output->wlr_output->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020)) {
		unsupported_reason = "BT2020 primaries not supported by output";
	} else if (!(output->wlr_output->supported_transfer_functions &
			WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ)) {
		unsupported_reason = "PQ transfer function not supported by output";
	} else if (!server.renderer->features.output_color_transform) {
		unsupported_reason = "renderer doesn't support output color transforms";
	}
	if (unsupported_reason_ptr != NULL) {
		*unsupported_reason_ptr = unsupported_reason;
	}
	return unsupported_reason == NULL;
}

static bool output_is_hdr_active(output_t *output) {
	return output->wlr_output->image_description != NULL;
}

static enum wlr_scale_filter_mode get_scale_filter(output_t *output,
		struct wlr_scene_buffer *buffer) {
	if (buffer->dst_width > 0 && buffer->dst_height > 0 &&
		(buffer->dst_width < buffer->WLR_PRIVATE.buffer_width ||
		buffer->dst_height < buffer->WLR_PRIVATE.buffer_height))
		return WLR_SCALE_FILTER_BILINEAR;

	switch (output->scale_filter_mode) {
	case SCALE_FILTER_LINEAR:
		return WLR_SCALE_FILTER_BILINEAR;
	case SCALE_FILTER_NEAREST:
	case SCALE_FILTER_AUTO:
	default:
		return WLR_SCALE_FILTER_NEAREST;
	}
}

static void output_configure_scene_iterator(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	output_t *output = data;
	if (!output)
		return;

	buffer->filter_mode = get_scale_filter(output, buffer);
}

static void output_configure_scene(output_t *output) {
	if (!output)
		return;
	if (output->scale_filter_mode == output->applied_scale_filter)
		return;

	wlr_scene_node_for_each_buffer(&server.bg_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.bot_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.tile_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.float_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.top_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.full_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.over_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.lock_tree->node, output_configure_scene_iterator, output);

	wlr_scene_node_for_each_buffer(&output->layer_bg->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_bottom->node, output_configure_scene_iterator,
		output);
	wlr_scene_node_for_each_buffer(&output->layer_top->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_overlay->node, output_configure_scene_iterator,
		output);

	output->applied_scale_filter = output->scale_filter_mode;
}

static struct wlr_surface *fullscreen_surface(output_t *output);

static bool output_can_tear(output_t *output) {
	return output->allow_tearing;
}

static bool output_has_fullscreen_cover(output_t *output) {
	if (!output || !output->desk || !output->desk->focus)
		return false;

	node_t *node = output->desk->focus;
	client_t *client = node->client;
	if (!client || client->state != STATE_FULLSCREEN)
		return false;
	if (client->toplevel && client->toplevel->mapped)
		return true;

	return (client->xwayland_view && client->xwayland_view->mapped);
}

static bool fullscreen_has_effects(output_t *output) {
	node_t *node = output->desk->focus;
	client_t *client = node->client;
	return client->blur || client->mica || client->acrylic;
}

static struct wlr_surface *fullscreen_surface(output_t *output) {
	if (!output || !output->desk || !output->desk->focus)
		return NULL;

	node_t *node = output->desk->focus;
	client_t *client = node->client;
	if (!client || client->state != STATE_FULLSCREEN)
		return NULL;

	if (client->toplevel && client->toplevel->mapped)
		return client->toplevel->xdg_toplevel->base->surface;
	if (client->xwayland_view && client->xwayland_view->mapped)
		return client->xwayland_view->xwayland_surface->surface;

	return NULL;
}

static void output_configure_scene_visible(output_t *output) {
	if (!output)
		return;
	if (output->scale_filter_mode == output->applied_scale_filter)
		return;

	wlr_scene_node_for_each_buffer(&server.full_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.over_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.shader_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.drag_tree->node, output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.lock_tree->node, output_configure_scene_iterator, output);

	wlr_scene_node_for_each_buffer(&output->layer_overlay->node, output_configure_scene_iterator,
		output);

	output->applied_scale_filter = output->scale_filter_mode;
}

static bool output_try_direct_scanout(output_t *output) {
	if (!output_has_fullscreen_cover(output))
		return false;
	if (fullscreen_has_effects(output))
		return false;
	if (server.locked || screen_shader_enabled)
		return false;

	struct wlr_surface *surface = fullscreen_surface(output);
	if (!surface || !wlr_surface_has_buffer(surface) || !surface->buffer)
		return false;

	struct wlr_buffer *buf = &surface->buffer->base;

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	wlr_buffer_lock(buf);
	wlr_output_state_set_buffer(&state, buf);

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, (unsigned int)output->width,
		(unsigned int)output->height);
	wlr_output_state_set_damage(&state, &damage);
	pixman_region32_fini(&damage);

	state.tearing_page_flip = output_can_tear(output);

	bool ok = wlr_output_test_state(output->wlr_output, &state);
	if (!ok) {
		wlr_log(WLR_DEBUG, "Direct scanout test failed for %s", output->name);
		wlr_buffer_unlock(buf);
		wlr_output_state_finish(&state);
		return false;
	}

	ok = wlr_output_commit_state(output->wlr_output, &state);
	if (!ok)
		wlr_log(WLR_ERROR, "Direct scanout commit failed for %s", output->name);

	wlr_buffer_unlock(buf);
	wlr_output_state_finish(&state);
	return ok;
}

const long NSEC_PER_MSEC = 1000000;
const long NSEC_PER_SEC = 1000000000;

static void output_repaint(output_t *output) {
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene,
		output->wlr_output);
	if (!scene_output)
		return;

	bool has_fs = output_has_fullscreen_cover(output);
	bool fs_effects = has_fs && fullscreen_has_effects(output);

	if (has_fs)
		output_configure_scene_visible(output);
	else
		output_configure_scene(output);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	bool animating = animation_update_output(output, now);

	if (effects_state.available && (fs_effects || !has_fs))
		effects_output_frame(output, scene_output);

	if (has_fs && !fs_effects && output_try_direct_scanout(output)) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_scene_output_send_frame_done(scene_output, &now);
		if (animating)
			output_schedule_frame(output);

		return;
	}

	struct wlr_scene_output_state_options opts = {
		.color_transform = output->color_transform,
	};

	struct wlr_output_state pending;
	wlr_output_state_init(&pending);
	if (!wlr_scene_output_build_state(scene_output, &pending, &opts)) {
		wlr_output_state_finish(&pending);
		if (animating)
			output_schedule_frame(output);

		return;
	}

	if (output_can_tear(output)) {
		pending.tearing_page_flip = true;

		if (!wlr_output_test_state(output->wlr_output, &pending)) {
			wlr_log(WLR_DEBUG, "Output test failed, retrying without tearing page-flip");
			pending.tearing_page_flip = false;
		}
	}

	if (!wlr_output_commit_state(output->wlr_output, &pending))
		wlr_log(WLR_ERROR, "Failed to commit output state");
	wlr_output_state_finish(&pending);

	output->hdr = output_is_hdr_active(output);

	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);

	if (animating)
		output_schedule_frame(output);
}

void output_schedule_frame(output_t *output) {
	if (!output || output->frame_scheduled)
		return;
	output->frame_scheduled = true;
	wlr_output_schedule_frame(output->wlr_output);
}

static int output_repaint_timer_handler(void *data) {
	output_t *output = data;
	if (!output->wlr_output->enabled)
		return 0;

	output->wlr_output->frame_pending = false;
	output_repaint(output);
	return 0;
}

void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	output_t *output = wl_container_of(listener, output, frame);
	if (!output->wlr_output->enabled)
		return;
	output->frame_scheduled = false;

	if (output->max_render_time >= 0) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		int msec_until_refresh = 0;
		if (output->last_presentation.tv_sec > 0) {
			struct timespec predicted_refresh = output->last_presentation;
			predicted_refresh.tv_nsec += (long)(output->refresh_nsec % NSEC_PER_SEC);
			predicted_refresh.tv_sec += (long)(output->refresh_nsec / NSEC_PER_SEC);
			if (predicted_refresh.tv_nsec >= NSEC_PER_SEC) {
				predicted_refresh.tv_sec += 1;
				predicted_refresh.tv_nsec -= NSEC_PER_SEC;
			}

			if (predicted_refresh.tv_sec >= now.tv_sec) {
				long nsec_until_refresh = (predicted_refresh.tv_sec - now.tv_sec) * NSEC_PER_SEC +
					(predicted_refresh.tv_nsec - now.tv_nsec);
				msec_until_refresh = (int)(nsec_until_refresh / NSEC_PER_MSEC);
			}
		}

		int delay = msec_until_refresh - output->max_render_time;

		if (delay < 1) {
			output_repaint(output);
		} else {
			output->wlr_output->frame_pending = true;
			wl_event_source_timer_update(output->repaint_timer, delay);
		}
	} else {
		output_repaint(output);
	}
}

static void handle_output_present(struct wl_listener *listener, void *data) {
	output_t *output = wl_container_of(listener, output, present);
	struct wlr_output_event_present *event = data;

	if (!output->enabled || !event->presented)
		return;

	output->last_presentation = event->when;
	output->refresh_nsec = event->refresh;
}

void output_request_state(struct wl_listener *listener, void *data) {
	output_t *output = wl_container_of(listener, output, request_state);
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	output_t *output = wl_container_of(listener, output, destroy);
	layer_surface_t *layer, *tmp;

	for (size_t i = 0; i < 4; i++)
		wl_list_for_each_safe(layer, tmp, &output->layers[i], link)
			wlr_layer_surface_v1_destroy(layer->layer_surface);

	if (output->lock_surface)
		destroy_lock_surface(&output->destroy_lock_surface, NULL);

	if (output->enabled)
		output_disable(output);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->present.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);

	if (output->repaint_timer) {
		wl_event_source_remove(output->repaint_timer);
		output->repaint_timer = NULL;
	}

	if (output->prev)
		output->prev->next = output->next;
	else
		mon_head = output->next;

	if (output->next)
		output->next->prev = output->prev;
	else
		mon_tail = output->prev;

	if (server.focused_output == output)
		server.focused_output = (output->next) ? output->next : output->prev;
	if (mon == output)
		mon = mon_head;

	wlr_color_transform_unref(output->color_transform);
	effects_output_fini(output->effects);

	desktop_t *d = output->desk_head;
	while (d) {
		desktop_t *next = d->next;

		if (d->root) {
			node_t *n = first_extrema(d->root);
			while (n) {
				n->output = NULL;
				n = next_leaf(n, d->root);
			}
		}

		d->output = NULL;
		d->prev = NULL;
		d->next = NULL;

		if (!orphan_desk_head) {
			orphan_desk_head = d;
			orphan_desk_tail = d;
		} else {
			orphan_desk_tail->next = d;
			d->prev = orphan_desk_tail;
			orphan_desk_tail = d;
		}

		d = next;
	}
	output->desk_head = NULL;
	output->desk_tail = NULL;
	output->desk = NULL;

	ipc_put_status(SUB_MASK_MONITOR_REMOVE, "monitor_remove[%s]\n", output->name);
	free(output);
}

void handle_new_output(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_output *wlr_output = data;
	output_t *output = calloc(1, sizeof(*output));
	if (!output)
		return;

	output->wlr_output = wlr_output;
	wlr_output_init_render(wlr_output, server.allocator, server.renderer);

	// init output info
	output->enabled = false;
	output->allow_tearing = false;
	output->max_render_time = -1;
	strncpy(output->name, wlr_output->name, SMALEN - 1);
	output->name[SMALEN - 1] = 0;
	if (wlr_output_is_headless(wlr_output)) {
		server.headless_output_counter++;
		snprintf(output->name, SMALEN, "HEADLESS-%u", server.headless_output_counter);
		wlr_output_set_name(wlr_output, output->name);
	}
	output->id = next_monitor_id++;
	output->wired = true;
	output->padding = (padding_t){0};
	output->rectangle = (struct wlr_box){
		0,
		0,
		1920,
		1080
	};

	// restore orphaned desktops or create default workspace
	if (orphan_desk_head) {
		output->desk = orphan_desk_head;
		output->desk_head = orphan_desk_head;
		output->desk_tail = orphan_desk_tail;

		output->desk_head->prev = NULL;

		desktop_t *d = orphan_desk_head;
		while (d) {
			d->output = output;
			if (d->root) {
				node_t *n = first_extrema(d->root);
				while (n) {
					n->output = output;
					n = next_leaf(n, d->root);
				}
			}
			d = d->next;
		}

		orphan_desk_head = NULL;
		orphan_desk_tail = NULL;

		if (output->desk->root && output->desk->focus)
			focus_node(output, output->desk, output->desk->focus);
		arrange(output, output->desk, true);
	} else {
		desktop_t *d = calloc(1, sizeof(desktop_t));
		if (d) {
			d->id = next_desktop_id++;
			strncpy(d->name, "default", SMALEN - 1);
			d->layout = LAYOUT_TILED;
			d->user_layout = LAYOUT_TILED;
			d->window_gap = window_gap;
			d->master_stack_count = 1;
			d->padding = (padding_t){0};
			d->root = NULL;
			d->focus = NULL;
			d->output = output;

			output->desk = d;
			output->desk_head = d;
			output->desk_tail = d;
		}
	}

	// add to monitor linked list
	if (!mon)
		mon = output;

	if (!mon_head)
		mon_head = output;

	if (mon_tail) {
		output->prev = mon_tail;
		mon_tail->next = output;
		mon_tail = output;
	} else {
		mon = output;
		mon_head = output;
		mon_tail = output;
	}

	if (!server.focused_output)
		server.focused_output = output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->present.notify = handle_output_present;
	wl_signal_add(&wlr_output->events.present, &output->present);

	output->repaint_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display),
		output_repaint_timer_handler, output);

	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	for (int i = 0; i < 4; i++)
		wl_list_init(&output->layers[i]);

	output->layer_bg = wlr_scene_tree_create(server.bg_tree);
	output->layer_bottom = wlr_scene_tree_create(server.bot_tree);
	output->layer_top = wlr_scene_tree_create(server.top_tree);
	output->layer_overlay = wlr_scene_tree_create(server.over_tree);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server.output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);

	wlr_output->data = output;

	struct wlr_box layout_box;
	wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
	output->rectangle = layout_box;
	output->usable_area = layout_box;
	output->effects = effects_output_init(output->rectangle.width, output->rectangle.height);

	output_enable(output);
	ipc_put_status(SUB_MASK_MONITOR_ADD, "monitor_add[%s]\n", output->name);
	output_update_manager_config();
}

void output_enable(output_t *output) {
	if (output->enabled)
		return;

	output->enabled = true;
	output->lx = output->rectangle.x;
	output->ly = output->rectangle.y;
	output->width = output->rectangle.width;
	output->height = output->rectangle.height;

	output->detected_subpixel = output->wlr_output->subpixel;
	output->scale_filter_mode = SCALE_FILTER_NEAREST;

	output_update_usable_area(output);

	if (server.workspace_manager && !wl_list_empty(&server.workspace_manager->groups)) {
		struct wlr_ext_workspace_group_handle_v1 *group;
		group = wl_container_of(server.workspace_manager->groups.next, group, link);
		wlr_ext_workspace_group_handle_v1_output_enter(group, output->wlr_output);
	}
}

void output_disable(output_t *output) {
	if (!output->enabled)
		return;

	output->enabled = false;
	if (output->desk) {
		output->desk->output = NULL;
		output->desk = NULL;
	}
}

void output_destroy(output_t *output) {
	if (!output)
		return;

	if (output->layer_bg)
		wlr_scene_node_destroy(&output->layer_bg->node);
	if (output->layer_bottom)
		wlr_scene_node_destroy(&output->layer_bottom->node);
	if (output->layer_top)
		wlr_scene_node_destroy(&output->layer_top->node);
	if (output->layer_overlay)
		wlr_scene_node_destroy(&output->layer_overlay->node);

	free(output);
}

output_t *output_from_wlr_output(struct wlr_output *wlr_output) {
	if (!wlr_output)
		return NULL;

	return wlr_output->data;
}

output_t *output_get_in_direction(output_t *reference, uint32_t direction) {
	if (!reference || !direction)
		return NULL;

	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout, reference->wlr_output, &output_box);

	int lx = output_box.x + output_box.width / 2;
	int ly = output_box.y + output_box.height / 2;

	struct wlr_output *wlr_adjacent = wlr_output_layout_adjacent_output(server.output_layout, direction,
		reference->wlr_output, lx, ly);

	if (!wlr_adjacent)
		return NULL;

	return output_from_wlr_output(wlr_adjacent);
}

void output_update_usable_area(output_t *output) {
	if (!output || !output->enabled)
		return;

	output->usable_area.x = 0;
	output->usable_area.y = 0;
	output->usable_area.width = output->width;
	output->usable_area.height = output->height;
}

void output_set_scale_filter(output_t *output, enum scale_filter_mode mode) {
	if (!output)
		return;

	output->scale_filter_mode = mode;
	output_configure_scene(output);
}

void output_get_identifier(char *identifier, size_t len, output_t *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	snprintf(identifier, len, "%s %s %s", wlr_output->make ? wlr_output->make : "Unknown",
		wlr_output->model ? wlr_output->model : "Unknown",
		wlr_output->serial ? wlr_output->serial : "Unknown");
}

void output_update_scale(output_t *output, float scale) {
	if (!output || !output->wlr_output)
		return;

	struct wlr_output *wlr_output = output->wlr_output;

	wlr_log(WLR_INFO, "Updating output '%s' scale to %.2f", wlr_output->name, scale);

	struct wlr_box layout_box;
	wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
	output->rectangle = layout_box;
	output->usable_area = layout_box;
	output->lx = layout_box.x;
	output->ly = layout_box.y;
	output->width = layout_box.width;
	output->height = layout_box.height;
	output->rectangle = layout_box;

	toplevel_t *toplevel;
	wl_list_for_each(toplevel, &server.toplevels, link) {
		if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base ||
			!toplevel->xdg_toplevel->base->surface || !toplevel->node)
			continue;

		node_t *n = toplevel->node;
		if (n->output && n->output == output) {
			struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
			wlr_log(WLR_DEBUG, "Notifying toplevel of scale %.2f", scale);
			wlr_fractional_scale_v1_notify_scale(surface, scale);
			wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
		}
	}

	// notify all layer surfaces on this output
	for (int i = 0; i < 4; i++) {
		layer_surface_t *layer;
		wl_list_for_each(layer, &output->layers[i], link) {
			if (!layer->layer_surface || !layer->layer_surface->surface)
				continue;

			struct wlr_surface *surface = layer->layer_surface->surface;
			wlr_log(WLR_DEBUG, "Notifying layer surface of scale %.2f", scale);
			wlr_fractional_scale_v1_notify_scale(surface, scale);
			wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
		}
	}

	effects_invalidate_mica(output->effects);

	// rearrange all desktop on this output
	for (desktop_t *d = output->desk; d != NULL; d = d->next)
		arrange(output, d, true);

	update_idle_inhibitors(NULL);
}

output_t *output_get_valid(void) {
	for (output_t *m = mon_head; m != NULL; m = m->next)
		if (m->enabled && m->wlr_output)
			return m;

	return NULL;
}
