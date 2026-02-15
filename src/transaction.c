#define WLR_USE_UNSTABLE
#include "transaction.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>

// timeout in milliseconds
#define TXN_TIMEOUT_MS 200

// transaction state
static struct {
    struct bwm_transaction *pending_transaction;
    struct bwm_transaction *queued_transaction;
    node_t **dirty_nodes;
    size_t dirty_count;
    size_t dirty_capacity;
} txn_state = {0};

static struct bwm_transaction *transaction_create(void) {
    struct bwm_transaction *txn = calloc(1, sizeof(*txn));
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

static void transaction_destroy(struct bwm_transaction *txn) {
    if (!txn)
        return;

    // free all instructions
    struct bwm_transaction_inst *instruction, *tmp;
    wl_list_for_each_safe(instruction, tmp, &txn->instructions, link) {
        node_t *node = instruction->node;
        node->ntxnrefs--;

        if (node->instruction == instruction)
            node->instruction = NULL;

        if (node->destroying && node->ntxnrefs == 0)
            free_node(node);

        wl_list_remove(&instruction->link);
        free(instruction);
    }

    if (txn->timer)
        wl_event_source_remove(txn->timer);

    free(txn);
}

static void copy_node_state(node_t *node,
                           struct bwm_transaction_inst *instruction) {
    if (!node || !instruction)
        return;

    // copy pending state to instruction
    instruction->rectangle = node->pending.rectangle;
    instruction->split_ratio = node->pending.split_ratio;
    instruction->split_type = node->pending.split_type;
    instruction->hidden = node->pending.hidden;

    if (node->client) {
        instruction->state = node->client->state;
        instruction->tiled_rectangle = node->client->tiled_rectangle;
        instruction->floating_rectangle = node->client->floating_rectangle;
        instruction->content_rect = node->pending.rectangle;
    }
}

static void transaction_add_node(struct bwm_transaction *txn, node_t *node,
                                 bool server_request) {
    if (!txn || !node)
        return;

    // check if already in transaction
    struct bwm_transaction_inst *existing;
    wl_list_for_each(existing, &txn->instructions, link) {
        if (existing->node == node) {
            copy_node_state(node, existing);
            return;
        }
    }

    // freate new instruction
    struct bwm_transaction_inst *instruction = calloc(1, sizeof(*instruction));
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

    // update node refs
    node->instruction = instruction;
    node->ntxnrefs++;

    wl_list_insert(&txn->instructions, &instruction->link);
}

static void apply_node_state(node_t *node,
                             struct bwm_transaction_inst *instruction) {
    if (!node || !instruction)
        return;

    // update current state
    node->current.rectangle = instruction->rectangle;
    node->current.split_ratio = instruction->split_ratio;
    node->current.split_type = instruction->split_type;
    node->current.hidden = instruction->hidden;
    node->rectangle = instruction->rectangle;
    node->split_ratio = instruction->split_ratio;
    node->split_type = instruction->split_type;
    node->hidden = instruction->hidden;

