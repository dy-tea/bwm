#include "animation.h"
#include "cursor.h"
#include "effects.h"
#include "idle.h"
#include "once.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

// transaction state
static struct {
	struct transaction_t *pending_transaction;
	struct transaction_t *queued_transaction;
	node_t **dirty_nodes;
	size_t dirty_count;
	size_t dirty_capacity;
} txn_state = {0};

static void transaction_commit(struct transaction_t *txn);
static void transaction_commit_pending(void);
static void _transaction_commit_dirty(bool server_request);

static struct transaction_t *transaction_create(void) {
	struct transaction_t *txn = calloc(1, sizeof(*txn));
	if (!txn) {
		wlr_log(WLR_ERROR, "Failed to allocate transaction");
		return NULL;
	}
	wl_list_init(&txn->instructions);
	clock_gettime(CLOCK_MONOTONIC, &txn->commit_time);
	txn->num_waiting = 0;
	txn->num_configures = 0;
	txn->timer = NULL;
	return txn;
}

static void transaction_destroy(transaction_t *txn) {
	if (!txn)
		return;

	bool is_pending = (txn == txn_state.pending_transaction);

	// free all instructions
	transaction_inst_t *instruction, *tmp;
	wl_list_for_each_safe(instruction, tmp, &txn->instructions, link) {
		node_t *node = instruction->node;

		wlr_log(WLR_DEBUG, "transaction_destroy: node %u ntxnrefs=%zu destroying=%d", node->id,
			(size_t)node->ntxnrefs, node->destroying);

		if (is_pending)
			node->pending_inst = NULL;

		node->ntxnrefs--;

		if (node->instruction == instruction)
			node->instruction = NULL;

		if (node->destroying && node->ntxnrefs == 0) {
			wlr_log(WLR_DEBUG, "transaction_destroy: freeing destroying node %u", node->id);
			free_node(node);
		}

		wl_list_remove(&instruction->link);
		free(instruction);
	}

	if (txn->timer)
		wl_event_source_remove(txn->timer);

	free(txn);
}

static void copy_node_state(node_t *node, transaction_inst_t *instruction) {
	if (!node || !instruction)
		return;

	// copy pending state to instruction
	instruction->rectangle = node->pending.rectangle;
	instruction->split_ratio = node->pending.split_ratio;
	instruction->split_type = node->pending.split_type;
	instruction->hidden = node->pending.hidden;

	if (node->client) {
		instruction->previous_tiled_rectangle = node->client->committed_tiled_rectangle;
		instruction->state = node->client->state;
		instruction->tiled_rectangle = node->client->arranged_rectangle;
		instruction->floating_rectangle = node->client->floating_rectangle;
		instruction->content_rect = node->pending.rectangle;
		instruction->scene_tree = client_get_scene_tree(node->client);
	}
}

static void transaction_add_node(transaction_t *txn, node_t *node, bool server_request) {
	if (!txn || !node)
		return;

	// check cached pending instruction pointer
	if (node->pending_inst) {
		copy_node_state(node, node->pending_inst);
		if (server_request)
			node->pending_inst->server_request = true;

		return;
	}

	// create new instruction
	transaction_inst_t *instruction = calloc(1, sizeof(*instruction));
	if (!instruction) {
		wlr_log(WLR_ERROR, "Failed to allocate transaction instruction");
		return;
	}

	instruction->transaction = txn;
	instruction->node = node;
	instruction->waiting = false;
	instruction->server_request = server_request;
	instruction->serial = 0;

	copy_node_state(node, instruction);

	// instruction is updated at commit
	node->pending_inst = instruction;
	node->ntxnrefs++;
	wlr_log(WLR_DEBUG, "transaction_add_node: node %u ntxnrefs=%zu destroying=%d", node->id,
		(size_t)node->ntxnrefs, node->destroying);

	wl_list_insert(&txn->instructions, &instruction->link);
}

