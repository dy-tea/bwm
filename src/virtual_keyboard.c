#include "keyboard.h"
#include "server.h"
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	keyboard_create(&virtual_keyboard->keyboard.base);
}

void virtual_keyboard_init(void) {
	server.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	if (!server.virtual_keyboard_manager) {
		wlr_log(WLR_ERROR, "Failed to create virtual keyboard manager");
		exit(EXIT_FAILURE);
	}
	server.new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&server.virtual_keyboard_manager->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);
}

void virtual_keyboard_fini(void) {
	wl_list_remove(&server.new_virtual_keyboard.link);
}
