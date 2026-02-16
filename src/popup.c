#define WLR_USE_UNSTABLE
#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include "popup.h"

void popup_unconstrain(struct bwm_popup *popup) {
  struct wlr_scene_tree *parent_scene_tree = popup->parent_tree;
  if (parent_scene_tree == NULL)
    return;

  struct wlr_surface *parent_surface = popup->xdg_popup->parent;
  if (parent_surface == NULL)
    return;

  struct wlr_xdg_surface *parent_xdg_surface = wlr_xdg_surface_try_from_wlr_surface(parent_surface);
  if (parent_xdg_surface == NULL)
    return;

  struct wlr_box box = {
    .x = -parent_xdg_surface->current.geometry.x,
    .y = -parent_xdg_surface->current.geometry.y,
    .width = parent_xdg_surface->current.geometry.width,
    .height = parent_xdg_surface->current.geometry.height,
  };

  wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
}

void popup_commit(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_popup *popup = wl_container_of(listener, popup, commit);

  if (popup->xdg_popup->base->initial_commit)
    return;

  popup_unconstrain(popup);
}

void popup_reposition(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_popup *popup = wl_container_of(listener, popup, commit);
  popup_unconstrain(popup);
}

void popup_destroy(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_popup *popup = wl_container_of(listener, popup, destroy);

  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->reposition.link);
  wl_list_remove(&popup->new_popup.link);
  wl_list_remove(&popup->destroy.link);

  free(popup);
}

void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
	(void)listener;
  struct wlr_xdg_popup *xdg_popup = data;
  struct bwm_popup *popup = calloc(1, sizeof(struct bwm_popup));

  popup->xdg_popup = xdg_popup;

  popup->parent_tree = wlr_scene_xdg_surface_create(popup->parent_tree, xdg_popup->base);
  popup->xdg_popup->base->data = popup->parent_tree;

  // listeners
  popup->commit.notify = popup_commit;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

  popup->new_popup.notify = handle_new_xdg_popup;
  wl_signal_add(&xdg_popup->base->events.new_popup, &popup->new_popup);

  popup->reposition.notify = popup_reposition;
  wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);

  popup->destroy.notify = popup_destroy;
  wl_signal_add(&xdg_popup->base->events.destroy, &popup->destroy);
}
