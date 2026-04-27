#include "xwayland.h"
#include "server.h"
#include "toplevel.h"
#include "types.h"
#include "tree.h"
#include "input_method.h"
#include "rule.h"
#include "workspace.h"
#include "scroller.h"
#include "keyboard.h"
#include "tabs.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static const char *atom_map[ATOM_LAST] = {
	[NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	[NET_WM_WINDOW_TYPE_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
	[NET_WM_WINDOW_TYPE_UTILITY] = "_NET_WM_WINDOW_TYPE_UTILITY",
	[NET_WM_WINDOW_TYPE_TOOLBAR] = "_NET_WM_WINDOW_TYPE_TOOLBAR",
	[NET_WM_WINDOW_TYPE_SPLASH] = "_NET_WM_WINDOW_TYPE_SPLASH",
	[NET_WM_WINDOW_TYPE_MENU] = "_NET_WM_WINDOW_TYPE_MENU",
	[NET_WM_WINDOW_TYPE_DROPDOWN_MENU] = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
	[NET_WM_WINDOW_TYPE_POPUP_MENU] = "_NET_WM_WINDOW_TYPE_POPUP_MENU",
	[NET_WM_WINDOW_TYPE_TOOLTIP] = "_NET_WM_WINDOW_TYPE_TOOLTIP",
	[NET_WM_WINDOW_TYPE_NOTIFICATION] = "_NET_WM_WINDOW_TYPE_NOTIFICATION",
	[NET_WM_STATE_MODAL] = "_NET_WM_STATE_MODAL",
};

// Forward declarations
static void xwayland_view_destroy(struct bwm_xwayland_view *xwayland_view);
static struct bwm_xwayland_view *create_xwayland_view(struct wlr_xwayland_surface *xsurface);
static struct bwm_xwayland_unmanaged *create_unmanaged(struct wlr_xwayland_surface *xsurface);
static void begin_xwayland_interactive(struct bwm_xwayland_view *xwayland_view,
	enum cursor_mode mode, uint32_t edges);

static void unmanaged_handle_request_configure(struct wl_listener *listener, void *data) {
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, request_configure);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;
	struct wlr_xwayland_surface_configure_event *ev = data;

	wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width, ev->height);

	if (surface->surface_scene) {
		int abs_x = ev->x;
		int abs_y = ev->y;

		if (xsurface->parent) {
			if (!xsurface->parent->override_redirect && xsurface->parent->data) {
				struct bwm_xwayland_view *parent_view = xsurface->parent->data;
				if (parent_view->xwayland_surface && parent_view->node) {
					int x_offset = ev->x - parent_view->xwayland_surface->x;
					int y_offset = ev->y - parent_view->xwayland_surface->y;
					if (parent_view->node->client && parent_view->node->client->state == STATE_FLOATING) {
						abs_x = parent_view->node->client->floating_rectangle.x + x_offset;
						abs_y = parent_view->node->client->floating_rectangle.y + y_offset;
					} else if (parent_view->node) {
						abs_x = parent_view->node->rectangle.x + x_offset;
						abs_y = parent_view->node->rectangle.y + y_offset;
					}
				} else goto add_abs;
			} else {
add_abs:
				abs_x += xsurface->parent->x;
				abs_y += xsurface->parent->y;
			}
		}

		wlr_scene_node_set_position(&surface->surface_scene->buffer->node,
			abs_x, abs_y);
	}
}

static void unmanaged_handle_set_geometry(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, set_geometry);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	int abs_x = xsurface->x;
	int abs_y = xsurface->y;

	if (xsurface->parent) {
		if (!xsurface->parent->override_redirect && xsurface->parent->data) {
			struct bwm_xwayland_view *parent_view = xsurface->parent->data;
			if (parent_view->xwayland_surface && parent_view->node) {
				// Convert from X11 space to screen space
				int x_offset = xsurface->x - parent_view->xwayland_surface->x;
				int y_offset = xsurface->y - parent_view->xwayland_surface->y;
				if (parent_view->node->client && parent_view->node->client->state == STATE_FLOATING) {
					abs_x = parent_view->node->client->floating_rectangle.x + x_offset;
					abs_y = parent_view->node->client->floating_rectangle.y + y_offset;
				} else if (parent_view->node) {
					abs_x = parent_view->node->rectangle.x + x_offset;
					abs_y = parent_view->node->rectangle.y + y_offset;
				}
			} else goto add_abs;
		} else {
add_abs:
			abs_x += xsurface->parent->x;
			abs_y += xsurface->parent->y;
		}
	}

	wlr_scene_node_set_position(&surface->surface_scene->buffer->node,
		abs_x, abs_y);
}

