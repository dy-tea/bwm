#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

struct bwm_output;
struct wlr_output;

#define MAXLEN 256

enum output_config_enable {
  OUTPUT_CONFIG_ENABLE_AUTO = -1,
  OUTPUT_CONFIG_DISABLE = 0,
  OUTPUT_CONFIG_ENABLE = 1,
};

enum output_config_dpms {
  OUTPUT_CONFIG_DPMS_AUTO = 0,
  OUTPUT_CONFIG_DPMS_ON = 1,
  OUTPUT_CONFIG_DPMS_OFF = 2,
};

enum output_config_scale_filter {
  OUTPUT_CONFIG_SCALE_FILTER_AUTO = 0,
  OUTPUT_CONFIG_SCALE_FILTER_LINEAR = 1,
  OUTPUT_CONFIG_SCALE_FILTER_NEAREST = 2,
};

enum output_config_adaptive_sync {
  OUTPUT_CONFIG_ADAPTIVE_SYNC_AUTO = 0,
  OUTPUT_CONFIG_ADAPTIVE_SYNC_DISABLED = 1,
  OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED = 2,
};

enum output_config_render_bit_depth {
  OUTPUT_CONFIG_RENDER_BIT_DEPTH_AUTO = 0,
  OUTPUT_CONFIG_RENDER_BIT_DEPTH_8 = 8,
  OUTPUT_CONFIG_RENDER_BIT_DEPTH_10 = 10,
};

enum output_config_subpixel {
  OUTPUT_CONFIG_SUBPIXEL_AUTO = 0,
  OUTPUT_CONFIG_SUBPIXEL_UNKNOWN = 1,
  OUTPUT_CONFIG_SUBPIXEL_NONE = 2,
  OUTPUT_CONFIG_SUBPIXEL_HORIZONTAL_RGB = 3,
  OUTPUT_CONFIG_SUBPIXEL_HORIZONTAL_BGR = 4,
  OUTPUT_CONFIG_SUBPIXEL_VERTICAL_RGB = 5,
  OUTPUT_CONFIG_SUBPIXEL_VERTICAL_BGR = 6,
};

struct output_config {
  struct wl_list link;
  char *name;
  int enable;
  int x, y;
  int width, height;
  float refresh_rate;
  float scale;
  int transform;
  char *background;
  char *background_option;
  enum output_config_dpms dpms_state;
  enum output_config_scale_filter scale_filter;
  enum output_config_adaptive_sync adaptive_sync;
  enum output_config_render_bit_depth render_bit_depth;
  enum output_config_subpixel subpixel;
};

struct output_config *output_config_create(const char *name);
void output_config_destroy(struct output_config *oc);
struct output_config *output_config_find(const char *name);
void output_config_add(struct output_config *oc);
void output_config_apply(struct output_config *oc);
void output_apply_all_config(void);
void output_config_update_from_wlr_output(struct bwm_output *output);
struct bwm_output *output_from_wlr_output(struct wlr_output *wlr_output);
void output_set_power(struct wlr_output *wlr_output, uint32_t mode);
void output_config_init(void);
void output_update_manager_config(void);
