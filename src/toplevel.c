#include "toplevel.h"
#include "keyboard.h"
#include "popup.h"
#include "server.h"
#include "tree.h"
#include "transaction.h"
#include <string.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

extern struct bwm_server server;

void toplevel_map(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, map);

  wlr_log(WLR_INFO, "Toplevel mapped");

  toplevel->mapped = true;
  toplevel->configured = false;

  monitor_t *m = server.focused_monitor;
  if (m == NULL) {
    wlr_log(WLR_ERROR, "No monitor available for toplevel");
    return;
  }

  desktop_t *d = m->desk;
  if (d == NULL) {
    wlr_log(WLR_ERROR, "No desktop available for toplevel");
    return;
  }

  node_t *n = make_node(0);
  if (n == NULL) {
    wlr_log(WLR_ERROR, "Failed to create node for toplevel");
    return;
  }

  n->client = make_client();
  if (n->client == NULL) {
    wlr_log(WLR_ERROR, "Failed to create client for toplevel");
    free_node(n);
    return;
  }

  // link client and toplevel
  n->client->toplevel = toplevel;
  toplevel->node = n;

  // set initial app_id and title
  const char *app_id = toplevel->xdg_toplevel->app_id;
  const char *title = toplevel->xdg_toplevel->title;

  if (app_id) {
    strncpy(n->client->app_id, app_id, MAXLEN - 1);
    n->client->app_id[MAXLEN - 1] = '\0';
  }

  if (title) {
    strncpy(n->client->title, title, MAXLEN - 1);
    n->client->title[MAXLEN - 1] = '\0';
  }

  wlr_log(WLR_INFO, "New window: %s (%s)", title ? title : "untitled",
          app_id ? app_id : "unknown");

  // insert node into tree
  node_t *focus = d->focus;
  insert_node(m, d, n, focus);

  focus_node(m, d, n);
  arrange(m, d, true);

  wlr_log(WLR_INFO, "Window mapped and tiled: %s",
          n->client->title[0] ? n->client->title : "untitled");
}

void toplevel_unmap(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

  wlr_log(WLR_INFO, "Toplevel unmapped");

  toplevel->mapped = false;
  toplevel->configured = false;

  if (toplevel->node && toplevel->node->client && toplevel->node->client->shown) {
    toplevel_save_buffer(toplevel);
    wlr_log(WLR_DEBUG, "Saved buffer for unmapping toplevel to prevent gap");
  }

  if (toplevel->node == NULL)
    return;

  node_t *n = toplevel->node;
  monitor_t *m = mon;
  desktop_t *d = m ? m->desk : NULL;

  if (m && d) {
    if (n)
      node_set_dirty(n);

    remove_node(m, d, n);

    if (n)
      n->destroying = true;

    arrange(m, d, true);

    // focus handling after removing node
    if (d->focus != NULL && d->focus->client != NULL &&
        d->focus->client->toplevel != NULL)
      focus_toplevel(d->focus->client->toplevel);
    else if (d->root != NULL) {
      d->focus = first_extrema(d->root);
      if (d->focus && d->focus->client && d->focus->client->toplevel)
        focus_toplevel(d->focus->client->toplevel);
    }
  }
}

void toplevel_commit(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

  if (toplevel->xdg_toplevel->base->initial_commit) {
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);

    wlr_xdg_toplevel_set_wm_capabilities(toplevel->xdg_toplevel,
        WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
        WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
    return;
  }

  if (toplevel->mapped && toplevel->xdg_toplevel->base->surface->mapped) {
    uint32_t serial = toplevel->xdg_toplevel->base->current.configure_serial;
    bool successful = transaction_notify_view_ready_by_serial(toplevel, serial);

    if (successful) {
      toplevel->configured = true;
      wlr_log(WLR_DEBUG, "Transaction completed for serial=%u", serial);
    }

    if (toplevel->saved_surface_tree && !successful)
      toplevel_send_frame_done(toplevel);
  }
}