static void unmanaged_handle_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, map);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	surface->surface_scene = wlr_scene_surface_create(server.over_tree, xsurface->surface);

	if (surface->surface_scene) {
		int abs_x = xsurface->x;
		int abs_y = xsurface->y;

		wlr_log(WLR_INFO, "Unmanaged window %u map: x=%d y=%d size=%dx%d parent=%p",
			xsurface->window_id, xsurface->x, xsurface->y,
			xsurface->width, xsurface->height, (void*)xsurface->parent);

		if (xsurface->parent) {
			wlr_log(WLR_INFO, "Parent window %u at X11 coords (%d,%d) override_redirect=%d data=%p",
				xsurface->parent->window_id,
				xsurface->parent->x, xsurface->parent->y,
				xsurface->parent->override_redirect, xsurface->parent->data);

			// If parent is a managed window, use its X11 coordinates
			if (!xsurface->parent->override_redirect && xsurface->parent->data) {
				struct bwm_xwayland_view *parent_view = xsurface->parent->data;
				wlr_log(WLR_INFO, "Parent view found: scene_tree=%p node=%p",
					(void*)parent_view->scene_tree, (void*)parent_view->node);
				if (parent_view->xwayland_surface && parent_view->node) {
					// Convert from X11 space to screen space
					int x_offset = xsurface->x - parent_view->xwayland_surface->x;
					int y_offset = xsurface->y - parent_view->xwayland_surface->y;
					if (parent_view->node->client && parent_view->node->client->state == STATE_FLOATING) {
						abs_x = parent_view->node->client->floating_rectangle.x + x_offset;
						abs_y = parent_view->node->client->floating_rectangle.y + y_offset;
					} else if (parent_view->node) {
						abs_x = parent_view->node->rectangle.x + x_offset;
						abs_y = parent_view->node->rectangle.y + y_offset;
					}
					wlr_log(WLR_INFO, "  Parent X11 at (%d,%d), node at (%d,%d), popup X11 at (%d,%d), offset (%d,%d), final: (%d,%d)",
						parent_view->xwayland_surface->x, parent_view->xwayland_surface->y,
						parent_view->node->rectangle.x, parent_view->node->rectangle.y,
						xsurface->x, xsurface->y,
						x_offset, y_offset,
						abs_x, abs_y);
				} else goto add_abs;
			} else {
add_abs:
				abs_x += xsurface->parent->x;
				abs_y += xsurface->parent->y;
			}
		} else {
			struct bwm_xwayland_view *focused_view = server.last_focused_xwayland_view;

			// If last_focused is NULL, try to find ANY mapped xwayland window by scanning nodes
			if (!focused_view) {
				wlr_log(WLR_INFO, "  last_focused is NULL, searching for any mapped xwayland window");
				monitor_t *mon = server.focused_monitor ? server.focused_monitor : mon_head;
				if (mon && mon->desk) {
					desktop_t *d = mon->desk;
					// Search through the tree for any xwayland window
					node_t *n = d->root;
					while (n && !focused_view) {
						if (n->client && n->client->toplevel == NULL) {
							if (n->rectangle.x > 0 || n->rectangle.y > 0 ||
								n->client->floating_rectangle.x > 0 || n->client->floating_rectangle.y > 0) {
								if (n->client->state == STATE_FLOATING) {
									abs_x = n->client->floating_rectangle.x + xsurface->x;
									abs_y = n->client->floating_rectangle.y + xsurface->y;
								} else {
									abs_x = n->rectangle.x + xsurface->x;
									abs_y = n->rectangle.y + xsurface->y;
								}
								wlr_log(WLR_INFO, "Using found node position, final: (%d,%d)", abs_x, abs_y);
								focused_view = (struct bwm_xwayland_view *)1;
								break;
							}
						}
						if (n->first_child) {
							n = n->first_child;
						} else if (n->second_child) {
							n = n->second_child;
						} else {
							while (n->parent) {
								if (n == n->parent->first_child && n->parent->second_child) {
									n = n->parent->second_child;
									break;
								}
								n = n->parent;
							}
							if (!n->parent) break;
						}
					}
				}
			}

			if (focused_view && focused_view != (struct bwm_xwayland_view *)1 &&
			    focused_view->mapped && focused_view->node && focused_view->node->client) {
				struct wlr_xwayland_surface *parent_xsurface = focused_view->xwayland_surface;

				int x_offset = xsurface->x - parent_xsurface->x;
				int y_offset = xsurface->y - parent_xsurface->y;
				if (focused_view->node->client->state == STATE_FLOATING) {
					abs_x = focused_view->node->client->floating_rectangle.x + x_offset;
					abs_y = focused_view->node->client->floating_rectangle.y + y_offset;
				} else {
					abs_x = focused_view->node->rectangle.x + x_offset;
					abs_y = focused_view->node->rectangle.y + y_offset;
				}
			}
		}

		wlr_scene_node_set_position(&surface->surface_scene->buffer->node,
			abs_x, abs_y);

		wl_signal_add(&xsurface->events.set_geometry, &surface->set_geometry);
		surface->set_geometry.notify = unmanaged_handle_set_geometry;
	}

	// Focus override-redirect windows that want focus
	if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
		struct wlr_xwayland *xwayland = server.xwayland.wlr_xwayland;
		wlr_xwayland_set_seat(xwayland, server.seat);
		wlr_seat_keyboard_notify_enter(server.seat, xsurface->surface, NULL, 0, NULL);
		input_method_relay_set_focus(server.input_method_relay, xsurface->surface);
	}
}

static void unmanaged_handle_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, unmap);

	if (surface->surface_scene) {
		wl_list_remove(&surface->set_geometry.link);
		wlr_scene_node_destroy(&surface->surface_scene->buffer->node);
		surface->surface_scene = NULL;
	}
}

static void unmanaged_handle_request_activate(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, request_activate);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	if (xsurface->surface == NULL || !xsurface->surface->mapped)
		return;

	if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
		wlr_seat_keyboard_notify_enter(server.seat, xsurface->surface, NULL, 0, NULL);
		input_method_relay_set_focus(server.input_method_relay, xsurface->surface);
	}
}

static void unmanaged_handle_associate(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, associate);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	wl_signal_add(&xsurface->surface->events.map, &surface->map);
	surface->map.notify = unmanaged_handle_map;
	wl_signal_add(&xsurface->surface->events.unmap, &surface->unmap);
	surface->unmap.notify = unmanaged_handle_unmap;
}

static void unmanaged_handle_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, dissociate);
	wl_list_remove(&surface->map.link);
	wl_list_remove(&surface->unmap.link);
}

