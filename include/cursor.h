#pragma once

#include "server.h"
#include "toplevel.h"

void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void cursor_frame(struct wl_listener *listener, void *data);
void request_cursor(struct wl_listener *listener, void *data);
void seat_pointer_focus_change(struct wl_listener *listener, void *data);
void handle_new_input(struct wl_listener *listener, void *data);

void begin_interactive(struct bwm_toplevel *toplevel, enum cursor_mode mode, uint32_t edges);
