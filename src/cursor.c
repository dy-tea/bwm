#include "config.h"
#include "cursor.h"
#include "effects.h"
#include "idle_power.h"
#include "input_method.h"
#include "layer.h"
#include "once.h"
#include "output.h"
#include "pointer_constraint.h"
#include "scroller.h"
#include "server.h"
#include "tablet.h"
#include "tabs.h"
#include "tiling_drag.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "xwayland.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/region.h>
#include <wlr/xwayland.h>

extern keybind_t keybinds[];
extern size_t num_keybinds;
extern submap_t *active_submap;
extern bool keybind_matches(const keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym,
	uint32_t keycode);
extern void execute_keybind(const keybind_t *kb);
extern bool handle_keybind_raw(uint32_t modifiers, uint32_t keycode, bool pressed);
extern hotcornerbind_t hotcorner_bindings[];
extern size_t num_hotcornerbinds;
extern bool hotcornerbind_matches(const hotcornerbind_t *hc, int corner_x, int corner_y);
extern hotcornerbind_t *hotcorner_bind_match(int corner_x, int corner_y);
extern void execute_hotcornerbind(const hotcornerbind_t *hc);
extern bool gapless_monocle;

#define RESIZE_RATIO_MIN 0.1
#define RESIZE_RATIO_MAX 0.9

// Hot corner settings
static int hotcorner_threshold = 20;
static int hotcorner_cooldown_ms = 300;
static uint32_t hotcorner_last_trigger = 0;
static int hotcorner_current_x = 0;
static int hotcorner_current_y = 0;

// get current time in milliseconds
static uint32_t get_time_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void reset_cursor_mode(void) {
	if (server.tiled_resize_node) {
		node_t *node = server.tiled_resize_node;
		if (node->output && node->desktop)
			arrange(node->output, node->desktop, true);
	}

	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.grabbed_toplevel = NULL;
	server.grabbed_xwayland_view = NULL;
	server.tiled_resize_node = NULL;
	server.tiled_resize_parent_vertical = NULL;
	server.tiled_resize_parent_horizontal = NULL;
}

void *desktop_type_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
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

static node_t *find_vertical_ancestor(node_t *node) {
	if (!node || !node->parent)
		return NULL;
	node_t *p = node->parent;
	while (p && p->split_type != TYPE_VERTICAL)
		p = p->parent;
	return p;
}

static node_t *find_horizontal_ancestor(node_t *node) {
	if (!node || !node->parent)
		return NULL;
	node_t *p = node->parent;

	while (p && p->split_type != TYPE_HORIZONTAL)
		p = p->parent;

	return p;
}

static uint32_t get_tiled_resizable_edges(node_t *node) {
	uint32_t edges = 0;

	if (!node || !node->parent)
		return 0;

	node_t *v_ancestor = find_vertical_ancestor(node);
	if (v_ancestor) {
		node_t *child = node;
		while (child && child->parent != v_ancestor)
			child = child->parent;

		if (child == v_ancestor->first_child)
			edges |= WLR_EDGE_RIGHT;
		else
			edges |= WLR_EDGE_LEFT;
	}

	node_t *h_ancestor = find_horizontal_ancestor(node);
	if (h_ancestor) {
		node_t *child = node;
		while (child && child->parent != h_ancestor)
			child = child->parent;

		if (child == h_ancestor->first_child)
			edges |= WLR_EDGE_BOTTOM;
		else
			edges |= WLR_EDGE_TOP;
	}

	return edges;
}

static void apply_leaf_positions(desktop_t *d) {
	if (!d || !d->root)
		return;

	FOR_EACH_LEAF(n, d->root) {
		if (!n->client)
			continue;

		struct wlr_box r = n->client->arranged_rectangle;
		if (r.width < 1 || r.height < 1)
			continue;

		struct wlr_scene_tree *st = client_get_scene_tree(n->client);
		if (!st)
			continue;

		if (st->node.x != r.x || st->node.y != r.y) {
			wlr_scene_node_set_position(&st->node, r.x, r.y);
			if (n->output)
				effects_dirty_corner_masks(n->output);
		}

		if (n->client->toplevel)
			wlr_xdg_toplevel_set_size(n->client->toplevel->xdg_toplevel, r.width, r.height);
		else if (n->client->xwayland_view)
			wlr_xwayland_surface_configure(n->client->xwayland_view->xwayland_surface, r.x, r.y, r.width,
				r.height);

		unsigned int bw = effective_border_width(d);
		if (bw != 0) {
			struct wlr_box geo = {
				0,
				0,
				r.width,
				r.height
			};
			update_borders(client_border_tree(n->client), client_border_rects(n->client), geo, bw);
			update_border_colors(n->client);
			if (n->client->border_radius > 0.0f) {
				surface_rounded_t *rounded = client_get_rounded(n->client);
				if (rounded) {
					rounded_mark_border_size(rounded, r.width, r.height, (int)bw,
						n->client->toplevel && n->client->toplevel->node &&
							n->client->toplevel->node->output ?
							n->client->toplevel->node->output->wlr_output->scale : 1.0f);
				}
			}
		}
	}
}

static int find_scroller_tile_idx(scroller_column_t *col, client_t *c) {
	for (int j = 0; j < col->tile_count; j++)
		if (col->tiles[j].client == c)
			return j;
	return -1;
}

static int find_scroller_column(scroller_state_t *s, client_t *c) {
	for (int i = 0; i < s->column_count; i++)
		if (find_scroller_tile_idx(&s->columns[i], c) >= 0)
			return i;
	return -1;
}

static double max_d(double a, double b) {
	return a > b ? a : b;
}

