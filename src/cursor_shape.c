#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/util/log.h>

static void handle_cursor_request_set_shape(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

	if (server.cursor_mode != CURSOR_PASSTHROUGH)
		return;

	if (event->seat_client == server.seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, wlr_cursor_shape_v1_name(event->shape));
}

void cursor_shape_init(void) {
	server.cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
	if (!server.cursor_shape_manager) {
		wlr_log(WLR_ERROR, "Failed to create cursor shape manager");
		exit(EXIT_FAILURE);
	}

	server.cursor_request_set_shape.notify = handle_cursor_request_set_shape;
	wl_signal_add(&server.cursor_shape_manager->events.request_set_shape,
		&server.cursor_request_set_shape);
}

void cursor_shape_fini(void) {
	wl_list_remove(&server.cursor_request_set_shape.link);
}
