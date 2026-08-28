#include "output.h"
#include "server.h"
#include "tiling_drag.h"
#include "tree.h"
#include "types.h"
#include <math.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

static struct wlr_box get_leaf_rect(node_t *node) {
	if (node) {
		if (node->client)
			return node->client->tiled_rectangle;
		return node->rectangle;
	}
	return (struct wlr_box){0};
}

static enum wlr_edges determine_drop_edge(node_t *target, double lx, double ly) {
	struct wlr_box rect = get_leaf_rect(target);
	double edge_threshold = 0.3;

	double dw = rect.width > 0 ? (double)rect.width : 1.0;
	double dh = rect.height > 0 ? (double)rect.height : 1.0;
	double min_dim = fmin(dw, dh);
	double threshold = min_dim * edge_threshold;

	double dist_left = lx - rect.x;
	double dist_right = (rect.x + rect.width) - lx;
	double dist_top = ly - rect.y;
	double dist_bottom = (rect.y + rect.height) - ly;

	if (dist_left < threshold && dist_left <= dist_right && dist_left <= dist_top &&
		dist_left <= dist_bottom)
		return WLR_EDGE_LEFT;
	if (dist_right < threshold && dist_right <= dist_left && dist_right <= dist_top &&
		dist_right <= dist_bottom)
		return WLR_EDGE_RIGHT;
	if (dist_top < threshold && dist_top <= dist_bottom && dist_top <= dist_left &&
		dist_top <= dist_right)
		return WLR_EDGE_TOP;
	if (dist_bottom < threshold && dist_bottom <= dist_top && dist_bottom <= dist_left &&
		dist_bottom <= dist_right)
		return WLR_EDGE_BOTTOM;

	return WLR_EDGE_NONE;
}

static void update_indicator(enum wlr_edges edge, node_t *target) {
	if (!server.tiling_drag_indicator || !target) {
		if (server.tiling_drag_indicator)
			wlr_scene_node_set_enabled(&server.tiling_drag_indicator->node, false);
		return;
	}

	struct wlr_box rect = get_leaf_rect(target);
	if (rect.width == 0 || rect.height == 0) {
		wlr_scene_node_set_enabled(&server.tiling_drag_indicator->node, false);
		return;
	}

	int iw, ih;
	switch (edge) {
	case WLR_EDGE_LEFT:
		iw = 4;
		ih = rect.height;
		wlr_scene_rect_set_size(server.tiling_drag_indicator, iw, ih);
		wlr_scene_node_set_position(&server.tiling_drag_indicator->node, rect.x, rect.y);
		break;
	case WLR_EDGE_RIGHT:
		iw = 4;
		ih = rect.height;
		wlr_scene_rect_set_size(server.tiling_drag_indicator, iw, ih);
		wlr_scene_node_set_position(&server.tiling_drag_indicator->node, rect.x + rect.width - iw,
			rect.y);
		break;
	case WLR_EDGE_TOP:
		iw = rect.width;
		ih = 4;
		wlr_scene_rect_set_size(server.tiling_drag_indicator, iw, ih);
		wlr_scene_node_set_position(&server.tiling_drag_indicator->node, rect.x, rect.y);
		break;
	case WLR_EDGE_BOTTOM:
		iw = rect.width;
		ih = 4;
		wlr_scene_rect_set_size(server.tiling_drag_indicator, iw, ih);
		wlr_scene_node_set_position(&server.tiling_drag_indicator->node, rect.x,
			rect.y + rect.height - ih);
		break;
	default:
		iw = rect.width;
		ih = rect.height;
		wlr_scene_rect_set_size(server.tiling_drag_indicator, iw, ih);
		wlr_scene_node_set_position(&server.tiling_drag_indicator->node, rect.x, rect.y);
		break;
	}

	wlr_scene_node_set_enabled(&server.tiling_drag_indicator->node, true);
}

static node_t *find_leaf_at(node_t *node, double lx, double ly, node_t *exclude) {
	if (!node || node->hidden)
		return NULL;

	if (is_leaf(node)) {
		if (node == exclude)
			return NULL;
		if (wlr_box_contains_point(&node->rectangle, lx, ly))
			return node;
		return NULL;
	}

	node_t *found = find_leaf_at(node->first_child, lx, ly, exclude);
	if (found)
		return found;
	return find_leaf_at(node->second_child, lx, ly, exclude);
}