// process cursor motion for tiled window resizing
static void process_cursor_tiled_resize(void) {
	node_t *node = server.tiled_resize_node;
	if (!node || !node->client)
		return;

	desktop_t *d = node->desktop;
	if (!d)
		return;

	// Handle scroller layout
	if (d->layout == LAYOUT_SCROLLER && d->scroller_state) {
		scroller_state_t *s = d->scroller_state;
		if (s->column_count == 0)
			return;

		int col = find_scroller_column(s, node->client);
		if (col < 0)
			return;

		double delta_x = server.cursor->x - server.grab_x;

		if (server.resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
			double area_w = max_d(1.0, (double)s->working_area.width);
			s->columns[col].width.value += delta_x / area_w;
			if (s->columns[col].width.value < 0.1)
				s->columns[col].width.value = 0.1;
			if (s->columns[col].width.value > 1.0)
				s->columns[col].width.value = 1.0;
			s->columns[col].width.type = SCROLLER_WIDTH_PROPORTION;
		}

		arrange(node->output, d, false);
		apply_leaf_positions(d);
		return;
	}

	// handle horizontal resizing
	if (server.tiled_resize_parent_vertical &&
			(server.resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))) {
		node_t *parent = server.tiled_resize_parent_vertical;
		double total_width = (double)parent->rectangle.width;

		if (total_width <= 0)
			total_width = 1.0;

		// calculate delta from initial grab position
		double delta_x = server.cursor->x - server.grab_x;

		// convert to ratio change
		double ratio_delta = delta_x / total_width;
		double new_ratio = server.tiled_resize_initial_ratio_v + ratio_delta;

		// clamp to valid range
		if (new_ratio < RESIZE_RATIO_MIN)
			new_ratio = RESIZE_RATIO_MIN;
		if (new_ratio > RESIZE_RATIO_MAX)
			new_ratio = RESIZE_RATIO_MAX;

		parent->split_ratio = new_ratio;
		parent->pending.split_ratio = new_ratio;
		parent->current.split_ratio = new_ratio;
	}

	// handle vertical resizing
	if (server.tiled_resize_parent_horizontal &&
			(server.resize_edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))) {
		node_t *parent = server.tiled_resize_parent_horizontal;
		double total_height = (double)parent->rectangle.height;

		if (total_height <= 0)
			total_height = 1.0;

		// calculate delta from initial grab position
		double delta_y = server.cursor->y - server.grab_y;

		// convert to ratio change
		double ratio_delta = delta_y / total_height;
		double new_ratio = server.tiled_resize_initial_ratio_h + ratio_delta;

		// clamp to valid range
		if (new_ratio < RESIZE_RATIO_MIN)
			new_ratio = RESIZE_RATIO_MIN;
		if (new_ratio > RESIZE_RATIO_MAX)
			new_ratio = RESIZE_RATIO_MAX;

		parent->split_ratio = new_ratio;
		parent->pending.split_ratio = new_ratio;
		parent->current.split_ratio = new_ratio;
	}

	// Use the proper layout function to recompute all positions
	if (node->output && d) {
		arrange(node->output, d, false);
		apply_leaf_positions(d);
	}
}

static void process_cursor_move(void) {
	toplevel_t *toplevel = server.grabbed_toplevel;
	xwayland_toplevel_t *xwayland_view = server.grabbed_xwayland_view;

	if (xwayland_view && xwayland_view->node && xwayland_view->node->client &&
			xwayland_view->node->client->state == STATE_FLOATING) {
		double x = server.cursor->x - server.grab_x;
		double y = server.cursor->y - server.grab_y;

		xwayland_view->node->client->floating_rectangle.x = (int)x;
		xwayland_view->node->client->floating_rectangle.y = (int)y;

		if (xwayland_view->scene_tree) {
			struct wlr_scene_node *stn = &xwayland_view->scene_tree->node;
			if (stn->x != x || stn->y != y) {
				wlr_scene_node_set_position(stn, x, y);
				if (xwayland_view->node->output)
					effects_dirty_corner_masks(xwayland_view->node->output);
			}
		}

		wlr_xwayland_surface_configure(xwayland_view->xwayland_surface, (int)x, (int)y,
			xwayland_view->xwayland_surface->width, xwayland_view->xwayland_surface->height);
		return;
	}

	if (!toplevel || !toplevel->node || !toplevel->node->client ||
		toplevel->node->client->state != STATE_FLOATING)
		return;

	double x = server.cursor->x - server.grab_x;
	double y = server.cursor->y - server.grab_y;

	toplevel->node->client->floating_rectangle.x = (int)x;
	toplevel->node->client->floating_rectangle.y = (int)y;

	struct wlr_scene_node *stn = &toplevel->scene_tree->node;
	if (stn->x != x || stn->y != y) {
		wlr_scene_node_set_position(stn, x, y);
		if (toplevel->node->output)
			effects_dirty_corner_masks(toplevel->node->output);
	}
}

