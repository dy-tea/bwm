#include "tree.h"
#include "server.h"
#include "toplevel.h"
#include "types.h"
#include "transaction.h"
#include "output.h"
#include "scroller.h"
#include "xwayland.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>

// global settings
automatic_scheme_t automatic_scheme = SCHEME_SPIRAL;
child_polarity_t initial_polarity = FIRST_CHILD;
bool single_monocle = false;
bool borderless_monocle = false;
bool borderless_singleton = false;
bool gapless_monocle = false;
bool removal_adjustment = true;
bool focus_follows_pointer = false;
bool pointer_follows_focus = false;
bool record_history = true;
bool click_to_focus = false;
int mapping_events_count = 0;
int directional_focus_tightness = 20;
int ignore_ewmh_fullscreen = 0;
padding_t monocle_padding = {0, 0, 0, 0};
padding_t padding = {0, 0, 0, 0};
int border_width = 2;
int window_gap = 10;
double split_ratio = 0.5;

// border colors
char normal_border_color[16] = "444444ff";
char active_border_color[16] = "555555ff";
char focused_border_color[16] = "1793dfff";
char presel_feedback_color[16] = "ff5555ff";

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

  // Initialize scroller properties
  scroller_init_client(c);

  return c;
}

void free_node(node_t *n) {
  if (n == NULL)
    return;

  // deadbeef my beloved
  static uintptr_t sentinel = 0xDEADBEEF;
  if ((uintptr_t)n->client == sentinel || (uintptr_t)n->presel == sentinel) {
    wlr_log(WLR_ERROR, "free_node: double-free detected!");
    return;
  }

  if (n->client != NULL) {
    free(n->client);
    n->client = NULL;
  }

  if (n->presel != NULL) {
    free(n->presel);
    n->presel = NULL;
  }

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
  if (p == r || p->parent == NULL)
    return NULL;
  return first_extrema(p->parent->second_child);
}

node_t *prev_leaf(node_t *n, node_t *r) {
  if (n == NULL || n == r)
    return NULL;
  node_t *p = n;
  for (; is_first_child(p) && p != r; p = p->parent)
    ;
  if (p == r || p->parent == NULL)
    return NULL;
  return second_extrema(p->parent->first_child);
}

void arrange(monitor_t *m, desktop_t *d, bool use_transaction) {
  if (d->root == NULL) {
    if (use_transaction)
      transaction_commit_dirty();
    return;
  }

  struct wlr_box rect;
  if (m->output)
    rect = m->output->usable_area;
  else
    rect = m->rectangle;

  rect.x += m->padding.left + d->padding.left;
  rect.y += m->padding.top + d->padding.top;
  rect.width -=
      m->padding.left + d->padding.left + d->padding.right + m->padding.right;
  rect.height -=
      m->padding.top + d->padding.top + d->padding.bottom + m->padding.bottom;

  // Handle scroller layout separately
  if (d->layout == LAYOUT_SCROLLER) {
    scroller_arrange(m, d, rect);
    if (use_transaction)
      transaction_commit_dirty();
    return;
  }

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

  if (use_transaction)
    transaction_commit_dirty();
}

