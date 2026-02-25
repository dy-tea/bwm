#pragma once

#include "lock.h"
#include "toplevel.h"
#include "types.h"
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

enum cursor_mode {
  CURSOR_PASSTHROUGH,
  CURSOR_MOVE,
  CURSOR_RESIZE,
};

struct bwm_server {
  struct wl_display *wl_display;
  struct wlr_backend *backend;
  struct wlr_session *session;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;

  struct wlr_linux_dmabuf_v1 *linux_dmabuf;

  struct wlr_scene_tree *bg_tree;
  struct wlr_scene_tree *bot_tree;
  struct wlr_scene_tree *tile_tree;
  struct wlr_scene_tree *float_tree;
  struct wlr_scene_tree *top_tree;
  struct wlr_scene_tree *full_tree;
  struct wlr_scene_tree *over_tree;
  struct wlr_scene_tree *drag_tree;
  struct wlr_scene_tree *lock_tree;

  struct wlr_xdg_shell *xdg_shell;
  struct wlr_xdg_activation_v1 *xdg_activation_v1;
  struct wl_listener xdg_activation_request_activate;
  struct wlr_layer_shell_v1 *layer_shell;
  struct wl_listener new_layer_surface;
  struct wl_listener new_xdg_toplevel;
  struct wl_list toplevels;

  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_mgr;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_absolute;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;

  struct wlr_pointer_constraints_v1 *pointer_constraints;
  struct wlr_pointer_constraint_v1 *active_pointer_constraint;
  bool cursor_requires_warp;
  pixman_region32_t pointer_confine;
  struct wl_listener new_pointer_constraint;
  struct wl_listener pointer_constraint_commit;

  struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
  struct wl_listener cursor_request_set_shape;

  struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
  struct wlr_idle_notifier_v1 *idle_notifier;

  struct wlr_seat *seat;
  struct wl_listener new_input;
  struct wl_listener request_cursor;
  struct wl_listener pointer_focus_change;
  struct wl_listener request_set_selection;
  struct wl_listener request_start_drag;
  struct wl_listener start_drag;
  struct wl_list keyboards;
  struct wl_list pointers;

  struct wlr_output_layout *output_layout;
  struct wl_list outputs;
  struct wl_listener new_output;

  struct wlr_output_power_manager_v1 *output_power_manager;
  struct wl_listener output_power_set_mode;

  struct wlr_output_manager_v1 *output_manager;
  struct wl_listener output_manager_apply;
  struct wl_listener output_manager_test;

  struct wlr_session_lock_manager_v1 *session_lock_manager;
  struct wl_listener new_session_lock;
  struct wlr_scene_rect *lock_background;
  struct wlr_session_lock_v1 *current_session_lock;
  bool locked;

  struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
  struct wl_listener new_idle_inhibitor;

  struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
  struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
  struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *foreign_toplevel_image_capture_source_manager;
  struct wl_listener new_toplevel_capture_request;

  // cursor state
  enum cursor_mode cursor_mode;
  struct bwm_toplevel *grabbed_toplevel;
  double grab_x, grab_y;
  struct wlr_box grab_geobox;
  uint32_t resize_edges;

  // bspwm integration
  monitor_t *monitors;
  monitor_t *focused_monitor;

  // workspace tracking
  struct wlr_ext_workspace_manager_v1 *workspace_manager;
  struct wl_listener workspace_commit;
};

extern struct bwm_server server;

void begin_interactive(struct bwm_toplevel *toplevel, enum cursor_mode mode, uint32_t edges);

void server_init(void);
int server_run(void);
void server_fini(void);