void toplevel_destroy(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

  wlr_log(WLR_INFO, "Toplevel destroyed");

  if (toplevel->node && toplevel->node->client) {
    toplevel->node->client->toplevel = NULL;
    toplevel->node = NULL;
  }

  wl_list_remove(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_remove(&toplevel->commit.link);
  wl_list_remove(&toplevel->new_xdg_popup.link);
  wl_list_remove(&toplevel->destroy.link);
  wl_list_remove(&toplevel->request_move.link);
  wl_list_remove(&toplevel->request_resize.link);
  wl_list_remove(&toplevel->request_maximize.link);
  wl_list_remove(&toplevel->request_fullscreen.link);
  wl_list_remove(&toplevel->set_title.link);
  wl_list_remove(&toplevel->set_app_id.link);
  wl_list_remove(&toplevel->link);

  free(toplevel);
}

void toplevel_request_move(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_move);
  wlr_log(WLR_DEBUG, "Toplevel requested move");
  if (toplevel->node && toplevel->node->client && toplevel->node->client->state == STATE_FLOATING)
      begin_interactive(toplevel, CURSOR_MOVE, 0);
  else if (toplevel->xdg_toplevel->base->initialized)
      wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void toplevel_request_resize(struct wl_listener *listener, void *data) {
  struct wlr_xdg_toplevel_resize_event *event = data;
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_resize);
  wlr_log(WLR_DEBUG, "Toplevel requested resize");
  if (toplevel->node && toplevel->node->client && toplevel->node->client->state == STATE_FLOATING)
      begin_interactive(toplevel, CURSOR_RESIZE, event->edges);
  else if (toplevel->xdg_toplevel->base->initialized)
      wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_maximize);

  if (toplevel->node && toplevel->node->client)
      toggle_monocle();
}

void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_fullscreen);
  struct wlr_xdg_toplevel_requested *event = data;

  if (toplevel->node == NULL || toplevel->node->client == NULL)
    return;

  monitor_t *m = mon;
  desktop_t *d = m ? m->desk : NULL;

  if (event->fullscreen) {
    set_state(m, d, toplevel->node, STATE_FULLSCREEN);
    wlr_scene_node_reparent(&toplevel->scene_tree->node, server.full_tree);
  } else {
    set_state(m, d, toplevel->node, STATE_TILED);
    wlr_scene_node_reparent(&toplevel->scene_tree->node, server.tile_tree);
  }

  wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, event->fullscreen);
}

void toplevel_set_title(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, set_title);

  if (toplevel->node && toplevel->node->client) {
    const char *title = toplevel->xdg_toplevel->title;
    if (title) {
      strncpy(toplevel->node->client->title, title, MAXLEN - 1);
      toplevel->node->client->title[MAXLEN - 1] = '\0';
      wlr_log(WLR_DEBUG, "Toplevel title changed: %s", title);
    }
  }
}

void toplevel_set_app_id(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, set_app_id);

  if (toplevel->node && toplevel->node->client) {
    const char *app_id = toplevel->xdg_toplevel->app_id;
    if (app_id) {
      strncpy(toplevel->node->client->app_id, app_id, MAXLEN - 1);
      toplevel->node->client->app_id[MAXLEN - 1] = '\0';
      wlr_log(WLR_DEBUG, "Toplevel app_id changed: %s", app_id);
    }
  }
}

void focus_toplevel(struct bwm_toplevel *toplevel) {
  if (toplevel == NULL || toplevel->xdg_toplevel == NULL)
    return;

  struct wlr_seat *seat = server.seat;
  struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

  if (surface == NULL)
    return;

  struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

  if (prev_surface == surface)
    return;

  if (prev_surface != NULL) {
    struct wlr_xdg_toplevel *prev_toplevel =
        wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
    if (prev_toplevel != NULL)
      wlr_xdg_toplevel_set_activated(prev_toplevel, false);
  }

  wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

  if (seat->keyboard_state.keyboard != NULL)
    wlr_seat_keyboard_notify_enter(seat, surface,
                                   seat->keyboard_state.keyboard->keycodes,
                                   seat->keyboard_state.keyboard->num_keycodes,
                                   &seat->keyboard_state.keyboard->modifiers);
}

