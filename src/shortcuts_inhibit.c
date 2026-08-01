#include "once.h"
#include "rule.h"
#include "server.h"
#include "toplevel.h"
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>

void handle_keyboard_shortcuts_inhibit_new_inhibitor(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
	const char *app_id = NULL;
	const char *title = NULL;
	const char *tag = NULL;

	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(inhibitor->surface);
	if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		app_id = xdg_surface->toplevel->app_id;
		title = xdg_surface->toplevel->title;
		toplevel_t *tl = xdg_surface->toplevel->base->data;
		if (tl)
			tag = tl->tag;
	} else {
		struct wlr_xwayland_surface *xwayland_surface =
			wlr_xwayland_surface_try_from_wlr_surface(inhibitor->surface);
		if (xwayland_surface) {
			app_id = xwayland_surface->class;
			title = xwayland_surface->title;
		}
	}

	bool allow = true;
	if (app_id || title || tag) {
		rule_consequence_t *rule = find_matching_rule(app_id, title, tag);
		if (rule && rule->has & RULE_TYPE_SHORTCUTS_INHIBITOR)
			allow = rule->flags & RULE_TYPE_SHORTCUTS_INHIBITOR;
	}

	if (allow)
		wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
}

void shortcuts_inhibit_init(void) {
	ONCE();
	server.keyboard_shortcuts_inhibit_manager =
		wlr_keyboard_shortcuts_inhibit_v1_create(server.wl_display);
	if (!server.keyboard_shortcuts_inhibit_manager) {
		wlr_log(WLR_ERROR, "Failed to create keyboard shortcuts inhibit manager");
		exit(EXIT_FAILURE);
	}
	server.keyboard_shortcuts_inhibit_new_inhibitor.notify =
		handle_keyboard_shortcuts_inhibit_new_inhibitor;
	wl_signal_add(&server.keyboard_shortcuts_inhibit_manager->events.new_inhibitor,
		&server.keyboard_shortcuts_inhibit_new_inhibitor);
}

void shortcuts_inhibit_fini(void) {
	ONCE();
	wl_list_remove(&server.keyboard_shortcuts_inhibit_new_inhibitor.link);
}