static void process_cursor_resize(void) {
	// handle tiled mode resize
	if (server.tiled_resize_node) {
		process_cursor_tiled_resize();
		return;
	}

	toplevel_t *toplevel = server.grabbed_toplevel;
	xwayland_toplevel_t *xwayland_view = server.grabbed_xwayland_view;

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

	if (xwayland_view && xwayland_view->node && xwayland_view->node->client) {
		xwayland_view->node->client->floating_rectangle.x = new_left;
		xwayland_view->node->client->floating_rectangle.y = new_top;
		xwayland_view->node->client->floating_rectangle.width = new_width;
		xwayland_view->node->client->floating_rectangle.height = new_height;

		if (xwayland_view->scene_tree) {
			struct wlr_scene_node *stn = &xwayland_view->scene_tree->node;
			if (stn->x != new_left || stn->y != new_top) {
				wlr_scene_node_set_position(stn, new_left, new_top);
				if (xwayland_view->node->output)
					effects_dirty_corner_masks(xwayland_view->node->output);
			}
		}

		wlr_xwayland_surface_configure(xwayland_view->xwayland_surface, new_left, new_top, new_width,
			new_height);

		if (!xwayland_view->node || !xwayland_view->node->client)
			return;

		// update borders
		client_t *client = xwayland_view->node->client;
		unsigned int bw = effective_border_width(xwayland_view->node->desktop);
		if (bw != 0) {
			const struct wlr_box geo = {
				0,
				0,
				new_width,
				new_height
			};
			update_borders(xwayland_view->border_tree, xwayland_view->border_rects, geo, bw);
			update_border_colors(client);
		}

		return;
	}

	if (!toplevel || !toplevel->node || !toplevel->node->client)
		return;

	client_t *client = toplevel->node->client;
	client->floating_rectangle.x = new_left;
	client->floating_rectangle.y = new_top;
	client->floating_rectangle.width = new_width;
	client->floating_rectangle.height = new_height;

	struct wlr_scene_node *stn = &toplevel->scene_tree->node;
	if (stn->x != new_left || stn->y != new_top) {
		wlr_scene_node_set_position(stn, new_left, new_top);
		if (toplevel->node->output)
			effects_dirty_corner_masks(toplevel->node->output);
	}
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);

	// update borders
	unsigned int bw = effective_border_width(toplevel->node->desktop);
	if (bw != 0) {
		const struct wlr_box geo = {
			0,
			0,
			new_width,
			new_height
		};
		update_borders(toplevel->border_tree, toplevel->border_rects, geo, bw);
		update_border_colors(client);
		if (client->border_radius > 0.0f && toplevel->rounded) {
			rounded_mark_border_size(toplevel->rounded, new_width, new_height, (int)bw,
				toplevel->node && toplevel->node->output ? toplevel->node->output->wlr_output->scale : 1.0f);
		}
	}
}

static void process_cursor_motion(uint32_t time, double dx, double dy, double dx_unaccel,
		double dy_unaccel) {
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(server.relative_pointer_manager, server.seat,
			time * 1000, dx, dy, dx_unaccel, dy_unaccel);

		if (server.active_pointer_constraint != NULL && server.cursor_mode != CURSOR_RESIZE &&
				server.cursor_mode != CURSOR_MOVE) {
			struct toplevel_t *toplevel = server.active_pointer_constraint->surface->data;
			if (toplevel != NULL &&
					server.active_pointer_constraint->surface == server.seat->pointer_state.focused_surface) {
				const struct wlr_box geo = toplevel->node->rectangle;

				// calculate constraint
				double sx = server.cursor->x - geo.x - geo.width;
				double sy = server.cursor->y - geo.y - geo.height;
				double cx, cy;

				// apply confine on region
				if (wlr_region_confine(&server.active_pointer_constraint->region, sx, sy, sx + dx, sy + dy, &cx,
						&cy)) {
					dx = cx - sx;
					dy = cy - sy;
				}

				// if pointer is locked, do not move it
				if (server.active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			} else {
				pointer_constrain(NULL);
			}
		}
	}

	if (server.cursor_mode == CURSOR_MOVE) {
		process_cursor_move();
		return;
	} else if (server.cursor_mode == CURSOR_RESIZE) {
		process_cursor_resize();
		return;
	} else if (server.cursor_mode == CURSOR_TILING_DRAG) {
		tiling_drag_motion();
		return;
	}

	// hot corner detection
	if (server.focused_output && num_hotcornerbinds > 0) {
		struct wlr_box output_box;
		wlr_output_layout_get_box(server.output_layout, server.focused_output->wlr_output, &output_box);

		int corner_x = 0;
		int corner_y = 0;

		double cx = server.cursor->x;
		double cy = server.cursor->y;

		if (cx <= output_box.x + hotcorner_threshold)
			corner_x = -1;
		else if (cx >= output_box.x + output_box.width - hotcorner_threshold)
			corner_x = 1;

		if (cy <= output_box.y + hotcorner_threshold)
			corner_y = -1;
		else if (cy >= output_box.y + output_box.height - hotcorner_threshold)
			corner_y = 1;

		if (corner_x != hotcorner_current_x || corner_y != hotcorner_current_y) {
			hotcorner_current_x = corner_x;
			hotcorner_current_y = corner_y;

			if (corner_x != 0 && corner_y != 0) {
				uint32_t now = get_time_ms();
				if (now - hotcorner_last_trigger > (uint32_t)hotcorner_cooldown_ms) {
					hotcornerbind_t *hc = hotcorner_bind_match(corner_x, corner_y);
					if (hc) {
						hotcorner_last_trigger = now;
						wlr_log(WLR_DEBUG, "Hot corner triggered: (%d,%d)", corner_x, corner_y);
						execute_hotcornerbind(hc);
					}
				}
			}
		}
	}

	if (server.seat->drag && server.seat->drag->icon && server.seat->drag->icon->data) {
		struct wlr_scene_node *node = server.seat->drag->icon->data;
		wlr_scene_node_set_position(node, server.cursor->x, server.cursor->y);
	}

	wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
	idle_power_notify_activity();

	double sx, sy;
	struct wlr_seat *seat = server.seat;
	struct wlr_surface *surface = NULL;
	void *type = desktop_type_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);
	if (type == NULL && !seat->drag)
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");

	output_t *m = output_at(server.cursor->x, server.cursor->y);
	if (m && m != server.focused_output)
		server.focused_output = m;

	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);

		// focus follows mouse
		if (focus_follows_mouse != FOLLOWS_NO && type != NULL) {
			node_t *node = NULL;

			struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
			if (xdg_surface != NULL && xdg_surface->role != WLR_XDG_SURFACE_ROLE_POPUP) {
				toplevel_t *toplevel = type;
				if (toplevel && toplevel->node)
					node = toplevel->node;
			} else {
				struct wlr_xwayland_surface *xwayland_surface =
					wlr_xwayland_surface_try_from_wlr_surface(surface);
				if (xwayland_surface != NULL) {
					xwayland_toplevel_t *xwayland_view = type;
					if (xwayland_view && xwayland_view->node)
						node = xwayland_view->node;
				}
			}

			if (node && node->output && node->desktop && node->desktop == node->output->desk)
				focus_node(node->output, node->desktop, node);
		} else if (focus_follows_mouse == FOLLOWS_ALWAYS) {
			wlr_seat_keyboard_notify_clear_focus(seat);
		}
	} else {
		wlr_seat_pointer_clear_focus(seat);
		if (focus_follows_mouse == FOLLOWS_ALWAYS)
			wlr_seat_keyboard_notify_clear_focus(seat);
	}

	if (m)
		output_schedule_frame(m);
}

