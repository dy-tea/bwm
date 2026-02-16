#define WLR_USE_UNSTABLE
#include "tree.h"
#include "toplevel.h"
#include "types.h"
#include "transaction.h"
#include <stdlib.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>

// global settings
automatic_scheme_t automatic_scheme = SCHEME_SPIRAL;
child_polarity_t initial_polarity = FIRST_CHILD;
bool single_monocle = false;
bool borderless_monocle = false;
bool gapless_monocle = false;
padding_t monocle_padding = {0, 0, 0, 0};
int border_width = 2;
int window_gap = 10;

// global state
monitor_t *mon = NULL;
monitor_t *mon_head = NULL;
monitor_t *mon_tail = NULL;
uint32_t next_node_id = 1;
uint32_t next_desktop_id = 1;
uint32_t next_monitor_id = 1;

node_t *make_node(uint32_t id) {
  node_t *n = (node_t *)calloc(1, sizeof(node_t));
  if (n == NULL)
    return NULL;

  n->id = id != 0 ? id : next_node_id++;
  n->split_type = TYPE_VERTICAL;
  n->split_ratio = 0.5;
  n->vacant = false;
  n->hidden = false;
  n->sticky = false;
  n->private_node = false;
  n->locked = false;
  n->marked = false;
  n->presel = NULL;
  n->first_child = NULL;
  n->second_child = NULL;
  n->parent = NULL;
  n->client = NULL;
  n->constraints.min_width = MIN_WIDTH;
  n->constraints.min_height = MIN_HEIGHT;

  // init transaction
  n->instruction = NULL;
  n->ntxnrefs = 0;
  n->dirty = false;
  n->destroying = false;

  // init current state
  n->current.rectangle = (struct wlr_box){0};
  n->current.split_ratio = 0.5;
  n->current.split_type = TYPE_VERTICAL;
  n->current.hidden = false;

  // init pending state
  n->pending.rectangle = (struct wlr_box){0};
  n->pending.split_ratio = 0.5;
  n->pending.split_type = TYPE_VERTICAL;
  n->pending.hidden = false;

  return n;
}

client_t *make_client(void) {
  client_t *c = (client_t *)calloc(1, sizeof(client_t));
  if (c == NULL)
    return NULL;

  c->state = STATE_TILED;
  c->last_state = STATE_TILED;
  c->layer = LAYER_NORMAL;
  c->last_layer = LAYER_NORMAL;
  c->urgent = false;
  c->shown = false;
  c->border_width = border_width;

  return c;
}

void free_node(node_t *n) {
  if (n == NULL)
    return;

  if (n->client != NULL)
    free(n->client);

  if (n->presel != NULL)
    free(n->presel);

  free(n);
}

bool is_leaf(node_t *n) {
  return (n != NULL && n->first_child == NULL &&
          n->second_child == NULL);
}

bool is_tiled(client_t *c) {
  return c != NULL &&
         (c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED);
}

bool is_floating(client_t *c) {
  return c != NULL && c->state == STATE_FLOATING;
}

bool is_first_child(node_t *n) {
  return n != NULL && n->parent != NULL && n->parent->first_child == n;
}

bool is_second_child(node_t *n) {
  return n != NULL && n->parent != NULL && n->parent->second_child == n;
}

unsigned int clients_count_in(node_t *n) {
  if (n == NULL)
    return 0;
  if (is_leaf(n))
    return n->client != NULL ? 1 : 0;
  return clients_count_in(n->first_child) + clients_count_in(n->second_child);
}

int tiled_count(node_t *n, bool include_receptacles) {
  if (n == NULL)
    return 0;

  if (is_leaf(n)) {
    if (n->client == NULL)
      return include_receptacles ? 1 : 0;
    return IS_TILED(n->client) ? 1 : 0;
  }

  return tiled_count(n->first_child, include_receptacles) +
         tiled_count(n->second_child, include_receptacles);
}

node_t *brother_tree(node_t *n) {
  if (n == NULL || n->parent == NULL)
    return NULL;
  return is_first_child(n) ? n->parent->second_child : n->parent->first_child;
}

node_t *first_extrema(node_t *n) {
  if (n == NULL)
    return NULL;
  for (; !is_leaf(n); n = n->first_child)
    ;
  return n;
}

