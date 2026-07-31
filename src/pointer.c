#include "pointer.h"
#include "seat.h"
#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>

void pointer_create(struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server.cursor, device);
	struct wlr_pointer *pointer = wlr_pointer_from_input_device(device);
	pointer_t *ptr = calloc(1, sizeof(*ptr));
	if (!ptr) {
		wlr_log(WLR_ERROR, "allocation failed");
		return;
	}
	ptr->wlr_pointer = pointer;
	ptr->seat = seat_default();
	wl_list_insert(&server.pointers, &ptr->link);
}
