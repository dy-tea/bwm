#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_seat.h>

typedef struct ime_relay_t ime_relay_t;

typedef struct seat_t {
	struct wlr_seat *wlr_seat;
	ime_relay_t *input_method_relay;

	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;

	struct wl_list tablets; // tablet_t.link
	struct wl_list tablet_pads; // tablet_pad_t.link
	struct wl_list touches; // touch_t.link

	char name[32];
	struct wl_list link;
} seat_t;

seat_t *seat_create(const char *name);
void seat_destroy(seat_t *seat);
seat_t *seat_find_by_name(const char *name);
seat_t *seat_find_by_wlr_seat(struct wlr_seat *wlr_seat);
seat_t *seat_default(void);
