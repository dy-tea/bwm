#include "layer.h"
#include "popup.h"
#include "server.h"
#include "output.h"
#include "tree.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>

extern struct bwm_server server;

static void layer_surface_destroy(struct wl_listener *listener, void *data);
static void layer_surface_map(struct wl_listener *listener, void *data);
static void layer_surface_unmap(struct wl_listener *listener, void *data);
static void layer_surface_commit(struct wl_listener *listener, void *data);

struct wlr_scene_tree *output_shell_layer(struct bwm_output *output, enum zwlr_layer_shell_v1_layer layer);

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  struct bwm_layer_surface *layer = wl_container_of(listener, layer, destroy);

  wl_list_remove(&layer->destroy.link);
  wl_list_remove(&layer->new_popup.link);
  wl_list_remove(&layer->map.link);
  wl_list_remove(&layer->unmap.link);
  wl_list_remove(&layer->surface_commit.link);
  wl_list_remove(&layer->link);

  arrange_layers(layer->output);
  free(layer);
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_layer_surface *layer = wl_container_of(listener, layer, map);

	arrange_layers(layer->output);

	if (layer->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		focus_layer_surface(layer);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  struct bwm_layer_surface *layer = wl_container_of(listener, layer, unmap);
  layer->mapped = false;
  wlr_scene_node_set_enabled(&layer->scene_tree->node, false);
  arrange_layers(layer->output);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
  (void)data;
  struct bwm_layer_surface *layer = wl_container_of(listener, layer, surface_commit);
  struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;

  if (!layer_surface->initialized)
  	return;

  bool needs_arrange = false;
  if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
    struct wlr_scene_tree *new_tree = output_shell_layer(layer->output, layer_surface->current.layer);
    wlr_scene_node_reparent(&layer->scene_layer->tree->node, new_tree);
    needs_arrange = true;
  }

  // rearrange if needed
  if (needs_arrange || layer_surface->initial_commit) {
    layer->mapped = layer_surface->surface->mapped;
    arrange_layers(layer->output);
  }
}

void handle_new_layer_surface(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_layer_surface_v1 *layer_surface = data;
  struct bwm_layer_surface *layer;
  struct bwm_output *output;

  wlr_log(WLR_INFO, "New layer surface on layer %d",
      layer_surface->pending.layer);

  if (!layer_surface->output) {
    wlr_log(WLR_INFO, "Layer surface has no output, using focused_monitor");
    if (!server.focused_monitor || !server.focused_monitor->output) {
      wlr_log(WLR_ERROR, "No focused monitor, destroying layer surface");
      wlr_layer_surface_v1_destroy(layer_surface);
      return;
    }
    layer_surface->output = server.focused_monitor->output->wlr_output;
  }

  output = layer_surface->output->data;
  if (!output) {
    wlr_log(WLR_ERROR, "No output data, destroying layer surface");
    wlr_layer_surface_v1_destroy(layer_surface);
    return;
  }

  layer = calloc(1, sizeof(*layer));
  if (!layer) {
    wlr_layer_surface_v1_destroy(layer_surface);
    return;
  }

  layer->layer_surface = layer_surface;
  layer->output = output;
  layer_surface->data = layer;

  struct wlr_scene_tree *layer_tree;
  switch (layer_surface->pending.layer) {
  case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
    layer_tree = output->layer_bg;
    break;
  case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
    layer_tree = output->layer_bottom;
    break;
  case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
    layer_tree = output->layer_top;
    break;
  case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    layer_tree = output->layer_overlay;
    break;
  default:
    layer_tree = output->layer_top;
    break;
  }

  layer->scene_layer = wlr_scene_layer_surface_v1_create(layer_tree, layer_surface);
  layer->scene_tree = layer->scene_layer->tree;

  layer->scene_tree->node.data = layer;

  layer->destroy.notify = layer_surface_destroy;
  wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

  layer->new_popup.notify = handle_new_layer_popup;
  wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);

  layer->map.notify = layer_surface_map;
  wl_signal_add(&layer_surface->surface->events.map, &layer->map);

  layer->unmap.notify = layer_surface_unmap;
  wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);

  layer->surface_commit.notify = layer_surface_commit;
  wl_signal_add(&layer_surface->surface->events.commit, &layer->surface_commit);

  wl_list_insert(&output->layers[layer_surface->pending.layer], &layer->link);

  wlr_surface_send_enter(layer_surface->surface, layer_surface->output);

  arrange_layers(output);
}

void arrange_layers(struct bwm_output *output) {
  struct wlr_box usable_area = output->rectangle;
  struct wlr_box full_area = output->rectangle;
  struct bwm_layer_surface *layer;
  int i;

  if (!output->wlr_output->enabled)
    return;

  for (i = 3; i >= 0; i--) {
    wl_list_for_each_reverse(layer, &output->layers[i], link) {
      if (!layer->layer_surface->initialized)
        continue;
      if (layer->layer_surface->current.exclusive_zone > 0)
        wlr_scene_layer_surface_v1_configure(layer->scene_layer,
            &full_area, &usable_area);
    }
  }

  if (!wlr_box_equal(&usable_area, &output->usable_area)) {
    output->usable_area = usable_area;
    monitor_t *m = output->monitor;
    if (m && m->desk)
      arrange(m, m->desk, true);
  }

  for (i = 3; i >= 0; i--) {
    wl_list_for_each_reverse(layer, &output->layers[i], link) {
      if (!layer->layer_surface->initialized)
        continue;
      if (layer->layer_surface->current.exclusive_zone <= 0)
        wlr_scene_layer_surface_v1_configure(layer->scene_layer,
            &full_area, &usable_area);
    }
  }
}

void focus_layer_surface(struct bwm_layer_surface *layer_surface) {
	if (!layer_surface->layer_surface || !layer_surface->layer_surface->surface
		|| !layer_surface->layer_surface->surface->mapped)
		return;

	struct wlr_surface *surface = layer_surface->layer_surface->surface;

	if (layer_surface->layer_surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
		return;

	if (layer_surface->scene_layer == NULL)
		return;

	// notify keyboard enter
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard)
	  wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes,
	                                  keyboard->num_keycodes,
	                                  &keyboard->modifiers);
}

struct wlr_scene_tree *output_shell_layer(struct bwm_output *output, enum zwlr_layer_shell_v1_layer layer) {
  switch (layer) {
  case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
    return output->layer_bg;
  case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
    return output->layer_bottom;
  case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
    return output->layer_top;
  case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    return output->layer_overlay;
  default:
    return output->layer_top;
  }
}