void toplevel_apply_geometry(struct bwm_toplevel *toplevel) {
  if (toplevel == NULL || toplevel->node == NULL ||
      toplevel->node->client == NULL)
    return;

  client_t *c = toplevel->node->client;
  struct wlr_box *rect;

  if (c->state == STATE_FULLSCREEN) {
    monitor_t *m = mon;
    if (m)
      rect = &m->rectangle;
    else return;
  }
  else if (c->state == STATE_FLOATING) rect = &c->floating_rectangle;
  else rect = &c->tiled_rectangle;

  wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, rect->width, rect->height);
  wlr_scene_node_set_position(&toplevel->scene_tree->node, rect->x, rect->y);

  wlr_log(WLR_DEBUG, "Applied geometry: %dx%d at %d,%d", rect->width,
          rect->height, rect->x, rect->y);
}

void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_xdg_toplevel *xdg_toplevel = data;

  wlr_log(WLR_INFO, "New XDG toplevel");

  struct bwm_toplevel *toplevel = calloc(1, sizeof(struct bwm_toplevel));

  toplevel->xdg_toplevel = xdg_toplevel;
  toplevel->mapped = false;
  toplevel->configured = false;

  // create parent scene tree container
  toplevel->scene_tree = wlr_scene_tree_create(server.tile_tree);
  if (!toplevel->scene_tree) {
    wlr_log(WLR_ERROR, "Failed to create scene tree for toplevel");
    free(toplevel);
    return;
  }

  // create content tree as child and add surface
  toplevel->content_tree = wlr_scene_tree_create(toplevel->scene_tree);
  if (!toplevel->content_tree) {
    wlr_log(WLR_ERROR, "Failed to create content tree for toplevel");
    wlr_scene_node_destroy(&toplevel->scene_tree->node);
    free(toplevel);
    return;
  }

  // create surface scene within the content tree
  struct wlr_scene_tree *xdg_tree =
      wlr_scene_xdg_surface_create(toplevel->content_tree, xdg_toplevel->base);
  if (!xdg_tree) {
    wlr_log(WLR_ERROR, "Failed to create XDG surface scene for toplevel");
    wlr_scene_node_destroy(&toplevel->scene_tree->node);
    free(toplevel);
    return;
  }

  toplevel->scene_tree->node.data = toplevel;
  xdg_toplevel->base->data = toplevel->scene_tree;

  wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

  // register event listeners
  toplevel->map.notify = toplevel_map;
  wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);

  toplevel->unmap.notify = toplevel_unmap;
  wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);

  toplevel->commit.notify = toplevel_commit;
  wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

  toplevel->new_xdg_popup.notify = handle_new_xdg_popup;
  wl_signal_add(&xdg_toplevel->base->events.new_popup, &toplevel->new_xdg_popup);

  toplevel->destroy.notify = toplevel_destroy;
  wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

  toplevel->request_move.notify = toplevel_request_move;
  wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);

  toplevel->request_resize.notify = toplevel_request_resize;
  wl_signal_add(&xdg_toplevel->events.request_resize,
                &toplevel->request_resize);

  toplevel->request_maximize.notify = toplevel_request_maximize;
  wl_signal_add(&xdg_toplevel->events.request_maximize,
                &toplevel->request_maximize);

  toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
  wl_signal_add(&xdg_toplevel->events.request_fullscreen,
                &toplevel->request_fullscreen);

  toplevel->set_title.notify = toplevel_set_title;
  wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

  toplevel->set_app_id.notify = toplevel_set_app_id;
  wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

  // add to toplevels list
  wl_list_insert(&server.toplevels, &toplevel->link);
}

bool toplevel_is_ready(struct bwm_toplevel *toplevel) {
  return toplevel &&
         toplevel->mapped &&
         toplevel->xdg_toplevel &&
         toplevel->xdg_toplevel->base &&
         toplevel->xdg_toplevel->base->surface &&
         toplevel->xdg_toplevel->base->surface->mapped;
}

static int buffer_copy_count = 0;

