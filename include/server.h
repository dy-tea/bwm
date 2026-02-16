#pragma once

#define WLR_USE_UNSTABLE
#include "toplevel.h"
#include "types.h"
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_ext_workspace_v1.h>

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

  struct wlr_scene_tree *tile_tree;
  struct wlr_scene_tree *float_tree;
  struct wlr_scene_tree *fullscreen_tree;

  struct wlr_xdg_shell *xdg_shell;
  struct wl_listener new_xdg_toplevel;
  struct wl_listener new_xdg_popup;
  struct wl_list toplevels;

  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_mgr;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_absolute;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;

  struct wlr_seat *seat;
  struct wl_listener new_input;
  struct wl_listener request_cursor;
  struct wl_listener pointer_focus_change;
  struct wl_listener request_set_selection;
  struct wl_list keyboards;

  struct wlr_output_layout *output_layout;
  struct wl_list outputs;
  struct wl_listener new_output;

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
