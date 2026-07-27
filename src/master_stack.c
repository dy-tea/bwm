#include "master_stack.h"
#include "output.h"
#include "toplevel.h"
#include "tree.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/util/log.h>

float master_stack_ratio = 0.5f;
master_area_orientation_t master_stack_orientation = MASTER_LEFT;
stack_layout_t master_stack_layout = STACK_VERTICAL;

typedef enum {
	DIRECTION_WEST,
	DIRECTION_SOUTH,
	DIRECTION_NORTH,
	DIRECTION_EAST,
} master_stack_direction_t;

static int compare_node_order(const void *lhs, const void *rhs) {
	const node_t *a = *(node_t *const *)lhs;
	const node_t *b = *(node_t *const *)rhs;
	uint64_t a_order = a->client->master_stack_order;
	uint64_t b_order = b->client->master_stack_order;

	if (a_order < b_order)
		return -1;
	if (a_order > b_order)
		return 1;
	return 0;
}

static int compare_master_then_order(const void *lhs, const void *rhs) {
	const node_t *a = *(node_t *const *)lhs;
	const node_t *b = *(node_t *const *)rhs;
	bool a_master = a->client->flags.master_stack_master;
	bool b_master = b->client->flags.master_stack_master;

	if (a_master != b_master)
		return a_master ? -1 : 1;
	return compare_node_order(lhs, rhs);
}

static int master_count_clamped(const desktop_t *d, int total_nodes) {
	if (!d || total_nodes <= 0)
		return 0;
	if (d->master_stack_count < 0)
		return 0;
	if (d->master_stack_count > total_nodes)
		return total_nodes;
	return d->master_stack_count;
}

static void reconcile_master_membership(desktop_t *d, node_t **nodes, int count) {
	int target = master_count_clamped(d, count);
	int actual = 0;

	// count number of actual masters
	for (int i = 0; i < count; ++i)
		actual += nodes[i]->client->flags.master_stack_master;

	if (actual > target) {
		for (int i = count - 1; i >= 0 && actual > target; --i) {
			client_t *client = nodes[i]->client;

			if (client->flags.master_stack_master) {
				client->flags.master_stack_master = false;
				--actual;
			}
		}
	} else if (actual < target) {
		for (int i = 0; i < count && actual < target; ++i) {
			client_t *client = nodes[i]->client;

			if (!client->flags.master_stack_master) {
				client->flags.master_stack_master = true;
				++actual;
			}
		}
	}
}

static int collect_tiled_nodes(desktop_t *d, node_t ***out_nodes) {
	if (out_nodes)
		*out_nodes = NULL;
	if (!d || !d->root || !out_nodes)
		return 0;

	int count = 0;
	for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root))
		if (n->client && IS_TILED(n->client))
			count++;

	if (count == 0)
		return 0;

	node_t **nodes = calloc((size_t)count, sizeof(*nodes));
	if (!nodes) {
		wlr_log(WLR_ERROR, "some bullshit");
		return 0;
	}

	int index = 0;
	for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root))
		if (n->client && IS_TILED(n->client))
			nodes[index++] = n;

	qsort(nodes, (size_t)count, sizeof(*nodes), compare_node_order);
	reconcile_master_membership(d, nodes, count);
	qsort(nodes, (size_t)count, sizeof(*nodes), compare_master_then_order);

	*out_nodes = nodes;
	return count;
}

