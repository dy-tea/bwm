#include "input.h"
#include "keyboard.h"
#include "once.h"
#include "output.h"
#include "pointer.h"
#include "seat.h"
#include "server.h"
#include "tablet.h"
#include "touch.h"
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/util/log.h>

static void handle_new_input(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_input_device *device = data;
	struct seat_t *seat = NULL;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		keyboard_t *keyboard = keyboard_create(device);
		if (keyboard)
			seat = keyboard->seat;
		input_apply_config(device);
		break;
	}
	case WLR_INPUT_DEVICE_POINTER:
		pointer_create(device);
		input_apply_config(device);
		break;
	case WLR_INPUT_DEVICE_TABLET: {
		tablet_t *tablet = tablet_create(device);
		if (tablet) {
			tablet_configure(tablet);
			seat = tablet->seat;
		}
		input_apply_config(device);
		break;
	}
	case WLR_INPUT_DEVICE_TABLET_PAD: {
		tablet_pad_t *pad = tablet_pad_create(device);
		if (pad) {
			tablet_pad_configure(pad);
			seat = pad->seat;
		}
		input_apply_config(device);
		break;
	}
	case WLR_INPUT_DEVICE_TOUCH: {
		touch_create(device);
		input_apply_config(device);
		break;
	}
	default:
		input_apply_config(device);
		break;
	}

	if (seat) {
		uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
		if (!wl_list_empty(&server.keyboards))
			caps |= WL_SEAT_CAPABILITY_KEYBOARD;
		if (server.num_touches > 0)
			caps |= WL_SEAT_CAPABILITY_TOUCH;
		wlr_seat_set_capabilities(seat->wlr_seat, caps);
	}
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_output *wlr_output = data;
	if (!wlr_output)
		return;

	output_create(wlr_output);
}

void backend_init(void) {
	ONCE();
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display),
		&server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "Failed to create backend");
		exit(EXIT_FAILURE);
	}

	// headless backend for virtual outputs
	server.headless_backend =
		wlr_headless_backend_create(wl_display_get_event_loop(server.wl_display));
	if (server.headless_backend) {
		wlr_log(WLR_INFO, "Created headless backend for virtual outputs");
		if (wlr_backend_is_multi(server.backend)) {
			wlr_multi_backend_add(server.backend, server.headless_backend);
			server.headless_output_counter = 0;
		}
	}

	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.new_input.notify = handle_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
}

void backend_fini(void) {
	ONCE();
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_output.link);
}
