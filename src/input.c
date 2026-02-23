#include "input.h"
#include "keyboard.h"
#include "cursor.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/backend/libinput.h>
#include <libinput.h>

extern struct bwm_server server;

static input_config_t *input_configs[MAX_INPUT_CONFIGS];
static size_t num_input_configs = 0;

static int parse_bool(const char *value, int default_val);
static int parse_scancode(const char *value);

static enum input_config_type device_type_to_input_type(struct wlr_input_device *device) {
  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    return INPUT_CONFIG_TYPE_KEYBOARD;
  case WLR_INPUT_DEVICE_POINTER: {
    struct libinput_device *libinput_dev = wlr_libinput_get_device_handle(device);
    if (libinput_dev && libinput_device_config_tap_get_finger_count(libinput_dev) > 0)
      return INPUT_CONFIG_TYPE_TOUCHPAD;
    return INPUT_CONFIG_TYPE_POINTER;
  }
  case WLR_INPUT_DEVICE_TOUCH:
    return INPUT_CONFIG_TYPE_TOUCH;
  case WLR_INPUT_DEVICE_TABLET:
    return INPUT_CONFIG_TYPE_TABLET;
  case WLR_INPUT_DEVICE_TABLET_PAD:
    return INPUT_CONFIG_TYPE_TABLET_PAD;
  case WLR_INPUT_DEVICE_SWITCH:
    return INPUT_CONFIG_TYPE_SWITCH;
  default:
    return INPUT_CONFIG_TYPE_ANY;
  }
}

static bool device_is_touchpad(struct wlr_input_device *device) {
  if (device->type != WLR_INPUT_DEVICE_POINTER)
    return false;

  if (!wlr_input_device_is_libinput(device))
    return false;

  struct libinput_device *libinput_dev = wlr_libinput_get_device_handle(device);
  if (!libinput_dev)
    return false;

  return libinput_device_config_tap_get_finger_count(libinput_dev) > 0;
}

input_config_t *input_config_create(const char *identifier) {
  input_config_t *config = calloc(1, sizeof(input_config_t));
  if (!config)
    return NULL;

  if (identifier)
    config->identifier = strdup(identifier);

  config->repeat_rate = 25;
  config->repeat_delay = 600;
  config->pointer_accel = FLT_MIN;
  config->accel_profile = -1;
  config->tap_button_map = -1;
  config->drag_lock = INPUT_CONFIG_DRAG_LOCK_DISABLED;
  config->scroll_factor = FLT_MIN;
  config->scroll_button = -1;
  config->scroll_button_lock = -1;
  config->rotation_angle = FLT_MIN;
  config->calibration_matrix_set = false;
  config->tap = -1;
  config->drag = -1;
  config->natural_scroll = -1;
  config->left_handed = -1;
  config->dwt = -1;
  config->dwtp = -1;
  config->click_method = -1;
  config->middle_emulation = -1;
  config->scroll_method = -1;

  return config;
}

void input_config_destroy(input_config_t *config) {
  if (!config)
    return;

  free(config->identifier);
  free(config->xkb_layout);
  free(config->xkb_model);
  free(config->xkb_options);
  free(config->xkb_rules);
  free(config->xkb_variant);
  free(config->xkb_file);
  free(config->xcursor_theme);
  free(config);
}

input_config_t *input_config_get(const char *identifier) {
  for (size_t i = 0; i < num_input_configs; i++) {
    if (input_configs[i] && input_configs[i]->identifier &&
      strcmp(input_configs[i]->identifier, identifier) == 0) {
      return input_configs[i];
    }
  }
  return NULL;
}

input_config_t *input_config_get_for_device(const char *identifier, enum input_config_type type) {
  input_config_t *result = NULL;

  for (size_t i = 0; i < num_input_configs; i++) {
    input_config_t *cfg = input_configs[i];
    if (!cfg)
      continue;

    if (cfg->identifier && identifier && strcmp(cfg->identifier, identifier) == 0)
      if (cfg->type == INPUT_CONFIG_TYPE_ANY || cfg->type == type)
        return cfg;
  }

  for (size_t i = 0; i < num_input_configs; i++) {
    input_config_t *cfg = input_configs[i];
    if (!cfg)
      continue;

    if (cfg->type != INPUT_CONFIG_TYPE_ANY && cfg->type == type && !cfg->identifier)
      if (!result)
        result = cfg;
  }

  if (!result) {
    for (size_t i = 0; i < num_input_configs; i++) {
      input_config_t *cfg = input_configs[i];
      if (!cfg)
        continue;

      if (cfg->identifier == NULL && cfg->type == INPUT_CONFIG_TYPE_ANY)
        if (!result)
          result = cfg;
    }
  }

  return result;
}

