#include "server.h"
#include "toplevel.h"
#include <wayland-server-core.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

typedef struct {
	struct wlr_tearing_control_v1 *tearing_control;
	struct wl_listener set_hint;
	struct wl_listener destroy;
	struct wl_list link;
} tearing_controller_t;

static struct toplevel_t *toplevel_from_wlr_surface(struct wlr_surface *surface) {
	if (!surface)
		return NULL;

	toplevel_t *toplevel;
	wl_list_for_each(toplevel, &server.toplevels, link) {
		if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base)
			continue;
		if (toplevel->xdg_toplevel->base->surface == surface)
			return toplevel;
	}

	return NULL;
}

static void handle_tearing_controller_set_hint(struct wl_listener *listener, void *data) {
	(void)data;
	tearing_controller_t *controller = wl_container_of(listener, controller, set_hint);

	struct toplevel_t *toplevel = toplevel_from_wlr_surface(controller->tearing_control->surface);

	if (toplevel)
		toplevel->tearing_hint = controller->tearing_control->current;
}

static void handle_tearing_controller_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	tearing_controller_t *controller = wl_container_of(listener, controller, destroy);
	wl_list_remove(&controller->set_hint.link);
	wl_list_remove(&controller->destroy.link);
	wl_list_remove(&controller->link);
	free(controller);
}

static void handle_new_tearing_hint(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_tearing_control_v1 *tearing_control = data;

	enum wp_tearing_control_v1_presentation_hint hint =
		wlr_tearing_control_manager_v1_surface_hint_from_surface(server.tearing_control_v1,
		tearing_control->surface);
	wlr_log(WLR_DEBUG, "New presentation hint %d received for surface %p", hint,
		(void *)tearing_control->surface);

	tearing_controller_t *controller = calloc(1, sizeof(*controller));
	if (!controller)
		return;

	controller->tearing_control = tearing_control;
	controller->set_hint.notify = handle_tearing_controller_set_hint;
	wl_signal_add(&tearing_control->events.set_hint, &controller->set_hint);
	controller->destroy.notify = handle_tearing_controller_destroy;
	wl_signal_add(&tearing_control->events.destroy, &controller->destroy);
	wl_list_init(&controller->link);

	wl_list_insert(&server.tearing_controllers, &controller->link);
}

void tearing_init(void) {
	server.tearing_control_v1 = wlr_tearing_control_manager_v1_create(server.wl_display, 1);
	if (!server.tearing_control_v1) {
		wlr_log(WLR_ERROR, "Failed to create tearing control manager");
		exit(EXIT_FAILURE);
	}
	server.tearing_control_new_object.notify = handle_new_tearing_hint;
	wl_list_init(&server.tearing_controllers);
	wl_signal_add(&server.tearing_control_v1->events.new_object, &server.tearing_control_new_object);
}

void tearing_fini(void) {
	wl_list_remove(&server.tearing_control_new_object.link);
}
