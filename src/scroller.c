#include "output.h"
#include "scroller.h"
#include "toplevel.h"
#include "tree.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

float scroller_default_proportion = 0.5f;
float scroller_default_proportion_single = 1.0f;
int scroller_structs = 0;
bool scroller_focus_center = false;
bool scroller_prefer_center = true;
bool scroller_prefer_overspread = false;
bool scroller_ignore_proportion_single = false;
bool edge_scroller_pointer_focus = true;

float *scroller_proportion_preset = NULL;
int scroller_proportion_preset_count = 0;

static int max_i(int a, int b) {
	return a > b ? a : b;
}

static double max_d(double a, double b) {
	return a > b ? a : b;
}

static scroller_tile_t *tile_grow(scroller_column_t *col) {
	if (col->tile_count >= col->capacity) {
		int new_cap = col->capacity ? col->capacity * 2 : 4;
		scroller_tile_t *t = realloc(col->tiles, (size_t)new_cap * sizeof(*t));
		if (!t)
			return NULL;
		col->tiles = t;
		col->capacity = new_cap;
	}
	return &col->tiles[col->tile_count];
}

static scroller_column_t *column_grow(scroller_state_t *s) {
	if (s->column_count >= s->capacity) {
		int new_cap = s->capacity ? s->capacity * 2 : 4;
		scroller_column_t *c = realloc(s->columns, (size_t)new_cap * sizeof(*c));
		if (!c)
			return NULL;
		s->columns = c;
		s->capacity = new_cap;
	}
	return &s->columns[s->column_count];
}

scroller_state_t *scroller_create(void) {
	scroller_state_t *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->view_offset = 0.0;
	return s;
}

void scroller_destroy(scroller_state_t *s) {
	if (!s)
		return;
	for (int i = 0; i < s->column_count; i++) {
		free(s->columns[i].tiles);
	}
	free(s->columns);
	free(s);
}

static double resolve_column_width(scroller_state_t *s, int col_idx, double gap, double area_w) {
	scroller_column_t *col = &s->columns[col_idx];
	switch (col->width.type) {
	case SCROLLER_WIDTH_PROPORTION:
		return max_d(1.0, (area_w - gap) * col->width.value - gap);
	case SCROLLER_WIDTH_FIXED:
		return max_d(1.0, col->width.value);
	}
	return 1.0;
}

static bool find_tile(scroller_state_t *s, client_t *c, int *out_col_idx, int *out_tile_idx) {
	for (int i = 0; i < s->column_count; i++) {
		for (int j = 0; j < s->columns[i].tile_count; j++) {
			if (s->columns[i].tiles[j].client == c) {
				if (out_col_idx)
					*out_col_idx = i;
				if (out_tile_idx)
					*out_tile_idx = j;
				return true;
			}
		}
	}
	return false;
}

static int create_column(scroller_state_t *s, client_t *client, bool activate) {
	scroller_column_t *col = column_grow(s);
	if (!col)
		return -1;

	memset(col, 0, sizeof(*col));
	col->width.type = SCROLLER_WIDTH_PROPORTION;
	col->width.value = scroller_default_proportion;
	col->active_tile_idx = 0;

	scroller_tile_t *t = tile_grow(col);
	if (!t)
		return -1;
	memset(t, 0, sizeof(*t));
	t->client = client;
	t->height.type = SCROLLER_HEIGHT_AUTO;
	t->height.value = 1.0;

	col->tile_count = 1;

	int idx = s->column_count;
	s->column_count = idx + 1;

	if (activate) {
		s->active_column_idx = idx;
	}

	return idx;
}

bool scroller_add_tile(scroller_state_t *s, client_t *client, bool activate) {
	if (!s || !client)
		return false;

	// if client is already tracked, do nothing
	if (find_tile(s, client, NULL, NULL))
		return true;

	int num_tiled = 0;
	for (int i = 0; i < s->column_count; i++)
		num_tiled += s->columns[i].tile_count;

	// create first column
	if (num_tiled == 0)
		return create_column(s, client, activate) >= 0;

	int insert_col = s->active_column_idx + 1;
	if (insert_col > s->column_count)
		insert_col = s->column_count;

	// shift columns right to make room at insert_col
	if (!column_grow(s))
		return false;

	memmove(&s->columns[insert_col + 1], &s->columns[insert_col],
		(size_t)(s->column_count - insert_col) * sizeof(scroller_column_t));
	s->column_count++;

	scroller_column_t *new_col = &s->columns[insert_col];
	memset(new_col, 0, sizeof(*new_col));
	new_col->width.type = SCROLLER_WIDTH_PROPORTION;
	new_col->width.value = scroller_default_proportion;
	new_col->active_tile_idx = 0;
	new_col->tiles = NULL;
	new_col->tile_count = 0;
	new_col->capacity = 0;

	scroller_tile_t *t = tile_grow(new_col);
	if (!t)
		return false;
	memset(t, 0, sizeof(*t));
	t->client = client;
	t->height.type = SCROLLER_HEIGHT_AUTO;
	t->height.value = 1.0;
	new_col->tile_count = 1;

	// adjust active_column_idx
	if (activate)
		s->active_column_idx = insert_col;
	else if (s->active_column_idx >= insert_col)
		s->active_column_idx++;

	return true;
}