bool input_config_add(input_config_t *config) {
  if (num_input_configs >= MAX_INPUT_CONFIGS) {
    wlr_log(WLR_ERROR, "Maximum number of input configs reached");
    return false;
  }

  input_configs[num_input_configs++] = config;
  return true;
}

void input_config_merge(input_config_t *base, input_config_t *overlay) {
  if (!base || !overlay)
    return;

  if (overlay->xkb_layout) {
    free(base->xkb_layout);
    base->xkb_layout = strdup(overlay->xkb_layout);
  }
  if (overlay->xkb_model) {
    free(base->xkb_model);
    base->xkb_model = strdup(overlay->xkb_model);
  }
  if (overlay->xkb_options) {
    free(base->xkb_options);
    base->xkb_options = strdup(overlay->xkb_options);
  }
  if (overlay->xkb_rules) {
    free(base->xkb_rules);
    base->xkb_rules = strdup(overlay->xkb_rules);
  }
  if (overlay->xkb_variant) {
    free(base->xkb_variant);
    base->xkb_variant = strdup(overlay->xkb_variant);
  }
  if (overlay->xkb_file) {
    free(base->xkb_file);
    base->xkb_file = strdup(overlay->xkb_file);
  }

  if (overlay->repeat_rate > 0)
    base->repeat_rate = overlay->repeat_rate;
  if (overlay->repeat_delay > 0)
    base->repeat_delay = overlay->repeat_delay;

  if (overlay->xkb_numlock != -1)
    base->xkb_numlock = overlay->xkb_numlock;
  if (overlay->xkb_capslock != -1)
    base->xkb_capslock = overlay->xkb_capslock;

  if (overlay->pointer_accel != FLT_MIN || (int)overlay->accel_profile != -1) {
    base->pointer_accel = overlay->pointer_accel;
    base->accel_profile = overlay->accel_profile;
  }

  if (overlay->natural_scroll != -1)
    base->natural_scroll = overlay->natural_scroll;
  if (overlay->left_handed != -1)
    base->left_handed = overlay->left_handed;
  if (overlay->tap != -1)
    base->tap = overlay->tap;
  if ((int)overlay->tap_button_map != -1)
    base->tap_button_map = overlay->tap_button_map;
  if (overlay->drag != -1)
    base->drag = overlay->drag;
  if (overlay->drag_lock != INPUT_CONFIG_DRAG_LOCK_DISABLED)
    base->drag_lock = overlay->drag_lock;
  if (overlay->dwt != -1)
    base->dwt = overlay->dwt;
  if (overlay->dwtp != -1)
    base->dwtp = overlay->dwtp;

  if ((int)overlay->click_method != -1)
    base->click_method = overlay->click_method;
  if (overlay->middle_emulation != -1)
    base->middle_emulation = overlay->middle_emulation;

  if ((int)overlay->scroll_method != -1)
    base->scroll_method = overlay->scroll_method;
  if (overlay->scroll_button != -1)
    base->scroll_button = overlay->scroll_button;
  if (overlay->scroll_button_lock != -1)
    base->scroll_button_lock = overlay->scroll_button_lock;
  if (overlay->scroll_factor != FLT_MIN)
    base->scroll_factor = overlay->scroll_factor;

  if (overlay->rotation_angle != FLT_MIN)
    base->rotation_angle = overlay->rotation_angle;

  if (overlay->calibration_matrix_set) {
    base->calibration_matrix_set = true;
    for (int i = 0; i < 6; i++)
      base->calibration_matrix[i] = overlay->calibration_matrix[i];
  }
}

