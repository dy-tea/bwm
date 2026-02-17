#include "server.h"
#include "keyboard.h"
#include "output.h"
#include "toplevel.h"
#include "popup.h"
#include "types.h"
#include "transaction.h"
#include "tree.h"
#include "workspace.h"
#include "ipc.h"
#include "layer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_pointer.h>

static void reset_cursor_mode(void) {
  server.cursor_mode = CURSOR_PASSTHROUGH;
  server.grabbed_toplevel = NULL;
}

static void *desktop_type_at(
      double lx, double ly, struct wlr_surface **surface,
      double *sx, double *sy) {
  struct wlr_scene_node *node = wlr_scene_node_at(
      &server.scene->tree.node, lx, ly, sx, sy);
  if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
      return NULL;

  struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
  if (!scene_surface)
    return NULL;

  *surface = scene_surface->surface;

  struct wlr_scene_tree *tree = node->parent;
  for (; tree != NULL && tree->node.data == NULL; tree = tree->node.parent)
    ;

  if (tree == NULL)
    return NULL;

  return tree->node.data;
}

static void process_cursor_move(void) {
  struct bwm_toplevel *toplevel = server.grabbed_toplevel;
  if (!toplevel || !toplevel->node || !toplevel->node->client || toplevel->node->client->state != STATE_FLOATING)
    return;

  double x = server.cursor->x - server.grab_x;
  double y = server.cursor->y - server.grab_y;

  toplevel->node->client->floating_rectangle.x = (int)x;
  toplevel->node->client->floating_rectangle.y = (int)y;

  wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
}

static void process_cursor_resize(void) {
  struct bwm_toplevel *toplevel = server.grabbed_toplevel;
  if (!toplevel || !toplevel->node || !toplevel->node->client)
    return;

  double border_x = server.cursor->x - server.grab_x;
  double border_y = server.cursor->y - server.grab_y;

  int new_left = server.grab_geobox.x;
  int new_right = server.grab_geobox.x + server.grab_geobox.width;
  int new_top = server.grab_geobox.y;
  int new_bottom = server.grab_geobox.y + server.grab_geobox.height;

  if (server.resize_edges & WLR_EDGE_TOP) {
    new_top = border_y;
    if (new_top >= new_bottom)
        new_top = new_bottom - 1;
  } else if (server.resize_edges & WLR_EDGE_BOTTOM) {
    new_bottom = border_y;
    if (new_bottom <= new_top)
      new_bottom = new_top + 1;
  }
  if (server.resize_edges & WLR_EDGE_LEFT) {
    new_left = border_x;
    if (new_left >= new_right)
      new_left = new_right - 1;
  } else if (server.resize_edges & WLR_EDGE_RIGHT) {
    new_right = border_x;
    if (new_right <= new_left)
      new_right = new_left + 1;
  }

  int new_width = new_right - new_left;
  int new_height = new_bottom - new_top;

  if (new_width < MIN_WIDTH)
    new_width = MIN_WIDTH;
  if (new_height < MIN_HEIGHT)
    new_height = MIN_HEIGHT;

  toplevel->node->client->floating_rectangle.x = new_left;
  toplevel->node->client->floating_rectangle.y = new_top;
  toplevel->node->client->floating_rectangle.width = new_width;
  toplevel->node->client->floating_rectangle.height = new_height;

  wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left, new_top);
  wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(uint32_t time) {
  if (server.cursor_mode == CURSOR_MOVE) {
    process_cursor_move();
    return;
  } else if (server.cursor_mode == CURSOR_RESIZE) {
    process_cursor_resize();
    return;
  }

  double sx, sy;
  struct wlr_seat *seat = server.seat;
  struct wlr_surface *surface = NULL;
  void *type = desktop_type_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);
  if (type == NULL)
  	wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");

  if (surface) {
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  } else {
    wlr_seat_pointer_clear_focus(seat);
  }
}