static void unmanaged_handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, destroy);

	wl_list_remove(&surface->request_configure.link);
	wl_list_remove(&surface->request_activate.link);
	wl_list_remove(&surface->associate.link);
	wl_list_remove(&surface->dissociate.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->override_redirect.link);
	free(surface);
}

static void unmanaged_handle_override_redirect(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_unmanaged *surface =
		wl_container_of(listener, surface, override_redirect);
	struct wlr_xwayland_surface *xsurface = surface->xwayland_surface;

	bool associated = xsurface->surface != NULL;
	bool mapped = associated && xsurface->surface->mapped;

	if (mapped)
		unmanaged_handle_unmap(&surface->unmap, NULL);
	if (associated)
		unmanaged_handle_dissociate(&surface->dissociate, NULL);

	unmanaged_handle_destroy(&surface->destroy, NULL);
	xsurface->data = NULL;

	struct bwm_xwayland_view *xwayland_view = create_xwayland_view(xsurface);
	if (xwayland_view && associated) {
		// idk
	}
}

static struct bwm_xwayland_unmanaged *create_unmanaged(struct wlr_xwayland_surface *xsurface) {
	struct bwm_xwayland_unmanaged *surface = calloc(1, sizeof(struct bwm_xwayland_unmanaged));
	if (surface == NULL)
		return NULL;

	surface->xwayland_surface = xsurface;

	wl_signal_add(&xsurface->events.request_configure, &surface->request_configure);
	surface->request_configure.notify = unmanaged_handle_request_configure;
	wl_signal_add(&xsurface->events.request_activate, &surface->request_activate);
	surface->request_activate.notify = unmanaged_handle_request_activate;
	wl_signal_add(&xsurface->events.associate, &surface->associate);
	surface->associate.notify = unmanaged_handle_associate;
	wl_signal_add(&xsurface->events.dissociate, &surface->dissociate);
	surface->dissociate.notify = unmanaged_handle_dissociate;
	wl_signal_add(&xsurface->events.destroy, &surface->destroy);
	surface->destroy.notify = unmanaged_handle_destroy;
	wl_signal_add(&xsurface->events.set_override_redirect, &surface->override_redirect);
	surface->override_redirect.notify = unmanaged_handle_override_redirect;

	return surface;
}

// TODO: maybe use this?
static void xwayland_view_get_constraints(struct bwm_xwayland_view *xwayland_view,
		double *min_width, double *max_width, double *min_height, double *max_height) {
	struct wlr_xwayland_surface *surface = xwayland_view->xwayland_surface;
	xcb_size_hints_t *size_hints = surface->size_hints;

	if (size_hints == NULL) {
		*min_width = 0;
		*max_width = DBL_MAX;
		*min_height = 0;
		*max_height = DBL_MAX;
		return;
	}

	*min_width = size_hints->min_width > 0 ? size_hints->min_width : 0;
	*max_width = size_hints->max_width > 0 ? size_hints->max_width : DBL_MAX;
	*min_height = size_hints->min_height > 0 ? size_hints->min_height : 0;
	*max_height = size_hints->max_height > 0 ? size_hints->max_height : DBL_MAX;
}

static bool xwayland_view_wants_floating(struct bwm_xwayland_view *xwayland_view) {
	struct wlr_xwayland_surface *surface = xwayland_view->xwayland_surface;

	if (surface->modal)
		return true;

	struct bwm_xwayland *xwayland = &server.xwayland;
	for (size_t i = 0; i < surface->window_type_len; i++) {
		xcb_atom_t type = surface->window_type[i];
		if (type == xwayland->atoms[NET_WM_WINDOW_TYPE_DIALOG] ||
			type == xwayland->atoms[NET_WM_WINDOW_TYPE_UTILITY] ||
			type == xwayland->atoms[NET_WM_WINDOW_TYPE_TOOLBAR] ||
			type == xwayland->atoms[NET_WM_WINDOW_TYPE_SPLASH]) {
			return true;
		}
	}

	xcb_size_hints_t *size_hints = surface->size_hints;
	if (size_hints != NULL &&
			size_hints->min_width > 0 && size_hints->min_height > 0 &&
			(size_hints->max_width == size_hints->min_width ||
			size_hints->max_height == size_hints->min_height)) {
		return true;
	}

	return false;
}

static void xwayland_view_configure(struct bwm_xwayland_view *xwayland_view,
		int x, int y, int width, int height) {
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;
	wlr_xwayland_surface_configure(xsurface, x, y, width, height);

	if (xwayland_view->scene_tree && xwayland_view->node &&
			xwayland_view->node->client &&
			xwayland_view->node->client->state == STATE_FLOATING) {
		wlr_scene_node_set_position(&xwayland_view->scene_tree->node, x, y);
	}
}

static void xwayland_view_set_fullscreen(struct bwm_xwayland_view *xwayland_view, bool fullscreen) {
	struct wlr_xwayland_surface *surface = xwayland_view->xwayland_surface;
	wlr_xwayland_surface_set_fullscreen(surface, fullscreen);
}

