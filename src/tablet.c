#include "cursor.h"
#include "seat.h"
#include "server.h"
#include "tablet.h"
#include <stdlib.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/log.h>

static void handle_tablet_tool_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	tablet_tool_t *tool = wl_container_of(listener, tool, tool_destroy);
	wl_list_remove(&tool->tool_destroy.link);
	wl_list_remove(&tool->set_cursor.link);
	free(tool);
}

static void handle_pad_tablet_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	tablet_pad_t *pad = wl_container_of(listener, pad, tablet_destroy);
	pad->tablet = NULL;
	wl_list_remove(&pad->tablet_destroy.link);
	wl_list_init(&pad->tablet_destroy.link);
}

static void attach_tablet_pad(tablet_pad_t *pad, tablet_t *tablet) {
	wlr_log(WLR_DEBUG, "Attaching tablet pad to tablet tool");
	pad->tablet = tablet;
	wl_list_remove(&pad->tablet_destroy.link);
	pad->tablet_destroy.notify = handle_pad_tablet_destroy;
	wl_signal_add(&tablet->wlr_tablet->base.events.destroy, &pad->tablet_destroy);
}

tablet_t *tablet_create(struct wlr_input_device *device) {
	tablet_t *tablet = calloc(1, sizeof(*tablet));
	if (!tablet)
		return NULL;

	wlr_cursor_attach_input_device(server.cursor, device);
	seat_t *seat = seat_default();

	tablet->wlr_tablet = wlr_tablet_from_input_device(device);
	tablet->seat = seat;
	tablet->wlr_tablet->data = tablet;
	wl_list_insert(&seat->tablets, &tablet->link);

	wlr_log(WLR_DEBUG, "Created tablet for device %s", device->name);
	return tablet;
}

void tablet_configure(tablet_t *tablet) {
	struct wlr_input_device *device = &tablet->wlr_tablet->base;

	if (!tablet->tablet_v2)
		tablet->tablet_v2 = wlr_tablet_create(server.tablet_v2, tablet->seat->wlr_seat, device);

#if WLR_HAS_LIBINPUT_BACKEND
	if (!wlr_input_device_is_libinput(device))
		return;

	struct libinput_device_group *group =
		libinput_device_get_device_group(wlr_libinput_get_device_handle(device));
	tablet_pad_t *pad;
	wl_list_for_each(pad, &tablet->seat->tablet_pads, link) {
		struct wlr_input_device *pad_device = &pad->wlr_pad->base;
		if (!wlr_input_device_is_libinput(pad_device))
			continue;

		struct libinput_device_group *pad_group =
			libinput_device_get_device_group(wlr_libinput_get_device_handle(pad_device));
		if (pad_group == group) {
			attach_tablet_pad(pad, tablet);
			break;
		}
	}
#endif
}

void tablet_destroy(tablet_t *tablet) {
	if (!tablet)
		return;
	wl_list_remove(&tablet->link);
	free(tablet);
}