bool scroller_add_tile_to_column(scroller_state_t *s, client_t *client, int col_idx,
		bool activate) {
	if (!s || !client)
		return false;
	if (col_idx < 0 || col_idx >= s->column_count)
		return false;

	if (find_tile(s, client, NULL, NULL))
		return true;

	scroller_column_t *col = &s->columns[col_idx];

	scroller_tile_t *t = tile_grow(col);
	if (!t)
		return false;
	memset(t, 0, sizeof(*t));
	t->client = client;
	t->height.type = SCROLLER_HEIGHT_AUTO;
	t->height.value = 1.0;

	col->tile_count++;
	if (activate) {
		col->active_tile_idx = col->tile_count - 1;
		s->active_column_idx = col_idx;
	}

	return true;
}

void scroller_remove_tile(scroller_state_t *s, client_t *client, struct output_t *m) {
	(void)m;
	if (!s || !client)
		return;

	int col_idx, tile_idx;
	if (!find_tile(s, client, &col_idx, &tile_idx))
		return;

	scroller_column_t *col = &s->columns[col_idx];

	// shift tiles left
	memmove(&col->tiles[tile_idx], &col->tiles[tile_idx + 1],
		(size_t)(col->tile_count - tile_idx - 1) * sizeof(scroller_tile_t));
	col->tile_count--;

	// fix active_tile_idx
	if (col->tile_count == 0) {
		free(col->tiles);
		col->tiles = NULL;
		col->capacity = 0;

		memmove(&s->columns[col_idx], &s->columns[col_idx + 1],
			(size_t)(s->column_count - col_idx - 1) * sizeof(scroller_column_t));
		s->column_count--;

		// fix active_column_idx
		if (s->column_count == 0) {
			s->active_column_idx = 0;
			return;
		}
		if (s->active_column_idx >= s->column_count)
			s->active_column_idx = s->column_count - 1;
		if (col_idx <= s->active_column_idx && s->active_column_idx > 0)
			s->active_column_idx--;
	} else {
		if (tile_idx < col->active_tile_idx)
			col->active_tile_idx--;
		else if (tile_idx == col->active_tile_idx && col->active_tile_idx >= col->tile_count)
			col->active_tile_idx = col->tile_count - 1;
	}
}

static void populate_from_tree(scroller_state_t *s, desktop_t *d) {
	if (!d->root)
		return;

	int num_tiled = 0;
	for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root))
		if (n->client && scroller_is_tiled(n->client))
			num_tiled++;

	if (num_tiled == 0)
		return;

	for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root)) {
		if (!n->client || !scroller_is_tiled(n->client))
			continue;
		scroller_add_tile(s, n->client, false);
	}

	// activate the currently focused window
	if (d->focus && d->focus->client) {
		int col, tile;
		if (find_tile(s, d->focus->client, &col, &tile)) {
			s->active_column_idx = col;
			s->columns[col].active_tile_idx = tile;
		}
	}
}

