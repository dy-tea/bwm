#pragma once

#include "types.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server.h>
#include <wlr/render/color.h>
#include <wlr/util/box.h>

typedef struct effects_output_t effects_output_t;
typedef struct desktop_t desktop_t;

enum scale_filter_mode {
	SCALE_FILTER_AUTO,
	SCALE_FILTER_LINEAR,
	SCALE_FILTER_NEAREST,
};

typedef struct output_t {
	struct wlr_output *wlr_output;
	struct wlr_box usable_area;
	struct wlr_scene_tree *layer_bg;
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *layer_top;
	struct wlr_scene_tree *layer_overlay;
	struct wl_list layers[4];
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_listener present;
	struct wl_event_source *repaint_timer;

	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wl_listener destroy_lock_surface;
	struct wl_listener map_lock_surface;

	bool enabled;
	bool allow_tearing;
	bool frame_scheduled;
	int lx, ly;
	int width, height;

	struct timespec last_presentation;
	uint64_t refresh_nsec;
	int max_render_time;

	enum scale_filter_mode scale_filter_mode, applied_scale_filter;
	enum wl_output_subpixel detected_subpixel;

	struct wlr_color_transform *color_transform;
	bool hdr;
	effects_output_t *effects;

	char name[64]; // SMALEN
	uint32_t id;
	bool wired;
	padding_t padding;
	unsigned int sticky_count;

	struct wlr_box rectangle;
	desktop_t *desk, *last_desk;
	desktop_t *desk_head, *desk_tail;
	struct output_t *prev, *next;
} output_t;

void output_create(struct wlr_output *wlr_output);
void output_enable(output_t *output);
void output_disable(output_t *output);
void output_destroy(output_t *output);
output_t *output_from_wlr_output(struct wlr_output *wlr_output);
output_t *output_get_in_direction(output_t *reference, uint32_t direction);
void output_update_usable_area(output_t *output);
void output_set_scale_filter(output_t *output, enum scale_filter_mode mode);
void output_get_identifier(char *identifier, size_t len, output_t *output);
void output_update_scale(output_t *output, float scale);
output_t *output_get_valid(void);
void output_schedule_frame(output_t *output);
bool output_supports_hdr(output_t *output, const char **unsupported_reason_ptr);