static void handle_tablet_tool_set_cursor(struct wl_listener *listener, void *data) {
	tablet_tool_t *tool = wl_container_of(listener, tool, set_cursor);
	struct wlr_tablet_v2_event_cursor *event = data;

	struct wlr_surface *focused = tool->tablet_v2_tool->focused_surface;
	if (!focused)
		return;

	struct wl_client *focused_client = wl_resource_get_client(focused->resource);
	if (!focused_client || event->seat_client->client != focused_client)
		return;

	wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

tablet_tool_t *tablet_tool_configure(tablet_t *tablet, struct wlr_tablet_tool *wlr_tool) {
	tablet_tool_t *tool = calloc(1, sizeof(*tool));
	if (!tool)
		return NULL;

	tool->seat = tablet->seat;
	tool->tablet = tablet;
	tool->wlr_tool = wlr_tool;
	tool->tablet_v2_tool = wlr_tablet_tool_create(server.tablet_v2, tablet->seat->wlr_seat, wlr_tool);

	tool->tool_destroy.notify = handle_tablet_tool_destroy;
	wl_signal_add(&wlr_tool->events.destroy, &tool->tool_destroy);

	tool->set_cursor.notify = handle_tablet_tool_set_cursor;
	wl_signal_add(&tool->tablet_v2_tool->events.set_cursor, &tool->set_cursor);

	wlr_tool->data = tool;

	wlr_log(WLR_DEBUG, "Configured tablet tool type=%d for seat %s", wlr_tool->type,
		tablet->seat->name);

	return tool;
}

void handle_tablet_tool_position(struct wlr_tablet_tool *wlr_tool, tablet_t *tablet, bool tip_down,
		double x, double y, uint32_t time) {
	tablet_tool_t *tool = wlr_tool->data;
	if (!tool)
		return;

	// warp cursor to position
	wlr_cursor_warp(server.cursor, NULL, x, y);

	// find surface at cursor position
	double sx, sy;
	struct wlr_surface *surface = NULL;
	desktop_type_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);

	if (surface && wlr_surface_accepts_tablet_v2(surface, tablet->tablet_v2)) {
		wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->tablet_v2_tool, tablet->tablet_v2, surface);
		wlr_tablet_v2_tablet_tool_notify_motion(tool->tablet_v2_tool, sx, sy);
		if (tip_down) {
			wlr_tablet_v2_tablet_tool_notify_down(tool->tablet_v2_tool);
			wlr_tablet_tool_v2_start_implicit_grab(tool->tablet_v2_tool);
		}
	} else {
		// emulate pointer events
		if (surface) {
			wlr_seat_pointer_notify_enter(tablet->seat->wlr_seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(tablet->seat->wlr_seat, time, sx, sy);
		} else {
			wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
		}
	}
}

static void handle_pad_attach(struct wl_listener *listener, void *data) {
	tablet_pad_t *pad = wl_container_of(listener, pad, attach);
	struct wlr_tablet_tool *wlr_tool = data;
	tablet_tool_t *tool = wlr_tool->data;

	if (!tool)
		return;

	attach_tablet_pad(pad, tool->tablet);
}

static void handle_pad_ring(struct wl_listener *listener, void *data) {
	tablet_pad_t *pad = wl_container_of(listener, pad, ring);
	struct wlr_tablet_pad_ring_event *event = data;
	if (!pad->current_surface || !pad->tablet_v2_pad)
		return;

	wlr_tablet_v2_tablet_pad_notify_ring(pad->tablet_v2_pad, event->ring, event->position,
		event->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, event->time_msec);
}

static void handle_pad_strip(struct wl_listener *listener, void *data) {
	tablet_pad_t *pad = wl_container_of(listener, pad, strip);
	struct wlr_tablet_pad_strip_event *event = data;

	if (!pad->current_surface || !pad->tablet_v2_pad)
		return;

	wlr_tablet_v2_tablet_pad_notify_strip(pad->tablet_v2_pad, event->strip, event->position,
		event->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, event->time_msec);
}

static void handle_pad_button(struct wl_listener *listener, void *data) {
	tablet_pad_t *pad = wl_container_of(listener, pad, button);
	struct wlr_tablet_pad_button_event *event = data;

	if (!pad->current_surface || !pad->tablet_v2_pad)
		return;

	wlr_tablet_v2_tablet_pad_notify_mode(pad->tablet_v2_pad, event->group, event->mode,
		event->time_msec);

	wlr_tablet_v2_tablet_pad_notify_button(pad->tablet_v2_pad, event->button, event->time_msec,
		(enum zwp_tablet_pad_v2_button_state)event->state);
}

static void handle_pad_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	tablet_pad_t *pad = wl_container_of(listener, pad, surface_destroy);
	tablet_pad_set_focus(pad, NULL);
}

