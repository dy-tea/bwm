#include "types.h"
#include "keyboard.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include "workspace.h"
#include "config.h"
#include "input.h"
#include <stdlib.h>
#include <unistd.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/util/log.h>

extern struct bwm_server server;

extern keybind_t keybinds[MAX_KEYBINDS];
extern size_t num_keybinds;
extern submap_t *active_submap;

bool handle_keybind_raw(uint32_t modifiers, uint32_t keycode, bool pressed);

static monitor_t *find_monitor_for_desktop(desktop_t *d) {
  monitor_t *m = mon_head;
  while (m != NULL) {
    desktop_t *desk = m->desk_head;
    while (desk != NULL) {
      if (desk == d)
        return m;
      desk = desk->next;
    }
    m = m->next;
  }
  return NULL;
}

void handle_new_keyboard(struct wlr_input_device *device) {
  struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

  struct bwm_keyboard *keyboard = calloc(1, sizeof(struct bwm_keyboard));
  keyboard->wlr_keyboard = wlr_keyboard;

  input_config_t *config = input_config_get_for_device(device->name, INPUT_CONFIG_TYPE_KEYBOARD);
  if (config) {
    input_config_apply(config, device);
  } else {
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);
  }

  keyboard->repeat_rate = wlr_keyboard->repeat_info.rate;
  keyboard->repeat_delay = wlr_keyboard->repeat_info.delay;

  keyboard->destroy.notify = keyboard_destroy;
  wl_signal_add(&device->events.destroy, &keyboard->destroy);

  wl_list_insert(&server.physical_keyboards, &keyboard->all_link);
  keyboard_group_add(keyboard);

  if (!server.seat->keyboard_state.keyboard)
    wlr_seat_set_keyboard(server.seat, keyboard->wlr_keyboard);

  wlr_log(WLR_INFO, "New keyboard configured: %s", device->name);
}

void keyboard_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
  struct bwm_keyboard *keyboard =
      wl_container_of(listener, keyboard, modifiers);
  wlr_seat_set_keyboard(server.seat, keyboard->wlr_keyboard);
  wlr_seat_keyboard_notify_modifiers(server.seat,
                                     &keyboard->wlr_keyboard->modifiers);
}

void keyboard_key(struct wl_listener *listener, void *data) {
  struct bwm_keyboard *keyboard = wl_container_of(listener, keyboard, key);
  struct wlr_keyboard_key_event *event = data;
  struct wlr_seat *seat = server.seat;

  wlr_idle_notifier_v1_notify_activity(server.idle_notifier, seat);

  // get keysym
  uint32_t keycode = event->keycode + 8;
  const xkb_keysym_t *syms;
  int nsyms =
      xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

  bool handled = false;
  uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    // first try raw keycode for number keys
    if (handle_keybind_raw(modifiers, event->keycode, true)) {
      handled = true;
    } else {
      // fallback to keysym
      for (int i = 0; i < nsyms; i++)
        if ((handled = handle_keybind(modifiers, syms[i])))
          break;
    }
  }

  if (!handled) {
    // pass key to focused client
    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                                 event->state);
  }
}

void keyboard_destroy(struct wl_listener *listener, void *data) {
  struct bwm_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
  struct wlr_input_device *device = data;
  (void)device;

  if (keyboard->group)
    keyboard_group_remove(keyboard);

  if (keyboard->all_link.next || keyboard->all_link.prev)
    wl_list_remove(&keyboard->all_link);

  if (keyboard->is_representative || keyboard->group == NULL)
    if (keyboard->active_link.next || keyboard->active_link.prev)
      wl_list_remove(&keyboard->active_link);

  if (keyboard->modifiers.link.next)
    wl_list_remove(&keyboard->modifiers.link);
  if (keyboard->key.link.next)
    wl_list_remove(&keyboard->key.link);
  if (keyboard->destroy.link.next)
    wl_list_remove(&keyboard->destroy.link);

  if (keyboard->wlr_keyboard->keymap)
    xkb_keymap_unref(keyboard->wlr_keyboard->keymap);
  free(keyboard);
}