void begin_interactive(toplevel_t *toplevel, enum cursor_mode mode, uint32_t edges) {
	server.grabbed_toplevel = toplevel;
	server.cursor_mode = mode;

	// clear tiled resize state
	server.tiled_resize_node = NULL;
	server.tiled_resize_parent_vertical = NULL;
	server.tiled_resize_parent_horizontal = NULL;

	if (mode == CURSOR_MOVE) {
		if (toplevel->node && toplevel->node->client) {
			server.grab_x = server.cursor->x - toplevel->node->client->floating_rectangle.x;
			server.grab_y = server.cursor->y - toplevel->node->client->floating_rectangle.y;
		}
	} else if (mode == CURSOR_RESIZE) {
		if (toplevel->node && toplevel->node->client && IS_TILED(toplevel->node->client)) {
			// init tiled resize state
			server.tiled_resize_node = toplevel->node;
			server.tiled_resize_parent_vertical = find_vertical_ancestor(toplevel->node);
			server.tiled_resize_parent_horizontal = find_horizontal_ancestor(toplevel->node);

			// store initial ratios
			if (server.tiled_resize_parent_vertical)
				server.tiled_resize_initial_ratio_v = server.tiled_resize_parent_vertical->split_ratio;

			if (server.tiled_resize_parent_horizontal)
				server.tiled_resize_initial_ratio_h = server.tiled_resize_parent_horizontal->split_ratio;

			// store grab position
			server.grab_x = server.cursor->x;
			server.grab_y = server.cursor->y;
			server.resize_edges = edges;
		} else {
			// floating resize
			double border_x = server.cursor->x;
			double border_y = server.cursor->y;
			if (edges & WLR_EDGE_RIGHT)
				border_x = toplevel->node->client->floating_rectangle.x +
					toplevel->node->client->floating_rectangle.width;
			if (edges & WLR_EDGE_BOTTOM)
				border_y = toplevel->node->client->floating_rectangle.y +
					toplevel->node->client->floating_rectangle.height;

			server.grab_x = server.cursor->x - border_x;
			server.grab_y = server.cursor->y - border_y;

			server.grab_geobox = toplevel->node->client->floating_rectangle;
			server.resize_edges = edges;
		}
	}
}