void begin_interactive(struct bwm_toplevel *toplevel, enum cursor_mode mode, uint32_t edges) {
  server.grabbed_toplevel = toplevel;
  server.cursor_mode = mode;

  if (mode == CURSOR_MOVE) {
    if (toplevel->node && toplevel->node->client) {
      server.grab_x = server.cursor->x - toplevel->node->client->floating_rectangle.x;
      server.grab_y = server.cursor->y - toplevel->node->client->floating_rectangle.y;
    }
  } else {
    double border_x = server.cursor->x;
    double border_y = server.cursor->y;
    if (edges & WLR_EDGE_RIGHT)
      border_x = toplevel->node->client->floating_rectangle.x + toplevel->node->client->floating_rectangle.width;
    if (edges & WLR_EDGE_BOTTOM)
      border_y = toplevel->node->client->floating_rectangle.y + toplevel->node->client->floating_rectangle.height;

    server.grab_x = server.cursor->x - border_x;
    server.grab_y = server.cursor->y - border_y;

    server.grab_geobox = toplevel->node->client->floating_rectangle;
    server.resize_edges = edges;
  }
}

void cursor_motion(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_motion_event *event = data;
  wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
  process_cursor_motion(event->time_msec);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_motion_absolute_event *event = data;
  wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);
  process_cursor_motion(event->time_msec);
}

void cursor_button(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_button_event *event = data;
  wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);

  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    reset_cursor_mode();
  } else {
    double sx, sy;
    struct wlr_surface *surface = NULL;
    void *type = desktop_type_at(
            server.cursor->x, server.cursor->y, &surface, &sx, &sy);
    if (type == NULL)
    	return;
    if (wlr_layer_surface_v1_try_from_wlr_surface(surface)) {
    	struct bwm_layer_surface* layer = type;
     	if (layer)
      	focus_layer_surface(layer);
    } else {
    	struct bwm_toplevel *toplevel = type;
	    if (toplevel)
	      focus_toplevel(toplevel);
    }
  }
}

void cursor_axis(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_axis_event *event = data;
  wlr_seat_pointer_notify_axis(server.seat, event->time_msec, event->orientation,
      event->delta, event->delta_discrete, event->source, event->relative_direction);
}

void cursor_frame(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
  wlr_seat_pointer_notify_frame(server.seat);
}

void request_cursor(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  if (event->seat_client == server.seat->pointer_state.focused_client)
    wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_seat_pointer_focus_change_event *event = data;
  if (event->new_surface == NULL)
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
}

void request_set_selection(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(server.seat, event->source, event->serial);
}

void handle_new_input(struct wl_listener *listener, void *data) {
	(void)listener;
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

  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&server.keyboards))
      caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  wlr_seat_set_capabilities(server.seat, caps);
}

void server_init(void) {
  server = (struct bwm_server){0};

  server.wl_display = wl_display_create();
  server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), &server.session);
  if (server.backend == NULL) {
    wlr_log(WLR_ERROR, "Failed to create backend");
    exit(EXIT_FAILURE);
  }

  server.renderer = wlr_renderer_autocreate(server.backend);
  if (server.renderer == NULL) {
    wlr_log(WLR_ERROR, "Failed to create renderer");
    exit(EXIT_FAILURE);
  }

  wlr_renderer_init_wl_display(server.renderer, server.wl_display);

  server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
  if (server.allocator == NULL) {
    wlr_log(WLR_ERROR, "Failed to create allocator");
    exit(EXIT_FAILURE);
  }

  wlr_compositor_create(server.wl_display, 5, server.renderer);
  wlr_subcompositor_create(server.wl_display);

  // dmabuf support
  if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF)) {
  	wlr_drm_create(server.wl_display, server.renderer);
    server.linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 4, server.renderer);
    wlr_export_dmabuf_manager_v1_create(server.wl_display);
  }

  wlr_data_device_manager_create(server.wl_display);
  wlr_primary_selection_v1_device_manager_create(server.wl_display);

  server.output_layout = wlr_output_layout_create(server.wl_display);
  wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

  wl_list_init(&server.outputs);

  // scene graph
  server.scene = wlr_scene_create();
  server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
  if (server.linux_dmabuf)
 		wlr_scene_set_linux_dmabuf_v1(server.scene, server.linux_dmabuf);

  // scene trees for layering
	server.bg_tree = wlr_scene_tree_create(&server.scene->tree);
	server.bot_tree = wlr_scene_tree_create(&server.scene->tree);
	server.tile_tree = wlr_scene_tree_create(&server.scene->tree);
	server.float_tree = wlr_scene_tree_create(&server.scene->tree);
	server.top_tree = wlr_scene_tree_create(&server.scene->tree);
	server.full_tree = wlr_scene_tree_create(&server.scene->tree);
	server.over_tree = wlr_scene_tree_create(&server.scene->tree);

  // xdg shell
  server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 5);
  wl_list_init(&server.toplevels);

  // layer shell
  server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 5);

  // cursor
  server.cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

  server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
  wlr_xcursor_manager_load(server.cursor_mgr, 1);

  server.cursor_mode = CURSOR_PASSTHROUGH;

  // seat
  server.seat = wlr_seat_create(server.wl_display, "seat0");
  wl_list_init(&server.keyboards);

  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  wlr_seat_set_capabilities(server.seat, caps);

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

  server.monitors = m;
  server.focused_monitor = m;

  // Initialize transaction system
  transaction_init();

  // Initialize workspace manager
  workspace_init();

  // Initialize IPC
  ipc_init();
}

