#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

// forward declarations
struct node_t;
struct bwm_toplevel;

struct bwm_transaction_inst {
  struct bwm_transaction *transaction;
  struct node_t *node;
  struct wl_list link;  // bwm_transaction::instructions

  // saved state
  struct wlr_box rectangle;
  double split_ratio;
  int split_type;  // split_type_t
  bool hidden;

  int state;  // client_state_t
  struct wlr_box tiled_rectangle;
  struct wlr_box floating_rectangle;
  struct wlr_box content_rect;
  uint32_t serial;
  bool waiting;
  bool server_request;
};

struct bwm_transaction {
  struct wl_event_source *timer;
  struct wl_list instructions;
  size_t num_waiting;
  size_t num_configures;
  struct timespec commit_time;
};

/**
 * Find all dirty nodes, create and commit a transaction containing them,
 * and unmark them as dirty.
 */
void transaction_commit_dirty(void);

/**
 * Same as transaction_commit_dirty, but signalling that this is a
 * client-initiated change that has already taken effect.
 */
void transaction_commit_dirty_client(void);

/**
 * Notify the transaction system that a view is ready for the new layout.
 * When all views in the transaction are ready, the layout will be applied.
 *
 * Returns true if this instruction was found and marked ready.
 */
bool transaction_notify_view_ready_by_serial(struct bwm_toplevel *toplevel,
                                              uint32_t serial);

/**
 * Notify the transaction system that a view is ready for the new layout,
 * identifying the instruction by geometry rather than serial.
 *
 * This can be used as a fallback for clients that don't properly track serials.
 * Returns true if this instruction was found and marked ready.
 */
bool transaction_notify_view_ready_by_geometry(struct bwm_toplevel *toplevel,
                                                int x, int y, int width, int height);

/**
 * Initialize the transaction system.
 * Must be called during server initialization.
 */
void transaction_init(void);

/**
 * Cleanup the transaction system.
 * Should be called during server shutdown.
 */
void transaction_fini(void);

/**
 * Mark a node as dirty and add it to the pending transaction.
 * Called by tree.c when node state changes.
 */
void transaction_add_dirty_node(struct node_t *node);
