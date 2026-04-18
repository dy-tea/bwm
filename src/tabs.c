#include "tabs.h"

#include "server.h"
#include "text.h"
#include "tree.h"
#include "types.h"
#include "xwayland.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

extern struct bwm_server server;

static const float color_bar_bg[4] = {0.10f, 0.10f, 0.12f, 1.0f};
static const float color_tab_bg[4] = {0.18f, 0.18f, 0.20f, 1.0f};
static const float color_tab_bg_active[4] = {0.09f, 0.58f, 0.87f, 1.0f};
static const float color_tab_text[4] = {0.85f, 0.85f, 0.88f, 1.0f};
static const float color_tab_text_active[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static const float color_tab_sep[4] = {0.0f, 0.0f, 0.0f, 1.0f};

int tab_bar_height(void) {
	return TAB_BAR_HEIGHT;
}

node_t *tabbed_ancestor(node_t *n) {
  for (node_t *p = n ? n->parent : NULL; p != NULL; p = p->parent)
    if (p->split_type == TYPE_TABBED)
      return p;
  return NULL;
}

static size_t collect_leaves(node_t *n, node_t **out, size_t cap) {
  if (n == NULL)
    return 0;

  if (is_leaf(n)) {
    if (n->client == NULL || n->client->state == STATE_FLOATING)
      return 0;
    if (out && cap > 0)
      out[0] = n;
    return 1;
  }

  size_t used = 0;
  used += collect_leaves(n->first_child, out ? out + used : NULL,
    cap > used ? cap - used : 0);
  used += collect_leaves(n->second_child, out ? out + used : NULL,
    cap > used ? cap - used : 0);
  return used;
}

static size_t count_leaves(node_t *n) {
	return collect_leaves(n, NULL, 0);
}

node_t *tab_focus_leaf(node_t *tabbed_node, node_t *focus) {
  if (tabbed_node == NULL)
    return NULL;

  // prefer focus if inside this group
  if (focus != NULL) {
    for (node_t *p = focus; p != NULL; p = p->parent)
      if (p == tabbed_node)
        return focus;
  }

  // pick first leaf in group
  node_t *first = NULL;
  collect_leaves(tabbed_node, &first, 1);
  return first;
}

node_t *tab_next_leaf(node_t *tabbed_node, node_t *focus) {
  if (tabbed_node == NULL)
    return NULL;
  size_t n = count_leaves(tabbed_node);
  if (n == 0)
    return NULL;
  node_t **arr = calloc(n, sizeof(node_t *));
  if (!arr)
    return NULL;
  collect_leaves(tabbed_node, arr, n);
  node_t *cur = tab_focus_leaf(tabbed_node, focus);
  size_t idx = 0;
  for (size_t i = 0; i < n; i++)
    if (arr[i] == cur) { idx = i; break; }
  node_t *res = arr[(idx + 1) % n];
  free(arr);
  return res;
}

node_t *tab_prev_leaf(node_t *tabbed_node, node_t *focus) {
  if (tabbed_node == NULL)
    return NULL;
  size_t n = count_leaves(tabbed_node);
  if (n == 0)
    return NULL;
  node_t **arr = calloc(n, sizeof(node_t *));
  if (!arr)
    return NULL;
  collect_leaves(tabbed_node, arr, n);
  node_t *cur = tab_focus_leaf(tabbed_node, focus);
  size_t idx = 0;
  for (size_t i = 0; i < n; i++)
    if (arr[i] == cur) { idx = i; break; }
  node_t *res = arr[(idx + n - 1) % n];
  free(arr);
  return res;
}

static const char *leaf_label(node_t *leaf) {
  if (leaf == NULL || leaf->client == NULL)
    return "?";
  if (leaf->client->title[0] != '\0')
    return leaf->client->title;
  if (leaf->client->app_id[0] != '\0')
    return leaf->client->app_id;
  return "(untitled)";
}

static void destroy_entries(struct bwm_tab_bar *bar) {
  if (bar == NULL || bar->entries == NULL)
    return;
  for (size_t i = 0; i < bar->entry_count; i++)
    if (bar->entries[i].tree)
      wlr_scene_node_destroy(&bar->entries[i].tree->node);
  free(bar->entries);
  bar->entries = NULL;
  bar->entry_count = 0;
}

static void build_entries(struct bwm_tab_bar *bar) {
  destroy_entries(bar);

  size_t count = count_leaves(bar->owner);
  if (count == 0)
    return;

  bar->entries = calloc(count, sizeof(struct bwm_tab_entry));
  if (!bar->entries)
    return;

  node_t **leaves = calloc(count, sizeof(node_t *));
  if (!leaves)
    return;
  collect_leaves(bar->owner, leaves, count);

  for (size_t i = 0; i < count; i++) {
    struct bwm_tab_entry *e = &bar->entries[i];
    e->leaf = leaves[i];

    e->tree = wlr_scene_tree_create(bar->tree);
    if (!e->tree)
      continue;

    e->bg = wlr_scene_rect_create(e->tree, 1, 1, color_tab_bg);
    e->border = wlr_scene_rect_create(e->tree, 1, TAB_BAR_BORDER, color_tab_sep);

    float text_color[4];
    memcpy(text_color, color_tab_text, sizeof(text_color));
    e->label = bwm_text_node_create(e->tree, leaf_label(leaves[i]), text_color, false);
  }

  free(leaves);
  bar->entry_count = count;
}

void tabs_create(node_t *n) {
  if (n == NULL)
    return;
  if (n->tab_bar != NULL)
    return;

  struct bwm_tab_bar *bar = calloc(1, sizeof(*bar));
  if (!bar)
    return;
  bar->owner = n;

  bar->tree = wlr_scene_tree_create(server.tile_tree);
  if (!bar->tree) {
    free(bar);
    return;
  }
  bar->bg = wlr_scene_rect_create(bar->tree, 1, 1, color_bar_bg);

  n->tab_bar = bar;
  build_entries(bar);
}

void tabs_destroy(node_t *n) {
  if (n == NULL || n->tab_bar == NULL)
    return;
  struct bwm_tab_bar *bar = n->tab_bar;
  destroy_entries(bar);
  if (bar->tree)
    wlr_scene_node_destroy(&bar->tree->node);
  free(bar);
  n->tab_bar = NULL;
}

void tabs_rebuild(node_t *n) {
  if (n == NULL || n->tab_bar == NULL)
    return;
  build_entries(n->tab_bar);
  tabs_arrange(n, n->tab_bar->rect);
  tabs_update_focus(n, NULL);
}

void tabs_arrange(node_t *n, struct wlr_box rect) {
  if (n == NULL || n->tab_bar == NULL)
    return;
  struct bwm_tab_bar *bar = n->tab_bar;
  bar->rect = rect;

  if (bar->tree == NULL)
    return;

  wlr_scene_node_set_position(&bar->tree->node, rect.x, rect.y);

  if (bar->bg)
    wlr_scene_rect_set_size(bar->bg, rect.width, TAB_BAR_HEIGHT);

  if (bar->entry_count == 0)
    return;

  int total_w = rect.width;
  int n_entries = (int)bar->entry_count;
  int per = total_w / n_entries;
  if (per < 1) per = 1;
  int remainder = total_w - per * n_entries;

  int x = 0;
  for (int i = 0; i < n_entries; i++) {
    struct bwm_tab_entry *e = &bar->entries[i];
    int w = per + (i < remainder ? 1 : 0);

    if (e->tree)
      wlr_scene_node_set_position(&e->tree->node, x, 0);
    if (e->bg)
      wlr_scene_rect_set_size(e->bg, w - 1, TAB_BAR_HEIGHT - TAB_BAR_BORDER);
    if (e->border) {
      wlr_scene_node_set_position(&e->border->node, 0,
	      TAB_BAR_HEIGHT - TAB_BAR_BORDER);
      wlr_scene_rect_set_size(e->border, w - 1, TAB_BAR_BORDER);
    }
    if (e->label) {
      int label_max = w - 8;
      if (label_max < 0) label_max = 0;
      bwm_text_node_set_max_width(e->label, label_max);
      int text_h = bwm_text_node_default_height();
      int label_y = (TAB_BAR_HEIGHT - TAB_BAR_BORDER - text_h) / 2;
      if (label_y < 0) label_y = 0;
      wlr_scene_node_set_position(e->label->node, 4, label_y);
    }

    e->hit_box = (struct wlr_box){
      .x = rect.x + x,
      .y = rect.y,
      .width = w,
      .height = TAB_BAR_HEIGHT,
    };

    x += w;
  }
}

void tabs_show(node_t *n, bool show) {
  if (n == NULL || n->tab_bar == NULL || n->tab_bar->tree == NULL)
    return;
  wlr_scene_node_set_enabled(&n->tab_bar->tree->node, show);
}

void tabs_update_focus(node_t *n, node_t *focus) {
  if (n == NULL || n->tab_bar == NULL)
    return;

  node_t *active = tab_focus_leaf(n, focus);
  for (size_t i = 0; i < n->tab_bar->entry_count; i++) {
    struct bwm_tab_entry *e = &n->tab_bar->entries[i];
    bool is_active = (e->leaf == active);
    if (e->bg)
      wlr_scene_rect_set_color(e->bg, is_active ? color_tab_bg_active : color_tab_bg);
    if (e->label) {
      float c[4];
      memcpy(c, is_active ? color_tab_text_active : color_tab_text, sizeof(c));
      bwm_text_node_set_color(e->label, c);
    }
  }
}

void tabs_update_label_for_leaf(node_t *leaf) {
  if (leaf == NULL)
    return;
  node_t *t = tabbed_ancestor(leaf);
  if (t == NULL || t->tab_bar == NULL)
    return;
  for (size_t i = 0; i < t->tab_bar->entry_count; i++) {
    struct bwm_tab_entry *e = &t->tab_bar->entries[i];
    if (e->leaf == leaf && e->label)
      bwm_text_node_set_text(e->label, leaf_label(leaf));
  }
}

node_t *tabs_hit_test(node_t *n, double lx, double ly) {
  if (n == NULL || n->tab_bar == NULL || n->tab_bar->tree == NULL)
    return NULL;
  if (!n->tab_bar->tree->node.enabled)
    return NULL;
  for (size_t i = 0; i < n->tab_bar->entry_count; i++) {
    struct bwm_tab_entry *e = &n->tab_bar->entries[i];
    if (e->leaf == NULL || e->leaf->destroying)
      continue;
    if (lx >= e->hit_box.x && lx < e->hit_box.x + e->hit_box.width &&
        ly >= e->hit_box.y && ly < e->hit_box.y + e->hit_box.height)
      return e->leaf;
  }
  return NULL;
}

static node_t *hit_test_subtree(node_t *n, double lx, double ly) {
  if (n == NULL)
    return NULL;
  if (n->split_type == TYPE_TABBED && n->tab_bar) {
    node_t *hit = tabs_hit_test(n, lx, ly);
    if (hit)
      return hit;
  }
  if (is_leaf(n))
    return NULL;
  node_t *r = hit_test_subtree(n->first_child, lx, ly);
  if (r)
    return r;
  return hit_test_subtree(n->second_child, lx, ly);
}

node_t *tabs_hit_test_desktop(struct desktop_t *d, double lx, double ly) {
  if (d == NULL || d->root == NULL)
    return NULL;
  return hit_test_subtree(d->root, lx, ly);
}
