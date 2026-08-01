#pragma once

#include <wayland-server.h>

void destroy_lock_surface(struct wl_listener *listener, void *data);

void session_lock_init(void);
void session_lock_fini(void);