// keybind handling using raw keycode (for number keys 1-0)
bool handle_keybind_raw(uint32_t modifiers, uint32_t keycode, bool pressed) {
  if (!pressed)
    return false;

  if (active_submap) {
    for (size_t i = 0; i < active_submap->num_keybinds; i++) {
      keybind_t *kb = &active_submap->keybinds[i];
      if (kb->use_keycode && keybind_matches(kb, modifiers, 0, keycode)) {
        execute_keybind(kb);
        return true;
      }
    }
    return false;
  }

  for (size_t i = 0; i < num_keybinds; i++) {
    keybind_t *kb = &keybinds[i];
    if (kb->use_keycode && keybind_matches(kb, modifiers, 0, keycode)) {
      execute_keybind(kb);
      return true;
    }
  }

  return false;
}

// keybind handling
bool handle_keybind(uint32_t modifiers, xkb_keysym_t sym) {
  // handle vt switch
  if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
    if (server.session) {
      wlr_session_change_vt(server.session,
                            (unsigned int)(sym + 1 - XKB_KEY_XF86Switch_VT_1));
      return true;
    }
  }

  if (active_submap && sym == XKB_KEY_Escape) {
    exit_submap();
    return true;
  }

  // check if in submap
  if (active_submap) {
    wlr_log(WLR_DEBUG, "In submap '%s' with %zu keybinds, looking for keysym=%u mod=%u",
        active_submap->name, active_submap->num_keybinds, sym, modifiers);
    for (size_t i = 0; i < active_submap->num_keybinds; i++) {
      keybind_t *kb = &active_submap->keybinds[i];
      wlr_log(WLR_DEBUG, "  checking submap keybind %zu: keysym=%u mod=%u action=%d",
          i, kb->keysym, kb->modifiers, kb->action);
      if (!kb->use_keycode && keybind_matches(kb, modifiers, sym, 0)) {
        execute_keybind(kb);
        return true;
      }
    }
    return false;
  }

  // check global user-defined keybinds
  for (size_t i = 0; i < num_keybinds; i++) {
    keybind_t *kb = &keybinds[i];
    if (!kb->use_keycode && keybind_matches(kb, modifiers, sym, 0)) {
      execute_keybind(kb);
      return true;
    }
  }

  return false;
}

// navigation actions
void focus_west(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_WEST);
  if (n != NULL) {
    n = second_extrema(n);
    if (n != NULL) {
      focus_node(mon, mon->desk, n);
      wlr_log(WLR_DEBUG, "Focused west");
    }
  }
}

void focus_east(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_EAST);
  if (n != NULL) {
    n = first_extrema(n);
    if (n != NULL) {
      focus_node(mon, mon->desk, n);
      wlr_log(WLR_DEBUG, "Focused east");
    }
  }
}

void focus_north(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_NORTH);
  if (n != NULL) {
    n = second_extrema(n);
    if (n != NULL) {
      focus_node(mon, mon->desk, n);
      wlr_log(WLR_DEBUG, "Focused north");
    }
  }
}

void focus_south(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_SOUTH);
  if (n != NULL) {
    n = first_extrema(n);
    if (n != NULL) {
      focus_node(mon, mon->desk, n);
      wlr_log(WLR_DEBUG, "Focused south");
    }
  }
}

// Window swapping actions
void swap_west(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_WEST);
  if (n != NULL) {
    n = second_extrema(n);
    if (n != NULL) {
      swap_nodes(mon, mon->desk, mon->desk->focus, mon, mon->desk, n);
      wlr_log(WLR_INFO, "Swapped with west window");
    }
  }
}

void swap_east(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_EAST);
  if (n != NULL) {
    n = first_extrema(n);
    if (n != NULL) {
      swap_nodes(mon, mon->desk, mon->desk->focus, mon, mon->desk, n);
      wlr_log(WLR_INFO, "Swapped with east window");
    }
  }
}

void swap_north(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_NORTH);
  if (n != NULL) {
    n = second_extrema(n);
    if (n != NULL) {
      swap_nodes(mon, mon->desk, mon->desk->focus, mon, mon->desk, n);
      wlr_log(WLR_INFO, "Swapped with north window");
    }
  }
}

void swap_south(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = find_fence(mon->desk->focus, DIR_SOUTH);
  if (n != NULL) {
    n = first_extrema(n);
    if (n != NULL) {
      swap_nodes(mon, mon->desk, mon->desk->focus, mon, mon->desk, n);
      wlr_log(WLR_INFO, "Swapped with south window");
    }
  }
}