void scroller_arrange(struct output_t *m, desktop_t *d, struct wlr_box available) {
	if (!d)
		return;

	// lazily create scroller state
	if (!d->scroller_state)
		d->scroller_state = scroller_create();

	scroller_state_t *s = d->scroller_state;
	if (!s)
		return;

	// populate from BSP tree if scroller state is empty but toplevels exist
	if (s->column_count == 0 && d->root)
		populate_from_tree(s, d);

	if (s->column_count == 0)
		return;

	int gap = compute_window_gap(d);
	if (gap < 0)
		gap = 0;
	double area_w = max_d(1.0, (double)available.width);
	double area_h = max_d(1.0, (double)available.height);

	// resolve column widths
	for (int i = 0; i < s->column_count; i++)
		s->columns[i].resolved_width = resolve_column_width(s, i, (double)gap, area_w);

	// compute tile heights and positions (world space, relative to area origin)
	for (int i = 0; i < s->column_count; i++) {
		scroller_column_t *col = &s->columns[i];
		if (col->tile_count == 0)
			continue;

		double col_avail_h = area_h - (double)gap * 2.0;
		double total_weight = 0.0;
		double fixed_h = 0.0;

		for (int j = 0; j < col->tile_count; j++) {
			if (col->tiles[j].height.type == SCROLLER_HEIGHT_FIXED) {
				fixed_h += col->tiles[j].height.value;
			} else {
				total_weight += col->tiles[j].height.value;
			}
		}

		double gaps_total = (double)gap * (col->tile_count + 1);
		double auto_avail = col_avail_h - gaps_total - fixed_h;

		// distribute auto height
		double *tile_heights = malloc((size_t)col->tile_count * sizeof(*tile_heights));
		if (!tile_heights)
			return;

		double total_weight_safe = total_weight > 0.0 ? total_weight : 1.0;
		for (int j = 0; j < col->tile_count; j++) {
			if (col->tiles[j].height.type == SCROLLER_HEIGHT_FIXED) {
				tile_heights[j] = col->tiles[j].height.value;
			} else {
				double weight = col->tiles[j].height.value;
				double h = auto_avail * (weight / total_weight_safe);
				tile_heights[j] = max_d(1.0, h);
			}
		}

		// compute y positions
		double y = (double)gap;
		for (int j = 0; j < col->tile_count; j++) {
			col->tiles[j].rect.x = 0; // local X within column (centering not yet)
			col->tiles[j].rect.y = (int)y;
			col->tiles[j].rect.width = (int)col->resolved_width;
			col->tiles[j].rect.height = max_i(1, (int)tile_heights[j]);
			y += tile_heights[j] + (double)gap;
		}

		free(tile_heights);
	}

	// compute world-space column X positions
	double *col_xs = malloc((size_t)(s->column_count + 1) * sizeof(*col_xs));
	if (!col_xs)
		return;

	double wx = 0.0;
	for (int i = 0; i < s->column_count; i++) {
		col_xs[i] = wx;
		wx += s->columns[i].resolved_width + (double)gap;
	}
	col_xs[s->column_count] = wx; // sentinel

	// view position (camera in world space)
	double active_col_x = col_xs[s->active_column_idx];
	double view_pos = active_col_x + s->view_offset;

	// set pending rectangles on tree nodes.
	unsigned int bw = (unsigned int)effective_border_width(d);

	for (int i = 0; i < s->column_count; i++) {
		scroller_column_t *col = &s->columns[i];
		double screen_x = (double)available.x + col_xs[i] - view_pos;

		for (int j = 0; j < col->tile_count; j++) {
			client_t *c = col->tiles[j].client;
			if (!c || !c->toplevel || !c->toplevel->node)
				continue;

			node_t *node = c->toplevel->node;

			struct wlr_box outer = {
				.x = (int)(screen_x + 0.5),
				.y = (int)((double)available.y + col->tiles[j].rect.y + 0.5),
				.width = max_i(1, (int)(col->resolved_width + 0.5)),
				.height = max_i(1, col->tiles[j].rect.height),
			};

			struct wlr_box inner = apply_bleed(outer, (int)bw, 0);
			if (inner.width < SCROLLER_MIN_WIDTH)
				inner.width = SCROLLER_MIN_WIDTH;
			if (inner.width > outer.width)
				inner.width = outer.width;
			if (inner.height < SCROLLER_MIN_HEIGHT)
				inner.height = SCROLLER_MIN_HEIGHT;
			if (inner.height > outer.height)
				inner.height = outer.height;

			c->tiled_rectangle = inner;
			c->arranged_rectangle = inner;
			node_set_pending_rectangle(node, outer);
			node->output = m;
			node_set_dirty(node);
		}
	}

	free(col_xs);
}

static void apply_active_focus(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0) {
		d->focus = NULL;
		return;
	}

	scroller_column_t *col = &s->columns[s->active_column_idx];
	if (col->tile_count == 0) {
		d->focus = NULL;
		return;
	}

	client_t *c = col->tiles[col->active_tile_idx].client;
	if (c && c->toplevel && c->toplevel->node) {
		node_t *target = c->toplevel->node;
		d->focus = target;
		if (d->output)
			focus_node(d->output, d, target);
	}
}