void xwayland_view_set_activated(struct bwm_xwayland_view *xwayland_view, bool activated) {
	struct wlr_xwayland_surface *surface = xwayland_view->xwayland_surface;

	if (!surface || !surface->surface)
		return;

	if (activated && surface->minimized)
		wlr_xwayland_surface_set_minimized(surface, false);

	wlr_xwayland_surface_activate(surface, activated);

	if (activated) {
		wlr_xwayland_surface_set_fullscreen(surface, surface->fullscreen);

		// raise scene tree to top
		if (xwayland_view->scene_tree)
			wlr_scene_node_raise_to_top(&xwayland_view->scene_tree->node);

		// notify keyboard seat
		struct wlr_seat *seat = server.seat;
		if (seat->keyboard_state.keyboard != NULL) {
			wlr_seat_keyboard_notify_enter(seat, surface->surface,
				seat->keyboard_state.keyboard->keycodes,
				seat->keyboard_state.keyboard->num_keycodes,
				&seat->keyboard_state.keyboard->modifiers);
		}

		// update ime focus
		input_method_relay_set_focus(server.input_method_relay, surface->surface);

		// update foreign toplevel
		if (xwayland_view->foreign_toplevel)
			wlr_foreign_toplevel_handle_v1_set_activated(xwayland_view->foreign_toplevel, true);
	}
}

void xwayland_view_close(struct bwm_xwayland_view *xwayland_view) {
	if (!xwayland_view || !xwayland_view->xwayland_surface)
		return;
	struct wlr_xwayland_surface *surface = xwayland_view->xwayland_surface;
	wlr_xwayland_surface_close(surface);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, commit);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;
	struct wlr_surface_state *state = &xsurface->surface->current;

	struct wlr_box new_geo = {0};
	new_geo.width = state->width;
	new_geo.height = state->height;

	if (xwayland_view->geometry.width != new_geo.width ||
			xwayland_view->geometry.height != new_geo.height) {
		xwayland_view->geometry = new_geo;
		if (xwayland_view->node && xwayland_view->node->client &&
				xwayland_view->node->client->state == STATE_FLOATING) {
			xwayland_view->node->client->floating_rectangle.width = new_geo.width;
			xwayland_view->node->client->floating_rectangle.height = new_geo.height;
		} else if (xwayland_view->node && xwayland_view->node->client &&
			xwayland_view->node->client->state == STATE_TILED) {
		}
	}
}