void close_focused(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  kill_node(mon, mon->desk, mon->desk->focus);

  wlr_log(WLR_INFO, "Closing focused window");
}

void toggle_floating(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = mon->desk->focus;
  if (n->client == NULL)
    return;

  if (n->client->state == STATE_FLOATING) {
    n->hidden = false;
    wlr_scene_node_reparent(&n->client->toplevel->scene_tree->node,
                            server.tile_tree);
    set_state(mon, mon->desk, n, STATE_TILED);
    wlr_log(WLR_INFO, "Window tiled");
  } else if (n->client->state == STATE_TILED) {
    n->client->floating_rectangle = n->rectangle;
    n->hidden = true;
    wlr_scene_node_reparent(&n->client->toplevel->scene_tree->node,
                            server.float_tree);
    set_state(mon, mon->desk, n, STATE_FLOATING);
    wlr_log(WLR_INFO, "Window floating");
  }
}

void toggle_fullscreen(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = mon->desk->focus;
  if (n->client == NULL || n->client->toplevel == NULL)
    return;

  if (n->client->state == STATE_FULLSCREEN) {
    set_state(mon, mon->desk, n, n->client->last_state);
    if (n->client->last_state == STATE_FLOATING)
      wlr_scene_node_reparent(&n->client->toplevel->scene_tree->node,
                              server.float_tree);
    else if (n->client->last_state == STATE_TILED)
      wlr_scene_node_reparent(&n->client->toplevel->scene_tree->node,
                              server.tile_tree);
    wlr_xdg_toplevel_set_fullscreen(n->client->toplevel->xdg_toplevel, false);
    wlr_log(WLR_INFO, "Fullscreen disabled");
  } else {
    wlr_scene_node_reparent(&n->client->toplevel->scene_tree->node,
                            server.full_tree);
    set_state(mon, mon->desk, n, STATE_FULLSCREEN);
    wlr_xdg_toplevel_set_fullscreen(n->client->toplevel->xdg_toplevel, true);
    wlr_log(WLR_INFO, "Fullscreen enabled");
  }
}

void toggle_pseudo_tiled(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = mon->desk->focus;
  if (n->client == NULL)
    return;

  if (n->client->state == STATE_PSEUDO_TILED) {
    set_state(mon, mon->desk, n, STATE_TILED);
    wlr_log(WLR_INFO, "Window tiled");
  } else {
    struct wlr_box base_rect = n->client->toplevel->xdg_toplevel->base->geometry;
    n->client->floating_rectangle = (struct wlr_box){
      .x = 0,
      .y = 0,
      .width = base_rect.width,
      .height = base_rect.height
    };
    set_state(mon, mon->desk, n, STATE_PSEUDO_TILED);
    wlr_log(WLR_INFO, "Window pseudo_tiled");
  }
}

void focus_next_desktop(void) {
  if (mon == NULL || mon->desk == NULL)
    return;

  desktop_t *next = mon->desk->next;
  if (next != NULL) {
    mon->desk = next;
    wlr_log(WLR_INFO, "Switched to next desktop");
  }
}

void focus_prev_desktop(void) {
  if (mon == NULL || mon->desk == NULL)
    return;

  desktop_t *prev = mon->desk->prev;
  if (prev != NULL) {
    mon->desk = prev;
    wlr_log(WLR_INFO, "Switched to previous desktop");
  }
}