static void copy_node_current_state(node_t *node, transaction_inst_t *instruction) {
	if (!node || !instruction)
		return;

	node->current.rectangle = instruction->rectangle;
	node->current.split_ratio = instruction->split_ratio;
	node->current.split_type = instruction->split_type;
	node->current.hidden = instruction->hidden;
	node->rectangle = instruction->rectangle;
	node->split_ratio = instruction->split_ratio;
	node->split_type = instruction->split_type;
	node->hidden = instruction->hidden;

	if (!node->client) {
		wlr_log(WLR_DEBUG, "Skipping state copy for node %u - client is NULL", node->id);
		return;
	}

	if (node->destroying) {
		node->client->flags.shown = false;
		return;
	}

	node->client->state = instruction->state;
	node->client->tiled_rectangle = instruction->tiled_rectangle;
	node->client->floating_rectangle = instruction->floating_rectangle;
	node->client->committed_tiled_rectangle = instruction->tiled_rectangle;
}

static void arrange_node_geometry(node_t *node, transaction_inst_t *instruction) {
	if (!node || !instruction)
		return;

	if (!node->client) {
		wlr_log(WLR_DEBUG, "Skipping arrange for node %u - client is NULL", node->id);
		return;
	}

	if (node->destroying) {
		if (instruction->scene_tree)
			if (!animation_has_fade_out(instruction->scene_tree))
				wlr_scene_node_set_enabled(&instruction->scene_tree->node, false);

		return;
	}

	if (!node->client->toplevel && !node->client->xwayland_view) {
		wlr_log(WLR_DEBUG, "Skipping arrange for node %u - no toplevel or xwayland_view", node->id);
		return;
	}

	bool ready = node->client->toplevel ? toplevel_is_ready(node->client->toplevel) : true;
	if (!ready)
		return;

	struct wlr_box *rect;
	if (instruction->state == STATE_FULLSCREEN) {
		output_t *m = node->output;

		if (m)
			rect = &m->rectangle;
		else
			return;
	} else if (instruction->state == STATE_FLOATING)
		rect = &instruction->floating_rectangle;
	else
		rect = &instruction->tiled_rectangle;

	if (rect->width < 1 || rect->height < 1) {
		wlr_log(WLR_DEBUG, "Node %u content area too small (%dx%d), hiding", node->id, rect->width,
			rect->height);
		if (node->client->toplevel) {
			if (node->client->toplevel->saved_surface_tree)
				toplevel_remove_saved_buffer(node->client->toplevel);
		}
		struct wlr_scene_tree *st = client_get_scene_tree(node->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, false);

		return;
	}

	if (node->client->toplevel && node->client->toplevel->saved_surface_tree) {
		toplevel_remove_saved_buffer(node->client->toplevel);
		wlr_log(WLR_DEBUG, "Removed saved buffer for node %u", node->id);
	}

	wlr_log(WLR_DEBUG, "Arranging geometry for node %u: pos=(%d,%d) size=(%dx%d) serial=%u", node->id,
		rect->x, rect->y, rect->width, rect->height, instruction->serial);

	struct wlr_scene_tree *scene_tree = NULL;
	bool configured = false;

	scene_tree = client_get_scene_tree(node->client);
	configured = node->client->toplevel ? node->client->toplevel->configured : true;

	if (!scene_tree) {
		wlr_log(WLR_ERROR, "Node %u has no scene tree (toplevel or xwayland_view)", node->id);
		return;
	}

	if (node->output && scene_tree &&
			(scene_tree->node.x != rect->x || scene_tree->node.y != rect->y ||
			(instruction->previous_tiled_rectangle.width > 0 &&
			(instruction->previous_tiled_rectangle.width != rect->width ||
			instruction->previous_tiled_rectangle.height != rect->height))))
		effects_dirty_corner_masks(node->output);

	// attempt resize animation for size changes
	bool snapshot_resize = false;
	if (node->client->toplevel && instruction->previous_tiled_rectangle.width > 0 &&
			instruction->previous_tiled_rectangle.height > 0 &&
			(instruction->previous_tiled_rectangle.width != rect->width ||
			instruction->previous_tiled_rectangle.height != rect->height)) {
		snapshot_resize = animation_start_resize(node->client->toplevel,
			instruction->previous_tiled_rectangle, *rect);
		wlr_log(WLR_DEBUG, "Started resize animation for node %u: from=(%dx%d) to=(%dx%d) active=%d",
			node->id, instruction->previous_tiled_rectangle.width,
			instruction->previous_tiled_rectangle.height, rect->width, rect->height, snapshot_resize);
	}

	if (!snapshot_resize) {
		if (node->client->toplevel && node->client->toplevel->wants_fade) {
			node->client->toplevel->wants_fade = false;
			wlr_scene_node_set_position(&scene_tree->node, rect->x, rect->y);
			animation_fade_in(node->client->toplevel);
		} else if (instruction->previous_tiled_rectangle.width > 0 &&
				instruction->previous_tiled_rectangle.height > 0) {
			animation_apply_geometry_from(node, scene_tree, instruction->previous_tiled_rectangle, *rect,
				true);
		} else {
			animation_apply_geometry(node, scene_tree, *rect, true);
		}
	}

	// skip border/clip updates during resize animation (handled by animation tick)
	if (!snapshot_resize) {
		if (effective_border_width(node->desktop) != 0) {
			unsigned int bw = effective_border_width(node->desktop);
			struct wlr_scene_tree *border_tree = client_border_tree(node->client);
			struct wlr_scene_rect **border_rects = client_border_rects(node->client);

			if (node->client->toplevel) {
				struct toplevel_t *tl = node->client->toplevel;
				bool undersized = instruction->state != STATE_FLOATING &&
					instruction->state != STATE_FULLSCREEN && tl->geometry.width > 0 && tl->geometry.height > 0 &&
					((int)tl->geometry.width < rect->width || (int)tl->geometry.height < rect->height ||
					(int)tl->geometry.width > rect->width || (int)tl->geometry.height > rect->height);
				struct wlr_box geo;
				int border_x, border_y;
				if (undersized) {
					int center_x = (rect->width - (int)tl->geometry.width) / 2;
					int center_y = (rect->height - (int)tl->geometry.height) / 2;
					int cx = center_x > 0 ? center_x : 0;
					int cy = center_y > 0 ? center_y : 0;
					geo = (struct wlr_box){
						0,
						0,
						(int)tl->geometry.width < rect->width ? (int)tl->geometry.width : rect->width,
						(int)tl->geometry.height < rect->height ? (int)tl->geometry.height : rect->height
					};
					border_x = cx - (int)bw;
					border_y = cy - (int)bw;
				} else {
					geo = (struct wlr_box){
						0,
						0,
						rect->width,
						rect->height
					};
					border_x = -(int)bw;
					border_y = -(int)bw;
				}
				update_borders(border_tree, border_rects, geo, bw);
				wlr_scene_node_set_position(&border_tree->node, border_x, border_y);
				update_border_colors(node->client);
				surface_rounded_t *rounded = client_get_rounded(node->client);
				if (rounded && (node->client->border_radius > 0.0f || rounded->gradient_count >= 2)) {
					if (rounded->border_shader_node) {
						int new_fw = geo.width + 2 * (int)bw;
						int new_fh = geo.height + 2 * (int)bw;
						if (new_fw > 0 && new_fh > 0) {
							float scale = node->client->toplevel && node->client->toplevel->node &&
									node->client->toplevel->node->output ?
								node->client->toplevel->node->output->wlr_output->scale : 1.0f;
							int pfw = (int)((double)new_fw * scale + 0.5);
							int pfh = (int)((double)new_fh * scale + 0.5);
							if (rounded->border_shader_buf_w != pfw || rounded->border_shader_buf_h != pfh) {
								rounded->border_dirty = true;
								rounded->corner_mask_dirty = true;
							}
							wlr_scene_buffer_set_dest_size(rounded->border_shader_node, new_fw, new_fh);
						}
					}
				}
			} else if (border_tree) {
				const struct wlr_box geo = {
					0,
					0,
					rect->width,
					rect->height
				};
				update_borders(border_tree, border_rects, geo, bw);
				update_border_colors(node->client);
			}
		}
	}

	if (!snapshot_resize && node->client->toplevel)
		toplevel_center_and_clip_surface(node->client->toplevel);

	if (node->client->xwayland_view && node->client->xwayland_view->xwayland_surface) {
		struct wlr_xwayland_surface *xsurface = node->client->xwayland_view->xwayland_surface;
		wlr_log(WLR_INFO, "Transaction xwayland node %u: target=(%d,%d %dx%d) current=(%dx%d) state=%d",
			node->id, rect->x, rect->y, rect->width, rect->height, xsurface->width, xsurface->height,
			instruction->state);
		if ((int)rect->width != xsurface->width || (int)rect->height != xsurface->height) {
			wlr_xwayland_surface_configure(xsurface, rect->x, rect->y, rect->width, rect->height);
			node->client->xwayland_view->geometry.width = rect->width;
			node->client->xwayland_view->geometry.height = rect->height;
			wlr_log(WLR_INFO, "Transaction configured Xwayland: (%d,%d %dx%d)", rect->x, rect->y, rect->width,
				rect->height);
		} else {
			wlr_log(WLR_INFO, "Transaction skipped Xwayland configure (size unchanged)");
		}
	}

	if (node->client->flags.shown) {
		wlr_scene_node_set_enabled(&scene_tree->node, true);
		wlr_log(WLR_INFO, "Arranged layout for node %u [already shown]", node->id);
	} else {
		wlr_log(WLR_DEBUG, "Arranged layout for node %u [waiting to be shown] configured=%d shown=%d",
			node->id, configured, node->client->flags.shown);
	}
}

