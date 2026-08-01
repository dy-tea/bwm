#include "ipc.h"
#include "once.h"
#include "output.h"
#include "scratchpad.h"
#include "server.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

typedef struct scratchpad_entry_t {
	struct wl_list link;
	node_t *node;
	client_state_t saved_state;
	struct wlr_box saved_tiled_rect;
} scratchpad_entry_t;

static struct wl_list scratchpad_list;

static scratchpad_entry_t *scratchpad_find_entry(node_t *n) {
	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link)
		if (entry->node == n)
			return entry;

	return NULL;
}

bool scratchpad_has(node_t *n) {
	return n != NULL && n->scratchpad;
}

void scratchpad_add(node_t *n) {
	if (!n || !n->client || n->scratchpad)
		return;

	wlr_log(WLR_INFO, "scratchpad_add: node=%u app_id=%s state=%d", n->id,
		n->client->app_id[0] ? n->client->app_id : "(none)", n->client->state);

	desktop_t *d = n->desktop;
	client_state_t saved_state = n->client->state;
	struct wlr_box saved_tiled = n->client->tiled_rectangle;

	// compute a floating rectangle for use if shown as floating
	output_t *out = n->output;
	struct wlr_box floating_rect = {0};
	if (out) {
		floating_rect.width = out->usable_area.width * 0.5;
		floating_rect.height = out->usable_area.height * 0.5;
		floating_rect.x = out->usable_area.x + (out->usable_area.width - floating_rect.width) / 2;
		floating_rect.y = out->usable_area.y + (out->usable_area.height - floating_rect.height) / 2;
	} else {
		floating_rect.width = 800;
		floating_rect.height = 600;
		floating_rect.x = 100;
		floating_rect.y = 100;
	}

	// detach from tree
	if (d && (n->parent != NULL || d->root == n)) {
		remove_node(d, n);

		n->parent = NULL;
		n->first_child = NULL;
		n->second_child = NULL;

		if (d->focus == n || d->focus == NULL) {
			if (d->root) {
				d->focus = first_extrema(d->root);
				if (d->focus && d->focus->client)
					focus_node(d->output, d, d->focus);
			} else {
				d->focus = NULL;
			}
		}

		if (d->output && d->root)
			arrange(d->output, d, true);
	}

	// force floating when hidden
	n->client->state = STATE_FLOATING;
	n->hidden = true;
	n->client->floating_rectangle = floating_rect;

	// reparent to float_tree for the hidden position tracking
	struct wlr_scene_tree *st = client_get_scene_tree(n->client);
	if (st) {
		wlr_scene_node_reparent(&st->node, server.float_tree);
		wlr_scene_node_set_position(&st->node, floating_rect.x, floating_rect.y);
	}

	// disable scene
	n->client->flags.shown = false;
	if (st)
		wlr_scene_node_set_enabled(&st->node, false);

	n->desktop = NULL;
	n->output = NULL;
	n->scratchpad = true;

	scratchpad_entry_t *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wlr_log(WLR_ERROR, "allocation failed");
		return;
	}
	entry->node = n;
	entry->saved_state = saved_state;
	entry->saved_tiled_rect = saved_tiled;
	wl_list_insert(scratchpad_list.prev, &entry->link);

	transaction_commit_dirty();
	wlr_log(WLR_INFO, "scratchpad_add: done, node=%u saved_state=%d", n->id, saved_state);
}

void scratchpad_remove(node_t *n) {
	if (!n || !n->scratchpad)
		return;

	scratchpad_entry_t *entry = scratchpad_find_entry(n);
	if (entry) {
		wl_list_remove(&entry->link);
		free(entry);
	}

	n->scratchpad = false;
	wlr_log(WLR_INFO, "scratchpad_remove: node=%u removed from scratchpad", n->id);
}

void scratchpad_show(node_t *n) {
	if (!n || !n->client || !n->scratchpad)
		return;

	scratchpad_entry_t *entry = scratchpad_find_entry(n);
	if (!entry)
		return;

	client_state_t saved_state = entry->saved_state;

	output_t *out = server.focused_output;
	if (!out)
		return;

	desktop_t *d = out->desk;
	if (!d)
		d = out->desk_head;
	if (!d) {
		wlr_log(WLR_ERROR, "scratchpad_show: no desktop on output");
		return;
	}

	wlr_log(WLR_INFO, "scratchpad_show: node=%u on output=%s desktop=%s restoring state=%d", n->id,
		out->name, d->name, saved_state);

	n->desktop = d;
	n->output = out;

	struct wlr_scene_tree *st = client_get_scene_tree(n->client);

	if (saved_state == STATE_TILED || saved_state == STATE_PSEUDO_TILED) {
		// restore as tiled
		n->client->state = saved_state;
		n->hidden = false;

		if (st)
			wlr_scene_node_reparent(&st->node, server.tile_tree);

		n->client->flags.shown = true;
		if (st)
			wlr_scene_node_set_enabled(&st->node, true);

		node_t *ref = find_public(d);
		insert_node(d, n, ref);

		d->focus = n;
		focus_node(out, d, n);
		arrange(out, d, true);

		ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
			n->client && n->client->app_id[0] ? n->client->app_id : "?",
			n->client && n->client->title[0] ? n->client->title : "?", n->id);
	} else {
		// restore as floating
		n->client->state = STATE_FLOATING;
		n->hidden = true;

		if (st) {
			wlr_scene_node_reparent(&st->node, server.float_tree);
			wlr_scene_node_set_position(&st->node, n->client->floating_rectangle.x,
				n->client->floating_rectangle.y);
		}

		n->client->flags.shown = true;
		if (st)
			wlr_scene_node_set_enabled(&st->node, true);

		d->focus = n;
		focus_node(out, d, n);

		ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
			n->client && n->client->app_id[0] ? n->client->app_id : "?",
			n->client && n->client->title[0] ? n->client->title : "?", n->id);
	}

	transaction_commit_dirty();
	wlr_log(WLR_INFO, "scratchpad_show: done, node=%u shown on %s", n->id, d->name);
}

