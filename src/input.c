#include "input.h"
#include "keyboard.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_keyboard.h>
#include <libinput.h>

extern struct bwm_server server;

static input_config_t *input_configs[MAX_INPUT_CONFIGS];
static size_t num_input_configs = 0;

static int parse_bool(const char *value, int default_val);
static int parse_scancode(const char *value);

static enum input_config_type device_type_to_input_type(enum wlr_input_device_type type) {
  switch (type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    return INPUT_CONFIG_TYPE_KEYBOARD;
  case WLR_INPUT_DEVICE_POINTER:
    return INPUT_CONFIG_TYPE_POINTER;
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

input_config_t *input_config_create(const char *identifier) {
  input_config_t *config = calloc(1, sizeof(input_config_t));
  if (!config)
    return NULL;

  if (identifier)
    config->identifier = strdup(identifier);

  config->repeat_rate = 25;
  config->repeat_delay = 600;
  config->pointer_accel = 0;
  config->accel_profile = INPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
  config->tap_button_map = INPUT_CONFIG_TAP_BUTTON_MAP_LRM;
  config->drag_lock = INPUT_CONFIG_DRAG_LOCK_DISABLED;
  config->scroll_factor = 1;
  config->rotation_angle = 0;
  config->calibration_matrix_set = false;

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
  free(config);
}

static bool match_input_config(const input_config_t *config, const char *identifier, enum input_config_type type) {
  if (!config)
    return false;

  if (config->identifier && identifier && strcmp(config->identifier, identifier) == 0)
    if (config->type == INPUT_CONFIG_TYPE_ANY || config->type == type)
      return true;

  if (config->type != INPUT_CONFIG_TYPE_ANY && config->type == type)
    return true;

  if (config->identifier == NULL && config->type == INPUT_CONFIG_TYPE_ANY)
    return true;

  return false;
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
    if (match_input_config(input_configs[i], identifier, type)) {
      if (!result) {
        result = input_configs[i];
      } else if (input_configs[i]->identifier) {
        if (!result->identifier || strcmp(input_configs[i]->identifier, result->identifier) == 0)
          result = input_configs[i];
      }
    }
  }

  if (!result)
    for (size_t i = 0; i < num_input_configs; i++)
      if (match_input_config(input_configs[i], NULL, type))
        if (!result)
          result = input_configs[i];

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

  if (overlay->pointer_accel != 0 || overlay->accel_profile != INPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE) {
    base->pointer_accel = overlay->pointer_accel;
    base->accel_profile = overlay->accel_profile;
  }

  if (overlay->natural_scroll != -1)
    base->natural_scroll = overlay->natural_scroll;
  if (overlay->left_handed != -1)
    base->left_handed = overlay->left_handed;
  if (overlay->tap != -1)
    base->tap = overlay->tap;
  if (overlay->tap_button_map != INPUT_CONFIG_TAP_BUTTON_MAP_LRM)
    base->tap_button_map = overlay->tap_button_map;
  if (overlay->drag != -1)
    base->drag = overlay->drag;
  if (overlay->drag_lock != INPUT_CONFIG_DRAG_LOCK_DISABLED)
    base->drag_lock = overlay->drag_lock;
  if (overlay->dwt != -1)
    base->dwt = overlay->dwt;
  if (overlay->dwtp != -1)
    base->dwtp = overlay->dwtp;

  if (overlay->click_method != INPUT_CONFIG_CLICK_METHOD_NONE)
    base->click_method = overlay->click_method;
  if (overlay->middle_emulation != -1)
    base->middle_emulation = overlay->middle_emulation;

  if (overlay->scroll_method != INPUT_CONFIG_SCROLL_METHOD_NONE)
    base->scroll_method = overlay->scroll_method;
  if (overlay->scroll_button != -1)
    base->scroll_button = overlay->scroll_button;
  if (overlay->scroll_button_lock != -1)
    base->scroll_button_lock = overlay->scroll_button_lock;
  if (overlay->scroll_factor != 1)
    base->scroll_factor = overlay->scroll_factor;

  if (overlay->rotation_angle != 0)
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
  } else if (strcmp(name, "scroll_button") == 0) {
    config->scroll_button = parse_scancode(value);
  } else if (strcmp(name, "scroll_button_lock") == 0) {
    config->scroll_button_lock = parse_bool(value, -1);
  } else if (strcmp(name, "scroll_factor") == 0) {
    config->scroll_factor = atof(value);
  } else if (strcmp(name, "rotation_angle") == 0) {
    config->rotation_angle = atof(value);
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
    wlr_log(WLR_INFO, "Applied libinput config for device %s (type %d)", device->name, device->type);
    break;
  }
  default:
      break;
  }
}

void input_apply_config(struct wlr_input_device *device) {
  if (!device)
    return;

  enum input_config_type type = device_type_to_input_type(device->type);
  input_config_t *config = input_config_get_for_device(device->name, type);

  if (config)
    input_config_apply(config, device);
}

void input_apply_config_all_keyboards(void) {
  struct bwm_keyboard *keyboard;
  wl_list_for_each(keyboard, &server.keyboards, link) {
    struct wlr_input_device *device = &keyboard->wlr_keyboard->base;
    input_apply_config(device);
  }
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