static void handle_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, map);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	xwayland_view->mapped = true;

	if (!xsurface->surface) {
		wlr_log(WLR_ERROR, "XWayland surface %u has no wlr_surface at map time", xsurface->window_id);
		return;
	}

	wlr_log(WLR_DEBUG, "XWayland map: window_id=%u class='%s' title='%s' override_redirect=%d mapped=%d",
		xsurface->window_id,
		xsurface->class ? xsurface->class : "(null)",
		xsurface->title ? xsurface->title : "(null)",
		xsurface->override_redirect,
		xsurface->surface->mapped);

	wlr_scene_subsurface_tree_create(xwayland_view->content_tree, xsurface->surface);

	wl_signal_add(&xsurface->surface->events.commit, &xwayland_view->commit);
	xwayland_view->commit.notify = handle_commit;

	bool wants_float = xwayland_view_wants_floating(xwayland_view);

	xwayland_view->geometry.width = xsurface->width;
	xwayland_view->geometry.height = xsurface->height;

	monitor_t *mon = server.focused_monitor ? server.focused_monitor : mon_head;
	if (!mon) {
		wlr_log(WLR_ERROR, "No monitor available for xwayland view");
		return;
	}

	desktop_t *d = mon->desk;
	if (!d) {
		wlr_log(WLR_ERROR, "No desktop available for xwayland view");
		return;
	}

	node_t *node = make_node(next_node_id++);
	if (!node) {
		wlr_log(WLR_ERROR, "Failed to create node for xwayland view");
		return;
	}

	client_t *client = make_client();
	if (!client) {
		wlr_log(WLR_ERROR, "Failed to create client for xwayland view");
		free_node(node);
		return;
	}

	node->client = client;
	client->toplevel = NULL;
	client->xwayland_view = xwayland_view;
	xwayland_view->node = node;
	node->monitor = mon;

	const char *app_id = xsurface->class;
	const char *title = xsurface->title;

	if (app_id) {
		strncpy(client->app_id, app_id, MAXLEN - 1);
		client->app_id[MAXLEN - 1] = '\0';
	}

	if (title) {
		strncpy(client->title, title, MAXLEN - 1);
		client->title[MAXLEN - 1] = '\0';
	}

	rule_consequence_t *rule = find_matching_rule(app_id, title);

	if (rule && rule->has_manage && !rule->manage) {
		wlr_log(WLR_INFO, "XWayland window %s ignored by rule (manage=off)", app_id ? app_id : "?");
		xwayland_view->node = NULL;
		free_node(node);
		return;
	}

	if (rule && rule->has_state)
		client->state = rule->state;

	bool should_focus = true;
	if (rule && rule->has_focus && !rule->focus)
		should_focus = false;

	desktop_t *target_desktop = d;
	if (rule && rule->has_desktop) {
		desktop_t *new_desk = find_desktop_by_name(rule->desktop);
		if (new_desk)
			target_desktop = new_desk;
		else
			wlr_log(WLR_ERROR, "XWayland rule: desktop '%s' not found", rule->desktop);
	}

	monitor_t *target_monitor = target_desktop->monitor ? target_desktop->monitor : mon;
	node->monitor = target_monitor;

	if (rule && rule->has_hidden)
		node->hidden = rule->hidden;

	if (rule && rule->has_sticky)
		node->sticky = rule->sticky;

	if (rule && rule->has_locked)
		node->locked = rule->locked;

	if (rule && (rule->has_scroller_proportion || rule->has_scroller_proportion_single)) {
		scroller_apply_client_rules(client,
			rule->has_scroller_proportion ? rule->scroller_proportion : 0.0f,
			rule->has_scroller_proportion_single ? rule->scroller_proportion_single : 0.0f);
	}

	wlr_log(WLR_INFO, "XWayland window mapped: %s (%s) wants_float=%d size=%dx%d",
		title ? title : "untitled", app_id ? app_id : "unknown",
		wants_float, xsurface->width, xsurface->height);

	struct wlr_ext_foreign_toplevel_handle_v1_state ext_state = {
		.app_id = app_id,
		.title = title,
	};
	xwayland_view->ext_foreign_toplevel =
		wlr_ext_foreign_toplevel_handle_v1_create(server.foreign_toplevel_list, &ext_state);
	xwayland_view->ext_foreign_toplevel->data = xwayland_view;
	xwayland_view->foreign_identifier = xwayland_view->ext_foreign_toplevel->identifier;

	xwayland_view->foreign_toplevel =
		wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);

	if (app_id)
		wlr_foreign_toplevel_handle_v1_set_app_id(xwayland_view->foreign_toplevel, app_id);

	bool rule_forces_float = rule && rule->has_state && rule->state == STATE_FLOATING;
	if (wants_float || rule_forces_float) {
		wlr_scene_node_reparent(&xwayland_view->scene_tree->node, server.float_tree);
		client->floating_rectangle.x = xsurface->x;
		client->floating_rectangle.y = xsurface->y;
		client->floating_rectangle.width = xsurface->width;
		client->floating_rectangle.height = xsurface->height;
		client->state = STATE_FLOATING;
		client->last_state = STATE_TILED;

		wlr_scene_node_set_position(&xwayland_view->scene_tree->node,
			client->floating_rectangle.x, client->floating_rectangle.y);

		node->hidden = true;
		client->shown = true;
		wlr_scene_node_set_enabled(&xwayland_view->scene_tree->node, true);
	} else {
		if (client->state != STATE_PSEUDO_TILED)
			client->state = STATE_TILED;
		node->rectangle.width = xsurface->width;
		node->rectangle.height = xsurface->height;
		client->shown = true;
		wlr_scene_node_set_enabled(&xwayland_view->scene_tree->node, true);
		wlr_scene_node_set_enabled(&xwayland_view->content_tree->node, true);
		wlr_log(WLR_DEBUG, "XWayland window will be tiled, scene_tree=%p enabled=%d",
			(void*)xwayland_view->scene_tree,
			xwayland_view->scene_tree->node.enabled);
	}

	bool target_desktop_is_focused = (target_desktop == target_monitor->desk);

	insert_node(target_desktop, node, target_desktop->focus);

	if (target_desktop != d && !target_desktop_is_focused) {
		client->shown = false;
		wlr_scene_node_set_enabled(&xwayland_view->scene_tree->node, false);
	}

	if (should_focus && target_desktop_is_focused)
		focus_node(target_monitor, target_desktop, node);
	else if (should_focus && !target_desktop_is_focused)
		target_desktop->focus = node;

	if (rule && rule->has_state && rule->state == STATE_FULLSCREEN)
		toggle_fullscreen();

	if (disable_decorations)
		wlr_xwayland_surface_set_fullscreen(xsurface, true);

	arrange(target_monitor, target_desktop, target_desktop_is_focused);

	if (!wants_float && xwayland_view->node && xwayland_view->node->client) {
		client_t *client = xwayland_view->node->client;
		struct wlr_box *rect = &client->tiled_rectangle;
		wlr_xwayland_surface_configure(xsurface, rect->x, rect->y, rect->width, rect->height);
		xwayland_view->geometry.width = rect->width;
		xwayland_view->geometry.height = rect->height;
		wlr_scene_node_set_position(&xwayland_view->scene_tree->node, rect->x, rect->y);
	}

	xwayland_view_set_activated(xwayland_view, true);
	server.last_focused_xwayland_view = xwayland_view;

	xwayland_view->image_capture_surface = wlr_scene_surface_create(
		&xwayland_view->image_capture->tree, xsurface->surface);

	wlr_log(WLR_DEBUG, "XWayland window map complete: scene_tree enabled=%d shown=%d",
		xwayland_view->scene_tree->node.enabled, client->shown);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, unmap);

	xwayland_view->mapped = false;

	wl_list_remove(&xwayland_view->commit.link);

	xwayland_view->image_capture_surface = NULL;

	if (xwayland_view->ext_foreign_toplevel) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(xwayland_view->ext_foreign_toplevel);
		xwayland_view->ext_foreign_toplevel = NULL;
	}

	if (xwayland_view->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_destroy(xwayland_view->foreign_toplevel);
		xwayland_view->foreign_toplevel = NULL;
	}

	if (xwayland_view->scene_tree) {
		wlr_scene_node_set_enabled(&xwayland_view->scene_tree->node, false);
		wlr_scene_node_set_enabled(&xwayland_view->content_tree->node, false);
	}

	if (xwayland_view->node) {
		monitor_t *mon = xwayland_view->node->monitor;
		desktop_t *desk = NULL;
		if (mon) {
			for (desktop_t *d = mon->desk_head; d; d = d->next) {
				node_t *n = d->root;
				if (n == xwayland_view->node ||
				    (n && (n->first_child == xwayland_view->node || n->second_child == xwayland_view->node))) {
					desk = d;
					break;
				}
			}
		}

		if (xwayland_view->node->client)
			xwayland_view->node->client->xwayland_view = NULL;

		if (desk) {
			remove_node(desk, xwayland_view->node);
			if (mon && desk) {
				arrange(mon, desk, true);
				if (desk->focus != NULL && desk->focus->client != NULL)
					focus_node(mon, desk, desk->focus);
				else if (desk->root != NULL) {
					desk->focus = first_extrema(desk->root);
					if (desk->focus != NULL)
						focus_node(mon, desk, desk->focus);
				}
			}
		}

		xwayland_view->node->destroying = true;
		xwayland_view->node = NULL;
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, destroy);

	if (xwayland_view->mapped)
		handle_unmap(&xwayland_view->unmap, NULL);

	if (server.last_focused_xwayland_view == xwayland_view) {
		uint32_t window_id = xwayland_view->xwayland_surface ?
			xwayland_view->xwayland_surface->window_id : 0;
		wlr_log(WLR_INFO, "Clearing last_focused_xwayland_view (window %u being destroyed)",
			window_id);
		server.last_focused_xwayland_view = NULL;
	}

	if (xwayland_view->node && xwayland_view->node->client)
		xwayland_view->node->client->xwayland_view = NULL;
	xwayland_view->xwayland_surface = NULL;

	wl_list_remove(&xwayland_view->destroy.link);
	wl_list_remove(&xwayland_view->request_configure.link);
	wl_list_remove(&xwayland_view->request_fullscreen.link);
	wl_list_remove(&xwayland_view->request_minimize.link);
	wl_list_remove(&xwayland_view->request_activate.link);
	wl_list_remove(&xwayland_view->request_move.link);
	wl_list_remove(&xwayland_view->request_resize.link);
	wl_list_remove(&xwayland_view->set_title.link);
	wl_list_remove(&xwayland_view->set_class.link);
	wl_list_remove(&xwayland_view->set_hints.link);
	wl_list_remove(&xwayland_view->set_window_type.link);
	wl_list_remove(&xwayland_view->associate.link);
	wl_list_remove(&xwayland_view->dissociate.link);
	wl_list_remove(&xwayland_view->override_redirect.link);

	if (xwayland_view->image_capture != NULL) {
		wlr_scene_node_destroy(&xwayland_view->image_capture->tree.node);
		xwayland_view->image_capture = NULL;
		xwayland_view->image_capture_source = NULL;
	}

	destroy_borders(&xwayland_view->border_tree, xwayland_view->border_rects);

	if (xwayland_view->scene_tree) {
		wlr_scene_node_destroy(&xwayland_view->scene_tree->node);
		xwayland_view->scene_tree = NULL;
		xwayland_view->content_tree = NULL;
	}

	free(xwayland_view);
}