static void set_node_geom(node_t *n, struct wlr_box geom, output_t *m, desktop_t *d) {
	if (!n || !n->client)
		return;

	unsigned int bw = effective_border_width(d);
	struct wlr_box r = geom;
	r.x += bw;
	r.y += bw;
	r.width = (r.width > (int)(2 * bw)) ? r.width - 2 * bw : 0;
	r.height = (r.height > (int)(2 * bw)) ? r.height - 2 * bw : 0;

	if (r.width < MIN_WIDTH)
		r.width = MIN_WIDTH;
	if (r.height < MIN_HEIGHT)
		r.height = MIN_HEIGHT;

	if (n->client->state == STATE_PSEUDO_TILED) {
		if (r.width > n->client->floating_rectangle.width)
			r.width = n->client->floating_rectangle.width;
		if (r.height > n->client->floating_rectangle.height)
			r.height = n->client->floating_rectangle.height;

		if (r.width < MIN_WIDTH)
			r.width = MIN_WIDTH;
		if (r.height < MIN_HEIGHT)
			r.height = MIN_HEIGHT;
	}

	if ((int)n->constraints.min_width > MIN_WIDTH && r.width < (int)n->constraints.min_width &&
			geom.width > 0) {
		r.x = geom.x + (geom.width - (int)n->constraints.min_width) / 2;
		r.width = n->constraints.min_width;
	}
	if ((int)n->constraints.min_height > MIN_HEIGHT && r.height < (int)n->constraints.min_height &&
			geom.height > 0) {
		r.y = geom.y + (geom.height - (int)n->constraints.min_height) / 2;
		r.height = n->constraints.min_height;
	}

	n->client->tiled_rectangle = r;
	node_set_pending_rectangle(n, geom);
	n->output = m;
	node_set_dirty(n);
}

static void distribute_area(struct wlr_box area, struct wlr_box *out, int count, int gap,
		bool vertical) {
	if (count <= 0)
		return;

	int span = vertical ? area.height : area.width;
	int actual_gap = gap > 0 ? gap : 0;
	if (count > 1 && (long long)actual_gap * (count - 1) >= span) {
		int room_for_gaps = span - count;
		actual_gap = room_for_gaps > 0 ? room_for_gaps / (count - 1) : 0;
	}

	int usable = span - actual_gap * (count - 1);
	if (usable < 0)
		usable = 0;
	int base = usable / count;
	int remainder = usable % count;
	int cursor = vertical ? area.y : area.x;

	for (int i = 0; i < count; i++) {
		int size = base + (i < remainder ? 1 : 0);
		out[i] = area;
		if (vertical) {
			out[i].y = cursor;
			out[i].height = size;
		} else {
			out[i].x = cursor;
			out[i].width = size;
		}
		cursor += size + actual_gap;
	}
}

static int master_span_size(int span, int gap_count, int gap) {
	int usable = span - gap_count * (gap > 0 ? gap : 0);
	if (usable < 1)
		usable = 1;

	int size = (int)(usable * master_stack_ratio);
	if (size < 1)
		size = 1;
	if (size > usable)
		size = usable;
	return size;
}

static void apply_group(output_t *m, desktop_t *d, node_t **nodes, int count, struct wlr_box area,
		int gap, bool vertical, struct wlr_box *geoms) {
	distribute_area(area, geoms, count, gap, vertical);
	for (int i = 0; i < count; i++)
		set_node_geom(nodes[i], geoms[i], m, d);
}

static bool arrange_center(output_t *m, desktop_t *d, node_t **nodes, int mc, int sc,
		struct wlr_box rect, int gap, struct wlr_box *geoms) {
	if (sc == 0) {
		apply_group(m, d, nodes, mc, rect, gap, true, geoms);
		return true;
	}

	int master_width = master_span_size(rect.width, 2, gap);
	int master_x = rect.x + (rect.width - master_width) / 2;
	struct wlr_box master_area = {
		.x = master_x,
		.y = rect.y,
		.width = master_width,
		.height = rect.height,
	};
	struct wlr_box left_area = {
		.x = rect.x,
		.y = rect.y,
		.width = master_x - rect.x - gap,
		.height = rect.height,
	};
	struct wlr_box right_area = {
		.x = master_x + master_width + gap,
		.y = rect.y,
		.width = rect.x + rect.width - (master_x + master_width + gap),
		.height = rect.height,
	};

	if (left_area.width < 1)
		left_area.width = 1;
	if (right_area.width < 1)
		right_area.width = 1;

	apply_group(m, d, nodes, mc, master_area, gap, true, geoms);

	int left_count = (sc + 1) / 2;
	int right_count = sc / 2;
	int side_capacity = left_count > right_count ? left_count : right_count;
	node_t **side_nodes = calloc((size_t)side_capacity, sizeof(*side_nodes));
	if (!side_nodes) {
		wlr_log(WLR_ERROR, "master-stack side allocation failed");
		return false;
	}

	int left_index = 0;
	for (int i = 0; i < sc; i += 2)
		side_nodes[left_index++] = nodes[mc + i];
	apply_group(m, d, side_nodes, left_count, left_area, gap, true, geoms);

	int right_index = 0;
	for (int i = 1; i < sc; i += 2)
		side_nodes[right_index++] = nodes[mc + i];
	apply_group(m, d, side_nodes, right_count, right_area, gap, true, geoms);

	free(side_nodes);
	return true;
}