node_t *second_extrema(node_t *n) {
  if (n == NULL)
    return NULL;
  for (; !is_leaf(n); n = n->second_child)
    ;
  return n;
}

node_t *next_leaf(node_t *n, node_t *r) {
  if (n == NULL || n == r)
    return NULL;
  node_t *p = n;
  for (; is_second_child(p) && p != r; p = p->parent)
    ;
  if (p == r)
    return NULL;
  return first_extrema(p->parent->second_child);
}

node_t *prev_leaf(node_t *n, node_t *r) {
  if (n == NULL || n == r)
    return NULL;
  node_t *p = n;
  for (; is_first_child(p) && p != r; p = p->parent)
    ;
  if (p == r)
    return NULL;
  return second_extrema(p->parent->first_child);
}

void arrange(monitor_t *m, desktop_t *d) {
  if (d->root == NULL)
    return;

  struct wlr_box rect = m->rectangle;

  rect.x += m->padding.left + d->padding.left;
  rect.y += m->padding.top + d->padding.top;
  rect.width -=
      m->padding.left + d->padding.left + d->padding.right + m->padding.right;
  rect.height -=
      m->padding.top + d->padding.top + d->padding.bottom + m->padding.bottom;

  if (d->layout == LAYOUT_MONOCLE) {
    rect.x += monocle_padding.left;
    rect.y += monocle_padding.top;
    rect.width -= monocle_padding.left + monocle_padding.right;
    rect.height -= monocle_padding.top + monocle_padding.bottom;
  }

  if (!gapless_monocle || d->layout != LAYOUT_MONOCLE) {
    rect.x += d->window_gap;
    rect.y += d->window_gap;
    rect.width -= d->window_gap;
    rect.height -= d->window_gap;
  }

  apply_layout(m, d, d->root, rect, rect);

  // Commit the transaction with all dirty nodes
  transaction_commit_dirty();
}

void apply_layout(monitor_t *m, desktop_t *d, node_t *n, struct wlr_box rect,
                  struct wlr_box root_rect) {
  if (n == NULL)
    return;

  // Set pending state (like Sway does with container->pending)
  n->pending.rectangle = rect;
  node_set_dirty(n);

  wlr_log(WLR_DEBUG, "apply_layout: node %u pending_rect=(%d,%d %dx%d)",
          n->id, rect.x, rect.y, rect.width, rect.height);

  if (is_leaf(n)) {
    if (n->client == NULL)
      return;

    unsigned int bw = (
        (borderless_monocle && d->layout == LAYOUT_MONOCLE && IS_TILED(n->client))
        || n->client->state == STATE_FULLSCREEN)
                          ? 0
                          : n->client->border_width;

    struct wlr_box r;
    if (IS_FLOATING(n->client)) {
      r = n->client->floating_rectangle;
    } else if (n->client->state == STATE_FULLSCREEN) {
      r = m->rectangle;
    } else if (d->layout == LAYOUT_MONOCLE && IS_TILED(n->client)) {
      r = root_rect;
      int wg = gapless_monocle ? 0 : d->window_gap;
      r.x += bw;
      r.y += bw;
      r.width -= (wg + bw);
      r.height -= (wg + bw);
    } else {
      r = rect;

      int wg =
          (gapless_monocle && d->layout == LAYOUT_MONOCLE) ? 0 : d->window_gap;

      r.x += bw;
      r.y += bw;
      r.width -= (wg + bw);
      r.height -= (wg + bw);
    }

    // Enforce minimum size constraints
    if (r.width < MIN_WIDTH) r.width = MIN_WIDTH;
    if (r.height < MIN_HEIGHT) r.height = MIN_HEIGHT;

    n->client->tiled_rectangle = r;

    wlr_log(WLR_DEBUG, "apply_layout: node %u tiled_rect=(%d,%d %dx%d)",
            n->id, r.x, r.y, r.width, r.height);

  } else {
    struct wlr_box first_rect;
    struct wlr_box second_rect;

    if (d->layout == LAYOUT_MONOCLE) {
      // In monocle mode, pass full rectangle to all children
      first_rect = rect;
      second_rect = rect;
    } else if (n->split_type == TYPE_VERTICAL) {
      first_rect = rect;
      second_rect = rect;
      first_rect.width = (int)(n->split_ratio * rect.width);
      second_rect.x += first_rect.width;
      second_rect.width = rect.width - first_rect.width;
    } else {
      first_rect = rect;
      second_rect = rect;
      first_rect.height = (int)(n->split_ratio * rect.height);
      second_rect.y += first_rect.height;
      second_rect.height = rect.height - first_rect.height;
    }

    apply_layout(m, d, n->first_child, first_rect, root_rect);
    apply_layout(m, d, n->second_child, second_rect, root_rect);
  }
}