static bool node_in_transaction(transaction_t *txn, node_t *node) {
	(void)txn;
	return node && node->pending_inst;
}

static bool should_skip_node(node_t *node) {
	return txn_state.pending_transaction && node_in_transaction(txn_state.pending_transaction, node);
}

static void transaction_apply(transaction_t *txn) {
	if (!txn) {
		wlr_log(WLR_ERROR, "transaction_apply called with NULL txn");
		return;
	}

	if (debug_txn_timings) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double ms = (now.tv_sec - txn->commit_time.tv_sec) * 1000.0 + (now.tv_nsec -
			txn->commit_time.tv_nsec) / 1000000.0;
		wlr_log(WLR_INFO, "Transaction applying after %.1fms (%zu waiting, %zu total)", ms,
			txn->num_waiting, (size_t)wl_list_length(&txn->instructions));
	} else {
		wlr_log(WLR_INFO, "Transaction applying (%zu waiting, %zu total)", txn->num_waiting,
			(size_t)wl_list_length(&txn->instructions));
	}

	transaction_inst_t *instruction, *tmp;
	wl_list_for_each_safe(instruction, tmp, &txn->instructions, link) {
		if (!instruction->node) {
			wlr_log(WLR_ERROR, "Skipping instruction with NULL node");
			continue;
		}

		if (should_skip_node(instruction->node))
			continue;

		wlr_log(WLR_DEBUG, "Copying state for node %u (ntxnrefs=%zu destroying=%d)", instruction->node->id,
			(size_t)instruction->node->ntxnrefs, instruction->node->destroying);
		copy_node_current_state(instruction->node, instruction);
	}

	wl_list_for_each_safe(instruction, tmp, &txn->instructions, link) {
		if (!instruction->node)
			continue;
		if (should_skip_node(instruction->node))
			continue;

		wlr_log(WLR_DEBUG, "Arranging geometry for node %u (ntxnrefs=%zu destroying=%d)",
			instruction->node->id, (size_t)instruction->node->ntxnrefs, instruction->node->destroying);
		arrange_node_geometry(instruction->node, instruction);
	}
}

