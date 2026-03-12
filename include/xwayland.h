#pragma once

#include "types.h"
#include <wayland-server-core.h>
#include <wlr/xwayland.h>
#include <wlr/util/box.h>
#include <xcb/xproto.h>

// Forward declaration to avoid circular dependency
struct bwm_server;

enum atom_name {
	NET_WM_WINDOW_TYPE_NORMAL,
	NET_WM_WINDOW_TYPE_DIALOG,
	NET_WM_WINDOW_TYPE_UTILITY,
	NET_WM_WINDOW_TYPE_TOOLBAR,
	NET_WM_WINDOW_TYPE_SPLASH,
	NET_WM_WINDOW_TYPE_MENU,
	NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	NET_WM_WINDOW_TYPE_POPUP_MENU,
	NET_WM_WINDOW_TYPE_TOOLTIP,
	NET_WM_WINDOW_TYPE_NOTIFICATION,
	NET_WM_STATE_MODAL,
	ATOM_LAST,
};

struct bwm_xwayland {
	struct wlr_xwayland *wlr_xwayland;
	struct wlr_xcursor_manager *xcursor_manager;
	xcb_atom_t atoms[ATOM_LAST];
};

struct bwm_xwayland_view {
	node_t *node;
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *content_tree;

	struct wlr_scene_tree *border_tree;
	struct wlr_scene_rect *border_rects[4];

	bool mapped;
	struct wlr_box geometry;

	struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel;
	struct wlr_foreign_toplevel_handle_v1 *foreign_toplevel;
	char *foreign_identifier;

	struct wlr_ext_image_capture_source_v1 *image_capture_source;
	struct wlr_scene_surface *image_capture_surface;
	struct wlr_scene *image_capture;
	struct wlr_scene_tree *image_capture_tree;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_configure;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener request_activate;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener set_class;
	struct wl_listener set_hints;
	struct wl_listener set_window_type;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener override_redirect;
};

struct bwm_xwayland_unmanaged {
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_surface *surface_scene;

	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_geometry;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener override_redirect;
};

void handle_xwayland_ready(struct wl_listener *listener, void *data);
void handle_xwayland_surface(struct wl_listener *listener, void *data);

void xwayland_view_close(struct bwm_xwayland_view *xwayland_view);
void xwayland_view_set_activated(struct bwm_xwayland_view *xwayland_view, bool activated);
