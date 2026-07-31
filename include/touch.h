#pragma once
#include <wayland-server.h>
#include <wlr/types/wlr_touch.h>

typedef struct {
	struct wlr_touch *wlr_touch;
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener motion;
	struct wl_listener frame;
	struct wl_list link;
} touch_t;

touch_t *touch_create(struct wlr_input_device *device);
void touch_fini(void);