void master_stack_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	if (!d || !d->root)
		return;

	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	if (count == 0)
		return;

	int gap = compute_window_gap(d);
	struct wlr_box rect = available;
	rect.x += gap;
	rect.y += gap;
	rect.width -= 2 * gap;
	rect.height -= 2 * gap;
	if (rect.width < 1)
		rect.width = 1;
	if (rect.height < 1)
		rect.height = 1;

	int mc = master_count_clamped(d, count);
	int sc = count - mc;
	struct wlr_box *geoms = calloc((size_t)count, sizeof(*geoms));
	if (!geoms) {
		wlr_log(WLR_ERROR, "master-stack geometry allocation failed");
		free(nodes);
		return;
	}

	if (mc == 0) {
		apply_group(m, d, nodes, count, rect, gap, master_stack_layout == STACK_VERTICAL, geoms);
		free(geoms);
		free(nodes);
		return;
	}

	master_area_orientation_t orientation = master_stack_orientation;
	if (orientation == MASTER_CENTER && sc < 2)
		orientation = MASTER_LEFT;

	if (orientation == MASTER_CENTER) {
		arrange_center(m, d, nodes, mc, sc, rect, gap, geoms);
		free(geoms);
		free(nodes);
		return;
	}

	bool horizontal_split = orientation == MASTER_LEFT || orientation == MASTER_RIGHT;
	bool master_first = orientation == MASTER_LEFT || orientation == MASTER_TOP;

	if (sc == 0) {
		apply_group(m, d, nodes, mc, rect, gap, horizontal_split, geoms);
		free(geoms);
		free(nodes);
		return;
	}

	int span = horizontal_split ? rect.width : rect.height;
	int master_size = master_span_size(span, 1, gap);
	struct wlr_box master_area = rect;
	struct wlr_box stack_area = rect;

	if (horizontal_split) {
		master_area.width = master_size;
		stack_area.width = rect.width - master_size - gap;
		if (master_first) {
			stack_area.x = rect.x + master_size + gap;
		} else {
			master_area.x = rect.x + rect.width - master_size;
		}
	} else {
		master_area.height = master_size;
		stack_area.height = rect.height - master_size - gap;
		if (master_first) {
			stack_area.y = rect.y + master_size + gap;
		} else {
			master_area.y = rect.y + rect.height - master_size;
		}
	}

	if (stack_area.width < 1)
		stack_area.width = 1;
	if (stack_area.height < 1)
		stack_area.height = 1;

	apply_group(m, d, nodes, mc, master_area, gap, horizontal_split, geoms);
	apply_group(m, d, nodes + mc, sc, stack_area, gap, master_stack_layout == STACK_VERTICAL, geoms);

	free(geoms);
	free(nodes);
}

static int find_node_index(node_t **nodes, int count, const node_t *node) {
	for (int i = 0; i < count; i++)
		if (nodes[i] == node)
			return i;
	return -1;
}

bool master_stack_increment(desktop_t *d) {
	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	int mc = master_count_clamped(d, count);
	if (count == 0 || mc >= count) {
		free(nodes);
		return false;
	}

	node_t *target = d->focus && d->focus->client && !d->focus->client->flags.master_stack_master &&
		IS_TILED(d->focus->client) ? d->focus : nodes[mc];
	target->client->flags.master_stack_master = true;
	d->master_stack_count = mc + 1;
	free(nodes);
	return true;
}

bool master_stack_decrement(desktop_t *d) {
	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	int mc = master_count_clamped(d, count);
	if (mc == 0) {
		free(nodes);
		return false;
	}

	node_t *target = d->focus && d->focus->client && d->focus->client->flags.master_stack_master &&
		IS_TILED(d->focus->client) ? d->focus : nodes[mc - 1];
	target->client->flags.master_stack_master = false;
	d->master_stack_count = mc - 1;
	free(nodes);
	return true;
}