static bool should_configure(node_t *node, transaction_inst_t *instruction) {
	// holy checks
	if (!node || !instruction)
		return false;
	if (!node->client || !node->client->toplevel)
		return false;
	if (!node->client->toplevel->xdg_toplevel)
		return false;
	if (node->destroying)
		return false;
	if (!instruction->server_request)
		return false;

	// always configure if new window
	if (!node->client->toplevel->configured) {
		wlr_log(WLR_DEBUG, "should_configure node %u: NEW window, needs configure", node->id);
		return true;
	}

	// determine target size based on state
	struct wlr_box target_rect;
	if (node->client->state == STATE_FULLSCREEN) {
		output_t *m = node->output;

		if (!m)
			return false;

		target_rect = m->rectangle;
	} else if (node->client->state == STATE_FLOATING)
		target_rect = instruction->floating_rectangle;
	else
		target_rect = instruction->tiled_rectangle;

	// don't configure windows that are too small to show
	if (target_rect.width < 1 || target_rect.height < 1)
		return false;

	// compare target size against client's committed geometry
	int client_w = node->client->toplevel->geometry.width;
	int client_h = node->client->toplevel->geometry.height;
	bool size_changed = client_w != target_rect.width || client_h != target_rect.height;

	wlr_log(WLR_DEBUG, "should_configure node %u: client_geometry=(%dx%d) target=(%dx%d) "
		"last_requested=(%dx%d) changed=%d", node->id, client_w, client_h, target_rect.width,
			target_rect.height, node->client->toplevel->last_requested.width,
			node->client->toplevel->last_requested.height, size_changed);

	if (!size_changed)
		return false;

	if (node->client->toplevel->last_requested.width == target_rect.width &&
			node->client->toplevel->last_requested.height == target_rect.height) {
		wlr_log(WLR_DEBUG, "should_configure node %u: target unchanged since last configure, skipping",
			node->id);
		return false;
	}

	return true;
}

