#define WLR_USE_UNSTABLE
#include "toplevel.h"
#include "keyboard.h"
#include "server.h"
#include "tree.h"
#include "transaction.h"
#include <string.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

extern struct bwm_server server;

void toplevel_map(struct wl_listener *listener, void *data) {
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, map);

  wlr_log(WLR_INFO, "Toplevel mapped");

  toplevel->mapped = true;
  toplevel->configured = false;

  monitor_t *m = mon;
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

  arrange(m, d);
  focus_node(m, d, n);

  // Don't set shown = true yet - wait for transaction to complete
  wlr_log(WLR_INFO, "Window mapped and tiled: %s",
          n->client->title[0] ? n->client->title : "untitled");
}

void toplevel_unmap(struct wl_listener *listener, void *data) {
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

  wlr_log(WLR_INFO, "Toplevel unmapped");

  toplevel->mapped = false;
  toplevel->configured = false;

  // Disable the scene node immediately to hide it
  wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

  if (toplevel->node == NULL)
    return;

  node_t *n = toplevel->node;
  monitor_t *m = mon;
  desktop_t *d = m ? m->desk : NULL;

  if (m && d) {
    remove_node(m, d, n);
    arrange(m, d);

    if (d->focus != NULL && d->focus->client != NULL &&
        d->focus->client->toplevel != NULL)
      focus_toplevel(d->focus->client->toplevel);
    else if (d->root != NULL) {
      d->focus = first_extrema(d->root);
      if (d->focus && d->focus->client && d->focus->client->toplevel)
        focus_toplevel(d->focus->client->toplevel);
    }
  }

  toplevel->node = NULL;
}

void toplevel_commit(struct wl_listener *listener, void *data) {
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

  if (toplevel->xdg_toplevel->base->initial_commit) {
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    return;
  }

  if (toplevel->mapped && toplevel->xdg_toplevel->base->surface->mapped) {
    uint32_t serial = toplevel->xdg_toplevel->base->current.configure_serial;
    if (transaction_notify_view_ready_by_serial(toplevel, serial))
      toplevel->configured = true;
  }
}

void toplevel_destroy(struct wl_listener *listener, void *data) {
  struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

  wlr_log(WLR_INFO, "Toplevel destroyed");

  wl_list_remove(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_remove(&toplevel->commit.link);
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
  // handle floating window move or swap if moved over other tiling toplevel in tiling mode
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_move);
  wlr_log(WLR_DEBUG, "Toplevel requested move");
  if (toplevel->xdg_toplevel->base->initialized)
      wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void toplevel_request_resize(struct wl_listener *listener, void *data) {
  struct bwm_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_resize);
  wlr_log(WLR_DEBUG, "Toplevel requested resize");
  if (toplevel->xdg_toplevel->base->initialized)
      wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void toplevel_request_maximize(struct wl_listener *listener, void *data) {
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

  if (event->fullscreen)
    set_state(m, d, toplevel->node, STATE_FULLSCREEN);
  else
    set_state(m, d, toplevel->node, STATE_TILED);

  wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, event->fullscreen);
}

void toplevel_set_title(struct wl_listener *listener, void *data) {
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
  struct wlr_xdg_toplevel *xdg_toplevel = data;

  wlr_log(WLR_INFO, "New XDG toplevel");

  struct bwm_toplevel *toplevel = calloc(1, sizeof(struct bwm_toplevel));

  toplevel->xdg_toplevel = xdg_toplevel;
  toplevel->mapped = false;
  toplevel->configured = false;

  toplevel->scene_tree =
      wlr_scene_xdg_surface_create(&server.scene->tree, xdg_toplevel->base);
  if (!toplevel->scene_tree) {
    wlr_log(WLR_ERROR, "Failed to create scene tree for toplevel");
    free(toplevel);
    return;
  }

  toplevel->scene_tree->node.data = toplevel;
  xdg_toplevel->base->data = toplevel->scene_tree;

  wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

  toplevel->node = NULL;

  // register event listeners
  toplevel->map.notify = toplevel_map;
  wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);

  toplevel->unmap.notify = toplevel_unmap;
  wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);

  toplevel->commit.notify = toplevel_commit;
  wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

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