void send_to_desktop(int desktop_index) {
  if (mon == NULL)
    return;

  // get index of desktop
  desktop_t *target = mon->desk_head;
  int idx = 0;
  for (; target != NULL && idx < desktop_index; target = target->next, ++idx)
    ;

  if (!target) {
    wlr_log(WLR_ERROR, "Desktop not found at index: %d", desktop_index);
    return;
  }

  if (mon->desk == target)
    return;

  if (mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = mon->desk->focus;
  if (n == NULL || n->client == NULL)
    return;

  desktop_t *src_desk = mon->desk;

  n->destroying = false;
  n->ntxnrefs = 0;

  n->client->shown = false;
  wlr_scene_node_set_enabled(&n->client->toplevel->scene_tree->node, false);

  // remove from source desktop
  remove_node(mon, src_desk, n);

  // update focus on source desktop
  if (src_desk->focus == n) {
    if (src_desk->root != NULL) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus != NULL) {
        src_desk->focus = new_focus;
        focus_node(mon, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }
  }

  monitor_t *target_mon = find_monitor_for_desktop(target);
  if (target_mon == NULL) {
    wlr_log(WLR_ERROR, "Could not find monitor for desktop: %s", target->name);
    return;
  }

  insert_node(target_mon, target, n, find_public(target));
  target->focus = n;

  arrange(mon, src_desk, true);
  arrange(target_mon, target, false);

  wlr_log(WLR_INFO, "Sent window to desktop: %s", target->name);
}

void send_to_desktop_by_name(const char *name) {
  if (mon == NULL)
    return;

  desktop_t *target = find_desktop_by_name(name);
  if (!target) {
    wlr_log(WLR_ERROR, "Desktop not found: %s", name);
    return;
  }

  if (mon->desk == target)
    return;

  if (mon->desk == NULL || mon->desk->focus == NULL)
    return;

  node_t *n = mon->desk->focus;
  if (n == NULL || n->client == NULL)
    return;

  desktop_t *src_desk = mon->desk;

  monitor_t *target_mon = find_monitor_for_desktop(target);
  if (target_mon == NULL) {
    wlr_log(WLR_ERROR, "Could not find monitor for desktop: %s", target->name);
    return;
  }

  n->destroying = false;
  n->ntxnrefs = 0;

  n->client->shown = false;
  wlr_scene_node_set_enabled(&n->client->toplevel->scene_tree->node, false);

  remove_node(mon, src_desk, n);

  if (src_desk->focus == n) {
    if (src_desk->root != NULL) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus != NULL) {
        src_desk->focus = new_focus;
        focus_node(mon, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }
  }

  // add to target desktop
  insert_node(target_mon, target, n, find_public(target));
  target->focus = n;

  // Ensure the moved node respects initial_polarity
  // In SPIRAL mode, insert_node may place it as second_child regardless
  if (n->parent != NULL) {
    if (initial_polarity == FIRST_CHILD && n->parent->second_child == n) {
      // Node is second child but should be first, swap them
      node_t *p = n->parent;
      node_t *sibling = p->first_child;
      p->first_child = n;
      p->second_child = sibling;
      n->parent = p;
      if (sibling != NULL)
        sibling->parent = p;
    } else if (initial_polarity == SECOND_CHILD && n->parent->first_child == n) {
      // Node is first child but should be second, swap them
      node_t *p = n->parent;
      node_t *sibling = p->second_child;
      p->second_child = n;
      p->first_child = sibling;
      n->parent = p;
      if (sibling != NULL)
        sibling->parent = p;
    }
  }

  arrange(mon, src_desk, true);
  arrange(target_mon, target, false);

  wlr_log(WLR_INFO, "Sent window to desktop: %s", target->name);
}

void send_to_next_desktop(void) {
  if (mon == NULL || mon->desk == NULL)
    return;

  desktop_t *next = mon->desk->next;
  if (next != NULL) {
    send_to_desktop_by_name(next->name);
    wlr_log(WLR_INFO, "Sent window to next desktop");
  }
}

void send_to_prev_desktop(void) {
  if (mon == NULL || mon->desk == NULL)
    return;

  desktop_t *prev = mon->desk->prev;
  if (prev != NULL) {
    send_to_desktop_by_name(prev->name);
    wlr_log(WLR_INFO, "Sent window to previous desktop");
  }
}

void toggle_monocle(void) {
  if (mon == NULL || mon->desk == NULL)
    return;

  desktop_t *d = mon->desk;

  if (d->layout == LAYOUT_MONOCLE) {
    d->layout = d->user_layout;
    wlr_log(WLR_INFO, "Switched to tiled layout");

    if (d->root) {
      for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root)) {
        if (n->client && n->client->toplevel && n->client->state != STATE_FULLSCREEN) {
          n->client->toplevel->client_maximized = false;
          wlr_xdg_toplevel_set_maximized(n->client->toplevel->xdg_toplevel, false);
        }
      }
    }
  } else {
    d->user_layout = d->layout;
    d->layout = LAYOUT_MONOCLE;
    wlr_log(WLR_INFO, "Switched to monocle layout");

    if (d->root) {
      for (node_t *n = first_extrema(d->root); n != NULL; n = next_leaf(n, d->root)) {
        if (n->client && n->client->toplevel && n->client->state != STATE_FULLSCREEN) {
          n->client->toplevel->client_maximized = true;
          wlr_xdg_toplevel_set_maximized(n->client->toplevel->xdg_toplevel, true);
        }
      }
    }
  }

  arrange(mon, d, true);

  if (d->focus != NULL)
    focus_node(mon, d, d->focus);
}

