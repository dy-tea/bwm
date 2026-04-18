#pragma once

#include "types.h"
#include <stdbool.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#define TAB_BAR_HEIGHT 22
#define TAB_BAR_BORDER 1

struct bwm_tab_entry {
  node_t *leaf;
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *bg;
  struct wlr_scene_rect *border;
  struct bwm_text_node *label;
  struct wlr_box hit_box;
};

struct bwm_tab_bar {
  node_t *owner;
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *bg;
  struct bwm_tab_entry *entries;
  size_t entry_count;
  struct wlr_box rect;
};

int tab_bar_height(void);

void tabs_create(node_t *n);
void tabs_destroy(node_t *n);

void tabs_rebuild(node_t *n);
void tabs_arrange(node_t *n, struct wlr_box rect);
void tabs_update_focus(node_t *n, node_t *focus);
void tabs_update_label_for_leaf(node_t *leaf);

node_t *tabbed_ancestor(node_t *n);
node_t *tab_focus_leaf(node_t *tabbed_node, node_t *focus);
node_t *tab_next_leaf(node_t *tabbed_node, node_t *focus);
node_t *tab_prev_leaf(node_t *tabbed_node, node_t *focus);

node_t *tabs_hit_test(node_t *n, double lx, double ly);
node_t *tabs_hit_test_desktop(struct desktop_t *d, double lx, double ly);

void tabs_show(node_t *n, bool show);
