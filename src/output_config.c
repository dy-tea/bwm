#include "output_config.h"
#include "output.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <wayland-server-core.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/util/log.h>

static struct wl_list output_configs;

void output_config_init(void) {
  wl_list_init(&output_configs);
}

struct output_config *output_config_create(const char *name) {
  struct output_config *oc = calloc(1, sizeof(struct output_config));
  if (!oc)
    return NULL;

  oc->name = strdup(name);
  oc->enable = OUTPUT_CONFIG_ENABLE_AUTO;
  oc->x = -1;
  oc->y = -1;
  oc->width = -1;
  oc->height = -1;
  oc->refresh_rate = -1;
  oc->scale = -1;
  oc->transform = -1;
  oc->dpms_state = OUTPUT_CONFIG_DPMS_AUTO;
  oc->scale_filter = OUTPUT_CONFIG_SCALE_FILTER_AUTO;
  oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_AUTO;
  oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_AUTO;
  oc->subpixel = OUTPUT_CONFIG_SUBPIXEL_AUTO;

  return oc;
}

void output_config_destroy(struct output_config *oc) {
  if (!oc)
    return;
  free(oc->name);
  free(oc->background);
  free(oc->background_option);
  free(oc);
}

struct output_config *output_config_find(const char *name) {
  struct output_config *oc;
  wl_list_for_each(oc, &output_configs, link)
    if (strcmp(oc->name, name) == 0)
      return oc;
  return NULL;
}

void output_config_add(struct output_config *oc) {
  struct output_config *existing = output_config_find(oc->name);
  if (existing) {
    wl_list_remove(&existing->link);
    output_config_destroy(existing);
  }
  wl_list_insert(&output_configs, &oc->link);
}

void output_set_power(struct wlr_output *wlr_output, uint32_t mode) {
  if (!wlr_output)
    return;

  struct wlr_output_state state;
  wlr_output_state_init(&state);

  if (mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF) {
    wlr_output_state_set_enabled(&state, false);
  } else {
    wlr_output_state_set_enabled(&state, true);
  }

  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);
}

