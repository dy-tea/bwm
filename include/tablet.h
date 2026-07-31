#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>

struct seat_t;

typedef struct tablet_t {
	struct wl_list link;
	struct seat_t *seat;
	struct wlr_tablet *wlr_tablet;
	struct wlr_tablet_v2_tablet *tablet_v2;
} tablet_t;

typedef struct tablet_tool_t {
	struct seat_t *seat;
	struct wlr_tablet_tool *wlr_tool;
	struct wlr_tablet_v2_tablet_tool *tablet_v2_tool;
	tablet_t *tablet;

	struct wl_listener set_cursor;
	struct wl_listener tool_destroy;
} tablet_tool_t;

typedef struct tablet_pad_t {
	struct wl_list link;
	struct seat_t *seat;
	struct wlr_tablet_pad *wlr_pad;
	struct wlr_tablet_v2_tablet_pad *tablet_v2_pad;
	tablet_t *tablet;

	struct wl_listener attach;
	struct wl_listener button;
	struct wl_listener ring;
	struct wl_listener strip;
	struct wl_listener tablet_destroy;
	struct wl_listener surface_destroy;

	struct wlr_surface *current_surface;
} tablet_pad_t;

tablet_t *tablet_create(struct wlr_input_device *device);
void tablet_configure(tablet_t *tablet);
void tablet_destroy(tablet_t *tablet);

tablet_tool_t *tablet_tool_configure(tablet_t *tablet, struct wlr_tablet_tool *wlr_tool);

tablet_pad_t *tablet_pad_create(struct wlr_input_device *device);
void tablet_pad_configure(tablet_pad_t *pad);
void tablet_pad_destroy(tablet_pad_t *pad);
void tablet_pad_set_focus(tablet_pad_t *pad, struct wlr_surface *surface);
void tablet_pads_set_focus(struct seat_t *seat, struct wlr_surface *surface);

void handle_tablet_tool_position(struct wlr_tablet_tool *wlr_tool, tablet_t *tablet, bool tip_down,
	double x, double y, uint32_t time);
