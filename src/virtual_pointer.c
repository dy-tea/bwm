#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_virtual_pointer_v1 *pointer = event->new_pointer;
	struct wlr_input_device *device = &pointer->pointer.base;

	wlr_cursor_attach_input_device(server.cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(server.cursor, device, event->suggested_output);
}

void virtual_pointer_init(void) {
	server.virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server.wl_display);
	if (!server.virtual_pointer_manager) {
		wlr_log(WLR_ERROR, "Failed to create virtual pointer manager");
		exit(EXIT_FAILURE);
	}
	server.new_virtual_pointer.notify = handle_new_virtual_pointer;
	wl_signal_add(&server.virtual_pointer_manager->events.new_virtual_pointer,
		&server.new_virtual_pointer);
}

void virtual_pointer_fini(void) {
	wl_list_remove(&server.new_virtual_pointer.link);
}