static int handle_timeout(void *data) {
	transaction_t *txn = data;

	if (!txn)
		return 0;

	wlr_log(WLR_DEBUG, "Transaction timed out (%zu/%zu ready) - retrying unresponsive",
		wl_list_length(&txn->instructions) - txn->num_waiting,
		(size_t)wl_list_length(&txn->instructions));

	bool need_retry = false;
	transaction_inst_t *inst;
	wl_list_for_each(inst, &txn->instructions, link) {
		if (!inst->waiting || !inst->node->client || !inst->node->client->toplevel)
			continue;
		transaction_add_dirty_node(inst->node);
		need_retry = true;
	}

	transaction_apply(txn);
	cursor_rebase();

	if (txn == txn_state.queued_transaction)
		txn_state.queued_transaction = NULL;

	transaction_destroy(txn);
	transaction_commit_pending();

	if (need_retry)
		_transaction_commit_dirty(true);

	return 0;
}

static void transaction_progress(void) {
	if (!txn_state.queued_transaction)
		return;
	if (txn_state.queued_transaction->num_waiting > 0)
		return;

	transaction_apply(txn_state.queued_transaction);
	cursor_rebase();
	update_idle_inhibitors(NULL);

	transaction_destroy(txn_state.queued_transaction);
	txn_state.queued_transaction = NULL;

	transaction_commit_pending();
}