void scratchpad_hide(node_t *n) {
	if (!n || !n->client)
		return;
	if (!n->scratchpad && n->desktop == NULL)
		return;

	wlr_log(WLR_INFO, "scratchpad_hide: node=%u scratchpad=%d", n->id, n->scratchpad);

	// save current position and state
	struct wlr_scene_tree *st = client_get_scene_tree(n->client);
	if (st) {
		n->client->floating_rectangle.x = st->node.x;
		n->client->floating_rectangle.y = st->node.y;
	}

	// if first time hiding, save current state
	if (!n->scratchpad) {
		desktop_t *d = n->desktop;
		if (d && (n->parent != NULL || d->root == n)) {
			remove_node(d, n);
			n->parent = NULL;
			n->first_child = NULL;
			n->second_child = NULL;

			if (d->focus == n || d->focus == NULL) {
				if (d->root) {
					d->focus = first_extrema(d->root);
					if (d->focus && d->focus->client)
						focus_node(d->output, d, d->focus);
				} else {
					d->focus = NULL;
				}
			}

			if (d->output && d->root)
				arrange(d->output, d, true);
		}

		// force floating for hidden state
		n->client->state = STATE_FLOATING;
		n->hidden = true;

		// add to scratchpad list with saved state
		scratchpad_entry_t *entry = calloc(1, sizeof(*entry));
		if (!entry) {
			wlr_log(WLR_ERROR, "allocation failed");
			return;
		}
		entry->node = n;
		entry->saved_state = STATE_FLOATING;
		entry->saved_tiled_rect = n->client->tiled_rectangle;
		wl_list_insert(scratchpad_list.prev, &entry->link);
		n->scratchpad = true;
	} else {
		scratchpad_entry_t *entry = scratchpad_find_entry(n);
		if (entry)
			entry->saved_state = STATE_FLOATING;

		// detach from tree if still in it
		desktop_t *d = n->desktop;
		output_t *out = n->output;
		if (d && (n->parent != NULL || d->root == n)) {
			remove_node(d, n);
			n->parent = NULL;
			n->first_child = NULL;
			n->second_child = NULL;

			if (d->focus == n || d->focus == NULL) {
				if (d->root) {
					d->focus = first_extrema(d->root);
					if (d->focus && d->focus->client)
						focus_node(out, d, d->focus);
				} else {
					d->focus = NULL;
				}
			}

			if (out && d->root)
				arrange(out, d, true);
		}

		n->client->state = STATE_FLOATING;
		n->hidden = true;
	}

	// disable scene
	n->client->flags.shown = false;
	if (st)
		wlr_scene_node_set_enabled(&st->node, false);

	n->desktop = NULL;
	n->output = NULL;

	transaction_commit_dirty();
	wlr_log(WLR_INFO, "scratchpad_hide: done, node=%u hidden", n->id);
}

void scratchpad_toggle(node_t *n) {
	if (!n || !n->client)
		return;

	wlr_log(WLR_INFO, "scratchpad_toggle: node=%u scratchpad=%d desktop=%p", n->id, n->scratchpad,
		(void *)n->desktop);

	if (n->scratchpad && n->desktop == NULL) {
		scratchpad_show(n);
	} else if (n->desktop == server.focused_output->desk) {
		scratchpad_hide(n);
	} else if (n->desktop != NULL) {
		scratchpad_hide(n);
		scratchpad_show(n);
	} else {
		scratchpad_add(n);
	}
}

void scratchpad_toggle_auto(void) {
	output_t *out = server.focused_output;
	if (!out || !out->desk)
		return;

	desktop_t *current_desk = out->desk;

	node_t *focused = current_desk->focus;
	if (focused && focused->client && focused->scratchpad && focused->desktop == current_desk) {
		scratchpad_hide(focused);
		transaction_commit_dirty();
		return;
	}

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node && entry->node->client && entry->node->desktop == current_desk) {
			scratchpad_hide(entry->node);
			transaction_commit_dirty();
			return;
		}
	}

	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node && entry->node->client && entry->node->desktop == NULL) {
			scratchpad_show(entry->node);
			transaction_commit_dirty();
			return;
		}
	}

	wlr_log(WLR_DEBUG, "scratchpad_toggle_auto: no scratchpad entries to show");
}

node_t *scratchpad_find_by_app_id(const char *app_id) {
	if (!app_id)
		return NULL;

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node && entry->node->client && strcmp(entry->node->client->app_id, app_id) == 0)
			return entry->node;
	}
	return NULL;
}

node_t *scratchpad_find_by_title(const char *title) {
	if (!title)
		return NULL;

	scratchpad_entry_t *entry;
	wl_list_for_each(entry, &scratchpad_list, link) {
		if (entry->node && entry->node->client && strcmp(entry->node->client->title, title) == 0)
			return entry->node;
	}
	return NULL;
}

void scratchpad_init(void) {
	ONCE();
	wl_list_init(&scratchpad_list);
	wlr_log(WLR_INFO, "Scratchpad initialized");
}

void scratchpad_fini(void) {
	ONCE();
	scratchpad_entry_t *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, &scratchpad_list, link) {
		if (entry->node)
			entry->node->scratchpad = false;
		wl_list_remove(&entry->link);
		free(entry);
	}
	wlr_log(WLR_INFO, "Scratchpad finalized");
}
