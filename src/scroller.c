#include "scroller.h"
#include "tree.h"
#include "toplevel.h"
#include <stdlib.h>
#include <math.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>

// Configuration defaults
float scroller_default_proportion = 0.66f;
float scroller_default_proportion_single = 0.8f;
int scroller_structs = 0;
bool scroller_focus_center = false;
bool scroller_prefer_center = true;
bool scroller_prefer_overspread = false;
bool scroller_ignore_proportion_single = false;
bool edge_scroller_pointer_focus = true;

// Proportion presets
float *scroller_proportion_preset = NULL;
int scroller_proportion_preset_count = 0;

bool scroller_is_tiled(client_t *c) {
  if (!c) return false;
  return c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED;
}

void scroller_init_client(client_t *c) {
  if (!c) return;

  c->scroller_proportion = 0.0f;
  c->scroller_proportion_single = scroller_default_proportion_single;
  c->stack_proportion = 1.0f;
  c->next_in_stack = NULL;
  c->prev_in_stack = NULL;
  c->old_scroller_proportion = 0.0f;
  c->old_stack_proportion = 1.0f;
  c->cursor_in_left_half = false;
  c->cursor_in_upper_half = false;
}

client_t *scroller_get_stack_head(client_t *c) {
  if (!c) return NULL;
  while (c->prev_in_stack)
    c = c->prev_in_stack;
  return c;
}

int scroller_collect_nodes(desktop_t *d, node_t ***out_nodes) {
  if (!d || !d->root) return 0;

  int count = 0;
  for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root))
    if (n->client && scroller_is_tiled(n->client) && !n->client->prev_in_stack)
      count++;

  if (count == 0) return 0;

  node_t **nodes = calloc(count, sizeof(node_t*));
  if (!nodes) return 0;

  int idx = 0;
  for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root))
    if (n->client && scroller_is_tiled(n->client) && !n->client->prev_in_stack)
      nodes[idx++] = n;

  *out_nodes = nodes;
  return count;
}

static void scroller_arrange_stack(node_t *head_node, struct wlr_box base_geom, int gap, monitor_t *m) {
  if (!head_node || !head_node->client) return;

  client_t *head = head_node->client;

  int count = 1;
  float total_proportion = head->stack_proportion;
  for (client_t *c = head->next_in_stack; c; c = c->next_in_stack) {
    count++;
    total_proportion += c->stack_proportion;
	}

  if (count == 1) {
    unsigned int bw = head->border_width;
    struct wlr_box r = base_geom;
    r.x += bw;
    r.y += bw;
    r.width = (r.width > (int)(2 * bw)) ? r.width - 2 * bw : 0;
    r.height = (r.height > (int)(2 * bw)) ? r.height - 2 * bw : 0;

    head->tiled_rectangle = r;

    wlr_log(WLR_DEBUG, "scroller_arrange_stack: setting node %u geom=(%d,%d %dx%d)",
            head_node->id, r.x, r.y, r.width, r.height);
    node_set_pending_rectangle(head_node, base_geom);
    head_node->monitor = m;
    node_set_dirty(head_node);
    return;
  }

  int current_y = base_geom.y;
  int remain_height = base_geom.height;
  float remain_proportion = total_proportion;

  for (client_t *c = head; c; c = c->next_in_stack) {
    struct wlr_box geom = base_geom;

    if (c->next_in_stack) {
      int height = (int)((c->stack_proportion / remain_proportion) * remain_height);
      geom.height = height - gap;
      geom.y = current_y;

      current_y += height;
      remain_height -= height;
      remain_proportion -= c->stack_proportion;
    } else {
      geom.y = current_y;
      geom.height = remain_height;
    }

    unsigned int bw = c->border_width;
    struct wlr_box r = geom;
    r.x += bw;
    r.y += bw;
    r.width = (r.width > (int)(2 * bw)) ? r.width - 2 * bw : 0;
    r.height = (r.height > (int)(2 * bw)) ? r.height - 2 * bw : 0;

    c->tiled_rectangle = r;

    node_t *stack_node = (c == head) ? head_node : NULL;
    if (!stack_node) {
      for (node_t *n = first_extrema(head_node->monitor->desk->root); n; n = next_leaf(n, head_node->monitor->desk->root)) {
        if (n->client == c) {
          stack_node = n;
          break;
        }
      }
    }

    if (stack_node) {
      node_set_pending_rectangle(stack_node, geom);
      stack_node->monitor = m;
      node_set_dirty(stack_node);
    }
  }
}

