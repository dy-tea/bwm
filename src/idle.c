#include "idle.h"
#include "once.h"
#include "server.h"
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_scene.h>

void update_idle_inhibitors(struct wlr_surface *sans) {
	bool inhibited = false;
	struct wlr_idle_inhibitor_v1 *idle;
	wl_list_for_each(idle, &server.idle_inhibit_manager->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(idle->surface);
		struct wlr_scene_tree *tree = surface->data;

		int _unused_x, _unused_y;
		if (sans != surface && (tree == NULL || wlr_scene_node_coords(&tree->node, &_unused_x,
				&_unused_y))) {
			inhibited = true;
			break;
		}
	}

	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, inhibited);
}

void handle_idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	idle_inhibitor_t *idle = wl_container_of(listener, idle, destroy);
	wl_list_remove(&idle->destroy.link);
	update_idle_inhibitors(wlr_surface_get_root_surface(idle->idle_inhibitor->surface));
	free(idle);
}

static void handle_new_idle_inhibitor(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;

	idle_inhibitor_t *idle = calloc(1, sizeof(*idle));
	if (!idle) {
		wlr_log(WLR_ERROR, "allocation failed");
		return;
	}
	idle->idle_inhibitor = idle_inhibitor;

	idle->destroy.notify = handle_idle_inhibitor_destroy;
	wl_signal_add(&idle_inhibitor->events.destroy, &idle->destroy);

	update_idle_inhibitors(NULL);
}

void idle_init(void) {
	ONCE();
	server.idle_inhibit_manager = wlr_idle_inhibit_v1_create(server.wl_display);
	if (!server.idle_inhibit_manager) {
		wlr_log(WLR_ERROR, "Failed to create idle inhibit manager");
		exit(EXIT_FAILURE);
	}
	server.new_idle_inhibitor.notify = handle_new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit_manager->events.new_inhibitor, &server.new_idle_inhibitor);
}

void idle_fini(void) {
	ONCE();
	wl_list_remove(&server.new_idle_inhibitor.link);
}