static int parse_bool(const char *value, int default_val) {
  if (!value)
    return default_val;

  if (strcmp(value, "true") == 0 || strcmp(value, "enabled") == 0 || strcmp(value, "1") == 0)
    return 1;
  else if (strcmp(value, "false") == 0 || strcmp(value, "disabled") == 0 || strcmp(value, "0") == 0)
    return 0;

  return default_val;
}

static int parse_scancode(const char *value) {
  if (!value)
    return -1;

  if (value[0] >= '0' && value[0] <= '9')
    return atoi(value);

  return -1;
}

bool input_config_set_value(input_config_t *config, const char *name, const char *value) {
  if (!config || !name)
    return false;

  if (strcmp(name, "xkb_layout") == 0) {
    free(config->xkb_layout);
    config->xkb_layout = strdup(value);
  } else if (strcmp(name, "xkb_model") == 0) {
    free(config->xkb_model);
    config->xkb_model = strdup(value);
  } else if (strcmp(name, "xkb_options") == 0) {
    free(config->xkb_options);
    config->xkb_options = strdup(value);
  } else if (strcmp(name, "xkb_rules") == 0) {
    free(config->xkb_rules);
    config->xkb_rules = strdup(value);
  } else if (strcmp(name, "xkb_variant") == 0) {
    free(config->xkb_variant);
    config->xkb_variant = strdup(value);
  } else if (strcmp(name, "xkb_file") == 0) {
    free(config->xkb_file);
    config->xkb_file = strdup(value);
  } else if (strcmp(name, "repeat_rate") == 0) {
    config->repeat_rate = atoi(value);
  } else if (strcmp(name, "repeat_delay") == 0) {
    config->repeat_delay = atoi(value);
  } else if (strcmp(name, "xkb_numlock") == 0) {
    config->xkb_numlock = parse_bool(value, -1);
  } else if (strcmp(name, "xkb_capslock") == 0) {
    config->xkb_capslock = parse_bool(value, -1);
  } else if (strcmp(name, "pointer_accel") == 0) {
    config->pointer_accel = atof(value);
  } else if (strcmp(name, "accel_profile") == 0) {
    if (strcmp(value, "flat") == 0)
      config->accel_profile = INPUT_CONFIG_ACCEL_PROFILE_FLAT;
    else if (strcmp(value, "adaptive") == 0)
      config->accel_profile = INPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
    else
      config->accel_profile = INPUT_CONFIG_ACCEL_PROFILE_FLAT;
  } else if (strcmp(name, "natural_scroll") == 0) {
    config->natural_scroll = parse_bool(value, -1);
  } else if (strcmp(name, "left_handed") == 0) {
    config->left_handed = parse_bool(value, -1);
  } else if (strcmp(name, "tap") == 0) {
    config->tap = parse_bool(value, -1);
  } else if (strcmp(name, "tap_button_map") == 0) {
    if (strcmp(value, "lrm") == 0)
      config->tap_button_map = INPUT_CONFIG_TAP_BUTTON_MAP_LRM;
    else if (strcmp(value, "lmr") == 0)
      config->tap_button_map = INPUT_CONFIG_TAP_BUTTON_MAP_LMR;
    else
      config->tap_button_map = INPUT_CONFIG_TAP_BUTTON_MAP_LRM;
  } else if (strcmp(name, "drag") == 0) {
    config->drag = parse_bool(value, -1);
  } else if (strcmp(name, "drag_lock") == 0) {
    config->drag_lock = parse_bool(value, 0) ? INPUT_CONFIG_DRAG_LOCK_ENABLED : INPUT_CONFIG_DRAG_LOCK_DISABLED;
  } else if (strcmp(name, "dwt") == 0) {
    config->dwt = parse_bool(value, -1);
  } else if (strcmp(name, "dwtp") == 0) {
    config->dwtp = parse_bool(value, -1);
  } else if (strcmp(name, "click_method") == 0) {
    if (strcmp(value, "button_areas") == 0)
      config->click_method = INPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
    else if (strcmp(value, "clickfinger") == 0)
      config->click_method = INPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
    else if (strcmp(value, "none") == 0)
      config->click_method = INPUT_CONFIG_CLICK_METHOD_NONE;
    else
      config->click_method = INPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
  } else if (strcmp(name, "middle_emulation") == 0) {
    config->middle_emulation = parse_bool(value, -1);
  } else if (strcmp(name, "scroll_method") == 0) {
    if (strcmp(value, "edge") == 0)
      config->scroll_method = INPUT_CONFIG_SCROLL_METHOD_EDGE;
    else if (strcmp(value, "button") == 0)
      config->scroll_method = INPUT_CONFIG_SCROLL_METHOD_BUTTON;
    else if (strcmp(value, "twofinger") == 0)
      config->scroll_method = INPUT_CONFIG_SCROLL_METHOD_TWOFINGER;
    else if (strcmp(value, "none") == 0)
      config->scroll_method = INPUT_CONFIG_SCROLL_METHOD_NONE;
    else
      config->scroll_method = INPUT_CONFIG_SCROLL_METHOD_TWOFINGER;
  } else if (strcmp(name, "scroll_button") == 0) {
    config->scroll_button = parse_scancode(value);
  } else if (strcmp(name, "scroll_button_lock") == 0) {
    config->scroll_button_lock = parse_bool(value, -1);
  } else if (strcmp(name, "scroll_factor") == 0) {
    config->scroll_factor = atof(value);
  } else if (strcmp(name, "rotation_angle") == 0) {
    config->rotation_angle = atof(value);
  } else if (strcmp(name, "xcursor_theme") == 0) {
    free(config->xcursor_theme);
    config->xcursor_theme = strdup(value);
  } else if (strcmp(name, "xcursor_size") == 0) {
    config->xcursor_size = atoi(value);
  } else {
    return false;
  }

  return true;
}

