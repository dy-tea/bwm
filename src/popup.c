#define WLR_USE_UNSTABLE
#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include "toplevel.h"
#include "popup.h"

void popup_unconstrain(struct bwm_popup *popup) {
    int lx, ly;
    wlr_scene_node_coords(&popup->parent_tree->node.parent->node, &lx, &ly);

    // TODO: get workspace and unconstrain from box
}

void popup_commit(struct wl_listener *listener, void *data) {
    struct bwm_popup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit)
        return;

    popup_unconstrain(popup);
}

void popup_reposition(struct wl_listener *listener, void *data) {
    struct bwm_popup *popup = wl_container_of(listener, popup, commit);
    popup_unconstrain(popup);
}

void popup_destroy(struct wl_listener *listener, void *data) {
    struct bwm_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->new_popup.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg_popup = data;
    struct bwm_popup *popup = calloc(1, sizeof(struct bwm_popup));
    struct bwm_toplevel *toplevel = wl_container_of(listener, toplevel, new_xdg_popup);

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
