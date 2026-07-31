#include "server.h"
#include "toplevel.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	if (!xdg_toplevel)
		return;

	toplevel_t *toplevel = toplevel_create(xdg_toplevel);
	if (!toplevel)
		return;

	wlr_log(WLR_INFO, "New XDG toplevel (%p)", (void *)toplevel);

	// add to toplevels list
	wl_list_insert(&server.toplevels, &toplevel->link);
}

void xdg_shell_init(void) {
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 5);
	if (!server.xdg_shell) {
		wlr_log(WLR_ERROR, "Failed to create xdg shell");
		exit(EXIT_FAILURE);
	}

	wl_list_init(&server.toplevels);

	server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
}

void xdg_shell_fini(void) {
	wl_list_remove(&server.new_xdg_toplevel.link);
}
