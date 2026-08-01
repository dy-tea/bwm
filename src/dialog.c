#include "dialog.h"
#include "once.h"
#include "server.h"
#include "toplevel.h"
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void xdg_dialog_handle_new(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_dialog_v1 *dialog = data;
	toplevel_t *toplevel = dialog->xdg_toplevel->base->data;
	if (!toplevel)
		return;

	toplevel->is_dialog = true;
}

void dialog_init(void) {
	ONCE();
	struct wlr_xdg_wm_dialog_v1 *xdg_wm_dialog = wlr_xdg_wm_dialog_v1_create(server.wl_display, 1);
	if (!xdg_wm_dialog) {
		wlr_log(WLR_ERROR, "Failed to create xdg wm dialog");
		exit(EXIT_FAILURE);
	}
	server.xdg_dialog_new_dialog.notify = xdg_dialog_handle_new;
	wl_signal_add(&xdg_wm_dialog->events.new_dialog, &server.xdg_dialog_new_dialog);
}

void dialog_fini(void) {
	ONCE();
	wl_list_remove(&server.xdg_dialog_new_dialog.link);
}
