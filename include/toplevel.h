#pragma once

#include "types.h"
#include <wayland-server.h>

struct bwm_toplevel {
  struct wl_list link;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree;      // Parent container
  struct wlr_scene_tree *content_tree;    // XDG surface content
  struct wlr_scene_tree *saved_surface_tree;  // Saved buffer snapshot

  node_t *node;

  bool mapped;
  bool configured;

  // listeners
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener commit;
  struct wl_listener destroy;
  struct wl_listener request_move;
  struct wl_listener request_resize;
  struct wl_listener request_maximize;
  struct wl_listener request_fullscreen;
  struct wl_listener set_title;
  struct wl_listener set_app_id;
};

// toplevel lifecycle
void handle_new_xdg_toplevel(struct wl_listener *listener, void *data);
void toplevel_map(struct wl_listener *listener, void *data);
void toplevel_unmap(struct wl_listener *listener, void *data);
void toplevel_commit(struct wl_listener *listener, void *data);
void toplevel_destroy(struct wl_listener *listener, void *data);

// toplevel requests
void toplevel_request_move(struct wl_listener *listener, void *data);
void toplevel_request_resize(struct wl_listener *listener, void *data);
void toplevel_request_maximize(struct wl_listener *listener, void *data);
void toplevel_request_fullscreen(struct wl_listener *listener, void *data);

// toplevel properties
void toplevel_set_title(struct wl_listener *listener, void *data);
void toplevel_set_app_id(struct wl_listener *listener, void *data);

// helper functions
void focus_toplevel(struct bwm_toplevel *toplevel);
void toplevel_apply_geometry(struct bwm_toplevel *toplevel);
bool toplevel_is_ready(struct bwm_toplevel *toplevel);

// buffer saving for transactions
void toplevel_save_buffer(struct bwm_toplevel *toplevel);
void toplevel_remove_saved_buffer(struct bwm_toplevel *toplevel);
void toplevel_send_frame_done(struct bwm_toplevel *toplevel);
