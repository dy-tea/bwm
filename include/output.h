#pragma once

#include <wayland-server.h>

struct bwm_output {
  struct wl_list link;
  struct wlr_output *wlr_output;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
};

void handle_new_output(struct wl_listener *listener, void *data);
