#include "layout.h"
#include "master_stack.h"
#include "scroller.h"
#include "toplevel.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static void tiled_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	int wg = compute_window_gap(d);
	available.x += wg;
	available.y += wg;
	available.width -= wg;
	available.height -= wg;
	apply_layout(m, d, d->root, available, available);
}

static void monocle_arrange(output_t *m, desktop_t *d, struct wlr_box available) {
	available.x += monocle_padding.left;
	available.y += monocle_padding.top;
	available.width -= monocle_padding.left + monocle_padding.right;
	available.height -= monocle_padding.top + monocle_padding.bottom;
	if (!gapless_monocle) {
		int wg = compute_window_gap(d);
		available.x += wg;
		available.y += wg;
		available.width -= 2 * wg;
		available.height -= 2 * wg;
	}
	apply_layout(m, d, d->root, available, available);
}

static void monocle_on_focus(output_t *m, desktop_t *d, node_t *n) {
	(void)m;
	if (!d->root)
		return;

	FOR_EACH_LEAF(node, d->root) {
		if (!node->client)
			continue;

		bool should_show = (node == n);
		node->client->flags.shown = should_show;
		struct wlr_scene_tree *st = client_get_scene_tree(node->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, should_show);
	}
}

static void scroller_on_focus(output_t *m, desktop_t *d, node_t *n) {
	if (!d)
		return;

	if (!d->scroller_state)
		d->scroller_state = scroller_create();

	scroller_state_t *s = d->scroller_state;
	if (!s)
		return;

	if (s->column_count == 0 && d->root) {
		FOR_EACH_LEAF(leaf, d->root)
			if (leaf->client)
				leaf->client->flags.shown = true;
	} else {
		for (int i = 0; i < s->column_count; i++) {
			for (int j = 0; j < s->columns[i].tile_count; j++) {
				client_t *c = s->columns[i].tiles[j].client;
				if (c)
					c->flags.shown = true;
			}
		}
	}

	if (n != NULL && n->client->toplevel && n->client->toplevel->configured) {
		wlr_log(WLR_DEBUG, "scroller_on_focus: triggering arrange");
		arrange(m, d, true);
	} else {
		wlr_log(WLR_DEBUG, "scroller_on_focus: skipping arrange (initial map)");
	}
}

static int tiled_collect(desktop_t *d, node_t ***out_nodes) {
	return collect_tiled_leaves(d, out_nodes);
}

static int scroller_collect_fn(desktop_t *d, node_t ***out_nodes) {
	return scroller_collect(d, out_nodes);
}

static bool scroller_focus(desktop_t *d, direction_t dir) {
	switch (dir) {
	case DIR_WEST:
		return scroller_focus_prev(d);
	case DIR_EAST:
		return scroller_focus_next(d);
	case DIR_NORTH:
		return scroller_focus_up(d);
	case DIR_SOUTH:
		return scroller_focus_down(d);
	}
	return false;
}

static bool tiled_focus(desktop_t *d, direction_t dir) {
	node_t *n = find_fence(d->focus, dir);
	if (n != NULL) {
		n = second_extrema(n);
		if (n != NULL)
			return focus_node(mon, d, n);
	} else if (focus_wrapping && d->root) {
		node_t *w = second_extrema(d->root);
		if (w && w != d->focus)
			return focus_node(mon, d, w);
	}
	return false;
}

static bool tiled_swap(output_t *m, desktop_t *d, direction_t dir) {
	node_t *n = find_fence(d->focus, dir);
	if (n != NULL) {
		n = first_extrema(n);
		if (n != NULL) {
			swap_nodes(m, d, d->focus, m, d, n);
			return true;
		}
	}
	return false;
}

static const layout_impl_t tiled_impl = {
	.name = "tiled",
	.arrange = tiled_arrange,
	.on_focus = NULL,
	.focus = tiled_focus,
	.swap = tiled_swap,
	.init_client = NULL,
	.collect = tiled_collect,
	.single_visible = false,
	.has_directional_nav = false,
};

static const layout_impl_t monocle_impl = {
	.name = "monocle",
	.arrange = monocle_arrange,
	.on_focus = monocle_on_focus,
	.focus = tiled_focus,
	.swap = NULL,
	.init_client = NULL,
	.collect = tiled_collect,
	.single_visible = true,
	.has_directional_nav = false,
};

static const layout_impl_t scroller_impl = {
	.name = "scroller",
	.arrange = scroller_arrange,
	.on_focus = scroller_on_focus,
	.focus = scroller_focus,
	.swap = scroller_swap,
	.init_client = NULL,
	.collect = scroller_collect_fn,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t master_stack_impl = {
	.name = "master_stack",
	.arrange = master_stack_arrange,
	.on_focus = NULL,
	.focus = master_stack_focus,
	.swap = master_stack_swap,
	.init_client = NULL,
	.collect = master_stack_collect,
	.single_visible = false,
	.has_directional_nav = true,
};

static const layout_impl_t *registry[] = {
	[LAYOUT_TILED] = &tiled_impl,
	[LAYOUT_MONOCLE] = &monocle_impl,
	[LAYOUT_SCROLLER] = &scroller_impl,
	[LAYOUT_MASTER_STACK] = &master_stack_impl,
};

const layout_impl_t *layout_get_impl(layout_t layout) {
	if ((size_t)layout >= sizeof(registry) / sizeof(registry[0]))
		return NULL;
	return registry[layout];
}

void layout_set(desktop_t *d, layout_t new_layout) {
	if (!d)
		return;

	// destroy scroller state when leaving scroller layout
	if (d->layout == LAYOUT_SCROLLER && new_layout != LAYOUT_SCROLLER) {
		scroller_destroy(d->scroller_state);
		d->scroller_state = NULL;
	}

	d->layout = new_layout;
}

void layout_toggle(desktop_t *d, layout_t target) {
	if (!d)
		return;
	if (d->layout == target) {
		layout_set(d, d->user_layout);
	} else {
		d->user_layout = d->layout;
		layout_set(d, target);
	}
}

void layout_cycle(output_t *m, desktop_t *d, int direction) {
	if (!d)
		return;

	int num_layouts = sizeof(registry) / sizeof(registry[0]);
	int current = (int)d->layout;
	int next = (current + direction) % num_layouts;
	if (next < 0)
		next += num_layouts;

	d->user_layout = d->layout;
	layout_set(d, (layout_t)next);
	arrange(m, d, true);
	if (d->focus)
		focus_node(m, d, d->focus);
}