void input_config_apply(input_config_t *config, struct wlr_input_device *device) {
  if (!config || !device)
    return;

  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD: {
    struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);
    if (!keyboard)
      break;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context)
      break;

    struct xkb_rule_names names = { 0 };
    if (config->xkb_layout)
      names.layout = config->xkb_layout;
    if (config->xkb_model)
      names.model = config->xkb_model;
    if (config->xkb_options)
      names.options = config->xkb_options;
    if (config->xkb_rules)
      names.rules = config->xkb_rules;
    if (config->xkb_variant)
      names.variant = config->xkb_variant;

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap) {
      wlr_keyboard_set_keymap(keyboard, keymap);
      xkb_keymap_unref(keymap);
    }

    xkb_context_unref(context);

    if (config->repeat_rate > 0 && config->repeat_delay > 0)
      wlr_keyboard_set_repeat_info(keyboard, config->repeat_rate, config->repeat_delay);

    if (config->xkb_numlock == 1 || config->xkb_capslock == 1) {
      uint32_t leds = 0;
      if (config->xkb_numlock == 1)
        leds |= WLR_LED_NUM_LOCK;
      if (config->xkb_capslock == 1)
        leds |= WLR_LED_CAPS_LOCK;
      wlr_keyboard_led_update(keyboard, leds);
    }

    wlr_log(WLR_INFO, "Applied keyboard config for device %s", device->name);
    break;
  }
  case WLR_INPUT_DEVICE_POINTER:
  case WLR_INPUT_DEVICE_TOUCH:
  case WLR_INPUT_DEVICE_TABLET:
  case WLR_INPUT_DEVICE_TABLET_PAD:
  case WLR_INPUT_DEVICE_SWITCH: {
    if (config->xcursor_theme || config->xcursor_size > 0) {
      char *theme = config->xcursor_theme ? config->xcursor_theme : NULL;
      int size = config->xcursor_size > 0 ? config->xcursor_size : 24;

      if (server.cursor_mgr)
        wlr_xcursor_manager_destroy(server.cursor_mgr);
      server.cursor_mgr = wlr_xcursor_manager_create(theme, size);
      if (server.cursor_mgr) {
        wlr_xcursor_manager_load(server.cursor_mgr, 1);
        wlr_log(WLR_INFO, "Applied xcursor theme '%s' size %d", theme ? theme : "default", size);
      }
    }

    if (!wlr_input_device_is_libinput(device))
      return;

    struct libinput_device *libinput_dev = wlr_libinput_get_device_handle(device);
    if (libinput_dev) {
      bool is_touchpad = device_is_touchpad(device);

      if (is_touchpad) {
        if (libinput_device_config_tap_get_finger_count(libinput_dev) > 0) {
          if (config->tap != -1) {
            enum libinput_config_tap_state tap = config->tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED;
            libinput_device_config_tap_set_enabled(libinput_dev, tap);
          }

          if (config->drag != -1) {
            enum libinput_config_drag_state drag = config->drag ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED;
            libinput_device_config_tap_set_drag_enabled(libinput_dev, drag);
          }

          if (config->drag_lock == INPUT_CONFIG_DRAG_LOCK_ENABLED)
            libinput_device_config_tap_set_drag_lock_enabled(libinput_dev, LIBINPUT_CONFIG_DRAG_LOCK_ENABLED);
          else if (config->drag != -1)
            libinput_device_config_tap_set_drag_lock_enabled(libinput_dev, LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);

          if ((int)config->tap_button_map != -1) {
            enum libinput_config_tap_button_map map = (config->tap_button_map == INPUT_CONFIG_TAP_BUTTON_MAP_LMR) ?
              LIBINPUT_CONFIG_TAP_MAP_LMR : LIBINPUT_CONFIG_TAP_MAP_LRM;
            libinput_device_config_tap_set_button_map(libinput_dev, map);
          }
        }

        if (config->natural_scroll != -1 && libinput_device_config_scroll_has_natural_scroll(libinput_dev))
          libinput_device_config_scroll_set_natural_scroll_enabled(libinput_dev, config->natural_scroll);

        if (config->dwt != -1 && libinput_device_config_dwt_is_available(libinput_dev)) {
          enum libinput_config_dwt_state dwt = config->dwt ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED;
          libinput_device_config_dwt_set_enabled(libinput_dev, dwt);
        }

        if (config->left_handed != -1 && libinput_device_config_left_handed_is_available(libinput_dev))
          libinput_device_config_left_handed_set(libinput_dev, config->left_handed);

        if (config->middle_emulation != -1 && libinput_device_config_middle_emulation_is_available(libinput_dev)) {
          enum libinput_config_middle_emulation_state mid = config->middle_emulation ?
            LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
          libinput_device_config_middle_emulation_set_enabled(libinput_dev, mid);
        }

        if ((int)config->scroll_method != -1 && libinput_device_config_scroll_get_methods(libinput_dev) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL) {
          enum libinput_config_scroll_method method;
          switch (config->scroll_method) {
          case INPUT_CONFIG_SCROLL_METHOD_EDGE:
            method = LIBINPUT_CONFIG_SCROLL_EDGE;
            break;
          case INPUT_CONFIG_SCROLL_METHOD_BUTTON:
            method = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
            break;
          case INPUT_CONFIG_SCROLL_METHOD_TWOFINGER:
            method = LIBINPUT_CONFIG_SCROLL_2FG;
            break;
          case INPUT_CONFIG_SCROLL_METHOD_NONE:
            method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
            break;
          default:
            method = LIBINPUT_CONFIG_SCROLL_2FG;
          }
          libinput_device_config_scroll_set_method(libinput_dev, method);
        }

        if (config->scroll_button != -1 && libinput_device_config_scroll_get_methods(libinput_dev) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL) {
          libinput_device_config_scroll_set_button(libinput_dev, config->scroll_button);
        }

        if (config->scroll_button_lock != -1 && libinput_device_config_scroll_get_methods(libinput_dev) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL) {
          enum libinput_config_scroll_button_lock_state lock = config->scroll_button_lock ?
            LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED : LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_DISABLED;
          libinput_device_config_scroll_set_button_lock(libinput_dev, lock);
        }

        if ((int)config->click_method != -1 && libinput_device_config_click_get_methods(libinput_dev) != LIBINPUT_CONFIG_CLICK_METHOD_NONE) {
          enum libinput_config_click_method method;
          switch (config->click_method) {
          case INPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
            method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
            break;
          case INPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
            method = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
            break;
          case INPUT_CONFIG_CLICK_METHOD_NONE:
            method = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
            break;
          default:
            method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
          }
          libinput_device_config_click_set_method(libinput_dev, method);
        }

        if (libinput_device_config_accel_is_available(libinput_dev)) {
          if (config->pointer_accel != FLT_MIN)
            libinput_device_config_accel_set_speed(libinput_dev, config->pointer_accel);

          if ((int)config->accel_profile != -1) {
            enum libinput_config_accel_profile profile = (config->accel_profile == INPUT_CONFIG_ACCEL_PROFILE_FLAT) ?
              LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            libinput_device_config_accel_set_profile(libinput_dev, profile);
          }
        }
      } else {
        if (config->natural_scroll != -1 && libinput_device_config_scroll_has_natural_scroll(libinput_dev))
          libinput_device_config_scroll_set_natural_scroll_enabled(libinput_dev, config->natural_scroll);

        if (config->scroll_button != -1)
          libinput_device_config_scroll_set_button(libinput_dev, config->scroll_button);

        if (config->scroll_button_lock != -1) {
          enum libinput_config_scroll_button_lock_state lock = config->scroll_button_lock ?
            LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED : LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_DISABLED;
          libinput_device_config_scroll_set_button_lock(libinput_dev, lock);
        }

        if (config->left_handed != -1 && libinput_device_config_left_handed_is_available(libinput_dev))
          libinput_device_config_left_handed_set(libinput_dev, config->left_handed);

        if (libinput_device_config_accel_is_available(libinput_dev)) {
          if (config->pointer_accel != FLT_MIN)
            libinput_device_config_accel_set_speed(libinput_dev, config->pointer_accel);

          if ((int)config->accel_profile != -1) {
            enum libinput_config_accel_profile profile = (config->accel_profile == INPUT_CONFIG_ACCEL_PROFILE_FLAT) ?
              LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            libinput_device_config_accel_set_profile(libinput_dev, profile);
          }
        }
      }
      wlr_log(WLR_INFO, "Applied libinput config for device %s (type %s)", device->name, is_touchpad ? "touchpad" : "mouse");
    } else {
      wlr_log(WLR_INFO, "Applied libinput config for device %s (type %d)", device->name, device->type);
    }
    break;
  }
  default:
    break;
  }
}