bool scroller_focus_next(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;
	if (s->active_column_idx >= s->column_count - 1) {
		if (!focus_wrapping)
			return false;
		s->active_column_idx = 0;
	} else {
		s->active_column_idx++;
	}
	s->view_offset = 0.0; // reset scroll for now
	apply_active_focus(d);
	return true;
}

bool scroller_focus_prev(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;
	if (s->active_column_idx == 0) {
		if (!focus_wrapping)
			return false;
		s->active_column_idx = s->column_count - 1;
	} else {
		s->active_column_idx--;
	}
	s->view_offset = 0.0;
	apply_active_focus(d);
	return true;
}

bool scroller_focus_down(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	scroller_column_t *col = &s->columns[s->active_column_idx];
	if (col->tile_count == 0)
		return false;
	if (col->active_tile_idx >= col->tile_count - 1)
		return false;

	col->active_tile_idx++;
	apply_active_focus(d);
	return true;
}

bool scroller_focus_up(desktop_t *d) {
	scroller_state_t *s = d->scroller_state;
	if (!s || s->column_count == 0)
		return false;

	scroller_column_t *col = &s->columns[s->active_column_idx];
	if (col->tile_count == 0)
		return false;
	if (col->active_tile_idx == 0)
		return false;

	col->active_tile_idx--;
	apply_active_focus(d);
	return true;
}

void scroller_center_window(desktop_t *d, client_t *client) {
	scroller_state_t *s = d->scroller_state;
	if (!s || !client)
		return;

	int col_idx, tile_idx;
	if (!find_tile(s, client, &col_idx, &tile_idx))
		return;

	s->active_column_idx = col_idx;
	s->columns[col_idx].active_tile_idx = tile_idx;
	s->view_offset = 0.0;
	apply_active_focus(d);
}

bool scroller_is_tiled(const client_t *c) {
	if (!c)
		return false;
	return c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED;
}

void scroller_apply_client_rules(client_t *c, float rule_proportion, float rule_proportion_single) {
	if (!c)
		return;
	(void)c;
	(void)rule_proportion;
	(void)rule_proportion_single;
	// TODO: store per-client proportion overrides if needed later.
}

void scroller_resize_width(client_t *client, float delta) {
	if (!client)
		return;
	// Find the client's column in the scroller state (needs desktop_t).
	// For now, stub – proportion is set via config.
	// This will be wired through IPC / keybindings properly.
	(void)delta;
	wlr_log(WLR_DEBUG, "scroller_resize_width: stub (client=%p delta=%.2f)", (void *)client, delta);
}

void scroller_resize_stack(client_t *client, float delta) {
	(void)client;
	(void)delta;
	wlr_log(WLR_DEBUG, "scroller_resize_stack: stub");
}

void scroller_set_proportion(client_t *client, float proportion) {
	if (!client)
		return;
	// TODO: find column and update its width.
	(void)proportion;
	wlr_log(WLR_DEBUG, "scroller_set_proportion: stub (client=%p prop=%.2f)", (void *)client,
		proportion);
}

void scroller_cycle_proportion_preset(client_t *client) {
	if (!client || !scroller_proportion_preset || scroller_proportion_preset_count == 0)
		return;
	// TODO: find column, cycle through presets.
	wlr_log(WLR_DEBUG, "scroller_cycle_proportion_preset: stub");
}

int scroller_collect(desktop_t *d, node_t ***out_nodes) {
	scroller_state_t *s = d ? d->scroller_state : NULL;
	if (!s || s->column_count == 0) {
		if (out_nodes)
			*out_nodes = NULL;
		return 0;
	}

	// count total tiles
	int total = 0;
	for (int i = 0; i < s->column_count; i++)
		total += s->columns[i].tile_count;

	if (total == 0) {
		if (out_nodes)
			*out_nodes = NULL;
		return 0;
	}

	node_t **nodes = calloc((size_t)total, sizeof(*nodes));
	if (!nodes) {
		if (out_nodes)
			*out_nodes = NULL;
		return 0;
	}

	int idx = 0;
	for (int i = 0; i < s->column_count; i++) {
		for (int j = 0; j < s->columns[i].tile_count; j++) {
			client_t *c = s->columns[i].tiles[j].client;
			if (c && c->toplevel)
				nodes[idx++] = c->toplevel->node;
		}
	}

	*out_nodes = nodes;
	return idx;
}
