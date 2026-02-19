#pragma once

#include <wayland-server.h>

struct bwm_session_lock {
	struct wlr_session_lock_v1 *session_lock;
	struct wlr_scene_tree *scene_tree;

	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

void handle_new_session_lock(struct wl_listener *listener, void *data);
void destroy_lock_surface(struct wl_listener *listener, void *data);
