#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

struct bwm_layer_surface {
  struct wl_list link;
  struct wlr_layer_surface_v1 *layer_surface;
  struct wlr_scene_tree *scene_tree;
  struct wlr_scene_layer_surface_v1 *scene_layer;
  struct bwm_output *output;
  bool mapped;

  struct wl_listener new_popup;
  struct wl_listener destroy;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener surface_commit;
};

void handle_new_layer_surface(struct wl_listener *listener, void *data);
void arrange_layers(struct bwm_output *output);
void focus_layer_surface(struct bwm_layer_surface *layer_surface);