bool master_stack_promote(desktop_t *d) {
	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	int mc = master_count_clamped(d, count);
	int focus_index = find_node_index(nodes, count, d ? d->focus : NULL);
	if (focus_index < 0 || nodes[focus_index]->client->flags.master_stack_master) {
		free(nodes);
		return false;
	}

	nodes[focus_index]->client->flags.master_stack_master = true;
	d->master_stack_count = mc + 1;
	free(nodes);
	return true;
}

bool master_stack_demote(desktop_t *d) {
	if (!d || !d->focus || !d->focus->client || !IS_TILED(d->focus->client) ||
		!d->focus->client->flags.master_stack_master)
		return false;

	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	int mc = master_count_clamped(d, count);
	if (mc == 0) {
		free(nodes);
		return false;
	}

	d->focus->client->flags.master_stack_master = false;
	d->master_stack_count = mc - 1;
	free(nodes);
	return true;
}

void master_stack_set_count(desktop_t *d, int count) {
	if (!d)
		return;
	d->master_stack_count = count < 0 ? 0 : count;
}

bool master_stack_adjust_ratio(float delta) {
	float ratio = master_stack_ratio + delta;
	if (ratio < 0.1f)
		ratio = 0.1f;
	if (ratio > 0.9f)
		ratio = 0.9f;
	if (ratio == master_stack_ratio)
		return false;

	master_stack_ratio = ratio;
	return true;
}

void master_stack_set_orientation(master_area_orientation_t orientation) {
	if (orientation < MASTER_LEFT || orientation > MASTER_CENTER)
		return;

	master_stack_orientation = orientation;
	master_stack_layout = orientation == MASTER_TOP ||
		orientation == MASTER_BOTTOM ? STACK_HORIZONTAL : STACK_VERTICAL;
}

void master_stack_flip_orientation(void) {
	switch (master_stack_orientation) {
	case MASTER_LEFT:
		master_stack_orientation = MASTER_RIGHT;
		break;
	case MASTER_RIGHT:
		master_stack_orientation = MASTER_LEFT;
		break;
	case MASTER_TOP:
		master_stack_orientation = MASTER_BOTTOM;
		break;
	case MASTER_BOTTOM:
		master_stack_orientation = MASTER_TOP;
		break;
	case MASTER_CENTER:
		master_stack_orientation = MASTER_CENTER;
		break;
	}
}

void master_stack_cycle_orientation(void) {
	master_stack_set_orientation((master_stack_orientation + 1) % 5);
}

void master_stack_cycle_stack_layout(void) {
	master_stack_layout = (master_stack_layout + 1) % 2;
}

int master_stack_collect(desktop_t *d, node_t ***out_nodes) {
	return collect_tiled_nodes(d, out_nodes);
}

int master_stack_find_index(desktop_t *d, const node_t *n) {
	if (!d || !n)
		return -1;

	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	int index = find_node_index(nodes, count, n);
	free(nodes);
	return index;
}

static node_t **gather_focused(desktop_t *d, int *out_total, int *out_index) {
	node_t **nodes = NULL;
	int count = collect_tiled_nodes(d, &nodes);
	int index = find_node_index(nodes, count, d ? d->focus : NULL);
	if (index < 0) {
		free(nodes);
		return NULL;
	}

	*out_total = count;
	*out_index = index;
	return nodes;
}

static struct wlr_box node_layout_box(const node_t *node) {
	if (node->pending.rectangle.width > 0 && node->pending.rectangle.height > 0)
		return node->pending.rectangle;
	return node->rectangle;
}