void cursor_motion(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_cursor_motion(event->time_msec, event->delta_x, event->delta_y, event->unaccel_dx,
		event->unaccel_dy);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_motion_absolute_event *event = data;

	// warp cursor
	if (event->time_msec)
		wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);

	// get absolute pos
	double lx, ly, dx, dy;
	wlr_cursor_absolute_to_layout_coords(server.cursor, &event->pointer->base, event->x, event->y, &lx,
		&ly);
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
	idle_power_notify_activity();

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server.cursor_mode == CURSOR_TILING_DRAG) {
			tiling_drag_finish();
			server.cursor_buttons &= ~(1 << (event->button - 272));
			return;
		}
		reset_cursor_mode();
		server.cursor_buttons &= ~(1 << (event->button - 272));
	} else {
		// tab bar click
		if (event->button == BTN_LEFT) {
			for (output_t *m = mon_head; m != NULL; m = m->next) {
				desktop_t *d = m->desk;
				if (d == NULL)
					continue;

				node_t *tab_leaf = tabs_hit_test_desktop(d, server.cursor->x, server.cursor->y);
				if (tab_leaf != NULL) {
					server.focus_from_click = true;
					focus_node(m, d, tab_leaf);
					arrange(m, d, true);
					server.cursor_buttons |= 1 << (event->button - 272);
					return;
				}
			}
		}

		// middle-click on tab bar closes the tab
		if (event->button == BTN_MIDDLE) {
			for (output_t *m = mon_head; m != NULL; m = m->next) {
				desktop_t *d = m->desk;
				if (d == NULL)
					continue;

				node_t *tab_leaf = tabs_hit_test_desktop(d, server.cursor->x, server.cursor->y);
				if (tab_leaf != NULL) {
					kill_node(d, tab_leaf);
					server.cursor_buttons |= 1 << (event->button - 272);
					return;
				}
			}
		}

		double sx, sy;
		struct wlr_surface *surface = NULL;
		void *type = desktop_type_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);
		if (type == NULL)
			return;

		struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
		if (xdg_surface != NULL && xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
			// le skip
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
					output_t *m = xwayland_view->node->output;
					desktop_t *d = xwayland_view->node->desktop;
					if (d && d != (m ? m->desk : NULL))
						d->focus = xwayland_view->node;
					server.focus_from_click = true;
					focus_node(m, d, xwayland_view->node);
				}
			} else {
				toplevel_t *toplevel = type;
				if (toplevel && toplevel->node) {
					output_t *m = toplevel->node->output;
					desktop_t *d = toplevel->node->desktop;
					if (!d)
						d = m ? m->desk : NULL;
					focus_node(m, d, toplevel->node);
				}
			}
		}

		// add to cursor buttons
		server.cursor_buttons |= 1 << (event->button - 272);

		// perform binds
		struct wlr_keyboard *wlr_keyboard = wlr_seat_get_keyboard(server.seat);
		uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_keyboard);
		for (uint32_t i = 0; i != 5; ++i) {
			if (server.cursor_buttons & (1 << i)) {
				uint32_t keycode = 0x20000000 + i + 272;
				handle_keybind_raw(modifiers, keycode, true);

				keybind_t *matched_kb = NULL;
				if (active_submap) {
					for (size_t j = 0; j < active_submap->num_keybinds; j++) {
						keybind_t *kb = &active_submap->keybinds[j];
						if (kb->use_keycode && keybind_matches(kb, modifiers, 0, keycode)) {
							matched_kb = kb;
							break;
						}
					}
				}
				if (!matched_kb) {
					for (size_t j = 0; j < num_keybinds; j++) {
						keybind_t *kb = &keybinds[j];
						if (kb->use_keycode && keybind_matches(kb, modifiers, 0, keycode)) {
							matched_kb = kb;
							break;
						}
					}
				}

				if (matched_kb && (matched_kb->action == BIND_INTERACTIVE_MOVE ||
						matched_kb->action == BIND_INTERACTIVE_RESIZE || matched_kb->action == BIND_TILING_DRAG)) {
					toplevel_t *toplevel = NULL;
					if (type && ((toplevel_t *)type)->node)
						toplevel = type;

					if (toplevel && toplevel->node && toplevel->node->client) {
						if (matched_kb->action == BIND_TILING_DRAG) {
							if (IS_TILED(toplevel->node->client))
								tiling_drag_begin(toplevel->node);
						} else if (matched_kb->action == BIND_INTERACTIVE_MOVE) {
							if (toplevel->node->client->state == STATE_FLOATING)
								begin_interactive(toplevel, CURSOR_MOVE, 0);
							else if (IS_TILED(toplevel->node->client))
								tiling_drag_begin(toplevel->node);
						} else if (matched_kb->action == BIND_INTERACTIVE_RESIZE) {
							client_t *c = toplevel->node->client;
							desktop_t *d = toplevel->node->desktop;
							uint32_t edges = 0;

							if (c->state == STATE_FLOATING) {
								// floating resize
								double wx = c->floating_rectangle.x;
								double wy = c->floating_rectangle.y;
								double ww = c->floating_rectangle.width;
								double wh = c->floating_rectangle.height;
								double cx = server.cursor->x;
								double cy = server.cursor->y;

								double third_w = ww / 3.0;
								double third_h = wh / 3.0;

								bool in_left = cx < wx + third_w;
								bool in_right = cx > wx + ww - third_w;
								bool in_top = cy < wy + third_h;
								bool in_bottom = cy > wy + wh - third_h;

								if (in_left || in_right)
									edges |= in_left ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;

								if (in_top || in_bottom)
									edges |= in_top ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;

								if (edges == 0) {
									double dist_left = cx - wx;
									double dist_right = (wx + ww) - cx;
									double dist_top = cy - wy;
									double dist_bottom = (wy + wh) - cy;

									double min_dist = dist_top;
									edges = WLR_EDGE_TOP;

									if (dist_bottom < min_dist) {
										min_dist = dist_bottom;
										edges = WLR_EDGE_BOTTOM;
									}
									if (dist_left < min_dist) {
										min_dist = dist_left;
										edges = WLR_EDGE_LEFT;
									}
									if (dist_right < min_dist)
										edges = WLR_EDGE_RIGHT;
								}
							} else if (IS_TILED(c)) {
								// for scroller layouts, only allow horizontal edges (column width resize)
								if (d && d->layout == LAYOUT_SCROLLER) {
									double wx = c->tiled_rectangle.x;
									double ww = c->tiled_rectangle.width;
									double cx = server.cursor->x;

									double third_w = ww / 3.0;

									bool in_left = cx < wx + third_w;
									bool in_right = cx > wx + ww - third_w;

									if (in_left)
										edges = WLR_EDGE_LEFT;
									else if (in_right)
										edges = WLR_EDGE_RIGHT;
									else {
										double dist_left = cx - wx;
										double dist_right = (wx + ww) - cx;
										edges = dist_left < dist_right ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
									}
								} else {
									edges = get_tiled_resizable_edges(toplevel->node);

									if (edges != 0) {
										// determine edge
										double wx = c->tiled_rectangle.x;
										double wy = c->tiled_rectangle.y;
										double ww = c->tiled_rectangle.width;
										double wh = c->tiled_rectangle.height;
										double cx = server.cursor->x;
										double cy = server.cursor->y;

										double third_w = ww / 3.0;
										double third_h = wh / 3.0;

										bool in_left = cx < wx + third_w;
										bool in_right = cx > wx + ww - third_w;
										bool in_top = cy < wy + third_h;
										bool in_bottom = cy > wy + wh - third_h;

										uint32_t clicked_edges = 0;

										if (in_left || in_right)
											clicked_edges |= in_left ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;

										if (in_top || in_bottom)
											clicked_edges |= in_top ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;

										if (clicked_edges == 0) {
											double dist_left = cx - wx;
											double dist_right = (wx + ww) - cx;
											double dist_top = cy - wy;
											double dist_bottom = (wy + wh) - cy;

											double min_dist = INFINITY;

											if ((edges & WLR_EDGE_LEFT) && dist_left < min_dist) {
												min_dist = dist_left;
												clicked_edges = WLR_EDGE_LEFT;
											}
											if ((edges & WLR_EDGE_RIGHT) && dist_right < min_dist) {
												min_dist = dist_right;
												clicked_edges = WLR_EDGE_RIGHT;
											}
											if ((edges & WLR_EDGE_TOP) && dist_top < min_dist) {
												min_dist = dist_top;
												clicked_edges = WLR_EDGE_TOP;
											}
											if ((edges & WLR_EDGE_BOTTOM) && dist_bottom < min_dist) {
												min_dist = dist_bottom;
												clicked_edges = WLR_EDGE_BOTTOM;
											}
										}

										// intersect clicked edges with resizable edges
										edges = clicked_edges & edges;
									}
								}
							}

							if (edges != 0)
								begin_interactive(toplevel, CURSOR_RESIZE, edges);
						}
					}
				}
			}
		}
	}
}

