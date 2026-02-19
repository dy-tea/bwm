#include "lock.h"
#include "server.h"
#include "output.h"
#include "tree.h"
#include <stdlib.h>
#include <wayland-util.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_session_lock_v1.h>

void destroy_unlock(struct bwm_session_lock *session_lock, const bool unlock) {
	if (server.current_session_lock == NULL)
		return;

	wl_list_remove(&session_lock->new_surface.link);
	wl_list_remove(&session_lock->unlock.link);
	wl_list_remove(&session_lock->destroy.link);

	server.current_session_lock = NULL;

	wlr_seat_keyboard_notify_clear_focus(server.seat);

	if (!(server.locked = !unlock)) {
		wlr_scene_node_set_enabled(&server.lock_background->node, false);

		focus_node(server.focused_monitor, server.focused_monitor->desk, server.focused_monitor->desk->focus);
	}

	wlr_scene_node_destroy(&session_lock->scene_tree->node);
	free(session_lock);
}

void destroy_lock_surface(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_output *output = wl_container_of(listener, output, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface = output->lock_surface;

	output->lock_surface = NULL;
	wl_list_remove(&output->destroy_lock_surface.link);

	if (surface->surface != server.seat->keyboard_state.focused_surface)
		return;

	if (server.locked && server.current_session_lock && !wl_list_empty(&server.current_session_lock->surfaces)) {
		surface = wl_container_of(server.current_session_lock->surfaces.next, surface, link);

		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
		wlr_seat_keyboard_notify_enter(server.seat, surface->surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else if (!server.locked) {
		focus_node(server.focused_monitor, server.focused_monitor->desk, server.focused_monitor->desk->focus);
	} else {
		wlr_seat_keyboard_notify_clear_focus(server.seat);
	}
}

void lock_new_surface(struct wl_listener *listener, void *data) {
	struct bwm_session_lock *session_lock = wl_container_of(listener, session_lock, new_surface);
	struct wlr_session_lock_surface_v1 *surface = data;
	struct bwm_output *output = surface->output->data;

	struct wlr_scene_tree *scene_tree = wlr_scene_subsurface_tree_create(session_lock->scene_tree, surface->surface);
	surface->surface->data = scene_tree;
	output->lock_surface = surface;

	struct wlr_box box = output->rectangle;
	wlr_scene_node_set_position(&scene_tree->node, box.x, box.y);
  wlr_session_lock_surface_v1_configure(surface, box.width, box.height);

  output->destroy_lock_surface.notify = destroy_lock_surface;
  wl_signal_add(&surface->events.destroy, &output->destroy_lock_surface);

  if (mon->output == output) {
  	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
  	if (keyboard)
   		wlr_seat_keyboard_notify_enter(server.seat, surface->surface,
       	keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
  }
}

void lock_unlock(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_session_lock *session_lock = wl_container_of(listener, session_lock, unlock);
	destroy_unlock(session_lock, true);
}

void lock_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_session_lock *session_lock = wl_container_of(listener, session_lock, destroy);
	destroy_unlock(session_lock, false);
}

void handle_new_session_lock(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_session_lock_v1 *lock = data;

  wlr_scene_node_set_enabled(&server.lock_background->node, true);

  if (server.current_session_lock != NULL) {
    wlr_session_lock_v1_destroy(lock);
    return;
  }

  struct bwm_session_lock *session_lock = calloc(1, sizeof(struct bwm_session_lock));
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
