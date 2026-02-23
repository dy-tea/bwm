#pragma once

#include "server.h"
#include "toplevel.h"
#include <wlr/types/wlr_pointer.h>

struct bwm_pointer {
	struct wlr_pointer *wlr_pointer;
	struct wl_list link;
};

struct bwm_cursor_constraint {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener set_region;
	struct wl_listener destroy;
};

void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void cursor_frame(struct wl_listener *listener, void *data);

void request_cursor(struct wl_listener *listener, void *data);
void seat_pointer_focus_change(struct wl_listener *listener, void *data);

void handle_new_input(struct wl_listener *listener, void *data);
void handle_pointer_constraint(struct wl_listener *listener, void *data);
void handle_cursor_request_set_shape(struct wl_listener *listener, void *data);

void begin_interactive(struct bwm_toplevel *toplevel, enum cursor_mode mode, uint32_t edges);