static int directional_target(node_t **nodes, int count, int source_index,
		master_stack_direction_t direction, bool wrap) {
	struct wlr_box source = node_layout_box(nodes[source_index]);
	int64_t source_x = (int64_t)source.x * 2 + source.width;
	int64_t source_y = (int64_t)source.y * 2 + source.height;
	int target = -1;
	int64_t best_primary = INT64_MAX;
	int64_t best_secondary = INT64_MAX;

	for (int i = 0; i < count; i++) {
		if (i == source_index)
			continue;
		struct wlr_box candidate = node_layout_box(nodes[i]);
		int64_t x = (int64_t)candidate.x * 2 + candidate.width;
		int64_t y = (int64_t)candidate.y * 2 + candidate.height;
		int64_t dx = x - source_x;
		int64_t dy = y - source_y;
		int64_t primary = 0;
		int64_t secondary = 0;
		bool eligible = false;

		switch (direction) {
		case DIRECTION_WEST:
			eligible = dx < 0;
			primary = -dx;
			secondary = llabs(dy);
			break;
		case DIRECTION_SOUTH:
			eligible = dy > 0;
			primary = dy;
			secondary = llabs(dx);
			break;
		case DIRECTION_NORTH:
			eligible = dy < 0;
			primary = -dy;
			secondary = llabs(dx);
			break;
		case DIRECTION_EAST:
			eligible = dx > 0;
			primary = dx;
			secondary = llabs(dy);
			break;
		}

		if (eligible && (primary < best_primary || (primary == best_primary &&
				secondary < best_secondary))) {
			target = i;
			best_primary = primary;
			best_secondary = secondary;
		}
	}

	if (target >= 0 || !wrap)
		return target;

	best_primary = INT64_MAX;
	best_secondary = INT64_MAX;
	for (int i = 0; i < count; i++) {
		if (i == source_index)
			continue;
		struct wlr_box candidate = node_layout_box(nodes[i]);
		int64_t x = (int64_t)candidate.x * 2 + candidate.width;
		int64_t y = (int64_t)candidate.y * 2 + candidate.height;
		int64_t primary;
		int64_t secondary;

		if (direction == DIRECTION_WEST || direction == DIRECTION_EAST) {
			primary = direction == DIRECTION_WEST ? -x : x;
			secondary = llabs(y - source_y);
		} else {
			primary = direction == DIRECTION_NORTH ? -y : y;
			secondary = llabs(x - source_x);
		}

		if (primary < best_primary || (primary == best_primary && secondary < best_secondary)) {
			target = i;
			best_primary = primary;
			best_secondary = secondary;
		}
	}
	return target;
}

static bool focus_in_direction(desktop_t *d, master_stack_direction_t direction) {
	node_t **nodes = NULL;
	int count = 0;
	int index = -1;
	nodes = gather_focused(d, &count, &index);
	if (!nodes)
		return false;

	int target = directional_target(nodes, count, index, direction, focus_wrapping);
	if (target >= 0)
		d->focus = nodes[target];
	free(nodes);
	return target >= 0;
}

static bool swap_in_direction(output_t *m, desktop_t *d, master_stack_direction_t direction) {
	node_t **nodes = NULL;
	int count = 0;
	int index = -1;
	nodes = gather_focused(d, &count, &index);
	if (!nodes)
		return false;

	int target = directional_target(nodes, count, index, direction, focus_wrapping);
	if (target < 0) {
		free(nodes);
		return false;
	}

	client_t *focused = nodes[index]->client;
	client_t *other = nodes[target]->client;
	uint64_t order = focused->master_stack_order;
	bool is_master = focused->flags.master_stack_master;
	focused->master_stack_order = other->master_stack_order;
	focused->flags.master_stack_master = other->flags.master_stack_master;
	other->master_stack_order = order;
	other->flags.master_stack_master = is_master;

	free(nodes);
	arrange(m, d, true);
	return true;
}

bool master_stack_focus_south(desktop_t *d) {
	return focus_in_direction(d, DIRECTION_SOUTH);
}

bool master_stack_focus_north(desktop_t *d) {
	return focus_in_direction(d, DIRECTION_NORTH);
}

bool master_stack_focus_east(desktop_t *d) {
	return focus_in_direction(d, DIRECTION_EAST);
}

bool master_stack_focus_west(desktop_t *d) {
	return focus_in_direction(d, DIRECTION_WEST);
}

bool master_stack_swap_south(output_t *m, desktop_t *d) {
	return swap_in_direction(m, d, DIRECTION_SOUTH);
}

bool master_stack_swap_north(output_t *m, desktop_t *d) {
	return swap_in_direction(m, d, DIRECTION_NORTH);
}

bool master_stack_swap_east(output_t *m, desktop_t *d) {
	return swap_in_direction(m, d, DIRECTION_EAST);
}

bool master_stack_swap_west(output_t *m, desktop_t *d) {
	return swap_in_direction(m, d, DIRECTION_WEST);
}
