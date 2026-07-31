#include "cursor.h"
#include "idle_power.h"
#include "layer.h"
#include "server.h"
#include "touch.h"
#include "tree.h"
#include <wayland-util.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void handle_touch_down(struct wl_listener *listener, void *data) {
	touch_t *touch = wl_container_of(listener, touch, down);
	struct wlr_touch_down_event *event = data;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server.cursor, &event->touch->base, event->x, event->y, &lx,
		&ly);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	void *type = desktop_type_at(lx, ly, &surface, &sx, &sy);

	if (surface) {
		wlr_seat_touch_notify_down(server.seat, surface, event->time_msec, event->touch_id, sx, sy);
		wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
		idle_power_notify_activity();

		// touch-to-focus
		if (type) {
			output_t *output = output_at(lx, ly);

			struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
			if (xdg_surface != NULL && xdg_surface->role != WLR_XDG_SURFACE_ROLE_POPUP) {
				toplevel_t *toplevel = type;
				if (toplevel && toplevel->node) {
					if (output)
						focus_node(output, toplevel->node->desktop, toplevel->node);
				}
			} else if (wlr_layer_surface_v1_try_from_wlr_surface(surface)) {
				layer_surface_t *layer = type;
				if (layer)
					focus_layer_surface(layer);
			} else {
				struct wlr_xwayland_surface *xwayland_surface =
					wlr_xwayland_surface_try_from_wlr_surface(surface);
				if (xwayland_surface != NULL) {
					xwayland_toplevel_t *xwayland_view = type;
					if (xwayland_view && xwayland_view->node) {
						if (output)
							focus_node(output, xwayland_view->node->desktop, xwayland_view->node);
					}
				}
			}
		}
	}
}

static void handle_touch_up(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_touch_up_event *event = data;

	wlr_seat_touch_notify_up(server.seat, event->time_msec, event->touch_id);
}

static void handle_touch_motion(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_touch_motion_event *event = data;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server.cursor, &event->touch->base, event->x, event->y, &lx,
		&ly);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	desktop_type_at(lx, ly, &surface, &sx, &sy);

	if (surface)
		wlr_seat_touch_notify_motion(server.seat, event->time_msec, event->touch_id, sx, sy);
}

static void handle_touch_frame(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	wlr_seat_touch_notify_frame(server.seat);
}

touch_t *touch_create(struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server.cursor, device);
	touch_t *t = calloc(1, sizeof(*t));
	if (!t) {
		wlr_log(WLR_ERROR, "allocation failed");
		return NULL;
	}
	t->wlr_touch = wlr_touch_from_input_device(device);
	t->wlr_touch->data = t;

	t->down.notify = handle_touch_down;
	wl_signal_add(&t->wlr_touch->events.down, &t->down);

	t->up.notify = handle_touch_up;
	wl_signal_add(&t->wlr_touch->events.up, &t->up);

	t->motion.notify = handle_touch_motion;
	wl_signal_add(&t->wlr_touch->events.motion, &t->motion);

	t->frame.notify = handle_touch_frame;
	wl_signal_add(&t->wlr_touch->events.frame, &t->frame);

	wl_list_insert(&server.touches, &t->link);
	server.num_touches++;

	return t;
}

void touch_fini(void) {
	touch_t *t, *next;
	wl_list_for_each_safe(t, next, &server.touches, link) {
		wl_list_remove(&t->down.link);
		wl_list_remove(&t->up.link);
		wl_list_remove(&t->motion.link);
		wl_list_remove(&t->frame.link);
		wl_list_remove(&t->link);
		free(t);
	}
}