unsigned int node_area(desktop_t *d, node_t *n) {
  if (n == NULL)
    return 0;
  return n->rectangle.width * n->rectangle.height;
}

node_t *find_public(desktop_t *d) {
  unsigned int b_area = 0;
  node_t *b = NULL;

  for (node_t *n = first_extrema(d->root); n != NULL;
       n = next_leaf(n, d->root)) {
    if (n->vacant)
      continue;
    unsigned int n_area = node_area(d, n);
    if (n_area > b_area && (n->presel != NULL || !n->private_node)) {
      b = n;
      b_area = n_area;
    }
  }

  return b;
}

node_t *insert_node(monitor_t *m, desktop_t *d, node_t *n, node_t *f) {
  if (d == NULL || n == NULL)
    return NULL;

  if (f == NULL)
    f = d->root;

  if (f == NULL) {
    d->root = n;
    return f;
  }

  // If f is a receptacle with no preselection, replace it
  if (IS_RECEPTACLE(f) && f->presel == NULL) {
    node_t *p = f->parent;
    if (p != NULL) {
      if (is_first_child(f))
          p->first_child = n;
      else
          p->second_child = n;
    } else d->root = n;
    n->parent = p;
    free_node(f);
    return NULL;
  }

  node_t *c = make_node(0);
  node_t *p = f->parent;

  if (f->presel == NULL && f->private_node) {
    node_t *k = find_public(d);
    if (k != NULL) {
      f = k;
      p = f->parent;
    }
  }

  n->parent = c;

  if (f->presel == NULL) {
    bool single_tiled = f->client != NULL && IS_TILED(f->client) && tiled_count(d->root, true) == 1;

    if (p == NULL || automatic_scheme != SCHEME_SPIRAL || single_tiled) {
      // normal insertion
      if (p != NULL) {
          if (is_first_child(f))
              p->first_child = c;
          else
              p->second_child = c;
      } else d->root = c;

      c->parent = p;
      f->parent = c;

      if (initial_polarity == FIRST_CHILD) {
        c->first_child = n;
        c->second_child = f;
      } else {
        c->first_child = f;
        c->second_child = n;
      }

      // determine split type
      if (p == NULL || automatic_scheme == SCHEME_LONGEST_SIDE || single_tiled) {
        c->split_type = f->rectangle.width >= f->rectangle.height
                            ? TYPE_VERTICAL
                            : TYPE_HORIZONTAL;
      } else if (automatic_scheme == SCHEME_ALTERNATE) {
        node_t *q = p;
        for (; q != NULL && (q->first_child->vacant || q->second_child->vacant); q = q->parent)
          ;
        if (q == NULL)
          q = p;
        if (q != NULL)
          c->split_type = (q->split_type == TYPE_HORIZONTAL) ? TYPE_VERTICAL
                                                             : TYPE_HORIZONTAL;
        else
          c->split_type = TYPE_VERTICAL;
      } else {
        c->split_type = TYPE_VERTICAL;
      }

      c->split_ratio = 0.5;

      // sync with pending state
      c->pending.split_type = c->split_type;
      c->pending.split_ratio = c->split_ratio;
      c->current.split_type = c->split_type;
      c->current.split_ratio = c->split_ratio;
    } else {
      // spiral insertion
      node_t *g = p->parent;
      c->parent = g;

      if (g != NULL) {
        if (is_first_child(p))
          g->first_child = c;
        else
          g->second_child = c;
      } else d->root = c;

      c->split_type = p->split_type;
      c->split_ratio = p->split_ratio;

      // sync with pending state
      c->pending.split_type = c->split_type;
      c->pending.split_ratio = c->split_ratio;
      c->current.split_type = c->split_type;
      c->current.split_ratio = c->split_ratio;

      p->parent = c;

      int rot;
      if (is_first_child(f)) {
        c->first_child = n;
        c->second_child = p;
        rot = 90;
      } else {
        c->first_child = p;
        c->second_child = n;
        rot = 270;
      }

      if (!n->vacant)
        rotate_tree(p, rot);
    }
  } else {
    // presel
    if (p != NULL) {
        if (is_first_child(f))
            p->first_child = c;
        else
            p->second_child = c;
    }

    c->split_ratio = f->presel->split_ratio;
    c->parent = p;
    f->parent = c;

    switch (f->presel->split_dir) {
    case DIR_WEST:
      c->split_type = TYPE_VERTICAL;
      c->pending.split_type = TYPE_VERTICAL;
      c->current.split_type = TYPE_VERTICAL;
      c->first_child = n;
      c->second_child = f;
      break;
    case DIR_EAST:
      c->split_type = TYPE_VERTICAL;
      c->pending.split_type = TYPE_VERTICAL;
      c->current.split_type = TYPE_VERTICAL;
      c->first_child = f;
      c->second_child = n;
      break;
    case DIR_NORTH:
      c->split_type = TYPE_HORIZONTAL;
      c->pending.split_type = TYPE_HORIZONTAL;
      c->current.split_type = TYPE_HORIZONTAL;
      c->first_child = n;
      c->second_child = f;
      break;
    case DIR_SOUTH:
      c->split_type = TYPE_HORIZONTAL;
      c->pending.split_type = TYPE_HORIZONTAL;
      c->current.split_type = TYPE_HORIZONTAL;
      c->first_child = f;
      c->second_child = n;
      break;
    }

    if (d->root == f)
      d->root = c;

    presel_cancel(m, d, f);
  }

  return f;
}

