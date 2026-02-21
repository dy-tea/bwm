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

    wlr_log(WLR_DEBUG, "transaction_destroy: node %u ntxnrefs=%zu destroying=%d",
            node->id, (size_t)node->ntxnrefs, node->destroying);

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

  wlr_log(WLR_DEBUG, "transaction_add_node: node %u ntxnrefs=%zu destroying=%d",
          node->id, (size_t)node->ntxnrefs, node->destroying);

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

  // check if client exists and is valid
  if (!node->client) {
    wlr_log(WLR_DEBUG, "Skipping apply for node %u - client is NULL", node->id);
    return;
  }

  // check if toplevel was destroyed
  if (!node->client->toplevel) {
    wlr_log(WLR_DEBUG, "Skipping apply for node %u - toplevel already destroyed", node->id);
    return;
  }

  node->client->state = instruction->state;

  // copy rectangles
  node->client->tiled_rectangle = instruction->tiled_rectangle;
  node->client->floating_rectangle = instruction->floating_rectangle;

  // node is destroying, hide it now atomically with other changes
  if (node->destroying) {
    node->client->shown = false;
    return;
  }

  // apply geometry
  bool ready = toplevel_is_ready(node->client->toplevel);
  if (ready) {
    wlr_log(WLR_DEBUG, "Transaction apply: node %u tiled_rect=(%d,%d %dx%d)",
            node->id,
            instruction->tiled_rectangle.x,
            instruction->tiled_rectangle.y,
            instruction->tiled_rectangle.width,
            instruction->tiled_rectangle.height);

    struct wlr_box *rect;
    if (node->client->state == STATE_FULLSCREEN) {
      monitor_t *m = node->monitor;
      if (m)
        rect = &m->rectangle;
      else return;
    } else if (node->client->state == STATE_FLOATING)
      rect = &instruction->floating_rectangle;
    else
      rect = &instruction->tiled_rectangle;

    if (node->client->toplevel && node->client->toplevel->saved_surface_tree) {
      toplevel_remove_saved_buffer(node->client->toplevel);
      wlr_log(WLR_DEBUG, "Removed saved buffer for node %u", node->id);
    }

    wlr_log(WLR_DEBUG, "Applying geometry to node %u: pos=(%d,%d) size=(%dx%d) serial=%u",
            node->id, rect->x, rect->y, rect->width, rect->height, instruction->serial);

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

static void transaction_apply(struct bwm_transaction *txn) {
  if (!txn) {
    wlr_log(WLR_ERROR, "transaction_apply called with NULL txn");
    return;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  double ms = (now.tv_sec - txn->commit_time.tv_sec) * 1000.0 +
              (now.tv_nsec - txn->commit_time.tv_nsec) / 1000000.0;

  wlr_log(WLR_INFO, "Transaction applying after %.1fms (%zu waiting, %zu total",
          ms, txn->num_waiting, (size_t)wl_list_length(&txn->instructions));

  struct bwm_transaction_inst *instruction, *tmp;
  wl_list_for_each_safe(instruction, tmp, &txn->instructions, link) {
    if (!instruction->node) {
      wlr_log(WLR_ERROR, "Skipping instruction with NULL node");
      continue;
    }
    wlr_log(WLR_DEBUG, "Applying instruction for node %u (ntxnrefs=%zu destroying=%d)",
            instruction->node->id, (size_t)instruction->node->ntxnrefs, instruction->node->destroying);
    apply_node_state(instruction->node, instruction);
  }
}

static bool should_configure(node_t *node,
                            struct bwm_transaction_inst *instruction) {
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
    monitor_t *m = node->monitor;
    if (!m) return false;
    target_rect = m->rectangle;
  } else if (node->client->state == STATE_FLOATING)
    target_rect = instruction->floating_rectangle;
  else
    target_rect = instruction->tiled_rectangle;

  // get committed size from surface
  struct wlr_xdg_surface *xdg_surface = node->client->toplevel->xdg_toplevel->base;
  int current_width = xdg_surface->current.geometry.width;
  int current_height = xdg_surface->current.geometry.height;

  // if geometry not set, use surface size
  if (current_width == 0 || current_height == 0) {
    if (xdg_surface->surface) {
      current_width = xdg_surface->surface->current.width;
      current_height = xdg_surface->surface->current.height;
    }
  }

  bool size_changed = current_width != target_rect.width ||
                      current_height != target_rect.height;

  wlr_log(WLR_DEBUG, "should_configure node %u: current=(%dx%d) target=(%dx%d) changed=%d",
          node->id, current_width, current_height,
          target_rect.width, target_rect.height, size_changed);

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

  // send configure to clients and save buffers
  struct bwm_transaction_inst *instruction;
  wl_list_for_each(instruction, &txn->instructions, link) {
    node_t *node = instruction->node;

    if (should_configure(node, instruction)) {
      if (node->client && node->client->toplevel &&
        toplevel_is_ready(node->client->toplevel)) {

        // determine the correct rectangle based on client state
        struct wlr_box *rect;
        if (node->client->state == STATE_FULLSCREEN) {
          monitor_t *m = node->monitor;
          if (m)
            rect = &m->rectangle;
          else
            rect = &instruction->rectangle;
        } else if (node->client->state == STATE_FLOATING)
          rect = &instruction->floating_rectangle;
        else
          rect = &instruction->tiled_rectangle;

        // send configure with new size
        instruction->serial = wlr_xdg_toplevel_set_size(
          node->client->toplevel->xdg_toplevel,
          rect->width,
          rect->height);

        // wait for all mapped toplevels to respond
        instruction->waiting = true;
        txn->num_waiting++;

        if (node->client->shown && !node->client->toplevel->saved_surface_tree
            && node->client->toplevel->configured) {
          toplevel_save_buffer(node->client->toplevel);
          wlr_log(WLR_DEBUG, "Saved buffer for node %u (shown=true)", node->id);
        }

        num_configures++;

        wlr_log(WLR_DEBUG,
                "Sent configure to node %u: serial=%u size=(%dx%d) waiting=%d",
                node->id, instruction->serial,
                rect->width,
                rect->height,
                instruction->waiting);

        toplevel_send_frame_done(node->client->toplevel);
      }
    }

    node->instruction = instruction;
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
  if (!node || node->dirty)
    return;

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