static void save_buffer_iterator(struct wlr_scene_buffer *buffer,
                                 int sx, int sy, void *data) {
  struct wlr_scene_tree *tree = data;

  buffer_copy_count++;
  wlr_log(WLR_DEBUG, "save_buffer_iterator called: buffer=%p, sx=%d, sy=%d",
          (void*)buffer, sx, sy);

  // ignore buffers with no content
  if (!buffer->buffer) {
    wlr_log(WLR_DEBUG, "Skipping buffer with no content");
    return;
  }

  struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(tree, NULL);
  if (!sbuf) {
    wlr_log(WLR_ERROR, "Could not allocate a scene buffer when saving a surface");
    return;
  }

  wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
  wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
  wlr_scene_buffer_set_source_box(sbuf, &buffer->src_box);
  wlr_scene_node_set_position(&sbuf->node, sx, sy);
  wlr_scene_buffer_set_transform(sbuf, buffer->transform);
  wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);

  wlr_log(WLR_DEBUG, "Successfully copied buffer %dx%d at (%d,%d)",
          buffer->dst_width, buffer->dst_height, sx, sy);
}

void toplevel_save_buffer(struct bwm_toplevel *toplevel) {
  if (!toplevel || !toplevel->scene_tree || !toplevel->content_tree)
    return;

  // removed saved buffer
  if (toplevel->saved_surface_tree) {
    wlr_log(WLR_DEBUG, "Removing existing saved buffer before saving new one");
    toplevel_remove_saved_buffer(toplevel);
  }

  toplevel->saved_surface_tree = wlr_scene_tree_create(toplevel->scene_tree);
  if (!toplevel->saved_surface_tree) {
    wlr_log(WLR_ERROR, "Could not allocate a scene tree node when saving a surface");
    return;
  }

  wlr_scene_node_set_enabled(&toplevel->saved_surface_tree->node, false);

  // copy scene buffers
  buffer_copy_count = 0;
  wlr_log(WLR_DEBUG, "Starting buffer iteration for content_tree=%p",
          (void*)toplevel->content_tree);

  wlr_scene_node_for_each_buffer(&toplevel->content_tree->node,
                                 save_buffer_iterator,
                                 toplevel->saved_surface_tree);

  wlr_log(WLR_DEBUG, "Buffer iteration complete, copied %d buffers", buffer_copy_count);

  bool has_children = !wl_list_empty(&toplevel->saved_surface_tree->children);
  wlr_log(WLR_DEBUG, "After iteration: saved_surface_tree has_children=%d", has_children);

  if (!has_children) {
    // cleanup
    wlr_scene_node_destroy(&toplevel->saved_surface_tree->node);
    toplevel->saved_surface_tree = NULL;
    wlr_log(WLR_DEBUG, "No buffers to save for toplevel - destroyed saved tree");
  } else {
    wlr_scene_node_set_enabled(&toplevel->content_tree->node, false);
    wlr_scene_node_set_enabled(&toplevel->saved_surface_tree->node, true);
    wlr_log(WLR_DEBUG, "Saved buffer for toplevel - swapped content_tree for saved_surface_tree");
  }
}

void toplevel_remove_saved_buffer(struct bwm_toplevel *toplevel) {
  if (!toplevel || !toplevel->saved_surface_tree)
    return;

  wlr_log(WLR_DEBUG, "Removing saved buffer for toplevel");

  wlr_scene_node_destroy(&toplevel->saved_surface_tree->node);
  toplevel->saved_surface_tree = NULL;

  if (toplevel->content_tree)
    wlr_scene_node_set_enabled(&toplevel->content_tree->node, true);
}

static void send_frame_done_iterator(struct wlr_scene_buffer *scene_buffer,
                                     int x, int y, void *data) {
  struct timespec *when = data;
  struct wlr_scene_surface *scene_surface =
      wlr_scene_surface_try_from_buffer(scene_buffer);
  if (scene_surface == NULL)
    return;
  wlr_surface_send_frame_done(scene_surface->surface, when);
}

void toplevel_send_frame_done(struct bwm_toplevel *toplevel) {
  if (!toplevel || !toplevel->content_tree)
    return;

  struct timespec when;
  clock_gettime(CLOCK_MONOTONIC, &when);

  struct wlr_scene_node *node;
  wl_list_for_each(node, &toplevel->content_tree->children, link)
    wlr_scene_node_for_each_buffer(node, send_frame_done_iterator, &when);
}
