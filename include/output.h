#pragma once

#include <wayland-server.h>
#include <wlr/util/box.h>
#include <time.h>

struct monitor_t;

enum scale_filter_mode {
	SCALE_FILTER_AUTO,
	SCALE_FILTER_LINEAR,
	SCALE_FILTER_NEAREST,
};

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
  struct wl_listener present;

  struct wlr_session_lock_surface_v1 *lock_surface;
  struct wl_listener destroy_lock_surface;

  bool enabled;
  int lx, ly;
  int width, height;

  struct timespec last_presentation;
  uint64_t refresh_nsec;
  int max_render_time;

  enum scale_filter_mode scale_filter_mode;
  enum wl_output_subpixel detected_subpixel;
};

void handle_new_output(struct wl_listener *listener, void *data);
void output_enable(struct bwm_output *output);
void output_disable(struct bwm_output *output);
void output_destroy(struct bwm_output *output);
struct bwm_output *output_from_wlr_output(struct wlr_output *wlr_output);
struct bwm_output *output_get_in_direction(struct bwm_output *reference, uint32_t direction);
void output_update_usable_area(struct bwm_output *output);
void output_set_scale_filter(struct bwm_output *output, enum scale_filter_mode mode);
void output_get_identifier(char *identifier, size_t len, struct bwm_output *output);
