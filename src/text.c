#include "text.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define TEXT_FONT "Sans 10"
#define TEXT_HEIGHT 18

struct cairo_buffer {
  struct wlr_buffer base;
  cairo_surface_t *surface;
  cairo_t *cairo;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
  struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
  cairo_surface_destroy(buffer->surface);
  cairo_destroy(buffer->cairo);
  free(buffer);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
  (void)flags;
  struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
  *data = cairo_image_surface_get_data(buffer->surface);
  *stride = cairo_image_surface_get_stride(buffer->surface);
  *format = DRM_FORMAT_ARGB8888;
  return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
  (void)wlr_buffer;
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
  .destroy = cairo_buffer_destroy,
  .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
  .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

struct bwm_text_buffer {
  struct wlr_scene_buffer *buffer_node;
  char *text;
  struct bwm_text_node props;

  bool visible;
  float scale;

  struct wl_listener outputs_update;
  struct wl_listener destroy;
};

int bwm_text_node_default_height(void) { return TEXT_HEIGHT; }

static int get_text_width(const struct bwm_text_node *props) {
  int width = props->width;
  if (props->max_width >= 0 && props->max_width < width)
    width = props->max_width;
  if (width < 0)
    width = 0;
  return width;
}

static PangoLayout *make_pango_layout(cairo_t *cairo, const char *text,
		bool pango_markup) {
  PangoLayout *layout = pango_cairo_create_layout(cairo);
  PangoFontDescription *desc = pango_font_description_from_string(TEXT_FONT);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);

  if (pango_markup) {
    PangoAttrList *attrs;
    char *parsed_text;
    if (pango_parse_markup(text, -1, 0, &attrs, &parsed_text, NULL, NULL)) {
      pango_layout_set_text(layout, parsed_text, -1);
      pango_layout_set_attributes(layout, attrs);
      pango_attr_list_unref(attrs);
      g_free(parsed_text);
    } else {
      goto settext_default;
    }
  } else {
settext_default:
    pango_layout_set_text(layout, text, -1);
  }

  return layout;
}

static void text_calc_size(struct bwm_text_buffer *buffer) {
  cairo_surface_t *recorder = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
  cairo_t *c = cairo_create(recorder);
  cairo_surface_destroy(recorder);
  if (cairo_status(c) != CAIRO_STATUS_SUCCESS) {
    cairo_destroy(c);
    return;
  }

  PangoLayout *layout = make_pango_layout(c, buffer->text, buffer->props.pango_markup);

  int width = 0, height = 0;
  pango_layout_get_pixel_size(layout, &width, &height);

  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  buffer->props.baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
  pango_layout_iter_free(iter);

  buffer->props.width = width;

  g_object_unref(layout);
  cairo_destroy(c);

  wlr_scene_buffer_set_dest_size(buffer->buffer_node, get_text_width(&buffer->props),
    buffer->props.height);
}

static void render_backing_buffer(struct bwm_text_buffer *buffer) {
  if (!buffer->visible)
    return;

  if (buffer->props.max_width == 0) {
    wlr_scene_buffer_set_buffer(buffer->buffer_node, NULL);
    return;
  }

  float scale = buffer->scale > 0 ? buffer->scale : 1.0f;
  int width = (int)ceilf(get_text_width(&buffer->props) * scale);
  int height = (int)ceilf(buffer->props.height * scale);

  if (width <= 0 || height <= 0) {
    wlr_scene_buffer_set_buffer(buffer->buffer_node, NULL);
    return;
  }

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return;
  }

  struct cairo_buffer *cairo_buffer = calloc(1, sizeof(*cairo_buffer));
  if (!cairo_buffer) {
    cairo_surface_destroy(surface);
    return;
  }

  cairo_t *cairo = cairo_create(surface);
  if (!cairo) {
    cairo_surface_destroy(surface);
    free(cairo_buffer);
    return;
  }

  cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

  float *bg = buffer->props.background;
  cairo_set_source_rgba(cairo, bg[0], bg[1], bg[2], bg[3]);
  cairo_rectangle(cairo, 0, 0, width, height);
  cairo_fill(cairo);

  float *fg = buffer->props.color;
  cairo_set_source_rgba(cairo, fg[0], fg[1], fg[2], fg[3]);

  PangoLayout *layout = make_pango_layout(cairo, buffer->text, buffer->props.pango_markup);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

  cairo_scale(cairo, scale, scale);
  cairo_move_to(cairo, 0, 0);
  pango_cairo_show_layout(cairo, layout);
  g_object_unref(layout);

  cairo_surface_flush(surface);

  wlr_buffer_init(&cairo_buffer->base, &cairo_buffer_impl, width, height);
  cairo_buffer->surface = surface;
  cairo_buffer->cairo = cairo;

  wlr_scene_buffer_set_buffer(buffer->buffer_node, &cairo_buffer->base);
  wlr_buffer_drop(&cairo_buffer->base);
}