void scroller_arrange(monitor_t *m, desktop_t *d, struct wlr_box available) {
  if (!d || !d->root) return;

  wlr_log(WLR_DEBUG, "scroller_arrange: starting, available=(%d,%d %dx%d)",
          available.x, available.y, available.width, available.height);

  node_t **nodes = NULL;
  int n = scroller_collect_nodes(d, &nodes);
  wlr_log(WLR_DEBUG, "scroller_arrange: found %d tiled nodes", n);
  if (n == 0) return;

  int gappih = d->window_gap;
  int gappoh = d->window_gap;
  int gappov = d->window_gap;
  int gappiv = d->window_gap;

  int max_width = available.width - 2 * scroller_structs - gappih;
  int max_height = available.height - 2 * gappov;

  for (int i = 0; i < n; i++) {
    client_t *c = nodes[i]->client;
    if (c->scroller_proportion <= 0.0f && c->toplevel && c->toplevel->xdg_toplevel) {
      struct wlr_box geom = c->toplevel->xdg_toplevel->base->geometry;
      if (geom.width > 0 && geom.height > 0) {
        float aspect_ratio = (float)geom.width / (float)geom.height;

        int scaled_width = (int)(max_height * aspect_ratio);
        int scaled_height = max_height;

        if (scaled_width > max_width) {
          scaled_width = max_width;
          scaled_height = (int)(max_width / aspect_ratio);
        }

        float requested_proportion = (float)scaled_width / (float)max_width;

        if (requested_proportion < 0.1f) requested_proportion = 0.1f;
        if (requested_proportion > 1.0f) requested_proportion = 1.0f;

        c->scroller_proportion = requested_proportion;
        wlr_log(WLR_DEBUG, "scroller_arrange: client geom=%dx%d aspect=%.2f scaled=%dx%d proportion=%.2f",
                geom.width, geom.height, aspect_ratio, scaled_width, scaled_height, requested_proportion);
      } else {
        c->scroller_proportion = scroller_default_proportion;
        wlr_log(WLR_DEBUG, "scroller_arrange: client has no geometry, using default proportion");
      }
    } else if (c->scroller_proportion <= 0.0f) {
      c->scroller_proportion = scroller_default_proportion;
    }
  }

  node_t *focused_node = NULL;
  int focus_idx = 0;

  if (d->focus && d->focus->client && scroller_is_tiled(d->focus->client)) {
    client_t *focused_client = scroller_get_stack_head(d->focus->client);
    for (int i = 0; i < n; i++) {
      if (nodes[i]->client == focused_client) {
        focused_node = nodes[i];
        focus_idx = i;
        break;
      }
    }
  }

  if (!focused_node)
    focused_node = nodes[0];

  if (!focused_node) {
    free(nodes);
    return;
  }

  client_t *focused = focused_node->client;

  if (n == 1) {
    struct wlr_box geom;
    float proportion = scroller_ignore_proportion_single ? 1.0f :
    	(focused->scroller_proportion_single > 0.0f ?
      focused->scroller_proportion_single :
      scroller_default_proportion_single);

    geom.height = available.height - 2 * gappov;
    geom.width = (int)((available.width - 2 * gappoh) * proportion);
    geom.x = available.x + (available.width - geom.width) / 2;
    geom.y = available.y + gappov;

    wlr_log(WLR_DEBUG, "scroller_arrange: single window geom=(%d,%d %dx%d)",
            geom.x, geom.y, geom.width, geom.height);
    scroller_arrange_stack(focused_node, geom, gappiv, m);
    free(nodes);
    return;
  }

  struct wlr_box focused_geom;
  focused_geom.height = available.height - 2 * gappov;
  focused_geom.y = available.y + gappov;
  focused_geom.width = (int)(max_width * focused->scroller_proportion);

  bool need_center = scroller_focus_center;
  bool need_overspread = false;

  if (scroller_prefer_overspread && (focus_idx == 0 || focus_idx == n - 1)) {
    need_overspread = true;
    need_center = false;
  } else if (scroller_prefer_center && !scroller_focus_center) {
    need_center = true;
  }

  if (need_center) {
    focused_geom.x = available.x + (available.width - focused_geom.width) / 2;
  } else if (need_overspread) {
  	focused_geom.x = focus_idx == 0 ? available.x + scroller_structs
   		: available.x + available.width - focused_geom.width - scroller_structs;
  } else {
    focused_geom.x = available.x + scroller_structs;
  }

  scroller_arrange_stack(focused_node, focused_geom, gappiv, m);

  for (int i = 1; i <= focus_idx; i++) {
    node_t *node = nodes[focus_idx - i];
    client_t *c = node->client;
    struct wlr_box geom;
    geom.width = (int)(max_width * c->scroller_proportion);
    geom.height = focused_geom.height;
    geom.y = focused_geom.y;

    node_t *next_node = nodes[focus_idx - i + 1];
    geom.x = next_node->client->tiled_rectangle.x - gappih - geom.width;

    scroller_arrange_stack(node, geom, gappiv, m);
  }

  for (int i = 1; i < n - focus_idx; i++) {
    node_t *node = nodes[focus_idx + i];
    client_t *c = node->client;
    struct wlr_box geom;
    geom.width = (int)(max_width * c->scroller_proportion);
    geom.height = focused_geom.height;
    geom.y = focused_geom.y;

    node_t *prev_node = nodes[focus_idx + i - 1];
    geom.x = prev_node->client->tiled_rectangle.x + prev_node->client->tiled_rectangle.width + gappih;

    scroller_arrange_stack(node, geom, gappiv, m);
  }

  free(nodes);
}

