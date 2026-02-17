#pragma once

#include <wayland-server.h>
#include <wlr/util/box.h>

struct monitor_t;

struct bwm_output {
  struct wl_list link;
  struct wlr_output *wlr_output;
  struct wlr_box rectangle;
  struct wlr_box usable_area;
  struct wlr_scene_tree *layer_bg;
  struct wlr_scene_tree *layer_bottom;
  struct wlr_scene_tree *layer_top;
  struct wlr_scene_tree *layer_overlay;
  struct wl_list layers[4];
  struct monitor_t *monitor;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
};

void handle_new_output(struct wl_listener *listener, void *data);