static void handle_request_configure(struct wl_listener *listener, void *data) {
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	if (!xsurface->surface || !xsurface->surface->mapped) {
		wlr_xwayland_surface_configure(xsurface, ev->x, ev->y, ev->width, ev->height);
		return;
	}

	// honor floating window request
	if (xwayland_view->node && xwayland_view->node->client &&
			xwayland_view->node->client->state == STATE_FLOATING) {
		xwayland_view_configure(xwayland_view, ev->x, ev->y, ev->width, ev->height);
		if (xwayland_view->node) {
			xwayland_view->node->client->floating_rectangle.x = ev->x;
			xwayland_view->node->client->floating_rectangle.y = ev->y;
			xwayland_view->node->client->floating_rectangle.width = ev->width;
			xwayland_view->node->client->floating_rectangle.height = ev->height;
		}
	} else {
		if (xwayland_view->node && xwayland_view->node->client) {
			client_t *client = xwayland_view->node->client;
			struct wlr_box rect;
			if (client->state == STATE_FULLSCREEN && xwayland_view->node->monitor)
				rect = xwayland_view->node->monitor->rectangle;
			else
				rect = client->tiled_rectangle;
			wlr_xwayland_surface_configure(xsurface, rect.x, rect.y, rect.width, rect.height);
		}
	}
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, request_fullscreen);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	node_t *node = xwayland_view->node;
	if (!node || !node->client || !node->monitor || !node->desktop)
		return;

	client_t *client = node->client;
	monitor_t *m = node->monitor;
	desktop_t *d = node->desktop;

	wlr_log(WLR_INFO, "handle_request_fullscreen: xwayland wants fullscreen=%d, current state=%d last_state=%d",
		xsurface->fullscreen, client->state, client->last_state);

	struct wlr_scene_tree *scene_tree = client_get_scene_tree(client);
	if (!scene_tree) {
		wlr_log(WLR_ERROR, "handle_request_fullscreen: no scene tree");
		return;
	}

	if (xsurface->fullscreen) {
		if (client->state == STATE_FULLSCREEN) {
			wlr_xwayland_surface_set_fullscreen(xsurface, true);
			return;
		}
		wlr_scene_node_reparent(&scene_tree->node, server.full_tree);
		wlr_xwayland_surface_set_fullscreen(xsurface, true);
		set_state(m, d, node, STATE_FULLSCREEN);
	} else {
		if (client->state != STATE_FULLSCREEN) {
			wlr_xwayland_surface_set_fullscreen(xsurface, false);
			return;
		}
		wlr_log(WLR_INFO, "handle_request_fullscreen: denying remove-fullscreen");
		wlr_xwayland_surface_set_fullscreen(xsurface, true);
	}
}

static void handle_request_minimize(struct wl_listener *listener, void *data) {
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, request_minimize);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;
	struct wlr_xwayland_minimize_event *ev = data;

	if (xwayland_view->node)
		xwayland_view->node->hidden = ev->minimize;

	wlr_xwayland_surface_set_minimized(xsurface, ev->minimize);
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, request_activate);

	if (!xwayland_view->xwayland_surface->surface ||
			!xwayland_view->xwayland_surface->surface->mapped)
		return;

	if (xwayland_view->node) {
		monitor_t *mon = xwayland_view->node->monitor;
		if (mon) {
			for (desktop_t *d = mon->desk_head; d; d = d->next) {
				node_t *n = d->root;
				if (n == xwayland_view->node ||
				    (n && (n->first_child == xwayland_view->node || n->second_child == xwayland_view->node))) {
					focus_node(mon, d, xwayland_view->node);
					break;
				}
			}
		}
	}
}