void cursor_axis(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_axis_event *event = data;

	// cycle through tabs when cursor is over a tab bar
	for (output_t *m = mon_head; m != NULL; m = m->next) {
		desktop_t *d = m->desk;
		if (d == NULL)
			continue;

		node_t *tab_leaf = tabs_hit_test_desktop(d, server.cursor->x, server.cursor->y);
		if (tab_leaf != NULL) {
			node_t *tab_node = tabbed_ancestor(tab_leaf);
			if (tab_node != NULL) {
				node_t *next;
				double delta = event->delta_discrete != 0 ? (double)event->delta_discrete : event->delta;
				if (delta < 0)
					next = tab_prev_leaf(tab_node, d->focus);
				else
					next = tab_next_leaf(tab_node, d->focus);

				if (next != NULL && next != d->focus) {
					focus_node(m, d, next);
					arrange(m, d, true);
				}
			}
			return;
		}
	}

	output_t *m = server.focused_output;
	desktop_t *d = m ? m->desk : NULL;
	bool is_touchpad = (event->source == WL_POINTER_AXIS_SOURCE_FINGER ||
		event->source == WL_POINTER_AXIS_SOURCE_CONTINUOUS);

	// cancel any ongoing touchpad scroll gesture if the layout is not scroller
	if (server.touchpad_scroll.ongoing && (!d || d->layout != LAYOUT_SCROLLER))
		server.touchpad_scroll.ongoing = false;

	// handle touchpad horizontal scrolling in the scroller layout
	if (d && d->layout == LAYOUT_SCROLLER && is_touchpad &&
			event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		if (!server.touchpad_scroll.ongoing) {
			server.touchpad_scroll.ongoing = true;
			server.touchpad_scroll.is_touchpad = is_touchpad;
			scroller_view_offset_gesture_begin(d, is_touchpad);
		}
		scroller_view_offset_gesture_update(d, event->delta);
		arrange(m, d, true);
		return;
	}

	// cancel any ongoing touchpad scroll gesture if this event is not a
	// horizontal touchpad scroll
	if (server.touchpad_scroll.ongoing) {
		server.touchpad_scroll.ongoing = false;
		if (d && d->layout == LAYOUT_SCROLLER) {
			scroller_view_offset_gesture_end(d);
			arrange(m, d, true);
		}
	}

	wlr_seat_pointer_notify_axis(server.seat, event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
	wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
	idle_power_notify_activity();
}

void cursor_frame(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	wlr_seat_pointer_notify_frame(server.seat);
}

static bool gesture_binding_check(enum gesture_type type, uint8_t fingers) {
	for (size_t i = 0; i < num_gesturebinds; i++)
		if (gesturebind_matches(&gesture_bindings[i], type, fingers))
			return true;

	return false;
}

static gesturebind_t *gesture_bind_match(enum gesture_type type, uint8_t fingers,
		uint32_t directions) {
	gesturebind_t *best_match = NULL;
	int8_t best_score = -1;

	for (size_t i = 0; i < num_gesturebinds; i++) {
		gesturebind_t *gb = &gesture_bindings[i];
		if (!gesturebind_matches(gb, type, fingers))
			continue;

		if (gb->directions != GESTURE_DIRECTION_NONE)
			if ((directions & gb->directions) == 0)
				continue;

		gesture_t gest = {
			.type = type,
			.fingers = fingers,
			.directions = directions
		};
		gesture_t target = {
			.type = gb->type,
			.fingers = gb->fingers,
			.directions = gb->directions
		};
		int8_t score = gesture_compare(&gest, &target);

		if (score > best_score) {
			best_score = score;
			best_match = gb;
		}
	}

	return best_match;
}

void handle_pointer_hold_begin(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_hold_begin_event *event = data;

	if (gesture_binding_check(GESTURE_TYPE_HOLD, event->fingers)) {
		gesture_tracker_begin(&server.gesture_tracker, GESTURE_TYPE_HOLD, event->fingers);
	} else {
		wlr_pointer_gestures_v1_send_hold_begin(server.pointer_gestures, server.seat, event->time_msec,
			event->fingers);
	}
}

void handle_pointer_hold_end(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_hold_end_event *event = data;

	if (!gesture_tracker_check(&server.gesture_tracker, GESTURE_TYPE_HOLD)) {
		wlr_pointer_gestures_v1_send_hold_end(server.pointer_gestures, server.seat, event->time_msec,
			event->cancelled);
		return;
	}

	if (event->cancelled) {
		gesture_tracker_cancel(&server.gesture_tracker);
		return;
	}

	gesturebind_t *binding = gesture_bind_match(GESTURE_TYPE_HOLD, server.gesture_tracker.fingers, 0);
	if (binding)
		execute_gesturebind(binding);

	gesture_tracker_end(&server.gesture_tracker);
}

void handle_pointer_pinch_begin(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_pinch_begin_event *event = data;

	if (gesture_binding_check(GESTURE_TYPE_PINCH, event->fingers)) {
		gesture_tracker_begin(&server.gesture_tracker, GESTURE_TYPE_PINCH, event->fingers);
	} else {
		wlr_pointer_gestures_v1_send_pinch_begin(server.pointer_gestures, server.seat, event->time_msec,
			event->fingers);
	}
}

void handle_pointer_pinch_update(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_pinch_update_event *event = data;

	if (gesture_tracker_check(&server.gesture_tracker, GESTURE_TYPE_PINCH)) {
		gesture_tracker_update(&server.gesture_tracker, event->dx, event->dy, event->scale,
			event->rotation);
	} else {
		wlr_pointer_gestures_v1_send_pinch_update(server.pointer_gestures, server.seat, event->time_msec,
			event->dx, event->dy, event->scale, event->rotation);
	}
}

void handle_pointer_pinch_end(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_pinch_end_event *event = data;

	if (!gesture_tracker_check(&server.gesture_tracker, GESTURE_TYPE_PINCH)) {
		wlr_pointer_gestures_v1_send_pinch_end(server.pointer_gestures, server.seat, event->time_msec,
			event->cancelled);
		return;
	}

	if (event->cancelled) {
		gesture_tracker_cancel(&server.gesture_tracker);
		return;
	}

	uint32_t directions = GESTURE_DIRECTION_NONE;
	if (server.gesture_tracker.scale < 1.0)
		directions |= GESTURE_DIRECTION_INWARD;
	else if (server.gesture_tracker.scale > 1.0)
		directions |= GESTURE_DIRECTION_OUTWARD;

	gesturebind_t *binding = gesture_bind_match(GESTURE_TYPE_PINCH, server.gesture_tracker.fingers,
		directions);
	if (binding)
		execute_gesturebind(binding);

	gesture_tracker_end(&server.gesture_tracker);
}

void handle_pointer_swipe_begin(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_swipe_begin_event *event = data;

	if (gesture_binding_check(GESTURE_TYPE_SWIPE, event->fingers)) {
		gesture_tracker_begin(&server.gesture_tracker, GESTURE_TYPE_SWIPE, event->fingers);
	} else {
		wlr_pointer_gestures_v1_send_swipe_begin(server.pointer_gestures, server.seat, event->time_msec,
			event->fingers);
	}
}

void handle_pointer_swipe_update(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_swipe_update_event *event = data;

	if (gesture_tracker_check(&server.gesture_tracker, GESTURE_TYPE_SWIPE)) {
		gesture_tracker_update(&server.gesture_tracker, event->dx, event->dy, NAN, NAN);
	} else {
		wlr_pointer_gestures_v1_send_swipe_update(server.pointer_gestures, server.seat, event->time_msec,
			event->dx, event->dy);
	}
}

void handle_pointer_swipe_end(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_swipe_end_event *event = data;

	if (!gesture_tracker_check(&server.gesture_tracker, GESTURE_TYPE_SWIPE)) {
		wlr_pointer_gestures_v1_send_swipe_end(server.pointer_gestures, server.seat, event->time_msec,
			event->cancelled);
		return;
	}

	if (event->cancelled) {
		gesture_tracker_cancel(&server.gesture_tracker);
		return;
	}

	uint32_t directions = GESTURE_DIRECTION_NONE;
	double threshold = 30.0;

	if (server.gesture_tracker.dx < -threshold)
		directions |= GESTURE_DIRECTION_LEFT;
	else if (server.gesture_tracker.dx > threshold)
		directions |= GESTURE_DIRECTION_RIGHT;

	if (server.gesture_tracker.dy < -threshold)
		directions |= GESTURE_DIRECTION_UP;
	else if (server.gesture_tracker.dy > threshold)
		directions |= GESTURE_DIRECTION_DOWN;

	gesturebind_t *binding = gesture_bind_match(GESTURE_TYPE_SWIPE, server.gesture_tracker.fingers,
		directions);
	if (binding)
		execute_gesturebind(binding);
	gesture_tracker_end(&server.gesture_tracker);
}

void cursor_rebase(void) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	desktop_type_at(server.cursor->x, server.cursor->y, &surface, &sx, &sy);

	if (surface)
		wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
	else
		wlr_seat_pointer_clear_focus(server.seat);
}

