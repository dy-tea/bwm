#include "output.h"
#include "server.h"
#include "layer.h"
#include <time.h>
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

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

void output_request_state(struct wl_listener *listener, void *data) {
  struct bwm_output *output = wl_container_of(listener, output, request_state);
  struct wlr_output_event_request_state *event = data;
  wlr_output_commit_state(output->wlr_output, event->state);
}

void output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_output *output = wl_container_of(listener, output, destroy);
  struct bwm_layer_surface *layer, *tmp;

  for (size_t i = 0; i < 4; i++)
    wl_list_for_each_safe(layer, tmp, &output->layers[i], link)
      wlr_layer_surface_v1_destroy(layer->layer_surface);

  wl_list_remove(&output->frame.link);
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

  o->destroy.notify = output_destroy;
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
}
