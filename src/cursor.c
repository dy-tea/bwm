#include "cursor.h"
#include "keyboard.h"
#include "layer.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include "types.h"
#include "input.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/util/region.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>

static void cursor_constrain(struct wlr_pointer_constraint_v1 *constraint);

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

static void process_cursor_motion(uint32_t time, double dx, double dy, double dx_unaccel, double dy_unaccel) {
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
			server.relative_pointer_manager, server.seat, time * 1000,
			dx, dy, dx_unaccel, dy_unaccel);

		if (server.active_pointer_constraint != NULL &&
			server.cursor_mode != CURSOR_RESIZE && server.cursor_mode != CURSOR_MOVE) {
			struct bwm_toplevel *toplevel = server.active_pointer_constraint->surface->data;
			if (toplevel != NULL &&
				server.active_pointer_constraint->surface == server.seat->pointer_state.focused_surface) {
				const struct wlr_box geo = toplevel->node->rectangle;

				// calculate constraint
        double sx = server.cursor->x - geo.x - geo.width;
        double sy = server.cursor->y - geo.y - geo.height;
        double cx, cy;

        // apply confine on region
        if (wlr_region_confine(&server.active_pointer_constraint->region,
        	sx, sy, sx + dx, sy + dy, &cx, &cy)) {
          dx = cx - sx;
          dy = cy - sy;
        }

        // if pointer is locked, do not move it
        if (server.active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
          return;
			} else {
				cursor_constrain(NULL);
			}
		}
	}

  if (server.cursor_mode == CURSOR_MOVE) {
    process_cursor_move();
    return;
  } else if (server.cursor_mode == CURSOR_RESIZE) {
    process_cursor_resize();
    return;
  }

  if (server.seat->drag && server.seat->drag->icon && server.seat->drag->icon->data) {
    struct wlr_scene_node *node = server.seat->drag->icon->data;
    wlr_scene_node_set_position(node, server.cursor->x, server.cursor->y);
  }

  wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

  double sx, sy;
  struct wlr_seat *seat = server.seat;
  struct wlr_surface *surface = NULL;
  void *type = desktop_type_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);
  if (type == NULL && !seat->drag)
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
  process_cursor_motion(event->time_msec, event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_motion_absolute_event *event = data;

  // warp cursor
  if (event->time_msec)
    wlr_cursor_warp_absolute(server.cursor, &event->pointer->base,
      event->x, event->y);

  // get absolute pos
  double lx, ly, dx, dy;
  wlr_cursor_absolute_to_layout_coords(server.cursor, &event->pointer->base, event->x,
    event->y, &lx, &ly);
  dx = lx - server.cursor->x;
  dy = ly - server.cursor->y;

  // process motion
  process_cursor_motion(event->time_msec, dx, dy, dx, dy);
}

void cursor_button(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_button_event *event = data;
  wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);
  wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);

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
  wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
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

void handle_new_input(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_input_device *device = data;
  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    handle_new_keyboard(device);
    break;
  case WLR_INPUT_DEVICE_POINTER:
    wlr_cursor_attach_input_device(server.cursor, device);
    input_apply_config(device);
    break;
  default:
    input_apply_config(device);
    break;
  }

  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&server.keyboards))
      caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  wlr_seat_set_capabilities(server.seat, caps);
}

void request_set_selection(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(server.seat, event->source, event->serial);
}

void cursor_check_constraint_region(void) {
  struct wlr_pointer_constraint_v1 *constraint = server.active_pointer_constraint;
  pixman_region32_t *region = &constraint->region;
  struct bwm_toplevel *toplevel = constraint->surface->data;
  if (server.cursor_requires_warp && toplevel) {
    server.cursor_requires_warp = false;

    double sx = server.cursor->x + toplevel->node->rectangle.x;
    double sy = server.cursor->y + toplevel->node->rectangle.y;

    if (!pixman_region32_contains_point(region, floor(sx), floor(sy), NULL)) {
      int count;
      pixman_box32_t *boxes = pixman_region32_rectangles(region, &count);
      if (count > 0) {
        sx = (boxes[0].x1 + boxes[0].x2) / 2.0;
        sy = (boxes[0].y1 + boxes[0].y2) / 2.0;

        wlr_cursor_warp_closest(server.cursor, NULL,
          sx + toplevel->node->rectangle.x,
          sy + toplevel->node->rectangle.y);
      }
    }
  }

  // empty region if locked
  if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
      pixman_region32_copy(&server.pointer_confine, region);
  else
      pixman_region32_clear(&server.pointer_confine);
}