static int ipc_socket_handler(int fd, uint32_t mask, void *data) {
  (void)fd;
  (void)data;
  if (mask & WL_EVENT_READABLE) {
    int client_fd = accept(ipc_get_socket_fd(), NULL, NULL);
    if (client_fd >= 0) {
      ipc_handle_incoming(client_fd);
    }
  }
  return 0;
}

int server_run(void) {
  // backend listeners
  server.new_output.notify = handle_new_output;
  wl_signal_add(&server.backend->events.new_output, &server.new_output);

  server.new_input.notify = handle_new_input;
  wl_signal_add(&server.backend->events.new_input, &server.new_input);

  // xdg-shell listeners
  server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
  wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

  // layer-shell listeners
  server.new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

  // cursor listeners
  server.cursor_motion.notify = cursor_motion;
  wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);

  server.cursor_motion_absolute.notify = cursor_motion_absolute;
  wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);

  server.cursor_button.notify = cursor_button;
  wl_signal_add(&server.cursor->events.button, &server.cursor_button);

  server.cursor_axis.notify = cursor_axis;
  wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

  server.cursor_frame.notify = cursor_frame;
  wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

  // seat listeners
  server.request_cursor.notify = request_cursor;
  wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

  server.pointer_focus_change.notify = seat_pointer_focus_change;
  wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.pointer_focus_change);

  server.request_set_selection.notify = request_set_selection;
  wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

  const char *socket = wl_display_add_socket_auto(server.wl_display);
  if (!socket) {
    wlr_backend_destroy(server.backend);
    return 1;
  }

  if (!wlr_backend_start(server.backend)) {
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  setenv("WAYLAND_DISPLAY", socket, true);

  // add IPC socket to event loop
  struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
  int ipc_fd = ipc_get_socket_fd();
  if (ipc_fd >= 0)
    wl_event_loop_add_fd(event_loop, ipc_fd, WL_EVENT_READABLE, ipc_socket_handler, NULL);

  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
  wl_display_run(server.wl_display);
  return 0;
}

void server_fini(void) {
  transaction_fini();
  workspace_fini();
  ipc_cleanup();
  wl_display_destroy_clients(server.wl_display);

  wl_list_remove(&server.new_xdg_toplevel.link);
  wl_list_remove(&server.cursor_motion.link);
  wl_list_remove(&server.cursor_motion_absolute.link);
  wl_list_remove(&server.cursor_button.link);
  wl_list_remove(&server.cursor_axis.link);
  wl_list_remove(&server.cursor_frame.link);
  wl_list_remove(&server.request_cursor.link);
  wl_list_remove(&server.pointer_focus_change.link);
  wl_list_remove(&server.request_set_selection.link);

  wlr_scene_node_destroy(&server.scene->tree.node);
  wlr_cursor_destroy(server.cursor);
  wlr_xcursor_manager_destroy(server.cursor_mgr);
  wlr_allocator_destroy(server.allocator);
  wlr_renderer_destroy(server.renderer);
  wlr_backend_destroy(server.backend);
  wl_display_destroy(server.wl_display);
}