void tiling_drag_begin(node_t *node) {
	if (!node || !node->client || !IS_TILED(node->client))
		return;

	wlr_log(WLR_DEBUG, "tiling_drag_begin: node=%u app_id=%s", node->id, node->client->app_id);

	server.cursor_mode = CURSOR_TILING_DRAG;
	server.tiling_drag_node = node;
	server.tiling_drag_grab_x = server.cursor->x;
	server.tiling_drag_grab_y = server.cursor->y;
	server.tiling_drag_target_node = NULL;
	server.tiling_drag_target_edge = WLR_EDGE_NONE;
	server.tiling_drag_threshold_reached = false;

	if (!server.tiling_drag_indicator) {
		server.tiling_drag_indicator = wlr_scene_rect_create(server.drag_tree, 0, 0,
			tiling_drag_indicator_color_rgba);
	}
	wlr_scene_node_set_enabled(&server.tiling_drag_indicator->node, false);
}

void tiling_drag_motion(void) {
	if (!server.tiling_drag_node)
		return;

	double dx = server.cursor->x - server.tiling_drag_grab_x;
	double dy = server.cursor->y - server.tiling_drag_grab_y;
	double dist = sqrt(dx * dx + dy * dy);

	if (!server.tiling_drag_threshold_reached) {
		if (dist >= TILING_DRAG_THRESHOLD)
			server.tiling_drag_threshold_reached = true;
		return;
	}

	node_t *target = NULL;
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		desktop_t *d = m->desk;
		if (!d || !d->root)
			continue;
		target = find_leaf_at(d->root, server.cursor->x, server.cursor->y, server.tiling_drag_node);
		if (target)
			break;
	}

	if (target && target->client) {
		server.tiling_drag_target_node = target;
		server.tiling_drag_target_edge = determine_drop_edge(target, server.cursor->x, server.cursor->y);
		update_indicator(server.tiling_drag_target_edge, target);
	} else {
		server.tiling_drag_target_node = NULL;
		server.tiling_drag_target_edge = WLR_EDGE_NONE;
		if (server.tiling_drag_indicator)
			wlr_scene_node_set_enabled(&server.tiling_drag_indicator->node, false);
	}
}

void tiling_drag_finish(void) {
	node_t *dragged = server.tiling_drag_node;
	node_t *target = server.tiling_drag_target_node;

	if (!dragged || !target) {
		tiling_drag_abort();
		return;
	}

	wlr_log(WLR_DEBUG, "tiling_drag_finish: dragged=%u target=%u edge=%d", dragged->id, target->id,
		server.tiling_drag_target_edge);

	enum wlr_edges edge = server.tiling_drag_target_edge;
	desktop_t *src_desk = dragged->desktop;
	output_t *src_out = dragged->output;

	if (edge == WLR_EDGE_NONE) {
		swap_nodes(dragged->output, dragged->desktop, dragged, target->output, target->desktop, target);
	} else {
		desktop_t *target_desk = target->desktop;
		output_t *target_out = target->output;

		remove_node(dragged->desktop, dragged);

		direction_t dir;
		switch (edge) {
		case WLR_EDGE_LEFT:
			dir = DIR_WEST;
			break;
		case WLR_EDGE_RIGHT:
			dir = DIR_EAST;
			break;
		case WLR_EDGE_TOP:
			dir = DIR_NORTH;
			break;
		case WLR_EDGE_BOTTOM:
			dir = DIR_SOUTH;
			break;
		default:
			dir = DIR_EAST;
			break;
		}

		presel_dir(target, dir);
		insert_node(target_desk, dragged, target);
		presel_cancel(target);

		arrange(target_out, target_desk, true);
		if (src_desk != target_desk)
			arrange(src_out, src_desk, true);
	}

	tiling_drag_abort();
}

void tiling_drag_abort(void) {
	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.tiling_drag_node = NULL;
	server.tiling_drag_target_node = NULL;
	server.tiling_drag_target_edge = WLR_EDGE_NONE;

	if (server.tiling_drag_indicator) {
		wlr_scene_node_destroy(&server.tiling_drag_indicator->node);
		server.tiling_drag_indicator = NULL;
	}
}