void apply_layout(monitor_t *m, desktop_t *d, node_t *n, struct wlr_box rect,
                  struct wlr_box root_rect) {
  if (n == NULL)
    return;

  // skip hidden or floating nodes from tiled layout
  if (n->hidden)
    return;
  if (n->client && n->client->state == STATE_FLOATING)
    return;

  // set pending
  n->pending.rectangle = rect;
  n->monitor = m;
  node_set_dirty(n);

  wlr_log(WLR_DEBUG, "apply_layout: node %u pending_rect=(%d,%d %dx%d)",
          n->id, rect.x, rect.y, rect.width, rect.height);

  if (is_leaf(n)) {
    wlr_log(WLR_DEBUG, "apply_layout: node %u is_leaf, n->client=%p", n->id, (void*)n->client);
    if (n->client == NULL) {
      wlr_log(WLR_ERROR, "apply_layout: node %u has NULL client, returning early", n->id);
      return;
    }

    bool the_only_window = (mon_head == mon_tail) && d->root && d->root->client;
    unsigned int bw = (
        (borderless_monocle && d->layout == LAYOUT_MONOCLE && IS_TILED(n->client))
        || (borderless_singleton && the_only_window)
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
      int bleed = wg + 2 * bw;
      r.x += bw;
      r.y += bw;
      r.width = (bleed < r.width ? r.width - bleed : 0);
      r.height = (bleed < r.height ? r.height - bleed : 0);
    } else {
      r = rect;
      int wg = (gapless_monocle && d->layout == LAYOUT_MONOCLE) ? 0 : d->window_gap;
      int bleed = wg + 2 * bw;
      r.x += bw;
      r.y += bw;
      r.width = (bleed < r.width ? r.width - bleed : 0);
      r.height = (bleed < r.height ? r.height - bleed : 0);
    }

    // pseudo tile
    if (n->client->state == STATE_PSEUDO_TILED) {
      if (r.width > n->client->floating_rectangle.width)
        r.width = n->client->floating_rectangle.width;
      if (r.height > n->client->floating_rectangle.height)
        r.height = n->client->floating_rectangle.height;
    }

    n->client->tiled_rectangle = r;
    if (n->client->committed_tiled_rectangle.width == 0 &&
        n->client->committed_tiled_rectangle.height == 0)
      n->client->committed_tiled_rectangle = r;

    wlr_log(WLR_DEBUG, "apply_layout: node %u tiled_rect=(%d,%d %dx%d)",
            n->id, r.x, r.y, r.width, r.height);

  } else {
    struct wlr_box first_rect;
    struct wlr_box second_rect;

    // please help why am I doing this
    static uintptr_t sentinel = 0xDEADBEEF;
    bool first_valid = n->first_child != NULL &&
      (uintptr_t)n->first_child > 0x400000 &&
      (uintptr_t)n->first_child < 0x7fffffffffff &&
      (uintptr_t)n->first_child->client != sentinel &&
      !n->first_child->destroying;
    bool second_valid = n->second_child != NULL &&
      (uintptr_t)n->second_child > 0x400000 &&
      (uintptr_t)n->second_child < 0x7fffffffffff &&
      (uintptr_t)n->second_child->client != sentinel &&
      !n->second_child->destroying;

    if (n->first_child != NULL && !first_valid) {
      wlr_log(WLR_ERROR, "Node %u has invalid/destroying/freed first_child pointer %p (destroying=%d), nulling",
              n->id, (void*)n->first_child,
              n->first_child && (uintptr_t)n->first_child->client != sentinel ? n->first_child->destroying : -1);
      n->first_child = NULL;
      first_valid = false;
    }
    if (n->second_child != NULL && !second_valid) {
      wlr_log(WLR_ERROR, "Node %u has invalid/destroying/freed second_child pointer %p (destroying=%d), nulling",
              n->id, (void*)n->second_child,
              n->second_child && (uintptr_t)n->second_child->client != sentinel ? n->second_child->destroying : -1);
      n->second_child = NULL;
      second_valid = false;
    }

    // fix tree structure
    if ((first_valid && !second_valid) || (!first_valid && second_valid)) {
      node_t *valid_child = first_valid ? n->first_child : n->second_child;
      wlr_log(WLR_ERROR, "apply_layout: node %u has only one valid child (first_valid=%d second_valid=%d) - "
        "promoting child %u; this indicates a tree inconsistency that should have been "
        "resolved by remove_node", n->id, first_valid, second_valid, valid_child->id);

      if (n->parent != NULL) {
        if (is_first_child(n))
          n->parent->first_child = valid_child;
        else
          n->parent->second_child = valid_child;
        valid_child->parent = n->parent;
      } else {
        d->root = valid_child;
        valid_child->parent = NULL;
      }

      n->first_child = NULL;
      n->second_child = NULL;
      n->parent = NULL;

      apply_layout(m, d, valid_child, rect, root_rect);
      return;
    }

    bool first_fullscreen = first_valid && n->first_child->client && n->first_child->client->state == STATE_FULLSCREEN;
    bool second_fullscreen = second_valid && n->second_child->client && n->second_child->client->state == STATE_FULLSCREEN;
    bool first_hidden = first_valid && n->first_child->hidden;
    bool second_hidden = second_valid && n->second_child->hidden;

    if (d->layout == LAYOUT_MONOCLE) {
      first_rect = rect;
      second_rect = rect;
    } else if (n->split_type == TYPE_VERTICAL) {
      if ((first_hidden || first_fullscreen) && n->second_child && !(second_hidden || second_fullscreen)) {
        first_rect = (struct wlr_box){0, 0, 0, 0};
        second_rect = rect;
      } else if ((second_hidden || second_fullscreen) && n->first_child && !(first_hidden || first_fullscreen)) {
        first_rect = rect;
        second_rect = (struct wlr_box){0, 0, 0, 0};
      } else {
        first_rect = rect;
        second_rect = rect;
        int fence = (int)(n->split_ratio * rect.width);
        uint16_t first_min = n->first_child ? n->first_child->constraints.min_width : 0;
        uint16_t second_min = n->second_child ? n->second_child->constraints.min_width : 0;
        if (first_min + second_min <= rect.width) {
          if (fence < first_min)
            fence = first_min;
          else if (fence > rect.width - second_min)
            fence = rect.width - second_min;
        }
        first_rect.width = fence;
        second_rect.x += fence;
        second_rect.width = rect.width - fence;
      }
    } else {
      if ((first_hidden || first_fullscreen) && n->second_child && !(second_hidden || second_fullscreen)) {
        first_rect = (struct wlr_box){0, 0, 0, 0};
        second_rect = rect;
      } else if ((second_hidden || second_fullscreen) && n->first_child && !(first_hidden || first_fullscreen)) {
        first_rect = rect;
        second_rect = (struct wlr_box){0, 0, 0, 0};
      } else {
        first_rect = rect;
        second_rect = rect;
        int fence = (int)(n->split_ratio * rect.height);
        uint16_t first_min = n->first_child ? n->first_child->constraints.min_height : 0;
        uint16_t second_min = n->second_child ? n->second_child->constraints.min_height : 0;
        if (first_min + second_min <= rect.height) {
          if (fence < first_min)
            fence = first_min;
          else if (fence > rect.height - second_min)
            fence = rect.height - second_min;
        }
        first_rect.height = fence;
        second_rect.y += fence;
        second_rect.height = rect.height - fence;
      }
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

  n->desktop = d;

  wlr_log(WLR_DEBUG, "insert_node: n=%u (state=%d hidden=%d parent=%u) f=%u root=%u focus=%u",
    n->id, n->client ? n->client->state : -1, n->hidden,
    n->parent ? n->parent->id : 0,
    f ? f->id : 0,
    d->root ? d->root->id : 0,
    d->focus ? d->focus->id : 0);

  if (n->parent != NULL) {
    wlr_log(WLR_ERROR, "insert_node: node %u already has parent %u, inserting while still in tree",
      n->id, n->parent->id);
  }

  if (n->client && IS_FLOATING(n->client)) {
    wlr_log(WLR_ERROR, "insert_node: node %u is floating (state=%d), should have been set to tiled first",
      n->id, n->client->state);
    n->parent = NULL;
    return NULL;
  }

  if (f == NULL)
    f = d->root;

  // fall back to the tree root to avoid corrupting the tree structure
  if (f != NULL && f != d->root && f->parent == NULL) {
    wlr_log(WLR_DEBUG, "insert_node: focus node %u is not in BSP tree (floating/orphaned), falling back to root", f->id);
    f = d->root;
  }

  if (f == NULL) {
    wlr_log(WLR_DEBUG, "insert_node: empty tree, node %u becomes root", n->id);
    d->root = n;
    n->parent = NULL;
    return f;
  }

  // if f is a receptacle (leaf with no client) with no preselection, replace it
  if (IS_RECEPTACLE(f) && f->presel == NULL) {
    wlr_log(WLR_DEBUG, "insert_node: replacing receptacle %u with node %u", f->id, n->id);
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

  wlr_log(WLR_DEBUG, "insert_node: done, n=%u parent=%u root=%u",
    n->id, n->parent ? n->parent->id : 0, d->root ? d->root->id : 0);

  if (d->root && d->root->parent != NULL)
    wlr_log(WLR_ERROR, "insert_node: post-insert root %u has non-NULL parent %u, tree split detected",
      d->root->id, d->root->parent->id);

  return f;
}

void remove_node(monitor_t *m, desktop_t *d, node_t *n) {
  if (n == NULL || d == NULL)
    return;

  wlr_log(WLR_DEBUG, "remove_node: node=%u state=%d parent=%u root=%u focus=%u",
    n->id, n->client ? n->client->state : -1,
    n->parent ? n->parent->id : 0,
    d->root ? d->root->id : 0,
    d->focus ? d->focus->id : 0);

  node_t *p = n->parent;
  bool n_is_first = is_first_child(n);

  if (p == NULL) {
    if (d->root != n) {
      if (n->client && IS_TILED(n->client))
        wlr_log(WLR_ERROR, "remove_node: tiled node %u has no parent and is not root, tree is corrupt (root=%u)",
          n->id, d->root ? d->root->id : 0);
      else
        wlr_log(WLR_DEBUG, "remove_node: node %u has no parent and is not root, already detached", n->id);
      if (d->focus == n)
        d->focus = d->root ? first_extrema(d->root) : NULL;
      return;
    }

  	// check if root has brother
    node_t *b = brother_tree(n);
    if (b != NULL) {
    	// promote brother to root
      wlr_log(WLR_DEBUG, "remove_node: Node %u is root with brother %u, promoting brother to root",
              n->id, b->id);
      d->root = b;
      b->parent = NULL;
      if (n->parent != NULL)
        n->parent = NULL;
      if (d->focus == n)
        d->focus = b;
    } else {
      wlr_log(WLR_DEBUG, "remove_node: Node %u has no parent or brother, clearing desktop %s root",
              n->id, d->name);
      d->root = NULL;
      d->focus = NULL;
    }
  } else {
    node_t *b = brother_tree(n);
    node_t *g = p->parent;

    if (b == NULL) {
    	// remove your existence if you don't have a brother
      wlr_log(WLR_ERROR, "remove_node: Node %u brother is NULL, clearing desktop %s root (tree corrupted)",
              n->id, d->name);
      d->root = NULL;
      d->focus = NULL;
      return;
    }

    b->parent = g;

    if (g != NULL) {
      if (is_first_child(p))
        g->first_child = b;
      else
        g->second_child = b;
      if (n_is_first)
        p->first_child = NULL;
      else
        p->second_child = NULL;
      p->parent = NULL;
    } else d->root = b;

    // clear detached pointers
    n->parent = NULL;
    n->first_child = NULL;
    n->second_child = NULL;

    // adjust tree structure
    if (!n->vacant && removal_adjustment && (n->client == NULL || IS_TILED(n->client))) {
      if (automatic_scheme == SCHEME_SPIRAL) {
        if (n_is_first)
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
  }

  wlr_log(WLR_DEBUG, "remove_node: done, root=%u focus=%u n->parent=%u",
    d->root ? d->root->id : 0,
    d->focus ? d->focus->id : 0,
    n->parent ? n->parent->id : 0);

  if (d->root && d->root->parent != NULL)
    wlr_log(WLR_ERROR, "remove_node: post-remove root %u has non-NULL parent %u, tree split detected",
      d->root->id, d->root->parent->id);
}

void close_node(node_t *n) {
  if (n == NULL || n->client == NULL)
    return;

  if (n->client->toplevel != NULL)
  	wlr_xdg_toplevel_send_close(n->client->toplevel->xdg_toplevel);
  else
  	xwayland_view_close(n->client->xwayland_view);
}

void kill_node(monitor_t *m, desktop_t *d, node_t *n) {
  if (n == NULL)
    return;

  // freeze buffers before closing nodes
  toplevel_freeze_sibling_buffers(d, n);

  close_node(n);
}

bool focus_node(monitor_t *m, desktop_t *d, node_t *n) {
  if (m == NULL || d == NULL || n == NULL)
    return false;

  d->focus = n;
  mon = m;
  server.focused_monitor = m;

  bool is_current_desktop = (m->desk == d);
  if (is_current_desktop && d->layout == LAYOUT_MONOCLE && d->root != NULL) {
    // update visibility state and scene graph for all nodes
    for (node_t *node = first_extrema(d->root); node != NULL; node = next_leaf(node, d->root)) {
      if (node->client == NULL)
        continue;
      bool should_show = (node == n);
      node->client->shown = should_show;
      struct wlr_scene_tree *scene_tree = client_get_scene_tree(node->client);
      if (scene_tree)
        wlr_scene_node_set_enabled(&scene_tree->node, should_show);
    }
  } else if (is_current_desktop && d->layout == LAYOUT_SCROLLER && d->root != NULL) {
    for (node_t *node = first_extrema(d->root); node != NULL; node = next_leaf(node, d->root))
      if (node->client != NULL)
        node->client->shown = true;

    if (n != NULL && n->client != NULL && n->client->toplevel && n->client->toplevel->configured) {
      wlr_log(WLR_DEBUG, "focus_node: scroller layout, triggering arrange for scrolling effect");
      arrange(m, d, true);
    } else {
      wlr_log(WLR_DEBUG, "focus_node: scroller layout, skipping arrange (initial map)");
    }
  } else if (is_current_desktop) {
    // mark all windows as shown in tiled mode, but only for current desktop
    if (d->root != NULL)
      for (node_t *node = first_extrema(d->root); node != NULL; node = next_leaf(node, d->root))
        if (node->client != NULL)
          node->client->shown = true;
  }

  if (n != NULL && n->client != NULL) {
    if (n->client->toplevel)
      focus_toplevel(n->client->toplevel);
    else if (n->client->xwayland_view)
      xwayland_view_set_activated(n->client->xwayland_view, true);
  }

  // update border colors for all visible clients on this desktop
  if (is_current_desktop && d->root != NULL) {
    for (node_t *node = first_extrema(d->root); node != NULL; node = next_leaf(node, d->root)) {
      if (node->client == NULL)
        continue;
      if (node->client->toplevel) {
        update_border_colors(node->client->toplevel->border_tree,
                            node->client->toplevel->border_rects, node->client);
      } else if (node->client->xwayland_view) {
        update_border_colors(node->client->xwayland_view->border_tree,
                            node->client->xwayland_view->border_rects, node->client);
      }
    }
  }

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

  arrange(m1, d1, true);
  if (d1 != d2)
    arrange(m2, d2, true);
}

bool set_state(monitor_t *m, desktop_t *d, node_t *n, client_state_t s) {
  if (n == NULL || n->client == NULL)
    return false;

  n->client->last_state = n->client->state;
  n->client->state = s;

  arrange(m, d, true);
  return true;
}

void set_floating(monitor_t *m, desktop_t *d, node_t *n, bool value) {
  if (n == NULL || n->client == NULL)
    return;

  if (value) {
    n->hidden = true;
    set_state(m, d, n, STATE_FLOATING);
  } else {
    n->hidden = false;
    set_state(m, d, n, STATE_TILED);
  }
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

  if (n->client) {
    if (n->client->toplevel)
      update_border_colors(n->client->toplevel->border_tree,
                           n->client->toplevel->border_rects, n->client);
    else if (n->client->xwayland_view)
      update_border_colors(n->client->xwayland_view->border_tree,
                           n->client->xwayland_view->border_rects, n->client);
  }
}

void presel_cancel(monitor_t *m, desktop_t *d, node_t *n) {
  if (n == NULL || n->presel == NULL)
    return;

  free(n->presel);
  n->presel = NULL;

  if (n->client) {
    if (n->client->toplevel)
      update_border_colors(n->client->toplevel->border_tree,
                           n->client->toplevel->border_rects, n->client);
    else if (n->client->xwayland_view)
      update_border_colors(n->client->xwayland_view->border_tree,
                           n->client->xwayland_view->border_rects, n->client);
  }
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

  // flip if split direction matched flip type
  if ((flp == FLIP_HORIZONTAL && n->split_type == TYPE_HORIZONTAL) ||
      (flp == FLIP_VERTICAL && n->split_type == TYPE_VERTICAL)) {
    node_t *tmp = n->first_child;
    n->first_child = n->second_child;
    n->second_child = tmp;
  }

  flip_tree(n->first_child, flp);
  flip_tree(n->second_child, flp);
}

static void equalize_rec(node_t *n) {
  if (n == NULL)
    return;

  if (is_leaf(n))
    return;

  n->split_ratio = 0.5;

  equalize_rec(n->first_child);
  equalize_rec(n->second_child);
}

void equalize_tree(node_t *n) {
  if (n == NULL)
    return;

  equalize_rec(n);
}

static void balance_rec(node_t *n) {
  if (n == NULL)
    return;

  if (is_leaf(n))
    return;

  unsigned int c1 = clients_count_in(n->first_child);
  unsigned int total = c1 + clients_count_in(n->second_child);
  if (total > 0)
    n->split_ratio = (double)c1 / (double)total;

  balance_rec(n->first_child);
  balance_rec(n->second_child);
}

void balance_tree(node_t *n) {
  if (n == NULL)
    return;

  balance_rec(n);
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

struct wlr_scene_tree *client_get_scene_tree(client_t *client) {
  if (!client)
    return NULL;

  if (client->toplevel)
    return client->toplevel->scene_tree;

  if (client->xwayland_view)
    return client->xwayland_view->scene_tree;

  return NULL;
}

void print_tree(node_t *n, int depth) {
  if (n == NULL)
    return;

  for (int i = 0; i < depth; i++)
    printf("  ");

  if (is_leaf(n)) {
    printf("node %u: rect=(%d,%d %dx%d)", n->id, n->rectangle.x, n->rectangle.y,
           n->rectangle.width, n->rectangle.height);
    if (n->client) {
      printf(" client=%s", n->client->app_id[0] ? n->client->app_id : "(none)");
    } else {
      printf(" client=NULL");
    }
    printf("\n");
  } else {
    const char *st = n->split_type == TYPE_VERTICAL ? "V" : "H";
    printf("node %u: rect=(%d,%d %dx%d) split=%s ratio=%.2f\n", n->id,
           n->rectangle.x, n->rectangle.y, n->rectangle.width, n->rectangle.height,
           st, n->split_ratio);
    print_tree(n->first_child, depth + 1);
    print_tree(n->second_child, depth + 1);
  }
}

static bool validate_subtree(node_t *n, node_t *expected_parent, int depth) {
  if (n == NULL)
    return true;

  if (depth > 64) {
    wlr_log(WLR_ERROR, "validate_tree: depth limit reached at node %u, possible cycle", n->id);
    return false;
  }

  if (n->parent != expected_parent) {
    wlr_log(WLR_ERROR, "validate_tree: node %u has wrong parent: expected %u, got %u",
      n->id,
      expected_parent ? expected_parent->id : 0,
      n->parent ? n->parent->id : 0);
    return false;
  }

  if (is_leaf(n))
    return true;

  bool ok = true;
  if (n->first_child == NULL) {
    wlr_log(WLR_ERROR, "validate_tree: internal node %u has NULL first_child", n->id);
    ok = false;
  }
  if (n->second_child == NULL) {
    wlr_log(WLR_ERROR, "validate_tree: internal node %u has NULL second_child", n->id);
    ok = false;
  }
  if (!ok)
    return false;

  ok &= validate_subtree(n->first_child, n, depth + 1);
  ok &= validate_subtree(n->second_child, n, depth + 1);
  return ok;
}

void validate_tree(const char *context, desktop_t *d) {
  if (d == NULL)
    return;

  if (d->root == NULL) {
    wlr_log(WLR_DEBUG, "validate_tree [%s]: root is NULL", context);
    return;
  }

  if (d->root->parent != NULL) {
    wlr_log(WLR_ERROR,
            "validate_tree [%s]: d->root (node %u) has non-NULL parent (node %u), second tree detected",
            context, d->root->id, d->root->parent->id);
    return;
  }

  if (!validate_subtree(d->root, NULL, 0)) {
    wlr_log(WLR_ERROR, "validate_tree [%s]: tree is CORRUPT, my life is OVER :deadge: (root=%u)", context, d->root->id);
    return;
  }

  // verify d->focus is either in the tree or explicitly floating/NULL
  if (d->focus != NULL && d->focus->client != NULL && IS_TILED(d->focus->client)) {
    bool found = false;
    for (node_t *f = first_extrema(d->root); f != NULL; f = next_leaf(f, d->root)) {
      if (f == d->focus) {
        found = true;
        break;
      }
    }
    if (!found)
      wlr_log(WLR_ERROR, "validate_tree [%s]: d->focus (node %u, tiled) is NOT reachable from root %u, second tree",
        context, d->focus->id, d->root->id);
  }

  wlr_log(WLR_DEBUG, "validate_tree [%s]: OK (root=%u, focus=%u)", context,
    d->root->id, d->focus ? d->focus->id : 0);
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return 0;
}

static void parse_color(const char *hex, float *color) {
  if (!hex || strlen(hex) != 8) {
    color[0] = color[1] = color[2] = 0.5f;
    color[3] = 1.0f;
    return;
  }
  color[0] = (float)(hex_digit(hex[0]) * 16 + hex_digit(hex[1])) / 255.0f;
  color[1] = (float)(hex_digit(hex[2]) * 16 + hex_digit(hex[3])) / 255.0f;
  color[2] = (float)(hex_digit(hex[4]) * 16 + hex_digit(hex[5])) / 255.0f;
  color[3] = (float)(hex_digit(hex[6]) * 16 + hex_digit(hex[7])) / 255.0f;
}

static void get_border_color(client_t *client, float *color) {
  if (!client || border_width == 0) {
    color[0] = color[1] = color[2] = 0.0f;
    color[3] = 1.0f;
    return;
  }

  const char *color_str;
  monitor_t *m = client->toplevel ? client->toplevel->node->monitor :
  	client->xwayland_view ? client->xwayland_view->node->monitor : NULL;
  desktop_t *d = m ? m->desk : NULL;

  bool the_only_window = (mon_head == mon_tail) && d && d->root && d->root->client;
  bool no_border = (borderless_monocle && d &&
  	d->layout == LAYOUT_MONOCLE && IS_TILED(client)) ||
    (borderless_singleton && the_only_window) ||
    client->state == STATE_FULLSCREEN;

  if (no_border) {
    color[0] = color[1] = color[2] = 0.0f;
    color[3] = 0.0f;
    return;
  }

  // check if this client's node has an active preselection
  node_t *n = client->toplevel ? client->toplevel->node :
              client->xwayland_view ? client->xwayland_view->node : NULL;
  if (n && n->presel) {
    parse_color(presel_feedback_color, color);
    return;
  }

  bool is_focused = (d && d->focus && d->focus->client == client);
  bool is_active = (d && d->focus && d->focus->client != NULL);
  if (is_focused)
    color_str = focused_border_color;
  else if (is_active)
    color_str = active_border_color;
  else
    color_str = normal_border_color;

  parse_color(color_str, color);
}

void create_borders(struct wlr_scene_tree *parent, struct wlr_scene_tree **border_tree,
                    struct wlr_scene_rect *rects[4]) {
  if (border_width == 0) {
    if (*border_tree) {
      wlr_scene_node_destroy(&(*border_tree)->node);
      *border_tree = NULL;
      for (int i = 0; i < 4; i++)
        rects[i] = NULL;
    }
    return;
  }

  // create border tree container
  *border_tree = wlr_scene_tree_create(parent);
  if (!*border_tree)
    return;

  float color[4];
  parse_color(focused_border_color, color);

  rects[0] = wlr_scene_rect_create(*border_tree, 0, border_width, color);
  rects[1] = wlr_scene_rect_create(*border_tree, 0, border_width, color);
  rects[2] = wlr_scene_rect_create(*border_tree, border_width, 0, color);
  rects[3] = wlr_scene_rect_create(*border_tree, border_width, 0, color);
}

void destroy_borders(struct wlr_scene_tree **border_tree, struct wlr_scene_rect *rects[4]) {
  if (*border_tree) {
    wlr_scene_node_destroy(&(*border_tree)->node);
    *border_tree = NULL;
    for (int i = 0; i < 4; i++)
      rects[i] = NULL;
  }
}

void update_borders(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
                    struct wlr_box geo, unsigned int bw) {
  if (!border_tree || bw == 0) {
    if (border_tree)
      wlr_scene_node_set_enabled(&border_tree->node, false);
    return;
  }

  wlr_scene_node_set_enabled(&border_tree->node, true);

  int w = geo.width;
  int h = geo.height;
  int bwi = (int)bw;

  wlr_scene_node_set_position(&border_tree->node, -bwi, -bwi);

  // top
  if (rects[0]) {
    wlr_scene_node_set_position(&rects[0]->node, 0, 0);
    wlr_scene_rect_set_size(rects[0], w + 2 * bwi, bwi);
  }
  // bottom
  if (rects[1]) {
    wlr_scene_node_set_position(&rects[1]->node, 0, h + bwi);
    wlr_scene_rect_set_size(rects[1], w + 2 * bwi, bwi);
  }
  // left
  if (rects[2]) {
    wlr_scene_node_set_position(&rects[2]->node, 0, bwi);
    wlr_scene_rect_set_size(rects[2], bwi, h);
  }
  // right
  if (rects[3]) {
    wlr_scene_node_set_position(&rects[3]->node, w + bwi, bwi);
    wlr_scene_rect_set_size(rects[3], bwi, h);
  }
}

void update_border_colors(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
                          client_t *client) {
  if (border_width == 0 || !border_tree)
    return;

  float color[4];
  get_border_color(client, color);

  if (client->border_radius > 0.0f && client->toplevel) {
    struct bwm_toplevel *tl = client->toplevel;
    tl->border_color[0] = color[0];
    tl->border_color[1] = color[1];
    tl->border_color[2] = color[2];
    tl->border_color[3] = color[3];
    tl->border_dirty = true;
    static const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; i++)
      if (rects[i])
        wlr_scene_rect_set_color(rects[i], transparent);
    return;
  }

  for (int i = 0; i < 4; i++)
    if (rects[i])
      wlr_scene_rect_set_color(rects[i], color);
}