void input_apply_config(struct wlr_input_device *device) {
  if (!device)
    return;

  enum input_config_type type = device_type_to_input_type(device);
  input_config_t *config = input_config_get_for_device(device->name, type);

  if (config)
    input_config_apply(config, device);
}

void input_apply_config_all_keyboards(void) {
  struct bwm_keyboard *keyboard;
  wl_list_for_each(keyboard, &server.keyboards, link)
    input_apply_config(&keyboard->wlr_keyboard->base);
}

void input_apply_config_all_pointers(void) {
  struct bwm_pointer *pointer;
  wl_list_for_each(pointer, &server.pointers, link)
    input_apply_config(&pointer->wlr_pointer->base);
}

void input_init(void) {
  for (size_t i = 0; i < num_input_configs; i++) {
    input_config_destroy(input_configs[i]);
    input_configs[i] = NULL;
  }
  num_input_configs = 0;

  input_config_t *wildcard = input_config_create("*");
  if (wildcard)
    input_config_add(wildcard);

  wlr_log(WLR_INFO, "Input subsystem initialized");
}

void input_fini(void) {
  for (size_t i = 0; i < num_input_configs; i++) {
    input_config_destroy(input_configs[i]);
    input_configs[i] = NULL;
  }
  num_input_configs = 0;

  wlr_log(WLR_INFO, "Input subsystem finalized");
}

const char *input_config_type_str(enum input_config_type type) {
  switch (type) {
  case INPUT_CONFIG_TYPE_ANY:
    return "any";
  case INPUT_CONFIG_TYPE_KEYBOARD:
    return "keyboard";
  case INPUT_CONFIG_TYPE_POINTER:
    return "pointer";
  case INPUT_CONFIG_TYPE_TOUCHPAD:
    return "touchpad";
  case INPUT_CONFIG_TYPE_TOUCH:
    return "touch";
  case INPUT_CONFIG_TYPE_TABLET:
    return "tablet";
  case INPUT_CONFIG_TYPE_TABLET_PAD:
    return "tablet_pad";
  case INPUT_CONFIG_TYPE_SWITCH:
    return "switch";
  default:
    return "unknown";
  }
}