static void handle_tablet_tool_axis(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_tablet_tool_axis_event *event = data;
	tablet_t *tablet = event->tablet->data;
	if (!tablet)
		return;

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_X || event->updated_axes & WLR_TABLET_TOOL_AXIS_Y) {
		wlr_cursor_warp(server.cursor, NULL, event->x, event->y);
	}

	// send other axis events via tablet
	tablet_tool_t *tool = event->tool->data;
	if (tool && tool->tablet_v2_tool) {
		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
			wlr_tablet_v2_tablet_tool_notify_pressure(tool->tablet_v2_tool, event->pressure);

		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
			wlr_tablet_v2_tablet_tool_notify_distance(tool->tablet_v2_tool, event->distance);

		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X ||
			event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y)
			wlr_tablet_v2_tablet_tool_notify_tilt(tool->tablet_v2_tool, event->tilt_x, event->tilt_y);

		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
			wlr_tablet_v2_tablet_tool_notify_rotation(tool->tablet_v2_tool, event->rotation);

		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
			wlr_tablet_v2_tablet_tool_notify_slider(tool->tablet_v2_tool, event->slider);

		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
			wlr_tablet_v2_tablet_tool_notify_wheel(tool->tablet_v2_tool, event->wheel_delta, 0);
	}
}

static void handle_tablet_tool_proximity(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_tablet_tool_proximity_event *event = data;
	tablet_t *tablet = event->tablet->data;
	if (!tablet)
		return;

	if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
		tablet_tool_t *tool = event->tool->data;
		if (!tool) {
			tool = tablet_tool_configure(tablet, event->tool);
			if (!tool)
				return;
		}
		handle_tablet_tool_position(event->tool, tablet, false, event->x, event->y, event->time_msec);
	} else {
		tablet_tool_t *tool = event->tool->data;
		if (tool && tool->tablet_v2_tool)
			wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tablet_v2_tool);
	}
}

