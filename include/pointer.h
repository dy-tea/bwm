#pragma once

#include <wlr/types/wlr_input_device.h>

typedef struct pointer_t {
	struct seat_t *seat;
	struct wlr_pointer *wlr_pointer;
	struct wl_list link;
} pointer_t;

void pointer_create(struct wlr_input_device *device);