void rotate_clockwise(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  rotate_tree(mon->desk->root, 90);
  arrange(mon, mon->desk, true);
  wlr_log(WLR_INFO, "Rotated tree clockwise");
}

void rotate_counterclockwise(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  rotate_tree(mon->desk->root, 270);
  arrange(mon, mon->desk, true);
  wlr_log(WLR_INFO, "Rotated tree counterclockwise");
}

void flip_horizontal(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  flip_tree(mon->desk->root, FLIP_HORIZONTAL);
  arrange(mon, mon->desk, true);
  wlr_log(WLR_INFO, "Flipped tree horizontally");
}

void flip_vertical(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  flip_tree(mon->desk->root, FLIP_VERTICAL);
  arrange(mon, mon->desk, true);
  wlr_log(WLR_INFO, "Flipped tree vertically");
}

void presel_west(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  presel_dir(mon, mon->desk, mon->desk->focus, DIR_WEST);
  wlr_log(WLR_INFO, "Preselected west");
}

void presel_east(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  presel_dir(mon, mon->desk, mon->desk->focus, DIR_EAST);
  wlr_log(WLR_INFO, "Preselected east");
}

void presel_north(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  presel_dir(mon, mon->desk, mon->desk->focus, DIR_NORTH);
  wlr_log(WLR_INFO, "Preselected north");
}

void presel_south(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  presel_dir(mon, mon->desk, mon->desk->focus, DIR_SOUTH);
  wlr_log(WLR_INFO, "Preselected south");
}

void cancel_presel(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
    return;

  presel_cancel(mon, mon->desk, mon->desk->focus);
  wlr_log(WLR_INFO, "Cancelled preselection");
}

static bool repeat_info_match(struct wlr_keyboard *a, struct wlr_keyboard *b) {
  return a->repeat_info.rate == b->repeat_info.rate &&
         a->repeat_info.delay == b->repeat_info.delay;
}

// Remove a keyboard from its group (if any)
void keyboard_group_remove(struct bwm_keyboard *keyboard) {
  if (!keyboard->group)
    return;

  struct bwm_keyboard_group *group = keyboard->group;
  struct wlr_keyboard_group *wlr_group = group->wlr_group;

  wlr_log(WLR_DEBUG, "Removing keyboard %p from group %p", (void*)keyboard, (void*)wlr_group);

  wlr_keyboard_group_remove_keyboard(wlr_group, keyboard->wlr_keyboard);
  keyboard->group = NULL;

  if (wl_list_empty(&wlr_group->devices)) {
    wlr_log(WLR_DEBUG, "Destroying empty keyboard group %p", (void*)wlr_group);

    if (server.seat->keyboard_state.keyboard == group->representative->wlr_keyboard)
      wlr_seat_set_keyboard(server.seat, NULL);

    if (group->representative) {
      wl_list_remove(&group->representative->active_link);
      wl_list_remove(&group->representative->modifiers.link);
      wl_list_remove(&group->representative->key.link);
      free(group->representative);
    }

    wl_list_remove(&group->link);

    wlr_keyboard_group_destroy(wlr_group);
    free(group);
  }
}

void keyboard_group_remove_invalid(struct bwm_keyboard *keyboard) {
  if (!keyboard->group)
    return;

  struct bwm_keyboard_group *group = keyboard->group;
  keyboard_grouping_t grouping = get_keyboard_grouping();

  bool should_remove = false;
  switch (grouping) {
  case KEYBOARD_GROUP_NONE:
    should_remove = true;
    break;
  case KEYBOARD_GROUP_DEFAULT:  // fallthrough
  case KEYBOARD_GROUP_SMART: {
    if (!wlr_keyboard_keymaps_match(keyboard->wlr_keyboard->keymap, group->wlr_group->keyboard.keymap) ||
        !repeat_info_match(keyboard->wlr_keyboard, &group->wlr_group->keyboard)) {
      should_remove = true;
    }
    break;
  }
  }

  if (should_remove)
    keyboard_group_remove(keyboard);
}

