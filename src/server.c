#include "server.h"
#include "keyboard.h"
#include "output.h"
#include "toplevel.h"
#include "types.h"
#include "transaction.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

void cursor_motion(struct wl_listener *listener, void *data) {
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
    // TODO: process cursor motion for window management
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);
    // TODO: process cursor motion for window management
}

void cursor_button(struct wl_listener *listener, void *data) {
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);
}

void cursor_axis(struct wl_listener *listener, void *data) {
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server.seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source, event->relative_direction);
}

void cursor_frame(struct wl_listener *listener, void *data) {
    wlr_seat_pointer_notify_frame(server.seat);
}

void request_cursor(struct wl_listener *listener, void *data) {
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    if (event->seat_client == server.seat->pointer_state.focused_client)
        wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

void request_set_selection(struct wl_listener *listener, void *data) {
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server.seat, event->source, event->serial);
}

void handle_new_input(struct wl_listener *listener, void *data) {
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        handle_new_keyboard(device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server.cursor, device);
        break;
    default:
        break;
    }
}

struct bwm_server init(void) {
  struct bwm_server s = {0};

  s.wl_display = wl_display_create();
  s.backend = wlr_backend_autocreate(wl_display_get_event_loop(s.wl_display), &s.session);
  if (!s.backend) {
    wlr_log(WLR_ERROR, "Failed to create backend");
    exit(EXIT_FAILURE);
  }

  s.renderer = wlr_renderer_autocreate(s.backend);
  if (!s.renderer) {
    wlr_log(WLR_ERROR, "Failed to create renderer");
    exit(EXIT_FAILURE);
  }

  wlr_renderer_init_wl_display(s.renderer, s.wl_display);

  s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
  if (!s.allocator) {
    wlr_log(WLR_ERROR, "Failed to create allocator");
    exit(EXIT_FAILURE);
  }

  wlr_compositor_create(s.wl_display, 5, s.renderer);
  wlr_subcompositor_create(s.wl_display);

  wlr_data_device_manager_create(s.wl_display);

  s.output_layout = wlr_output_layout_create(s.wl_display);

  wl_list_init(&s.outputs);

  // scene graph
  s.scene = wlr_scene_create();
  s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);

  // xdg shell
  s.xdg_shell = wlr_xdg_shell_create(s.wl_display, 3);
  wl_list_init(&s.toplevels);

  // cursor
  s.cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(s.cursor, s.output_layout);

  s.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
  wlr_xcursor_manager_load(s.cursor_mgr, 1);

  // seat
  s.seat = wlr_seat_create(s.wl_display, "seat0");
  wl_list_init(&s.keyboards);

  // init desktop structure
  monitor_t *m = (monitor_t *)calloc(1, sizeof(monitor_t));
  m->id = next_monitor_id++;
  strncpy(m->name, "default", SMALEN - 1);
  m->wired = true;
  m->window_gap = window_gap;
  m->border_width = border_width;
  m->padding = (padding_t){0};
  m->rectangle = (struct wlr_box){0, 0, 1920, 1080};

  // create default desktop
  desktop_t *d = (desktop_t *)calloc(1, sizeof(desktop_t));
  d->id = next_desktop_id++;
  strncpy(d->name, "1", SMALEN - 1);
  d->layout = LAYOUT_TILED;
  d->user_layout = LAYOUT_TILED;
  d->window_gap = window_gap;
  d->border_width = border_width;
  d->padding = (padding_t){0};
  d->root = NULL;
  d->focus = NULL;

  m->desk = d;
  m->desk_head = d;
  m->desk_tail = d;

  mon = m;
  mon_head = m;
  mon_tail = m;

  s.monitors = m;
  s.focused_monitor = m;

  // Initialize transaction system
  transaction_init();

  return s;
}

int run(struct bwm_server *server) {
  const char *socket = wl_display_add_socket_auto(server->wl_display);
  if (!socket) {
    wlr_backend_destroy(server->backend);
    return 1;
  }

  // backend listeners
  server->new_output.notify = handle_new_output;
  wl_signal_add(&server->backend->events.new_output, &server->new_output);

  server->new_input.notify = handle_new_input;
  wl_signal_add(&server->backend->events.new_input, &server->new_input);

  // xdg-shell listeners
  server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
  wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

  // cursor listeners
  server->cursor_motion.notify = cursor_motion;
  wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);

  server->cursor_motion_absolute.notify = cursor_motion_absolute;
  wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);

  server->cursor_button.notify = cursor_button;
  wl_signal_add(&server->cursor->events.button, &server->cursor_button);

  server->cursor_axis.notify = cursor_axis;
  wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);

  server->cursor_frame.notify = cursor_frame;
  wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

  // seat listeners
  server->request_cursor.notify = request_cursor;
  wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);

  server->request_set_selection.notify = request_set_selection;
  wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);

  if (!wlr_backend_start(server->backend)) {
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->wl_display);
    return 1;
  }

  setenv("WAYLAND_DISPLAY", socket, true);

  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
  wl_display_run(server->wl_display);

  // destroy
  transaction_fini();
  wl_display_destroy_clients(server->wl_display);

  wl_list_remove(&server->new_xdg_toplevel.link);
  wl_list_remove(&server->cursor_motion.link);
  wl_list_remove(&server->cursor_motion_absolute.link);
  wl_list_remove(&server->cursor_button.link);
  wl_list_remove(&server->cursor_axis.link);
  wl_list_remove(&server->cursor_frame.link);
  wl_list_remove(&server->request_cursor.link);
  wl_list_remove(&server->request_set_selection.link);

  wlr_xcursor_manager_destroy(server->cursor_mgr);
  wlr_cursor_destroy(server->cursor);
  wlr_scene_node_destroy(&server->scene->tree.node);
  wlr_allocator_destroy(server->allocator);
  wlr_renderer_destroy(server->renderer);
  wlr_backend_destroy(server->backend);
  wl_display_destroy(server->wl_display);

  return 0;
}