static void handle_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, request_move);

	if (!xwayland_view->xwayland_surface->surface ||
			!xwayland_view->xwayland_surface->surface->mapped)
		return;

	if (xwayland_view->node && xwayland_view->node->client &&
			xwayland_view->node->client->state == STATE_FLOATING)
		begin_xwayland_interactive(xwayland_view, CURSOR_MOVE, 0);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, request_resize);
	struct wlr_xwayland_resize_event *ev = data;

	if (!xwayland_view->xwayland_surface->surface ||
			!xwayland_view->xwayland_surface->surface->mapped)
		return;

	if (xwayland_view->node && xwayland_view->node->client &&
			xwayland_view->node->client->state == STATE_FLOATING)
		begin_xwayland_interactive(xwayland_view, CURSOR_RESIZE, ev->edges);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, set_title);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	if (xwayland_view->node && xwayland_view->node->client && xsurface->title) {
		strncpy(xwayland_view->node->client->title, xsurface->title, MAXLEN - 1);
		xwayland_view->node->client->title[MAXLEN - 1] = '\0';
		tabs_update_label_for_leaf(xwayland_view->node);
	}
}

static void handle_set_class(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, set_class);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	if (xwayland_view->node && xwayland_view->node->client && xsurface->class) {
		strncpy(xwayland_view->node->client->app_id, xsurface->class, MAXLEN - 1);
		xwayland_view->node->client->app_id[MAXLEN - 1] = '\0';
		tabs_update_label_for_leaf(xwayland_view->node);
	}
}

static void handle_set_hints(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, set_hints);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	if (xwayland_view->node && xwayland_view->node->client) {
		if (xsurface->hints && (xsurface->hints->flags & XCB_ICCCM_WM_HINT_X_URGENCY))
			xwayland_view->node->client->urgent = true;
		else
			xwayland_view->node->client->urgent = false;
	}
}

static void handle_set_window_type(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, set_window_type);

	bool should_float = xwayland_view_wants_floating(xwayland_view);

	if (xwayland_view->node && xwayland_view->node->client) {
		client_state_t current_state = xwayland_view->node->client->state;
		bool is_floating = (current_state == STATE_FLOATING);

		if (should_float && !is_floating)
			xwayland_view->node->client->state = STATE_FLOATING;
		else if (!should_float && is_floating)
			xwayland_view->node->client->state = STATE_TILED;
	}
}

static void handle_associate(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, associate);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	wl_signal_add(&xsurface->surface->events.map, &xwayland_view->map);
	xwayland_view->map.notify = handle_map;
	wl_signal_add(&xsurface->surface->events.unmap, &xwayland_view->unmap);
	xwayland_view->unmap.notify = handle_unmap;
}

static void handle_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, dissociate);
	wl_list_remove(&xwayland_view->map.link);
	wl_list_remove(&xwayland_view->unmap.link);
}

static void handle_override_redirect(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_xwayland_view *xwayland_view =
		wl_container_of(listener, xwayland_view, override_redirect);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	bool associated = xsurface->surface != NULL;
	bool mapped = associated && xsurface->surface->mapped;

	if (mapped)
		handle_unmap(&xwayland_view->unmap, NULL);
	if (associated)
		handle_dissociate(&xwayland_view->dissociate, NULL);

	xwayland_view_destroy(xwayland_view);
	xsurface->data = NULL;

	struct bwm_xwayland_unmanaged *unmanaged = create_unmanaged(xsurface);
	if (unmanaged && associated) {
		unmanaged_handle_associate(&unmanaged->associate, NULL);
		if (mapped)
			unmanaged_handle_map(&unmanaged->map, NULL);
	}
}

static struct bwm_xwayland_view *create_xwayland_view(struct wlr_xwayland_surface *xsurface) {
	struct bwm_xwayland_view *xwayland_view = calloc(1, sizeof(struct bwm_xwayland_view));
	if (!xwayland_view)
		return NULL;

	xwayland_view->xwayland_surface = xsurface;
	xwayland_view->mapped = false;

	xwayland_view->scene_tree = wlr_scene_tree_create(server.tile_tree);
	if (!xwayland_view->scene_tree) {
		wlr_log(WLR_ERROR, "Failed to create scene tree for xwayland view");
		free(xwayland_view);
		return NULL;
	}

	xwayland_view->content_tree = wlr_scene_tree_create(xwayland_view->scene_tree);
	if (!xwayland_view->content_tree) {
		wlr_log(WLR_ERROR, "Failed to create content tree for xwayland view");
		wlr_scene_node_destroy(&xwayland_view->scene_tree->node);
		free(xwayland_view);
		return NULL;
	}

	xwayland_view->scene_tree->node.data = xwayland_view;
	wlr_scene_node_set_enabled(&xwayland_view->scene_tree->node, false);

	// initialize borders
	xwayland_view->border_tree = NULL;
	for (int i = 0; i < 4; i++)
		xwayland_view->border_rects[i] = NULL;
	create_borders(xwayland_view->scene_tree, &xwayland_view->border_tree,
	               xwayland_view->border_rects);

	// Create image capture scene
	xwayland_view->image_capture = wlr_scene_create();
	xwayland_view->image_capture_tree = wlr_scene_tree_create(&xwayland_view->image_capture->tree);

	wl_signal_add(&xsurface->events.destroy, &xwayland_view->destroy);
	xwayland_view->destroy.notify = handle_destroy;

	wl_signal_add(&xsurface->events.request_configure, &xwayland_view->request_configure);
	xwayland_view->request_configure.notify = handle_request_configure;