void scroller_stack_push(client_t *head, client_t *new_client) {
  if (!head || !new_client) return;
  if (new_client->prev_in_stack || new_client->next_in_stack) {
    wlr_log(WLR_ERROR, "Client already in a stack");
    return;
  }

  client_t *tail = head;
  while (tail->next_in_stack)
    tail = tail->next_in_stack;

  tail->next_in_stack = new_client;
  new_client->prev_in_stack = tail;
  new_client->next_in_stack = NULL;
}

void scroller_stack_remove(client_t *client) {
  if (!client) return;

  if (client->prev_in_stack)
    client->prev_in_stack->next_in_stack = client->next_in_stack;

  if (client->next_in_stack)
    client->next_in_stack->prev_in_stack = client->prev_in_stack;

  client->prev_in_stack = NULL;
  client->next_in_stack = NULL;
}

void scroller_resize_width(client_t *client, float delta) {
  if (!client) return;

  client_t *head = scroller_get_stack_head(client);
  float new_proportion = head->scroller_proportion + delta;

  if (new_proportion < 0.1f) new_proportion = 0.1f;
  if (new_proportion > 1.0f) new_proportion = 1.0f;

  head->scroller_proportion = new_proportion;
}

void scroller_resize_stack(client_t *client, float delta) {
  if (!client) return;

  float new_proportion = client->stack_proportion + delta;

  if (new_proportion < 0.1f) new_proportion = 0.1f;
  if (new_proportion > 1.0f) new_proportion = 1.0f;

  client->stack_proportion = new_proportion;
}

void scroller_set_proportion(client_t *client, float proportion) {
  if (!client) return;

  client_t *head = scroller_get_stack_head(client);

  if (proportion < 0.1f) proportion = 0.1f;
  if (proportion > 1.0f) proportion = 1.0f;

  head->scroller_proportion = proportion;

  wlr_log(WLR_DEBUG, "scroller_set_proportion: set to %.2f", proportion);
}

