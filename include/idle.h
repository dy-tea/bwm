#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>

typedef struct {
	struct wlr_idle_inhibitor_v1 *idle_inhibitor;
	struct wl_listener destroy;
} idle_inhibitor_t;

void update_idle_inhibitors(struct wlr_surface *sans);

void idle_init(void);
void idle_fini(void);
