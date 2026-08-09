#pragma once

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

typedef struct node_t node_t;
typedef struct desktop_t desktop_t;

#define TAB_BAR_HEIGHT 22
#define TAB_BAR_BORDER 1

typedef struct tab_entry_t {
	node_t *leaf;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *bg;
	struct wlr_scene_rect *border;
	struct text_node_t *label;
	struct wlr_box hit_box;
} tab_entry_t;

typedef struct tab_bar_t {
	node_t *owner;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *bg;
	tab_entry_t *entries;
	size_t entry_count;
	struct wlr_box rect;
} tab_bar_t;

extern float color_bar_bg[4];
extern float color_tab_bg[4];
extern float color_tab_bg_active[4];
extern float color_tab_text[4];
extern float color_tab_text_active[4];
extern float color_tab_sep[4];

size_t tabbed_leaf_count(node_t *n);
int tab_bar_height(node_t *n);

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

node_t *tabs_hit_test(const node_t *n, double lx, double ly);
node_t *tabs_hit_test_desktop(desktop_t *d, double lx, double ly);

void tabs_show(node_t *n, bool show);