static void handle_outputs_update(struct wl_listener *listener, void *data) {
  struct bwm_text_buffer *buffer =
      wl_container_of(listener, buffer, outputs_update);
  struct wlr_scene_outputs_update_event *event = data;

  float scale = 0;
  for (size_t i = 0; i < event->size; i++) {
    struct wlr_scene_output *o = event->active[i];
    if (scale < o->output->scale)
      scale = o->output->scale;
  }

  buffer->visible = event->size > 0;

  if (scale != buffer->scale) {
    buffer->scale = scale;
    render_backing_buffer(buffer);
  }
}

static void handle_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  struct bwm_text_buffer *buffer = wl_container_of(listener, buffer, destroy);

  wl_list_remove(&buffer->outputs_update.link);
  wl_list_remove(&buffer->destroy.link);

  free(buffer->text);
  free(buffer);
}

struct bwm_text_node *bwm_text_node_create(struct wlr_scene_tree *parent, const char *text,
		float color[4], bool pango_markup) {
  struct bwm_text_buffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer)
    return NULL;

  struct wlr_scene_buffer *node = wlr_scene_buffer_create(parent, NULL);
  if (!node) {
    free(buffer);
    return NULL;
  }

  buffer->buffer_node = node;
  buffer->props.node = &node->node;
  buffer->props.max_width = -1;
  buffer->text = strdup(text ? text : "");
  if (!buffer->text) {
    wlr_scene_node_destroy(&node->node);
    free(buffer);
    return NULL;
  }

  buffer->props.height = TEXT_HEIGHT;
  buffer->props.pango_markup = pango_markup;
  memcpy(buffer->props.color, color, sizeof(float) * 4);
  buffer->props.background[0] = buffer->props.background[1] = buffer->props.background[2] = buffer->props.background[3] = 0;
  buffer->scale = 1.0f;
  buffer->visible = true;

  buffer->destroy.notify = handle_destroy;
  wl_signal_add(&node->node.events.destroy, &buffer->destroy);
  buffer->outputs_update.notify = handle_outputs_update;
  wl_signal_add(&node->events.outputs_update, &buffer->outputs_update);

  text_calc_size(buffer);
  render_backing_buffer(buffer);

  return &buffer->props;
}

void bwm_text_node_set_color(struct bwm_text_node *node, float color[4]) {
  if (!node)
    return;
  if (memcmp(node->color, color, sizeof(float) * 4) == 0)
    return;
  memcpy(node->color, color, sizeof(float) * 4);
  struct bwm_text_buffer *buffer = wl_container_of(node, buffer, props);
  render_backing_buffer(buffer);
}

void bwm_text_node_set_background(struct bwm_text_node *node,
                                  float background[4]) {
  if (!node)
    return;
  if (memcmp(node->background, background, sizeof(float) * 4) == 0)
    return;
  memcpy(node->background, background, sizeof(float) * 4);
  struct bwm_text_buffer *buffer = wl_container_of(node, buffer, props);
  render_backing_buffer(buffer);
}

void bwm_text_node_set_text(struct bwm_text_node *node, const char *text) {
  if (!node)
    return;
  struct bwm_text_buffer *buffer = wl_container_of(node, buffer, props);
  if (!text)
    text = "";
  if (strcmp(buffer->text, text) == 0)
    return;
  char *new_text = strdup(text);
  if (!new_text)
    return;
  free(buffer->text);
  buffer->text = new_text;
  text_calc_size(buffer);
  render_backing_buffer(buffer);
}

void bwm_text_node_set_max_width(struct bwm_text_node *node, int max_width) {
  if (!node)
    return;
  struct bwm_text_buffer *buffer = wl_container_of(node, buffer, props);
  if (max_width == buffer->props.max_width)
    return;
  buffer->props.max_width = max_width;
  wlr_scene_buffer_set_dest_size(buffer->buffer_node, get_text_width(&buffer->props),
    buffer->props.height);
  render_backing_buffer(buffer);
}
