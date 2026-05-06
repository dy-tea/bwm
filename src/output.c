#include "output.h"
#include "server.h"
#include "layer.h"
#include "lock.h"
#include "idle.h"
#include "output_config.h"
#include "toplevel.h"
#include "tree.h"
#include "blur.h"
#include "types.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

static void handle_output_destroy(struct wl_listener *listener, void *data);

static enum wlr_scale_filter_mode get_scale_filter(struct bwm_output *output,
		struct wlr_scene_buffer *buffer) {
	if (buffer->dst_width > 0 && buffer->dst_height > 0 && (
			buffer->dst_width < buffer->WLR_PRIVATE.buffer_width ||
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

static void output_configure_scene_iterator(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	struct bwm_output *output = data;
	if (!output)
		return;

	buffer->filter_mode = get_scale_filter(output, buffer);
}

static void output_configure_scene(struct bwm_output *output) {
	if (!output)
		return;

	wlr_scene_node_for_each_buffer(&server.bg_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.bot_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.tile_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.float_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.top_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.full_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.over_tree->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&server.lock_tree->node,
		output_configure_scene_iterator, output);

	wlr_scene_node_for_each_buffer(&output->layer_bg->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_bottom->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_top->node,
		output_configure_scene_iterator, output);
	wlr_scene_node_for_each_buffer(&output->layer_overlay->node,
		output_configure_scene_iterator, output);
}

static bool output_can_tear(struct bwm_output *output) {
	return output->allow_tearing;
}

void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server.scene, output->wlr_output);

	if (!scene_output)
		return;

	output_configure_scene(output);

	if (blur_ctx.available)
		blur_output_frame(output, scene_output);

	struct wlr_scene_output_state_options opts = {
		.color_transform = output->color_transform,
	};

	struct wlr_output_state pending;
	wlr_output_state_init(&pending);
	if (!wlr_scene_output_build_state(scene_output, &pending, &opts)) {
		wlr_output_state_finish(&pending);
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

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_output_present(struct wl_listener *listener, void *data) {
  struct bwm_output *output = wl_container_of(listener, output, present);
  struct wlr_output_event_present *event = data;

  if (!output->enabled || !event->presented)
    return;

  output->last_presentation = event->when;
  output->refresh_nsec = event->refresh;
}

void output_request_state(struct wl_listener *listener, void *data) {
  struct bwm_output *output = wl_container_of(listener, output, request_state);
  struct wlr_output_event_request_state *event = data;
  wlr_output_commit_state(output->wlr_output, event->state);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_output *output = wl_container_of(listener, output, destroy);
  struct bwm_layer_surface *layer, *tmp;

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
  blur_output_fini(output->blur_ctx);
  free(output);
}

void handle_new_output(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_output *wlr_output = data;
  struct bwm_output *output = calloc(1, sizeof(struct bwm_output));
  if (!output)
    return;

  output->wlr_output = wlr_output;
  wlr_output_init_render(wlr_output, server.allocator, server.renderer);

  // init output info
  output->enabled = false;
  output->allow_tearing = false;
  strncpy(output->name, wlr_output->name, SMALEN - 1);
  output->name[SMALEN - 1] = 0;
  output->id = next_monitor_id++;
  output->wired = true;
  output->window_gap = window_gap;
  output->border_width = border_width;
  output->padding = (padding_t){0};
  output->rectangle = (struct wlr_box){0, 0, 1920, 1080};

  // create default workspace for output
  desktop_t *d = calloc(1, sizeof(desktop_t));
  if (d) {
    d->id = next_desktop_id++;
    strncpy(d->name, "default", SMALEN - 1);
    d->layout = LAYOUT_TILED;
    d->user_layout = LAYOUT_TILED;
    d->window_gap = window_gap;
    d->border_width = border_width;
    d->padding = (padding_t){0};
    d->root = NULL;
    d->focus = NULL;
    d->output = output;

    output->desk = d;
    output->desk_head = d;
    output->desk_tail = d;
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

  output->destroy.notify = handle_output_destroy;
  wl_signal_add(&wlr_output->events.destroy, &output->destroy);

  for (int i = 0; i < 4; i++)
    wl_list_init(&output->layers[i]);

  output->layer_bg = wlr_scene_tree_create(server.bg_tree);
  output->layer_bottom = wlr_scene_tree_create(server.bot_tree);
  output->layer_top = wlr_scene_tree_create(server.top_tree);
  output->layer_overlay = wlr_scene_tree_create(server.over_tree);

  struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server.output_layout, wlr_output);
  struct wlr_scene_output *scene_output = wlr_scene_output_create(server.scene, wlr_output);
  wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);

  wlr_output->data = output;

  struct wlr_box layout_box;
  wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
  output->rectangle = layout_box;
  output->usable_area = layout_box;
  output->blur_ctx = blur_output_init(output->rectangle.width, output->rectangle.height);

  // update monitor rectangle
  if (server.focused_output) {
	  wlr_output_layout_get_box(server.output_layout, wlr_output,
	    &server.focused_output->rectangle);
	  wlr_log(WLR_INFO, "Monitor rectangle updated: %dx%d at %d,%d",
	    server.focused_output->rectangle.width,
	    server.focused_output->rectangle.height,
	    server.focused_output->rectangle.x,
	    server.focused_output->rectangle.y);
  }

  output_enable(output);
  output_update_manager_config();
}

void output_enable(struct bwm_output *output) {
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

void output_disable(struct bwm_output *output) {
  if (!output->enabled)
    return;

  output->enabled = false;
  if (output->desk) {
    output->desk->output = NULL;
    output->desk = NULL;
  }
}

void output_destroy(struct bwm_output *output) {
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

struct bwm_output *output_from_wlr_output(struct wlr_output *wlr_output) {
  if (!wlr_output)
    return NULL;
  return wlr_output->data;
}

struct bwm_output *output_get_in_direction(struct bwm_output *reference, uint32_t direction) {
  if (!reference || !direction)
    return NULL;

  struct wlr_box output_box;
  wlr_output_layout_get_box(server.output_layout, reference->wlr_output, &output_box);

  int lx = output_box.x + output_box.width / 2;
  int ly = output_box.y + output_box.height / 2;

  struct wlr_output *wlr_adjacent = wlr_output_layout_adjacent_output(
      server.output_layout, direction, reference->wlr_output, lx, ly);

  if (!wlr_adjacent)
    return NULL;

  return output_from_wlr_output(wlr_adjacent);
}

void output_update_usable_area(struct bwm_output *output) {
  if (!output || !output->enabled)
    return;

  output->usable_area.x = 0;
  output->usable_area.y = 0;
  output->usable_area.width = output->width;
  output->usable_area.height = output->height;
}

void output_set_scale_filter(struct bwm_output *output, enum scale_filter_mode mode) {
  if (!output)
    return;
  output->scale_filter_mode = mode;
  output_configure_scene(output);
}

void output_get_identifier(char *identifier, size_t len, struct bwm_output *output) {
  struct wlr_output *wlr_output = output->wlr_output;
  snprintf(identifier, len, "%s %s %s",
      wlr_output->make ? wlr_output->make : "Unknown",
      wlr_output->model ? wlr_output->model : "Unknown",
      wlr_output->serial ? wlr_output->serial : "Unknown");
}

void output_update_scale(struct bwm_output *output, float scale) {
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

  struct bwm_toplevel *toplevel;
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
    struct bwm_layer_surface *layer;
    wl_list_for_each(layer, &output->layers[i], link) {
      if (!layer->layer_surface || !layer->layer_surface->surface)
        continue;

      struct wlr_surface *surface = layer->layer_surface->surface;
      wlr_log(WLR_DEBUG, "Notifying layer surface of scale %.2f", scale);
      wlr_fractional_scale_v1_notify_scale(surface, scale);
      wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
    }
  }

  blur_invalidate_mica(output->blur_ctx);

  // rearrange all desktop on this output
  for (desktop_t *d = output->desk; d != NULL; d = d->next)
    arrange(output, d, true);

  update_idle_inhibitors(NULL);
}

struct bwm_output *output_get_valid(void) {
  for (struct bwm_output *m = mon_head; m != NULL; m = m->next)
    if (m->enabled && m->wlr_output)
      return m;
  return NULL;
}
