#pragma once

#include "types.h"
#include <GLES2/gl2.h>
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

  struct wlr_scene_buffer *blur_node;
  struct wlr_scene_buffer *mica_node;
  struct wlr_scene_buffer *acrylic_node;
  bool blur_scene_hidden;
  struct wlr_buffer *blur_buf;
  GLuint blur_buf_fbo;
  struct wlr_buffer *acrylic_buf;
  GLuint acrylic_buf_fbo;

  // rounded borders
  struct wlr_scene_buffer *border_shader_node;
  struct wlr_buffer *border_shader_buf;
  GLuint border_shader_buf_fbo;
  int border_shader_buf_w;
  int border_shader_buf_h;
  bool border_dirty;
  float border_color[4];

  // corner mask
  struct wlr_scene_buffer *corner_mask_node;
  struct wlr_buffer *corner_mask_buf;
  GLuint corner_mask_buf_fbo;

  struct wlr_ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel;
  struct wlr_foreign_toplevel_handle_v1 *foreign_toplevel;

  char *foreign_identifier;

  struct wlr_ext_image_capture_source_v1 *image_capture_source;
  struct wlr_scene_surface *image_capture_surface;
  struct wlr_scene *image_capture;
  struct wlr_scene_tree *image_capture_tree;

  struct wlr_scene_tree *border_tree;
  struct wlr_scene_rect *border_rects[4];

  node_t *node;

  struct wlr_box geometry;  // Client-committed surface geometry
  struct wlr_box last_configured_size;  // Last size sent via configure

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
void toplevel_center_and_clip_surface(struct bwm_toplevel *toplevel);

// buffer saving for transactions
void toplevel_save_buffer(struct bwm_toplevel *toplevel);
void toplevel_remove_saved_buffer(struct bwm_toplevel *toplevel);
void toplevel_send_frame_done(struct bwm_toplevel *toplevel);

void toplevel_freeze_sibling_buffers(desktop_t *d, node_t *n);

void handle_new_toplevel_capture_request(struct wl_listener *listener, void *data);

void toplevel_set_blur(struct bwm_toplevel *tl, bool enabled);
void toplevel_set_mica(struct bwm_toplevel *tl, bool enabled);
void toplevel_set_acrylic(struct bwm_toplevel *tl, bool enabled);
void toplevel_set_border_radius(struct bwm_toplevel *tl, float radius);