void remove_node(monitor_t *m, desktop_t *d, node_t *n) {
  if (n == NULL || d == NULL)
    return;

  node_t *p = n->parent;

  if (p == NULL) {
    d->root = NULL;
    d->focus = NULL;
  } else {
    node_t *b = brother_tree(n);
    node_t *g = p->parent;

    b->parent = g;

    if (g != NULL) {
        if (is_first_child(p))
            g->first_child = b;
        else
            g->second_child = b;
    } else d->root = b;

    // adjust tree structure
    if (!n->vacant) {
      if (automatic_scheme == SCHEME_SPIRAL) {
        if (is_first_child(n))
          rotate_tree(b, 270);
        else
          rotate_tree(b, 90);
      } else if (automatic_scheme == SCHEME_LONGEST_SIDE || g == NULL) {
        if (p != NULL && !is_leaf(b)) {
          if (p->rectangle.width > p->rectangle.height) {
            b->split_type = TYPE_VERTICAL;
            b->pending.split_type = TYPE_VERTICAL;
            b->current.split_type = TYPE_VERTICAL;
          } else {
            b->split_type = TYPE_HORIZONTAL;
            b->pending.split_type = TYPE_HORIZONTAL;
            b->current.split_type = TYPE_HORIZONTAL;
          }
        }
      } else if (automatic_scheme == SCHEME_ALTERNATE) {
        if (g != NULL && !is_leaf(b)) {
          if (g->split_type == TYPE_HORIZONTAL) {
            b->split_type = TYPE_VERTICAL;
            b->pending.split_type = TYPE_VERTICAL;
            b->current.split_type = TYPE_VERTICAL;
          } else {
            b->split_type = TYPE_HORIZONTAL;
            b->pending.split_type = TYPE_HORIZONTAL;
            b->current.split_type = TYPE_HORIZONTAL;
          }
        }
      }
    }

    if (d->focus == n) {
      if (b != NULL && !is_leaf(b))
        d->focus = first_extrema(b);
      else
        d->focus = b;

      // give kb focus to new node
      if (d->focus != NULL && d->focus->client != NULL &&
          d->focus->client->toplevel != NULL)
        focus_toplevel(d->focus->client->toplevel);
    }

    if (p->ntxnrefs == 0) {
      free_node(p);
    } else {
      // mark for destruction
      p->destroying = true;
      wlr_log(WLR_DEBUG, "Marked parent node %u as destroying (ntxnrefs=%zu)",
              p->id, (size_t)p->ntxnrefs);
    }
  }

  if (!n->destroying) {
    n->destroying = true;
    wlr_log(WLR_DEBUG, "Marked removed node %u as destroying (ntxnrefs=%zu)",
            n->id, (size_t)n->ntxnrefs);
  }
}

