#include "lock.h"
#include "once.h"
#include "output.h"
#include "server.h"
#include "tree.h"
#include <stdlib.h>
#include <wayland-util.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>

typedef struct session_lock_t {
	struct wlr_session_lock_v1 *session_lock;
	struct wlr_scene_tree *scene_tree;

	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} session_lock_t;

static void destroy_unlock(session_lock_t *session_lock, const bool unlock) {
	if (server.current_session_lock == NULL)
		return;

	wl_list_remove(&session_lock->new_surface.link);
	wl_list_remove(&session_lock->unlock.link);
	wl_list_remove(&session_lock->destroy.link);

	server.current_session_lock = NULL;

	wlr_seat_keyboard_notify_clear_focus(server.seat);

	if (!(server.locked = !unlock)) {
		wlr_scene_node_set_enabled(&server.lock_background->node, false);

		focus_node(server.focused_output, server.focused_output->desk,
			server.focused_output->desk->focus);
	}

	wlr_scene_node_destroy(&session_lock->scene_tree->node);
	free(session_lock);
}

void destroy_lock_surface(struct wl_listener *listener, void *data) {
	(void)data;
	output_t *output = wl_container_of(listener, output, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface = output->lock_surface;
	bool was_focused = surface->surface == server.seat->keyboard_state.focused_surface;

	output->lock_surface = NULL;
	wl_list_remove(&output->destroy_lock_surface.link);
	wl_list_remove(&output->map_lock_surface.link);

	if (!server.locked || !server.current_session_lock)
		return;
	if (!was_focused)
		return;

	struct wlr_session_lock_surface_v1 *next;
	wl_list_for_each(next, &server.current_session_lock->surfaces, link) {
		if (next == surface || !next->surface->mapped)
			continue;
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
		wlr_seat_keyboard_notify_enter(server.seat, next->surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
		return;
	}

	wlr_seat_keyboard_notify_clear_focus(server.seat);
}

static void map_lock_surface(struct wl_listener *listener, void *data) {
	(void)data;
	output_t *output = wl_container_of(listener, output, map_lock_surface);
	struct wlr_session_lock_surface_v1 *surface = output->lock_surface;

	if (!surface || !server.current_session_lock || !server.locked)
		return;
	if (server.seat->keyboard_state.focused_surface)
		return;

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard)
		wlr_seat_keyboard_notify_enter(server.seat, surface->surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
}

static void lock_new_surface(struct wl_listener *listener, void *data) {
	session_lock_t *session_lock = wl_container_of(listener, session_lock, new_surface);
	struct wlr_session_lock_surface_v1 *surface = data;
	output_t *output = surface->output->data;

	struct wlr_scene_tree *scene_tree = wlr_scene_subsurface_tree_create(session_lock->scene_tree,
		surface->surface);
	surface->surface->data = scene_tree;
	output->lock_surface = surface;

	struct wlr_box box = output->rectangle;
	wlr_scene_node_set_position(&scene_tree->node, box.x, box.y);
	wlr_session_lock_surface_v1_configure(surface, box.width, box.height);

	output->destroy_lock_surface.notify = destroy_lock_surface;
	wl_signal_add(&surface->events.destroy, &output->destroy_lock_surface);

	output->map_lock_surface.notify = map_lock_surface;
	wl_signal_add(&surface->surface->events.map, &output->map_lock_surface);

	if (server.focused_output == output) {
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
		if (keyboard)
			wlr_seat_keyboard_notify_enter(server.seat, surface->surface, keyboard->keycodes,
				keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void lock_unlock(struct wl_listener *listener, void *data) {
	(void)data;
	session_lock_t *session_lock = wl_container_of(listener, session_lock, unlock);
	destroy_unlock(session_lock, true);
}

static void lock_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	session_lock_t *session_lock = wl_container_of(listener, session_lock, destroy);
	destroy_unlock(session_lock, false);
}

static void handle_new_session_lock(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_session_lock_v1 *lock = data;

	if (server.current_session_lock != NULL) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	struct wlr_box full_geo = {0};
	wlr_output_layout_get_box(server.output_layout, NULL, &full_geo);
	wlr_scene_rect_set_size(server.lock_background, full_geo.width, full_geo.height);
	wlr_scene_node_set_enabled(&server.lock_background->node, true);

	struct wlr_surface *focused = server.seat->keyboard_state.focused_surface;
	if (focused) {
		struct wl_client *lock_client = wl_resource_get_client(lock->resource);
		struct wl_client *focused_client = wl_resource_get_client(focused->resource);
		if (focused_client != lock_client)
			wlr_seat_keyboard_notify_clear_focus(server.seat);
	}

	session_lock_t *session_lock = calloc(1, sizeof(*session_lock));
	if (!session_lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	session_lock->scene_tree = wlr_scene_tree_create(server.lock_tree);
	session_lock->session_lock = lock;
	server.current_session_lock = session_lock->session_lock;

	session_lock->new_surface.notify = lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &session_lock->new_surface);

	session_lock->unlock.notify = lock_unlock;
	wl_signal_add(&lock->events.unlock, &session_lock->unlock);

	session_lock->destroy.notify = lock_destroy;
	wl_signal_add(&lock->events.destroy, &session_lock->destroy);

	wlr_session_lock_v1_send_locked(lock);
	server.locked = true;
}

void session_lock_init(void) {
	ONCE();
	server.session_lock_manager = wlr_session_lock_manager_v1_create(server.wl_display);
	if (!server.session_lock_manager) {
		wlr_log(WLR_ERROR, "Failed to create session lock manager");
		exit(EXIT_FAILURE);
	}

	server.new_session_lock.notify = handle_new_session_lock;
	wl_signal_add(&server.session_lock_manager->events.new_lock, &server.new_session_lock);

	server.locked = false;
	server.current_session_lock = NULL;
	const float lockcolor[] = {
		0.1f,
		0.1f,
		0.1f,
		1.0f
	};
	struct wlr_box full_geo = {0};
	wlr_output_layout_get_box(server.output_layout, NULL, &full_geo);
	server.lock_background = wlr_scene_rect_create(server.lock_tree, full_geo.width, full_geo.height,
		lockcolor);
	wlr_scene_node_set_enabled(&server.lock_background->node, false);
}

void session_lock_fini(void) {
	ONCE();
	wl_list_remove(&server.new_session_lock.link);
}
