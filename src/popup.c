#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include "popup.h"
#include "server.h"
#include "layer.h"
#include "output.h"
#include "toplevel.h"

static void create_xdg_popup(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree);

void popup_unconstrain(struct bwm_popup *popup) {
	int lx, ly;
  wlr_scene_node_coords(&popup->parent_tree->node.parent->node, &lx, &ly);

  struct wlr_output *wlr_output = wlr_output_layout_output_at(server.output_layout, lx, ly);
  if (wlr_output == NULL)
  	return;

  struct bwm_output *output = wlr_output->data;
  if (output == NULL)
  	return;

  struct wlr_box box = {
    output->rectangle.x - lx,
    output->rectangle.y - ly,
    output->rectangle.width,
    output->rectangle.height,
  };

  // unconstrain the popup from the relative box
  wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
}

void popup_new_popup(struct wl_listener *listener, void *data) {
  struct bwm_popup *popup = wl_container_of(listener, popup, new_popup);
  create_xdg_popup(data, popup->parent_tree);
}

void popup_commit(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_popup *popup = wl_container_of(listener, popup, commit);

  if (!popup->xdg_popup->base->initial_commit)
    return;

  popup_unconstrain(popup);
}

void popup_reposition(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_popup *popup = wl_container_of(listener, popup, reposition);
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

static void create_xdg_popup(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree) {
  struct bwm_popup *popup = calloc(1, sizeof(struct bwm_popup));
  popup->xdg_popup = xdg_popup;
  popup->parent_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
  xdg_popup->base->data = popup->parent_tree;

  popup->commit.notify = popup_commit;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

  popup->new_popup.notify = popup_new_popup;
  wl_signal_add(&xdg_popup->base->events.new_popup, &popup->new_popup);

  popup->reposition.notify = popup_reposition;
  wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);

  popup->destroy.notify = popup_destroy;
  wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, new_xdg_popup);
  create_xdg_popup(xdg_popup, toplevel->scene_tree);
}

void handle_new_layer_popup(struct wl_listener *listener, void *data) {
  struct wlr_xdg_popup *xdg_popup = data;
  struct bwm_layer_surface *layer = wl_container_of(listener, layer, new_popup);
  create_xdg_popup(xdg_popup, layer->scene_tree);
}