void close_node(node_t *n) {
  if (n == NULL || n->client == NULL || n->client->toplevel == NULL)
    return;

  wlr_xdg_toplevel_send_close(n->client->toplevel->xdg_toplevel);
}

void kill_node(monitor_t *m, desktop_t *d, node_t *n) {
  if (n == NULL)
    return;

  close_node(n);
}

bool focus_node(monitor_t *m, desktop_t *d, node_t *n) {
  if (m == NULL || d == NULL || n == NULL)
    return false;

  d->focus = n;
  mon = m;

  // Handle monocle mode visibility
  if (d->layout == LAYOUT_MONOCLE && d->root != NULL) {
    // mark visibility state
    for (node_t *node = first_extrema(d->root); node != NULL; node = next_leaf(node, d->root))
      if (node->client != NULL)
        node->client->shown = false;

    // mark focused window as shown
    if (n != NULL && n->client != NULL)
      n->client->shown = true;
  } else {
    // mark all windows as shown in tiled mode
    if (d->root != NULL)
      for (node_t *node = first_extrema(d->root); node != NULL; node = next_leaf(node, d->root))
        if (node->client != NULL)
          node->client->shown = true;
  }

  if (n != NULL && n->client != NULL)
    focus_toplevel(n->client->toplevel);

  return true;
}

bool activate_node(monitor_t *m, desktop_t *d, node_t *n) {
  return focus_node(m, d, n);
}

bool is_adjacent(node_t *a, node_t *b, direction_t dir) {
  if (a == NULL || b == NULL)
    return false;

  struct wlr_box ra = a->rectangle;
  struct wlr_box rb = b->rectangle;

  switch (dir) {
  case DIR_WEST:
    return ra.x == rb.x + rb.width;
  case DIR_EAST:
    return ra.x + ra.width == rb.x;
  case DIR_NORTH:
    return ra.y + ra.height == rb.y;
  case DIR_SOUTH:
    return ra.y == rb.y + rb.height;
  }

  return false;
}

node_t *find_fence(node_t *n, direction_t dir) {
  if (n == NULL)
    return NULL;

  node_t *p = n->parent;

  while (p != NULL) {
    node_t *brother = (is_first_child(n)) ? p->second_child : p->first_child;

    if (brother != NULL) {
      bool vertical = (dir == DIR_WEST || dir == DIR_EAST);
      bool horizontal = (dir == DIR_NORTH || dir == DIR_SOUTH);

      if ((vertical && p->split_type == TYPE_VERTICAL) ||
          (horizontal && p->split_type == TYPE_HORIZONTAL)) {
        bool first = (dir == DIR_WEST || dir == DIR_NORTH);

        if (first != is_first_child(n))
          return brother;
      }
    }

    n = p;
    p = n->parent;
  }

  return NULL;
}

void swap_nodes(monitor_t *m1, desktop_t *d1, node_t *n1, monitor_t *m2,
                desktop_t *d2, node_t *n2) {
  if (n1 == NULL || n2 == NULL || n1 == n2)
    return;

  bool n1_focused = (d1->focus == n1);
  bool n2_focused = (d2->focus == n2);

  client_t *c1 = n1->client;
  client_t *c2 = n2->client;

  n1->client = c2;
  n2->client = c1;

  bool tmp_vacant = n1->vacant;
  bool tmp_marked = n1->marked;
  bool tmp_locked = n1->locked;
  bool tmp_sticky = n1->sticky;
  bool tmp_private = n1->private_node;

  n1->vacant = n2->vacant;
  n1->marked = n2->marked;
  n1->locked = n2->locked;
  n1->sticky = n2->sticky;
  n1->private_node = n2->private_node;

  n2->vacant = tmp_vacant;
  n2->marked = tmp_marked;
  n2->locked = tmp_locked;
  n2->sticky = tmp_sticky;
  n2->private_node = tmp_private;

  if (c1 != NULL && c1->toplevel != NULL)
    c1->toplevel->node = n2;

  if (c2 != NULL && c2->toplevel != NULL)
    c2->toplevel->node = n1;

  if (n1_focused)
    focus_node(m2, d2, n2);
  if (n2_focused)
    focus_node(m1, d1, n1);

  arrange(m1, d1);
  if (d1 != d2)
    arrange(m2, d2);
}