static void transaction_commit(transaction_t *txn) {
	if (!txn)
		return;

	wlr_log(WLR_DEBUG, "transaction_commit: txn=%p with %zu instructions", (void *)txn,
		(size_t)wl_list_length(&txn->instructions));

	if (debug_txn_timings)
		clock_gettime(CLOCK_MONOTONIC, &txn->commit_time);

	size_t num_configures = 0;

	// send configure to clients and save buffers
	struct transaction_inst_t *instruction;
	wl_list_for_each(instruction, &txn->instructions, link) {
		node_t *node = instruction->node;

		if (should_configure(node, instruction)) {
			if (node->client && node->client->toplevel && toplevel_is_ready(node->client->toplevel)) {

				// determine the correct rectangle based on client state
				struct wlr_box *rect;
				if (node->client->state == STATE_FULLSCREEN) {
					output_t *m = node->output;
					if (m)
						rect = &m->rectangle;
					else
						rect = &instruction->rectangle;
				} else if (node->client->state == STATE_FLOATING)
					rect = &instruction->floating_rectangle;
				else
					rect = &instruction->tiled_rectangle;

				// send configure with new size
				instruction->serial = wlr_xdg_toplevel_set_size(node->client->toplevel->xdg_toplevel,
					rect->width, rect->height);

				node->client->toplevel->last_requested = *rect;

				bool has_stable_frame = node->client->toplevel->geometry.width > 0 ||
					node->client->toplevel->geometry.height > 0;
				instruction->require_geometry_match = has_stable_frame && node->client->flags.shown &&
					node->client->toplevel->configured;

				// wait for all mapped toplevels to respond
				instruction->waiting = true;
				txn->num_waiting++;

				num_configures++;

				wlr_log(WLR_DEBUG, "Sent configure to node %u: serial=%u size=(%dx%d) waiting=%d", node->id,
					instruction->serial, rect->width, rect->height, instruction->waiting);

				toplevel_save_buffer(node->client->toplevel);
				toplevel_send_frame_done(node->client->toplevel);
			}
		}

		node->instruction = instruction;
		node->pending_inst = NULL;
	}

	txn->num_configures = num_configures;

	wlr_log(WLR_DEBUG,
		"Transaction committing with %zu configures (%zu total instructions), waiting=%zu", num_configures,
		(size_t)wl_list_length(&txn->instructions), txn->num_waiting);

	// debug overrides
	if (debug_noatomic) {
		wlr_log(WLR_DEBUG, "debug_noatomic: forcing immediate apply");
		txn->num_waiting = 0;
	} else if (debug_txn_wait) {
		wlr_log(WLR_DEBUG, "debug_txn_wait: forcing transaction timeout");
		txn->num_waiting += 1000000;
	}

	if (txn->num_waiting == 0) {
		// no clients need configuring, apply immediately
		wlr_log(WLR_DEBUG, "Transaction applying immediately (no configures needed)");
		transaction_apply(txn);
		cursor_rebase();
		update_idle_inhibitors(NULL);
		transaction_destroy(txn);
	} else {
		// set up timer for client response timeout
		txn->timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display), handle_timeout,
			txn);

		if (txn->timer)
			wl_event_source_timer_update(txn->timer, txn_timeout_ms);

		txn_state.queued_transaction = txn;
	}
}

static void transaction_commit_pending(void) {
	if (txn_state.queued_transaction)
		return;
	if (!txn_state.pending_transaction)
		return;

	transaction_t *txn = txn_state.pending_transaction;
	txn_state.pending_transaction = NULL;
	transaction_commit(txn);
}

static void _transaction_commit_dirty(bool server_request) {
	if (txn_state.dirty_count == 0)
		return;

	// always accumulate dirty nodes into pending_transaction
	if (!txn_state.pending_transaction) {
		txn_state.pending_transaction = transaction_create();
		if (!txn_state.pending_transaction)
			return;
	}

	for (size_t i = 0; i < txn_state.dirty_count; i++) {
		node_t *node = txn_state.dirty_nodes[i];
		transaction_add_node(txn_state.pending_transaction, node, server_request);
		node->dirty = false;
	}
	txn_state.dirty_count = 0;

	transaction_commit_pending();
}

void transaction_commit_dirty(void) {
	wlr_log(WLR_DEBUG, "transaction_commit_dirty called with %zu dirty nodes", txn_state.dirty_count);
	_transaction_commit_dirty(true);
}

