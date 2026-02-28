#pragma once

#include "types.h"
#include <wayland-server-core.h>
#include <wayland-server.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>

struct bwm_toplevel {
  struct wl_list link;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree;      // Parent container
  struct wlr_scene_tree *content_tree;    // XDG surface content
  struct wlr_scene_tree *saved_surface_tree;  // Saved buffer snapshot

  struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel;
  struct wlr_foreign_toplevel_handle_v1 *foreign_toplevel;

  char *foreign_identifier;

  struct wlr_ext_image_capture_source_v1 *image_capture_source;
  struct wlr_scene_surface *image_capture_surface;
  struct wlr_scene *image_capture;
  struct wlr_scene_tree *image_capture_tree;

  node_t *node;

  struct wlr_box geometry;  // Client-committed surface geometry

  bool mapped;
  bool configured;
  bool client_maximized;

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
  struct wl_listener new_xdg_popup;

  // foreign toplevel listeners
  struct wl_listener foreign_activate_request;
  struct wl_listener foreign_fullscreen_request;
  struct wl_listener foreign_close_request;
  struct wl_listener foreign_destroy;
};

void handle_new_xdg_toplevel(struct wl_listener *listener, void *data);

// helper functions
void focus_toplevel(struct bwm_toplevel *toplevel);
void toplevel_apply_geometry(struct bwm_toplevel *toplevel);
bool toplevel_is_ready(struct bwm_toplevel *toplevel);
void update_foreign_toplevel_state(struct bwm_toplevel *toplevel);

// buffer saving for transactions
void toplevel_save_buffer(struct bwm_toplevel *toplevel);
void toplevel_remove_saved_buffer(struct bwm_toplevel *toplevel);
void toplevel_send_frame_done(struct bwm_toplevel *toplevel);

void handle_new_toplevel_capture_request(struct wl_listener *listener, void *data);