bool set_state(monitor_t *m, desktop_t *d, node_t *n, client_state_t s) {
  if (n == NULL || n->client == NULL)
    return false;

  n->client->last_state = n->client->state;
  n->client->state = s;

  arrange(m, d);
  return true;
}

void set_floating(monitor_t *m, desktop_t *d, node_t *n, bool value) {
  if (n == NULL || n->client == NULL)
    return;

  if (value)
    set_state(m, d, n, STATE_FLOATING);
  else
    set_state(m, d, n, STATE_TILED);
}

presel_t *make_presel(void) {
  presel_t *p = (presel_t *)calloc(1, sizeof(presel_t));
  if (p == NULL)
    return NULL;

  p->split_ratio = 0.5;
  p->split_dir = DIR_EAST;

  return p;
}

void presel_dir(monitor_t *m, desktop_t *d, node_t *n, direction_t dir) {
  if (n == NULL || !is_leaf(n))
    return;

  if (n->presel == NULL)
    n->presel = make_presel();

  n->presel->split_dir = dir;
}

void presel_cancel(monitor_t *m, desktop_t *d, node_t *n) {
  if (n == NULL || n->presel == NULL)
    return;

  free(n->presel);
  n->presel = NULL;
}

void rotate_tree(node_t *n, int deg) {
  if (n == NULL || is_leaf(n) || deg == 0)
    return;

  node_t *tmp;

  // swap children
  if ((deg == 90 && n->split_type == TYPE_HORIZONTAL) ||
      (deg == 270 && n->split_type == TYPE_VERTICAL) ||
      (deg == -90 && n->split_type == TYPE_VERTICAL) ||
      (deg == -270 && n->split_type == TYPE_HORIZONTAL) ||
      deg == 180 || deg == -180) {
    tmp = n->first_child;
    n->first_child = n->second_child;
    n->second_child = tmp;
    n->split_ratio = 1.0 - n->split_ratio;
  }

  // flip split type for quarter rotations
  if (deg != 180 && deg != -180) {
    if (n->split_type == TYPE_HORIZONTAL)
      n->split_type = TYPE_VERTICAL;
    else if (n->split_type == TYPE_VERTICAL)
      n->split_type = TYPE_HORIZONTAL;

    // sync with pending state
    n->pending.split_type = n->split_type;
    n->current.split_type = n->split_type;
  }

  rotate_tree(n->first_child, deg);
  rotate_tree(n->second_child, deg);
}

void flip_tree(node_t *n, flip_t flp) {
  if (n == NULL || is_leaf(n))
    return;

  node_t *tmp = n->first_child;
  n->first_child = n->second_child;
  n->second_child = tmp;

  flip_tree(n->first_child, flp);
  flip_tree(n->second_child, flp);
}

struct wlr_box get_rectangle(monitor_t *m, desktop_t *d, node_t *n) {
  if (n != NULL)
    return n->rectangle;
  return m->rectangle;
}

// Transaction helper functions

void node_set_dirty(node_t *n) {
  if (!n) return;

  transaction_add_dirty_node(n);
}

void node_set_pending_size(node_t *n, int width, int height) {
  if (!n) return;

  n->pending.rectangle.width = width;
  n->pending.rectangle.height = height;
  node_set_dirty(n);
}

void node_set_pending_position(node_t *n, int x, int y) {
  if (!n) return;

  n->pending.rectangle.x = x;
  n->pending.rectangle.y = y;
  node_set_dirty(n);
}

void node_set_pending_rectangle(node_t *n, struct wlr_box rect) {
  if (!n) return;

  n->pending.rectangle = rect;
  node_set_dirty(n);
}

void node_set_pending_hidden(node_t *n, bool hidden) {
  if (!n) return;

  n->pending.hidden = hidden;
  node_set_dirty(n);
}