void keyboard_group_add(struct bwm_keyboard *keyboard) {
  keyboard_grouping_t grouping = get_keyboard_grouping();

  if (grouping == KEYBOARD_GROUP_NONE) {
    keyboard->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&keyboard->wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_key;
    wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);
    wl_list_insert(&server.keyboards, &keyboard->active_link);
    return;
  }

  struct bwm_keyboard_group *group;
  wl_list_for_each(group, &server.keyboard_groups, link) {
    if (wlr_keyboard_keymaps_match(keyboard->wlr_keyboard->keymap, group->wlr_group->keyboard.keymap) &&
        repeat_info_match(keyboard->wlr_keyboard, &group->wlr_group->keyboard)) {
      wlr_log(WLR_DEBUG, "Adding keyboard %p to existing group %p", (void*)keyboard, (void*)group->wlr_group);
      wlr_keyboard_group_add_keyboard(group->wlr_group, keyboard->wlr_keyboard);
      keyboard->group = group;
      if (server.seat->keyboard_state.keyboard == keyboard->wlr_keyboard)
        wlr_seat_set_keyboard(server.seat, group->representative->wlr_keyboard);
      return;
    }
  }

  struct bwm_keyboard_group *new_group = calloc(1, sizeof(struct bwm_keyboard_group));
  if (!new_group) {
    wlr_log(WLR_ERROR, "Failed to allocate keyboard group");
    return;
  }

  new_group->wlr_group = wlr_keyboard_group_create();
  if (!new_group->wlr_group) {
    wlr_log(WLR_ERROR, "Failed to create wlr_keyboard_group");
    free(new_group);
    return;
  }
  new_group->wlr_group->data = new_group;

  wlr_keyboard_set_keymap(&new_group->wlr_group->keyboard, keyboard->wlr_keyboard->keymap);
  wlr_keyboard_set_repeat_info(&new_group->wlr_group->keyboard,
                               keyboard->wlr_keyboard->repeat_info.rate,
                               keyboard->wlr_keyboard->repeat_info.delay);

  struct bwm_keyboard *rep = calloc(1, sizeof(struct bwm_keyboard));
  if (!rep) {
    wlr_log(WLR_ERROR, "Failed to allocate group representative keyboard");
    wlr_keyboard_group_destroy(new_group->wlr_group);
    free(new_group);
    return;
  }
  rep->wlr_keyboard = &new_group->wlr_group->keyboard;
  rep->group = NULL;
  rep->is_representative = true;

  // listeners
  rep->modifiers.notify = keyboard_modifiers;
  wl_signal_add(&rep->wlr_keyboard->events.modifiers, &rep->modifiers);
  rep->key.notify = keyboard_key;
  wl_signal_add(&rep->wlr_keyboard->events.key, &rep->key);

  // add to list
  wl_list_insert(&server.keyboards, &rep->active_link);

  new_group->representative = rep;

  wl_list_insert(&server.keyboard_groups, &new_group->link);

  wlr_keyboard_group_add_keyboard(new_group->wlr_group, keyboard->wlr_keyboard);
  keyboard->group = new_group;

  if (server.seat->keyboard_state.keyboard == keyboard->wlr_keyboard)
    wlr_seat_set_keyboard(server.seat, new_group->representative->wlr_keyboard);

  wlr_log(WLR_DEBUG, "Created new keyboard group %p for keyboard %p", (void*)new_group, (void*)keyboard);
}

void keyboard_reapply_grouping(void) {
  struct bwm_keyboard *keyboard, *tmp;
  wl_list_for_each_safe(keyboard, tmp, &server.physical_keyboards, all_link) {
    if (keyboard->group) {
      keyboard_group_remove(keyboard);
    } else if (!keyboard->is_representative) {
      if (keyboard->active_link.next || keyboard->active_link.prev)
        wl_list_remove(&keyboard->active_link);
      if (keyboard->modifiers.link.next)
        wl_list_remove(&keyboard->modifiers.link);
      if (keyboard->key.link.next)
        wl_list_remove(&keyboard->key.link);
    }
    keyboard_group_add(keyboard);
  }

  if (!server.seat->keyboard_state.keyboard && !wl_list_empty(&server.keyboards)) {
    struct bwm_keyboard *first = wl_container_of(server.keyboards.next, first, active_link);
    if (first)
      wlr_seat_set_keyboard(server.seat, first->wlr_keyboard);
  }
}
