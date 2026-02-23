#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server.h>

struct wlr_input_device;

#define MAX_INPUT_CONFIGS 32

enum input_config_type {
  INPUT_CONFIG_TYPE_ANY,
  INPUT_CONFIG_TYPE_KEYBOARD,
  INPUT_CONFIG_TYPE_POINTER,
  INPUT_CONFIG_TYPE_TOUCH,
  INPUT_CONFIG_TYPE_TABLET,
  INPUT_CONFIG_TYPE_TABLET_PAD,
  INPUT_CONFIG_TYPE_SWITCH,
};

enum input_config_accel_profile {
  INPUT_CONFIG_ACCEL_PROFILE_FLAT,
  INPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE,
};

enum input_config_click_method {
  INPUT_CONFIG_CLICK_METHOD_NONE,
  INPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS,
  INPUT_CONFIG_CLICK_METHOD_CLICKFINGER,
};

enum input_config_scroll_method {
  INPUT_CONFIG_SCROLL_METHOD_NONE,
  INPUT_CONFIG_SCROLL_METHOD_EDGE,
  INPUT_CONFIG_SCROLL_METHOD_BUTTON,
  INPUT_CONFIG_SCROLL_METHOD_TWOFINGER,
};

enum input_config_tap_button_map {
  INPUT_CONFIG_TAP_BUTTON_MAP_LRM,
  INPUT_CONFIG_TAP_BUTTON_MAP_LMR,
};

enum input_config_drag_lock {
  INPUT_CONFIG_DRAG_LOCK_DISABLED,
  INPUT_CONFIG_DRAG_LOCK_ENABLED,
};

typedef struct {
  char *identifier;
  enum input_config_type type;

  char *xkb_layout;
  char *xkb_model;
  char *xkb_options;
  char *xkb_rules;
  char *xkb_variant;
  char *xkb_file;

  int repeat_rate;
  int repeat_delay;

  int xkb_numlock;
  int xkb_capslock;

  double pointer_accel;
  enum input_config_accel_profile accel_profile;

  int natural_scroll;
  int left_handed;
  int tap;
  enum input_config_tap_button_map tap_button_map;
  int drag;
  enum input_config_drag_lock drag_lock;
  int dwt;
  int dwtp;

  enum input_config_click_method click_method;
  int middle_emulation;

  enum input_config_scroll_method scroll_method;
  int scroll_button;
  int scroll_button_lock;
  int scroll_factor;

  float rotation_angle;

  float calibration_matrix[6];
  bool calibration_matrix_set;
} input_config_t;

input_config_t *input_config_create(const char *identifier);
void input_config_destroy(input_config_t *config);
input_config_t *input_config_get(const char *identifier);
input_config_t *input_config_get_for_device(const char *identifier, enum input_config_type type);
bool input_config_add(input_config_t *config);
void input_config_merge(input_config_t *base, input_config_t *overlay);
bool input_config_set_value(input_config_t *config, const char *name, const char *value);
void input_config_apply(input_config_t *config, struct wlr_input_device *device);

void input_init(void);
void input_fini(void);
void input_apply_config(struct wlr_input_device *device);
void input_apply_config_all_keyboards(void);

const char *input_config_type_str(enum input_config_type type);