void scroller_cycle_proportion_preset(client_t *client) {
  if (!client || !scroller_proportion_preset || scroller_proportion_preset_count == 0) {
    wlr_log(WLR_DEBUG, "scroller_cycle_proportion_preset: no presets available");
    return;
  }

  client_t *head = scroller_get_stack_head(client);

  int current_index = -1;
  float current_prop = head->scroller_proportion;

  for (int i = 0; i < scroller_proportion_preset_count; i++) {
    if (fabsf(scroller_proportion_preset[i] - current_prop) < 0.01f) {
      current_index = i;
      break;
    }
  }

  int next_index;
  if (current_index >= 0 && current_index < scroller_proportion_preset_count - 1) {
    next_index = current_index + 1;
  } else {
    next_index = 0;
  }

  float new_proportion = scroller_proportion_preset[next_index];
  head->scroller_proportion = new_proportion;

  wlr_log(WLR_INFO, "scroller_cycle_proportion_preset: %.2f -> %.2f (preset %d/%d)",
          current_prop, new_proportion, next_index + 1, scroller_proportion_preset_count);
}

void scroller_center_window(desktop_t *d, client_t *client) {
  if (!d || !client || !client->toplevel || !client->toplevel->node) {
    return;
  }

  node_t *n = client->toplevel->node;
  monitor_t *m = n->monitor;

  if (!m) {
    return;
  }

  if (d->layout != LAYOUT_SCROLLER) {
    wlr_log(WLR_DEBUG, "scroller_center_window: not in scroller layout");
    return;
  }

  if (d->focus != n) {
    d->focus = n;
  }

  bool old_center = scroller_focus_center;
  scroller_focus_center = true;

  arrange(m, d, true);

  scroller_focus_center = old_center;

  wlr_log(WLR_INFO, "scroller_center_window: centered window");
}

void scroller_apply_client_rules(client_t *c, float rule_proportion, float rule_proportion_single) {
  if (!c) return;

  if (rule_proportion > 0.0f) {
    if (rule_proportion < 0.1f) rule_proportion = 0.1f;
    if (rule_proportion > 1.0f) rule_proportion = 1.0f;

    c->scroller_proportion = rule_proportion;
    wlr_log(WLR_DEBUG, "scroller_apply_client_rules: set proportion to %.2f", rule_proportion);
  }

  if (rule_proportion_single > 0.0f) {
    if (rule_proportion_single < 0.1f) rule_proportion_single = 0.1f;
    if (rule_proportion_single > 1.0f) rule_proportion_single = 1.0f;

    c->scroller_proportion_single = rule_proportion_single;
    wlr_log(WLR_DEBUG, "scroller_apply_client_rules: set proportion_single to %.2f", rule_proportion_single);
  }
}

bool scroller_focus_next(desktop_t *d) {
  if (!d || !d->root || !d->focus || !d->focus->client) return false;

  client_t *current = scroller_get_stack_head(d->focus->client);

  node_t **nodes = NULL;
  int n = scroller_collect_nodes(d, &nodes);
  if (n == 0) return false;

  int current_idx = -1;
  for (int i = 0; i < n; i++) {
    if (nodes[i]->client == current) {
      current_idx = i;
      break;
    }
  }

  bool result = false;
  if (current_idx >= 0 && current_idx < n - 1) {
    d->focus = nodes[current_idx + 1];
    result = true;
  }

  free(nodes);
  return result;
}

bool scroller_focus_prev(desktop_t *d) {
  if (!d || !d->root || !d->focus || !d->focus->client) return false;

  client_t *current = scroller_get_stack_head(d->focus->client);

  node_t **nodes = NULL;
  int n = scroller_collect_nodes(d, &nodes);
  if (n == 0) return false;

  int current_idx = -1;
  for (int i = 0; i < n; i++) {
    if (nodes[i]->client == current) {
      current_idx = i;
      break;
    }
  }

  bool result = false;
  if (current_idx > 0) {
    d->focus = nodes[current_idx - 1];
    result = true;
  }

  free(nodes);
  return result;
}

bool scroller_focus_down(desktop_t *d) {
  if (!d || !d->focus || !d->focus->client) return false;

  client_t *current = d->focus->client;
  if (!current->next_in_stack) return false;

  for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root)) {
    if (n->client == current->next_in_stack) {
      d->focus = n;
      return true;
    }
  }

  return false;
}

bool scroller_focus_up(desktop_t *d) {
  if (!d || !d->focus || !d->focus->client) return false;

  client_t *current = d->focus->client;
  if (!current->prev_in_stack) return false;

  for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root)) {
    if (n->client == current->prev_in_stack) {
      d->focus = n;
      return true;
    }
  }

  return false;
}