static void handle_tablet_tool_tip(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_tablet_tool_tip_event *event = data;
	tablet_t *tablet = event->tablet->data;
	if (!tablet)
		return;

	tablet_tool_t *tool = event->tool->data;
	if (!tool)
		return;

	handle_tablet_tool_position(event->tool, tablet, event->state == WLR_TABLET_TOOL_TIP_DOWN, event->x,
		event->y, event->time_msec);
}

static void handle_tablet_tool_button(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_tablet_tool_button_event *event = data;
	tablet_t *tablet = event->tablet->data;
	if (!tablet)
		return;

	tablet_tool_t *tool = event->tool->data;
	if (!tool || !tool->tablet_v2_tool)
		return;

	wlr_tablet_v2_tablet_tool_notify_button(tool->tablet_v2_tool, event->button,
		(enum zwp_tablet_pad_v2_button_state)event->state);
}

void cursor_init(void) {
	ONCE();
	server.cursor = wlr_cursor_create();
	if (!server.cursor) {
		wlr_log(WLR_ERROR, "Failed to create cursor");
		exit(EXIT_FAILURE);
	}
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	if (!server.cursor_mgr) {
		wlr_log(WLR_ERROR, "Failed to create cursor manager");
		exit(EXIT_FAILURE);
	}
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.last_focused_xwayland_view = NULL;

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

	server.cursor_tablet_tool_axis.notify = handle_tablet_tool_axis;
	wl_signal_add(&server.cursor->events.tablet_tool_axis, &server.cursor_tablet_tool_axis);

	server.cursor_tablet_tool_proximity.notify = handle_tablet_tool_proximity;
	wl_signal_add(&server.cursor->events.tablet_tool_proximity, &server.cursor_tablet_tool_proximity);

	server.cursor_tablet_tool_tip.notify = handle_tablet_tool_tip;
	wl_signal_add(&server.cursor->events.tablet_tool_tip, &server.cursor_tablet_tool_tip);

	server.cursor_tablet_tool_button.notify = handle_tablet_tool_button;
	wl_signal_add(&server.cursor->events.tablet_tool_button, &server.cursor_tablet_tool_button);

	server.hold_begin.notify = handle_pointer_hold_begin;
	wl_signal_add(&server.cursor->events.hold_begin, &server.hold_begin);

	server.hold_end.notify = handle_pointer_hold_end;
	wl_signal_add(&server.cursor->events.hold_end, &server.hold_end);

	server.pinch_begin.notify = handle_pointer_pinch_begin;
	wl_signal_add(&server.cursor->events.pinch_begin, &server.pinch_begin);

	server.pinch_update.notify = handle_pointer_pinch_update;
	wl_signal_add(&server.cursor->events.pinch_update, &server.pinch_update);

	server.pinch_end.notify = handle_pointer_pinch_end;
	wl_signal_add(&server.cursor->events.pinch_end, &server.pinch_end);

	server.swipe_begin.notify = handle_pointer_swipe_begin;
	wl_signal_add(&server.cursor->events.swipe_begin, &server.swipe_begin);

	server.swipe_update.notify = handle_pointer_swipe_update;
	wl_signal_add(&server.cursor->events.swipe_update, &server.swipe_update);

	server.swipe_end.notify = handle_pointer_swipe_end;
	wl_signal_add(&server.cursor->events.swipe_end, &server.swipe_end);
}

void cursor_fini(void) {
	ONCE();
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.cursor_tablet_tool_axis.link);
	wl_list_remove(&server.cursor_tablet_tool_proximity.link);
	wl_list_remove(&server.cursor_tablet_tool_tip.link);
	wl_list_remove(&server.cursor_tablet_tool_button.link);

	wl_list_remove(&server.swipe_begin.link);
	wl_list_remove(&server.swipe_update.link);
	wl_list_remove(&server.swipe_end.link);

	wl_list_remove(&server.pinch_begin.link);
	wl_list_remove(&server.pinch_update.link);
	wl_list_remove(&server.pinch_end.link);

	wl_list_remove(&server.hold_begin.link);
	wl_list_remove(&server.hold_end.link);

	wlr_cursor_destroy(server.cursor);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
}
