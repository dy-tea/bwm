#include "input_method.h"
#include "seat.h"
#include "server.h"
#include "tablet.h"
#include "touch.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static void request_cursor(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (event->seat_client == server.seat->pointer_state.focused_client)
		wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL)
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server.seat, event->source, event->serial);
}

static void handle_request_start_drag(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server.seat, event->origin, event->serial))
		wlr_seat_start_pointer_drag(server.seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	wl_list_remove(&listener->link);
	free(listener);
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	struct wlr_scene_node *node = &wlr_scene_drag_icon_create(server.drag_tree, drag->icon)->node;
	drag->icon->data = node;

	struct wl_listener *listener_icon = calloc(1, sizeof(*listener_icon));
	if (!listener_icon) {
		wlr_log(WLR_ERROR, "allocation failed");
		return;
	}
	listener_icon->notify = handle_drag_icon_destroy;
	wl_signal_add(&drag->icon->events.destroy, listener_icon);
}

seat_t *seat_create(const char *name) {
	seat_t *seat = calloc(1, sizeof(seat_t));
	if (!seat)
		return NULL;

	strncpy(seat->name, name, sizeof(seat->name) - 1);
	seat->name[sizeof(seat->name) - 1] = '\0';

	seat->wlr_seat = wlr_seat_create(server.wl_display, name);
	if (!seat->wlr_seat) {
		free(seat);
		return NULL;
	}

	seat->input_method_relay = input_method_relay_create(seat->wlr_seat);
	wl_list_init(&seat->tablets);
	wl_list_init(&seat->tablet_pads);

	seat->request_cursor.notify = request_cursor;
	wl_signal_add(&seat->wlr_seat->events.request_set_cursor, &seat->request_cursor);

	seat->pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&seat->wlr_seat->pointer_state.events.focus_change, &seat->pointer_focus_change);

	seat->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&seat->wlr_seat->events.request_set_selection, &seat->request_set_selection);

	seat->request_start_drag.notify = handle_request_start_drag;
	wl_signal_add(&seat->wlr_seat->events.request_start_drag, &seat->request_start_drag);

	seat->start_drag.notify = handle_start_drag;
	wl_signal_add(&seat->wlr_seat->events.start_drag, &seat->start_drag);

	wl_list_insert(&server.seats, &seat->link);

	wlr_log(WLR_INFO, "Created seat '%s'", name);
	return seat;
}

void seat_destroy(seat_t *seat) {
	if (!seat)
		return;

	wl_list_remove(&seat->request_cursor.link);
	wl_list_remove(&seat->pointer_focus_change.link);
	wl_list_remove(&seat->request_set_selection.link);
	wl_list_remove(&seat->request_start_drag.link);
	wl_list_remove(&seat->start_drag.link);

	if (seat->input_method_relay)
		input_method_relay_finish(seat->input_method_relay);

	tablet_t *tablet, *tmp_tablet;
	wl_list_for_each_safe(tablet, tmp_tablet, &seat->tablets, link)
		tablet_destroy(tablet);

	tablet_pad_t *pad, *tmp_pad;
	wl_list_for_each_safe(pad, tmp_pad, &seat->tablet_pads, link)
		tablet_pad_destroy(pad);

	wl_list_remove(&seat->link);

	if (seat->wlr_seat)
		wlr_seat_destroy(seat->wlr_seat);

	wlr_log(WLR_INFO, "Destroyed seat '%s'", seat->name);
	free(seat);
}

seat_t *seat_find_by_name(const char *name) {
	seat_t *seat;
	wl_list_for_each(seat, &server.seats, link)
		if (strcmp(seat->name, name) == 0)
			return seat;

	return NULL;
}

seat_t *seat_find_by_wlr_seat(struct wlr_seat *wlr_seat) {
	seat_t *seat;
	wl_list_for_each(seat, &server.seats, link)
		if (seat->wlr_seat == wlr_seat)
			return seat;

	return NULL;
}

seat_t *seat_default(void) {
	if (wl_list_empty(&server.seats))
		return NULL;

	seat_t *s = NULL;
	s = wl_container_of(server.seats.next, s, link);
	return s;
}
