#pragma once

#include "surface.h"
#include "types.h"

#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <wlr/xwayland.h>
#include <xcb/xproto.h>

struct server_t;

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

typedef struct xwayland_t {
	struct wlr_xwayland *wlr_xwayland;
	struct wlr_xcursor_manager *xcursor_manager;
	xcb_atom_t atoms[ATOM_LAST];
	struct wl_list views;
} xwayland_t;

typedef struct xwayland_toplevel_t {
	node_t *node;
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *content_tree;
	struct wlr_scene_buffer *output_handler;

	struct wlr_scene_tree *border_tree;
	struct wlr_scene_rect *border_rects[4];

	surface_blur_t *blur;
	surface_rounded_t *rounded;
	surface_shadow_t *shadow;

	bool mapped;
	struct wlr_box geometry;

	struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel;
	struct wlr_foreign_toplevel_handle_v1 *foreign_toplevel;
	char *foreign_identifier;

	struct wlr_ext_image_capture_source_v1 *image_capture_source;
	struct wlr_scene_surface *image_capture_surface;
	struct wlr_scene *image_capture;
	struct wlr_scene_tree *image_capture_tree;
	void *capture_renderer;

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
	struct wl_listener set_startup_id;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener override_redirect;
	struct wl_listener outputs_update;

	struct wl_list link;
} xwayland_toplevel_t;

typedef struct xwayland_unmanaged_t {
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
} xwayland_unmanaged_t;

void xwayland_view_close(xwayland_toplevel_t *xwayland_view);
void xwayland_view_set_activated(xwayland_toplevel_t *xwayland_view, bool activated);

void xwayland_set_effect(xwayland_toplevel_t *xwayland_view, surface_effect_t effect, bool enabled);
void xwayland_set_border_radius(xwayland_toplevel_t *xwayland_view, float radius);
void xwayland_set_shadow(xwayland_toplevel_t *xwayland_view, bool enabled);

void xwayland_init(void);
void xwayland_fini(void);
