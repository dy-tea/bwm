#include "output.h"
#include "server.h"
#include "layer.h"
#include "lock.h"
#include "idle.h"
#include "output_config.h"
#include "toplevel.h"
#include "tree.h"
#include <time.h>
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

static void handle_output_destroy(struct wl_listener *listener, void *data);

void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_output *output = wl_container_of(listener, output, frame);
  struct wlr_scene_output *scene_output =
      wlr_scene_get_scene_output(server.scene, output->wlr_output);

  if (!scene_output)
    return;

  wlr_scene_output_commit(scene_output, NULL);

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
  wl_list_remove(&output->link);
  free(output);
}

void handle_new_output(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_output *wlr_output = data;
  struct bwm_output *o = calloc(1, sizeof(struct bwm_output));
  o->wlr_output = wlr_output;
  wlr_output_init_render(wlr_output, server.allocator, server.renderer);

  // Link output to a monitor
  if (server.focused_monitor && !server.focused_monitor->output) {
    server.focused_monitor->output = o;
    o->monitor = server.focused_monitor;
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);

  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode)
    wlr_output_state_set_mode(&state, mode);

  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  o->frame.notify = output_frame;
  wl_signal_add(&wlr_output->events.frame, &o->frame);

  o->request_state.notify = output_request_state;
  wl_signal_add(&wlr_output->events.request_state, &o->request_state);

  o->present.notify = handle_output_present;
  wl_signal_add(&wlr_output->events.present, &o->present);

  o->destroy.notify = handle_output_destroy;
  wl_signal_add(&wlr_output->events.destroy, &o->destroy);

  for (int i = 0; i < 4; i++)
    wl_list_init(&o->layers[i]);

  o->layer_bg = wlr_scene_tree_create(server.bg_tree);
  o->layer_bottom = wlr_scene_tree_create(server.bot_tree);
  o->layer_top = wlr_scene_tree_create(server.top_tree);
  o->layer_overlay = wlr_scene_tree_create(server.over_tree);

  wl_list_insert(&server.outputs, &o->link);
  struct wlr_output_layout_output *l_output =
      wlr_output_layout_add_auto(server.output_layout, wlr_output);
  struct wlr_scene_output *scene_output =
      wlr_scene_output_create(server.scene, wlr_output);
  wlr_scene_output_layout_add_output(server.scene_layout, l_output,
                                  scene_output);

  wlr_output->data = o;

  struct wlr_box layout_box;
  wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
  o->rectangle = layout_box;
  o->usable_area = layout_box;

  // update monitor rectangle
  if (server.focused_monitor) {
  wlr_output_layout_get_box(server.output_layout, wlr_output,
                              &server.focused_monitor->rectangle);
  wlr_log(WLR_INFO, "Monitor rectangle updated: %dx%d at %d,%d",
          server.focused_monitor->rectangle.width,
          server.focused_monitor->rectangle.height,
          server.focused_monitor->rectangle.x,
          server.focused_monitor->rectangle.y);
  }

  output_enable(o);
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
}

void output_disable(struct bwm_output *output) {
  if (!output->enabled)
    return;

  output->enabled = false;

  if (output->monitor && output->monitor->desk) {
    output->monitor->desk->monitor = NULL;
    output->monitor->desk = NULL;
  }

  output->monitor = NULL;
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

  if (output->monitor) {
    output->monitor->rectangle = layout_box;
    wlr_log(WLR_INFO, "Monitor rectangle updated after scale: %dx%d at %d,%d",
            layout_box.width, layout_box.height, layout_box.x, layout_box.y);
  }

  struct bwm_toplevel *toplevel;
  wl_list_for_each(toplevel, &server.toplevels, link) {
    if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base ||
        !toplevel->xdg_toplevel->base->surface || !toplevel->node)
      continue;

    node_t *n = toplevel->node;
    if (n->monitor && n->monitor->output == output) {
      struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
      wlr_log(WLR_DEBUG, "Notifying toplevel of scale %.2f", scale);
      wlr_fractional_scale_v1_notify_scale(surface, scale);
      wlr_surface_set_preferred_buffer_scale(surface, ceil(scale));
    }
  }

  // Notify all layer surfaces on this output
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

  // Rearrange all desktops on this monitor
  if (output->monitor) {
    monitor_t *m = output->monitor;
    for (desktop_t *d = m->desk; d != NULL; d = d->next)
      arrange(m, d, true);
  }

  update_idle_inhibitors(NULL);
}