tablet_pad_t *tablet_pad_create(struct wlr_input_device *device) {
	tablet_pad_t *pad = calloc(1, sizeof(*pad));
	if (!pad)
		return NULL;

	seat_t *seat = seat_default();
	if (!seat)
		return NULL;

	pad->wlr_pad = wlr_tablet_pad_from_input_device(device);
	pad->seat = seat;
	wl_list_init(&pad->attach.link);
	wl_list_init(&pad->button.link);
	wl_list_init(&pad->strip.link);
	wl_list_init(&pad->ring.link);
	wl_list_init(&pad->tablet_destroy.link);
	wl_list_init(&pad->surface_destroy.link);
	wl_list_insert(&seat->tablet_pads, &pad->link);

	wlr_log(WLR_DEBUG, "Created tablet pad for device %s", device->name);
	return pad;
}

void tablet_pad_configure(tablet_pad_t *pad) {
	struct wlr_input_device *device = &pad->wlr_pad->base;

	if (!pad->tablet_v2_pad)
		pad->tablet_v2_pad = wlr_tablet_pad_create(server.tablet_v2, pad->seat->wlr_seat, device);

	wl_list_remove(&pad->attach.link);
	pad->attach.notify = handle_pad_attach;
	wl_signal_add(&pad->wlr_pad->events.attach_tablet, &pad->attach);

	wl_list_remove(&pad->button.link);
	pad->button.notify = handle_pad_button;
	wl_signal_add(&pad->wlr_pad->events.button, &pad->button);

	wl_list_remove(&pad->strip.link);
	pad->strip.notify = handle_pad_strip;
	wl_signal_add(&pad->wlr_pad->events.strip, &pad->strip);

	wl_list_remove(&pad->ring.link);
	pad->ring.notify = handle_pad_ring;
	wl_signal_add(&pad->wlr_pad->events.ring, &pad->ring);

#if WLR_HAS_LIBINPUT_BACKEND
	if (!wlr_input_device_is_libinput(device))
		return;

	struct libinput_device_group *group =
		libinput_device_get_device_group(wlr_libinput_get_device_handle(device));
	tablet_t *tablet;
	wl_list_for_each(tablet, &pad->seat->tablets, link) {
		struct wlr_input_device *tablet_device = &tablet->wlr_tablet->base;
		if (!wlr_input_device_is_libinput(tablet_device))
			continue;

		struct libinput_device_group *tablet_group =
			libinput_device_get_device_group(wlr_libinput_get_device_handle(tablet_device));
		if (tablet_group == group) {
			attach_tablet_pad(pad, tablet);
			break;
		}
	}
#endif
}

void tablet_pad_destroy(tablet_pad_t *pad) {
	if (!pad)
		return;
	wl_list_remove(&pad->link);
	wl_list_remove(&pad->attach.link);
	wl_list_remove(&pad->button.link);
	wl_list_remove(&pad->strip.link);
	wl_list_remove(&pad->ring.link);
	wl_list_remove(&pad->surface_destroy.link);
	wl_list_remove(&pad->tablet_destroy.link);
	free(pad);
}

void tablet_pad_set_focus(tablet_pad_t *pad, struct wlr_surface *surface) {
	if (!pad || !pad->tablet_v2_pad || !pad->tablet)
		return;
	if (surface == pad->current_surface)
		return;

	// leave current surface
	if (pad->current_surface) {
		wlr_tablet_v2_tablet_pad_notify_leave(pad->tablet_v2_pad, pad->current_surface);
		wl_list_remove(&pad->surface_destroy.link);
		wl_list_init(&pad->surface_destroy.link);
		pad->current_surface = NULL;
	}

	if (!surface || !wlr_surface_accepts_tablet_v2(surface, pad->tablet->tablet_v2))
		return;

	wlr_tablet_v2_tablet_pad_notify_enter(pad->tablet_v2_pad, pad->tablet->tablet_v2, surface);
	pad->current_surface = surface;
	pad->surface_destroy.notify = handle_pad_surface_destroy;
	wl_signal_add(&surface->events.destroy, &pad->surface_destroy);
}

void tablet_pads_set_focus(seat_t *seat, struct wlr_surface *surface) {
	if (!seat)
		return;

	tablet_pad_t *pad;
	wl_list_for_each(pad, &seat->tablet_pads, link)
		tablet_pad_set_focus(pad, surface);
}