static void cursor_warp_to_constraint_hint(void) {
	struct wlr_pointer_constraint_v1 *active = server.active_pointer_constraint;
	if (active == NULL)
		return;

	if (active->current.cursor_hint.enabled) {
	  double sx = active->current.cursor_hint.x;
	  double sy = active->current.cursor_hint.y;

	  struct bwm_toplevel *toplevel = active->surface->data;
	  if (!toplevel)
	    return;

	  double lx = sx - toplevel->node->rectangle.x;
	  double ly = sy - toplevel->node->rectangle.y;

	  wlr_cursor_warp(server.cursor, NULL, lx, ly);
	  wlr_seat_pointer_warp(active->seat, sx, sy);
	}
}

void handle_cursor_contraint_commit(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	cursor_check_constraint_region();
}

static void cursor_constrain(struct wlr_pointer_constraint_v1 *constraint) {
	if (server.active_pointer_constraint == constraint)
    return;

  wl_list_remove(&server.pointer_constraint_commit.link);
  if (server.active_pointer_constraint) {
    if (!constraint)
      cursor_warp_to_constraint_hint();

    // deactivate current constraint
    wlr_pointer_constraint_v1_send_deactivated(server.active_pointer_constraint);
  }

  // set the new constraint
  server.active_pointer_constraint = constraint;

  if (!constraint) {
    wl_list_init(&server.pointer_constraint_commit.link);
    return;
  }

  server.cursor_requires_warp = true;

  if (pixman_region32_not_empty(&constraint->current.region))
    pixman_region32_intersect(&constraint->region,
                              &constraint->surface->input_region,
                              &constraint->current.region);
  else
    pixman_region32_copy(&constraint->region,
                          &constraint->surface->input_region);

  cursor_check_constraint_region();

  wlr_pointer_constraint_v1_send_activated(constraint);

  server.pointer_constraint_commit.notify = handle_cursor_contraint_commit;
  wl_signal_add(&constraint->surface->events.commit, &server.pointer_constraint_commit);
}

void handle_constraint_set_region(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	server.cursor_requires_warp = true;
}

void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct bwm_cursor_constraint *constraint = wl_container_of(listener, constraint, destroy);
	wl_list_remove(&constraint->set_region.link);
	wl_list_remove(&constraint->destroy.link);

  if (server.active_pointer_constraint == constraint->constraint) {
    cursor_warp_to_constraint_hint();

    if (constraint->constraint->link.next)
      wl_list_remove(&server.pointer_constraint_commit.link);

    wl_list_init(&server.pointer_constraint_commit.link);
    server.active_pointer_constraint = NULL;
  }
}

void handle_pointer_constraint(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_pointer_constraint_v1 *constraint = data;

  if (constraint->surface == server.seat->pointer_state.focused_surface) {
    server.active_pointer_constraint = constraint;

    struct bwm_cursor_constraint *cursor_constraint = calloc(1, sizeof(struct bwm_cursor_constraint));
    cursor_constraint->constraint = constraint;
    cursor_constraint->set_region.notify = handle_constraint_set_region;
    wl_signal_add(&constraint->events.set_region, &cursor_constraint->set_region);
    cursor_constraint->destroy.notify = handle_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &cursor_constraint->destroy);
  }
}

void handle_cursor_request_set_shape(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

  if (server.cursor_mode != CURSOR_PASSTHROUGH)
    return;

  if (event->seat_client == server.seat->pointer_state.focused_client)
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, wlr_cursor_shape_v1_name(event->shape));
}