	wl_signal_add(&xsurface->events.request_fullscreen, &xwayland_view->request_fullscreen);
	xwayland_view->request_fullscreen.notify = handle_request_fullscreen;

	wl_signal_add(&xsurface->events.request_minimize, &xwayland_view->request_minimize);
	xwayland_view->request_minimize.notify = handle_request_minimize;

	wl_signal_add(&xsurface->events.request_activate, &xwayland_view->request_activate);
	xwayland_view->request_activate.notify = handle_request_activate;

	wl_signal_add(&xsurface->events.request_move, &xwayland_view->request_move);
	xwayland_view->request_move.notify = handle_request_move;

	wl_signal_add(&xsurface->events.request_resize, &xwayland_view->request_resize);
	xwayland_view->request_resize.notify = handle_request_resize;

	wl_signal_add(&xsurface->events.set_title, &xwayland_view->set_title);
	xwayland_view->set_title.notify = handle_set_title;

	wl_signal_add(&xsurface->events.set_class, &xwayland_view->set_class);
	xwayland_view->set_class.notify = handle_set_class;

	wl_signal_add(&xsurface->events.set_hints, &xwayland_view->set_hints);
	xwayland_view->set_hints.notify = handle_set_hints;

	wl_signal_add(&xsurface->events.set_window_type, &xwayland_view->set_window_type);
	xwayland_view->set_window_type.notify = handle_set_window_type;

	wl_signal_add(&xsurface->events.associate, &xwayland_view->associate);
	xwayland_view->associate.notify = handle_associate;

	wl_signal_add(&xsurface->events.dissociate, &xwayland_view->dissociate);
	xwayland_view->dissociate.notify = handle_dissociate;

	wl_signal_add(&xsurface->events.set_override_redirect, &xwayland_view->override_redirect);
	xwayland_view->override_redirect.notify = handle_override_redirect;

	xsurface->data = xwayland_view;

	return xwayland_view;
}

static void xwayland_view_destroy(struct bwm_xwayland_view *xwayland_view) {
	if (xwayland_view->mapped)
		handle_unmap(&xwayland_view->unmap, NULL);

	wl_list_remove(&xwayland_view->destroy.link);
	wl_list_remove(&xwayland_view->request_configure.link);
	wl_list_remove(&xwayland_view->request_fullscreen.link);
	wl_list_remove(&xwayland_view->request_minimize.link);
	wl_list_remove(&xwayland_view->request_activate.link);
	wl_list_remove(&xwayland_view->request_move.link);
	wl_list_remove(&xwayland_view->request_resize.link);
	wl_list_remove(&xwayland_view->set_title.link);
	wl_list_remove(&xwayland_view->set_class.link);
	wl_list_remove(&xwayland_view->set_hints.link);
	wl_list_remove(&xwayland_view->set_window_type.link);
	wl_list_remove(&xwayland_view->associate.link);
	wl_list_remove(&xwayland_view->dissociate.link);
	wl_list_remove(&xwayland_view->override_redirect.link);

	free(xwayland_view);
}

static void begin_xwayland_interactive(struct bwm_xwayland_view *xwayland_view,
		enum cursor_mode mode, uint32_t edges) {
	if (!xwayland_view || !xwayland_view->node || !xwayland_view->node->client)
		return;

	if (xwayland_view->node->client->state != STATE_FLOATING)
		return;

	server.grabbed_toplevel = NULL;
	server.grabbed_xwayland_view = xwayland_view;
	server.cursor_mode = mode;

	struct client_t *client = xwayland_view->node->client;

	if (mode == CURSOR_MOVE) {
		server.grab_x = server.cursor->x - client->floating_rectangle.x;
		server.grab_y = server.cursor->y - client->floating_rectangle.y;
	} else {
		double border_x = server.cursor->x;
		double border_y = server.cursor->y;

		if (edges & WLR_EDGE_RIGHT)
			border_x = client->floating_rectangle.x + client->floating_rectangle.width;
		if (edges & WLR_EDGE_BOTTOM)
			border_y = client->floating_rectangle.y + client->floating_rectangle.height;

		server.grab_x = server.cursor->x - border_x;
		server.grab_y = server.cursor->y - border_y;
		server.grab_geobox = client->floating_rectangle;
		server.resize_edges = edges;
	}
}


void handle_xwayland_surface(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xwayland_surface *xsurface = data;

	if (xsurface->override_redirect) {
		create_unmanaged(xsurface);
		return;
	}

	create_xwayland_view(xsurface);
}


void handle_xwayland_ready(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	struct bwm_xwayland *xwayland = &server.xwayland;

	xcb_connection_t *xcb_conn = xcb_connect(NULL, NULL);
	int err = xcb_connection_has_error(xcb_conn);
	if (err)
		return;

	// intern atoms (microwave reference???)
	xcb_intern_atom_cookie_t cookies[ATOM_LAST];
	for (size_t i = 0; i < ATOM_LAST; i++)
		cookies[i] = xcb_intern_atom(xcb_conn, 0, strlen(atom_map[i]), atom_map[i]);

	for (size_t i = 0; i < ATOM_LAST; i++) {
		xcb_generic_error_t *error = NULL;
		xcb_intern_atom_reply_t *reply =
			xcb_intern_atom_reply(xcb_conn, cookies[i], &error);
		if (reply != NULL && error == NULL)
			xwayland->atoms[i] = reply->atom;
		free(reply);
		free(error);
	}

	xcb_disconnect(xcb_conn);

	wlr_xwayland_set_seat(xwayland->wlr_xwayland, server.seat);
}