void transaction_commit_dirty_client(void) {
	_transaction_commit_dirty(false);
}

static void set_instruction_ready(transaction_inst_t *instruction) {
	if (!instruction || !instruction->waiting)
		return;

	transaction_t *txn = instruction->transaction;
	instruction->waiting = false;
	txn->num_waiting--;

	wlr_log(WLR_DEBUG, "Instruction ready for node %u (%zu remaining)", instruction->node->id,
		txn->num_waiting);

	transaction_progress();
}

bool transaction_notify_view_ready_by_serial(toplevel_t *toplevel, uint32_t serial) {
	if (!toplevel || !toplevel->node)
		return false;

	node_t *node = toplevel->node;
	if (!node->instruction)
		return false;

	transaction_inst_t *instruction = node->instruction;

	if (instruction->serial == serial && instruction->waiting) {
		wlr_log(WLR_DEBUG, "View ready by serial %u for node %u", serial, node->id);
		set_instruction_ready(instruction);
		return true;
	}

	return false;
}

void transaction_notify_view_unmapped(node_t *node) {
	if (!node || !node->instruction || !node->instruction->waiting)
		return;

	transaction_inst_t *instruction = node->instruction;
	wlr_log(WLR_DEBUG, "View unmapped for node %u - marking instruction ready", node->id);
	set_instruction_ready(instruction);
}

bool transaction_notify_view_ready_by_geometry(toplevel_t *toplevel, int x, int y, int width,
		int height) {
	if (!toplevel || !toplevel->node)
		return false;

	node_t *node = toplevel->node;
	if (!node->instruction)
		return false;

	transaction_inst_t *instruction = node->instruction;

	if (instruction->waiting && (int)instruction->content_rect.x == x &&
			(int)instruction->content_rect.y == y && (int)instruction->content_rect.width == width &&
			(int)instruction->content_rect.height == height) {

		wlr_log(WLR_DEBUG, "View ready by geometry (%d,%d %dx%d) for node %u", x, y, width, height,
			node->id);
		set_instruction_ready(instruction);
		return true;
	}

	return false;
}

void transaction_add_dirty_node(node_t *node) {
	if (!node || node->dirty)
		return;

	node->dirty = true;

	// add to dirty list
	if (txn_state.dirty_count >= txn_state.dirty_capacity) {
		txn_state.dirty_capacity = txn_state.dirty_capacity == 0 ? 32 : txn_state.dirty_capacity * 2;
		node_t **new_nodes = realloc(txn_state.dirty_nodes, txn_state.dirty_capacity * sizeof(node_t *));
		if (!new_nodes) {
			wlr_log(WLR_ERROR, "failed to realloc dirty nodes array");
			return;
		}
		txn_state.dirty_nodes = new_nodes;
	}
	txn_state.dirty_nodes[txn_state.dirty_count++] = node;

	wlr_log(WLR_DEBUG, "transaction_add_dirty_node: node %u (total=%zu)", node->id,
		txn_state.dirty_count);
}

void transaction_init(void) {
	ONCE();
	txn_state.pending_transaction = NULL;
	txn_state.queued_transaction = NULL;
	txn_state.dirty_nodes = NULL;
	txn_state.dirty_count = 0;
	txn_state.dirty_capacity = 0;

	wlr_log(WLR_INFO, "Transaction system initialized");
}

void transaction_fini(void) {
	ONCE();
	if (txn_state.pending_transaction) {
		transaction_destroy(txn_state.pending_transaction);
		txn_state.pending_transaction = NULL;
	}

	if (txn_state.queued_transaction) {
		transaction_destroy(txn_state.queued_transaction);
		txn_state.queued_transaction = NULL;
	}

	if (txn_state.dirty_nodes) {
		free(txn_state.dirty_nodes);
		txn_state.dirty_nodes = NULL;
	}

	txn_state.dirty_count = 0;
	txn_state.dirty_capacity = 0;

	wlr_log(WLR_INFO, "Transaction system cleaned up");
}