    if (node->client && node->client->toplevel) {
        node->client->state = instruction->state;

        // copy rectangles
        node->client->tiled_rectangle = instruction->tiled_rectangle;
        node->client->floating_rectangle = instruction->floating_rectangle;

        // apply geometry
        if (toplevel_is_ready(node->client->toplevel)) {
            wlr_log(WLR_DEBUG, "Transaction apply: node %u tiled_rect=(%d,%d %dx%d)",
                    node->id,
                    instruction->tiled_rectangle.x,
                    instruction->tiled_rectangle.y,
                    instruction->tiled_rectangle.width,
                    instruction->tiled_rectangle.height);

            struct wlr_box *rect;
            if (node->client->state == STATE_FULLSCREEN) {
                monitor_t *m = mon;
                if (m)
                    rect = &m->rectangle;
                else return;
            } else if (node->client->state == STATE_FLOATING)
                rect = &instruction->floating_rectangle;
            else
                rect = &instruction->tiled_rectangle;

            wlr_xdg_toplevel_set_size(node->client->toplevel->xdg_toplevel, rect->width, rect->height);
            wlr_scene_node_set_position(&node->client->toplevel->scene_tree->node, rect->x, rect->y);

            if (node->client->shown) {
                wlr_scene_node_set_enabled(&node->client->toplevel->scene_tree->node, true);
                wlr_log(WLR_DEBUG, "Applied layout to node %u [already shown]", node->id);
            } else if (node->client->toplevel->configured) {
                wlr_scene_node_set_enabled(&node->client->toplevel->scene_tree->node, true);
                node->client->shown = true;
                wlr_log(WLR_DEBUG, "Applied layout to node %u [FIRST SHOW]", node->id);
            } else {
                wlr_log(WLR_DEBUG, "Applied layout to node %u [waiting for configure]", node->id);
            }
        }
    }
}

static void transaction_apply(struct bwm_transaction *txn) {
    if (!txn) {
        wlr_log(WLR_ERROR, "transaction_apply called with NULL txn");
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double ms = (now.tv_sec - txn->commit_time.tv_sec) * 1000.0 +
                (now.tv_nsec - txn->commit_time.tv_nsec) / 1000000.0;

    wlr_log(WLR_INFO, "=== Transaction APPLYING after %.1fms (%zu waiting, %zu total) ===",
            ms, txn->num_waiting, (size_t)wl_list_length(&txn->instructions));

    // apply all insts
    struct bwm_transaction_inst *instruction;
    wl_list_for_each(instruction, &txn->instructions, link) {
        wlr_log(WLR_DEBUG, "Applying instruction for node %u", instruction->node->id);
        apply_node_state(instruction->node, instruction);
    }

    wlr_log(WLR_INFO, "=== Transaction APPLY COMPLETE ===");
}

static bool should_configure(node_t *node,
                            struct bwm_transaction_inst *instruction) {
    if (!node || !instruction)
        return false;

    if (!node->client || !node->client->toplevel)
        return false;

    if (!node->client->toplevel->xdg_toplevel)
        return false;

    struct wlr_box current = node->current.rectangle;
    struct wlr_box new_rect = instruction->rectangle;

    bool size_changed = current.width != new_rect.width ||
                       current.height != new_rect.height;

    return size_changed;
}

static int handle_timeout(void *data) {
    struct bwm_transaction *txn = data;

    if (!txn)
        return 0;

    wlr_log(WLR_DEBUG, "Transaction timed out (%zu/%zu ready)",
            wl_list_length(&txn->instructions) - txn->num_waiting,
            (size_t)wl_list_length(&txn->instructions));

    transaction_apply(txn);

    if (txn == txn_state.queued_transaction)
        txn_state.queued_transaction = NULL;

    transaction_destroy(txn);

    // commit pending transaction if any
    if (txn_state.pending_transaction) {
        txn_state.pending_transaction = NULL;
        transaction_commit_dirty();
    }

    return 0;
}

static void transaction_progress(void) {
    if (!txn_state.queued_transaction)
        return;

    if (txn_state.queued_transaction->num_waiting == 0) {
        transaction_apply(txn_state.queued_transaction);
        transaction_destroy(txn_state.queued_transaction);
        txn_state.queued_transaction = NULL;

        // process pending transaction if any
        if (txn_state.pending_transaction)
            transaction_commit_dirty();
    }
}

static void transaction_commit(struct bwm_transaction *txn) {
    if (!txn)
        return;

    wlr_log(WLR_DEBUG, "transaction_commit: txn=%p with %zu instructions",
            (void*)txn, (size_t)wl_list_length(&txn->instructions));

    size_t num_configures = 0;

    // send configure to clients
    struct bwm_transaction_inst *instruction;
    wl_list_for_each(instruction, &txn->instructions, link) {
        if (should_configure(instruction->node, instruction)) {
            node_t *node = instruction->node;

            if (node->client && node->client->toplevel &&
                toplevel_is_ready(node->client->toplevel)) {

                // send configure with new size
                wlr_xdg_toplevel_set_size(
                    node->client->toplevel->xdg_toplevel,
                    instruction->rectangle.width,
                    instruction->rectangle.height);

                // get serial from scheduled configure
                instruction->serial = node->client->toplevel->xdg_toplevel->base->scheduled_serial;
                instruction->waiting = true;
                txn->num_waiting++;
                num_configures++;

                wlr_log(WLR_DEBUG,
                        "Sent configure to node %u: serial=%u size=(%dx%d)",
                        node->id, instruction->serial,
                        instruction->rectangle.width,
                        instruction->rectangle.height);
            }
        }
    }

txn->num_configures = num_configures;

wlr_log(WLR_DEBUG, "Transaction committing with %zu configures (%zu total instructions), waiting=%zu",
        num_configures, (size_t)wl_list_length(&txn->instructions), txn->num_waiting);

    if (txn->num_waiting == 0) {
        // no clients
        wlr_log(WLR_DEBUG, "Transaction applying immediately (no configures needed)");
        transaction_apply(txn);
        transaction_destroy(txn);
    } else {
        // wait for timeout
        txn->timer = wl_event_loop_add_timer(
            wl_display_get_event_loop(server.wl_display),
            handle_timeout, txn);

        if (txn->timer)
            wl_event_source_timer_update(txn->timer, TXN_TIMEOUT_MS);

        txn_state.queued_transaction = txn;
    }
}

static void _transaction_commit_dirty(bool server_request) {
    if (txn_state.dirty_count == 0)
        return;

    // add queued to pending
    if (txn_state.queued_transaction) {
        if (!txn_state.pending_transaction) {
            txn_state.pending_transaction = transaction_create();
            if (!txn_state.pending_transaction)
                return;
        }

        // add dirty nodes to pending
        for (size_t i = 0; i < txn_state.dirty_count; i++) {
            node_t *node = txn_state.dirty_nodes[i];
            transaction_add_node(txn_state.pending_transaction, node, server_request);
            node->dirty = false;
        }
        txn_state.dirty_count = 0;
        return;
    }

    // create new transaction
    struct bwm_transaction *txn = transaction_create();
    if (!txn)
        return;

    // add dirty nodes to transaction
    for (size_t i = 0; i < txn_state.dirty_count; i++) {
        node_t *node = txn_state.dirty_nodes[i];
        transaction_add_node(txn, node, server_request);
        node->dirty = false;
    }
    txn_state.dirty_count = 0;

    // Commit the transaction
    transaction_commit(txn);
}

void transaction_commit_dirty(void) {
    wlr_log(WLR_DEBUG, "transaction_commit_dirty called with %zu dirty nodes",
            txn_state.dirty_count);
    _transaction_commit_dirty(true);
}

void transaction_commit_dirty_client(void) {
    _transaction_commit_dirty(false);
}

static void set_instruction_ready(struct bwm_transaction_inst *instruction) {
    if (!instruction || !instruction->waiting)
        return;

    struct bwm_transaction *txn = instruction->transaction;

    instruction->waiting = false;
    txn->num_waiting--;

    wlr_log(WLR_DEBUG, "Instruction ready for node %u (%zu remaining)",
            instruction->node->id, txn->num_waiting);

    transaction_progress();
}

bool transaction_notify_view_ready_by_serial(struct bwm_toplevel *toplevel,
                                              uint32_t serial) {
    if (!toplevel || !toplevel->node)
        return false;

    node_t *node = toplevel->node;

    if (!node->instruction)
        return false;

    struct bwm_transaction_inst *instruction = node->instruction;

    if (instruction->serial == serial && instruction->waiting) {
        wlr_log(WLR_DEBUG, "View ready by serial %u for node %u",
                serial, node->id);
        set_instruction_ready(instruction);
        return true;
    }

    return false;
}

bool transaction_notify_view_ready_by_geometry(struct bwm_toplevel *toplevel,
                                                int x, int y, int width, int height) {
    if (!toplevel || !toplevel->node)
        return false;

    node_t *node = toplevel->node;

    if (!node->instruction)
        return false;

    struct bwm_transaction_inst *instruction = node->instruction;

    if (instruction->waiting &&
        (int)instruction->content_rect.x == x &&
        (int)instruction->content_rect.y == y &&
        (int)instruction->content_rect.width == width &&
        (int)instruction->content_rect.height == height) {

        wlr_log(WLR_DEBUG, "View ready by geometry (%d,%d %dx%d) for node %u",
                x, y, width, height, node->id);
        set_instruction_ready(instruction);
        return true;
    }

    return false;
}

void transaction_init(void) {
    txn_state.pending_transaction = NULL;
    txn_state.queued_transaction = NULL;
    txn_state.dirty_nodes = NULL;
    txn_state.dirty_count = 0;
    txn_state.dirty_capacity = 0;

    wlr_log(WLR_INFO, "Transaction system initialized");
}

void transaction_fini(void) {
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

void transaction_add_dirty_node(node_t *node) {
    if (!node || node->dirty) {
        return;
    }

    node->dirty = true;

    // add to dirty list
    if (txn_state.dirty_count >= txn_state.dirty_capacity) {
        txn_state.dirty_capacity = txn_state.dirty_capacity == 0 ? 32 :
                                   txn_state.dirty_capacity * 2;
        txn_state.dirty_nodes = realloc(txn_state.dirty_nodes,
                                       txn_state.dirty_capacity * sizeof(node_t*));
    }
    txn_state.dirty_nodes[txn_state.dirty_count++] = node;

    wlr_log(WLR_DEBUG, "transaction_add_dirty_node: node %u (total=%zu)",
            node->id, txn_state.dirty_count);
}