void output_config_apply(struct output_config *oc) {
  if (!oc)
    return;

  struct bwm_output *output = NULL;
  struct wlr_output *wlr_output = NULL;

  struct bwm_output *o;
  wl_list_for_each(o, &server.outputs, link) {
    if (strcmp(o->wlr_output->name, oc->name) == 0) {
      output = o;
      wlr_output = o->wlr_output;
      break;
    }
  }

  if (!wlr_output) {
    wlr_log(WLR_DEBUG, "Output %s not found, cannot apply config", oc->name);
    return;
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);

  if (oc->enable == OUTPUT_CONFIG_DISABLE) {
    wlr_output_state_set_enabled(&state, false);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
    return;
  }

  if (oc->enable == OUTPUT_CONFIG_ENABLE || oc->width > 0 || oc->height > 0) {
    wlr_output_state_set_enabled(&state, true);
    if (oc->width > 0 && oc->height > 0) {
      int mhz = (int)(oc->refresh_rate * 1000);
      if (wl_list_empty(&wlr_output->modes)) {
        wlr_output_state_set_custom_mode(&state, oc->width, oc->height, mhz);
      } else {
        struct wlr_output_mode *mode, *best = NULL;
        wl_list_for_each(mode, &wlr_output->modes, link) {
          if (mode->width != oc->width || mode->height != oc->height)
            continue;
          if (oc->refresh_rate <= 0) {
            if (!best || mode->refresh > best->refresh)
              best = mode;
          } else if (!best || abs(mode->refresh - mhz) < abs(best->refresh - mhz))
            best = mode;
        }
        if (best)
          wlr_output_state_set_mode(&state, best);
        else
          wlr_output_state_set_custom_mode(&state, oc->width, oc->height, mhz);
      }
    } else if (!wl_list_empty(&wlr_output->modes)) {
      struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
      if (preferred)
        wlr_output_state_set_mode(&state, preferred);
    }
  }

  if (oc->x >= 0 && oc->y >= 0)
    wlr_output_layout_add(server.output_layout, wlr_output, oc->x, oc->y);
  else
    wlr_output_layout_add_auto(server.output_layout, wlr_output);

  bool scale_changed = oc->scale > 0;
  float new_scale = oc->scale > 0 ? round(oc->scale * 120) / 120 : 1.0f;

  if (oc->scale > 0)
    wlr_output_state_set_scale(&state, new_scale);

  if (oc->transform >= 0)
    wlr_output_state_set_transform(&state, oc->transform);

  if (oc->subpixel != OUTPUT_CONFIG_SUBPIXEL_AUTO) {
    enum wl_output_subpixel subpixel;
    switch (oc->subpixel) {
    case OUTPUT_CONFIG_SUBPIXEL_NONE:
      subpixel = WL_OUTPUT_SUBPIXEL_NONE;
      break;
    case OUTPUT_CONFIG_SUBPIXEL_HORIZONTAL_RGB:
      subpixel = WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
      break;
    case OUTPUT_CONFIG_SUBPIXEL_HORIZONTAL_BGR:
      subpixel = WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
      break;
    case OUTPUT_CONFIG_SUBPIXEL_VERTICAL_RGB:
      subpixel = WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
      break;
    case OUTPUT_CONFIG_SUBPIXEL_VERTICAL_BGR:
      subpixel = WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
      break;
    default:
      subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
      break;
    }
    wlr_output_state_set_subpixel(&state, subpixel);
  }

  if (oc->adaptive_sync != OUTPUT_CONFIG_ADAPTIVE_SYNC_AUTO)
    wlr_output_state_set_adaptive_sync_enabled(&state,
        oc->adaptive_sync == OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED);

  if (oc->render_bit_depth != OUTPUT_CONFIG_RENDER_BIT_DEPTH_AUTO) {
    uint32_t render_format = DRM_FORMAT_XRGB8888;
    if (oc->render_bit_depth == OUTPUT_CONFIG_RENDER_BIT_DEPTH_10)
      render_format = DRM_FORMAT_XRGB2101010;
    wlr_output_state_set_render_format(&state, render_format);
  }

  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  if (scale_changed && output)
    output_update_scale(output, new_scale);

  if (oc->dpms_state == OUTPUT_CONFIG_DPMS_OFF)
    output_set_power(wlr_output, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
  else if (oc->dpms_state == OUTPUT_CONFIG_DPMS_ON)
    output_set_power(wlr_output, ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

void output_apply_all_config(void) {
  struct output_config *oc;
  wl_list_for_each(oc, &output_configs, link)
    output_config_apply(oc);
}

void output_config_update_from_wlr_output(struct bwm_output *output) {
  struct output_config *oc = output_config_find(output->wlr_output->name);
  if (!oc) {
    oc = output_config_create(output->wlr_output->name);
    if (oc)
      output_config_add(oc);
  }
}

void output_update_manager_config(void) {
  if (!server.output_manager)
    return;

  struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();

  struct bwm_output *output;
  wl_list_for_each(output, &server.outputs, link) {
    struct wlr_output_configuration_head_v1 *head =
        wlr_output_configuration_head_v1_create(config, output->wlr_output);

    struct wlr_output_head_v1_state *state = &head->state;
    state->enabled = output->wlr_output->enabled;
    state->mode = output->wlr_output->current_mode;
    if (output->wlr_output->current_mode) {
      state->custom_mode.width = output->wlr_output->current_mode->width;
      state->custom_mode.height = output->wlr_output->current_mode->height;
      state->custom_mode.refresh = output->wlr_output->current_mode->refresh;
    }
    state->scale = output->wlr_output->scale;
    state->transform = output->wlr_output->transform;
    state->adaptive_sync_enabled =
        output->wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_get(server.output_layout, output->wlr_output);
    if (layout_output) {
      state->x = layout_output->x;
      state->y = layout_output->y;
    }
  }

  wlr_output_manager_v1_set_configuration(server.output_manager, config);
}
