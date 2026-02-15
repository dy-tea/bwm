#define WLR_USE_UNSTABLE
#include "keyboard.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>

extern struct bwm_server server;

void handle_new_keyboard(struct wlr_input_device *device) {
  struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

  struct bwm_keyboard *keyboard = calloc(1, sizeof(struct bwm_keyboard));
  keyboard->wlr_keyboard = wlr_keyboard;

  // keyboard config
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

  wlr_keyboard_set_keymap(wlr_keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);

  wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

  // register listeners
  keyboard->modifiers.notify = keyboard_modifiers;
  wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);

  keyboard->key.notify = keyboard_key;
  wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);

  keyboard->destroy.notify = keyboard_destroy;
  wl_signal_add(&device->events.destroy, &keyboard->destroy);

  wlr_seat_set_keyboard(server.seat, keyboard->wlr_keyboard);

  wl_list_insert(&server.keyboards, &keyboard->link);

  wlr_log(WLR_INFO, "New keyboard configured");
}

void keyboard_modifiers(struct wl_listener *listener, void *data) {
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

  // get keysym
  uint32_t keycode = event->keycode + 8;
  const xkb_keysym_t *syms;
  int nsyms =
      xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

  bool handled = false;
  uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
    for (int i = 0; i < nsyms; i++)
      if ((handled = handle_keybind(modifiers, syms[i])))
        break;

  if (!handled) {
    // pass key to focused client
    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                                 event->state);
  }
}

void keyboard_destroy(struct wl_listener *listener, void *data) {
  struct bwm_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);

  wl_list_remove(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);

  free(keyboard);
}

// keybind handling
bool handle_keybind(uint32_t modifiers, xkb_keysym_t sym) {
  // super (mod4) is the primary modifier
#define DEBUG
#ifdef DEBUG
  bool mod = modifiers & WLR_MODIFIER_ALT;
#else
  bool mod = modifiers & WLR_MODIFIER_LOGO;
#endif
  bool shift = modifiers & WLR_MODIFIER_SHIFT;
  bool ctrl = modifiers & WLR_MODIFIER_CTRL;

  // handle vt switch
  if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
      if (server.session) {
          wlr_session_change_vt(server.session, (unsigned int)(sym + 1 - XKB_KEY_XF86Switch_VT_1));
          return true;
      }
  }

  // window navigation (Super + hjkl)
  if (mod && !shift && !ctrl) {
    switch (sym) {
    case XKB_KEY_h:
      focus_west();
      return true;
    case XKB_KEY_j:
      focus_south();
      return true;
    case XKB_KEY_k:
      focus_north();
      return true;
    case XKB_KEY_l:
      focus_east();
      return true;
    }
  }

  // window manipulation (Super + Shift + hjkl) - swap
  if (mod && shift && !ctrl) {
    switch (sym) {
    case XKB_KEY_H:
      swap_west();
      return true;
    case XKB_KEY_J:
      swap_south();
      return true;
    case XKB_KEY_K:
      swap_north();
      return true;
    case XKB_KEY_L:
      swap_east();
      return true;
    }
  }

  // close window (Super + Shift + q)
  if (mod && shift && sym == XKB_KEY_Q) {
    close_focused();
    return true;
  }

  // toggle floating (Super + Shift + space)
  if (mod && shift && sym == XKB_KEY_space) {
    toggle_floating();
    return true;
  }

  // toggle fullscreen (Super + f)
  if (mod && !shift && sym == XKB_KEY_f) {
    toggle_fullscreen();
    return true;
  }

  // toggle monocle (Super + m)
  if (mod && !shift && sym == XKB_KEY_m) {
    toggle_monocle();
    return true;
  }

  // preselection (Super + Ctrl + hjkl)
  if (mod && ctrl && !shift) {
    switch (sym) {
    case XKB_KEY_h:
      presel_west();
      return true;
    case XKB_KEY_j:
      presel_south();
      return true;
    case XKB_KEY_k:
      presel_north();
      return true;
    case XKB_KEY_l:
      presel_east();
      return true;
    }
  }

  // cancel preselection (Super + Ctrl + space)
  if (mod && ctrl && sym == XKB_KEY_space) {
    cancel_presel();
    return true;
  }

  // rotate (Super + r/R)
  if (mod && sym == XKB_KEY_r) {
    if (shift)
      rotate_counterclockwise();
    else
      rotate_clockwise();
    return true;
  }

  // flip (Super + {comma,period})
  if (mod && !shift) {
    if (sym == XKB_KEY_comma) {
      flip_horizontal();
      return true;
    } else if (sym == XKB_KEY_period) {
      flip_vertical();
      return true;
    }
  }

  // spawn terminal (Super + Return)
  if (mod && !shift && sym == XKB_KEY_Return) {
    spawn_terminal();
    return true;
  }

  // desktop switching (Super + 1-9)
  if (mod && !shift && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
    int desktop_num = sym - XKB_KEY_1;
    // TODO: implement desktop switching
    wlr_log(WLR_INFO, "Desktop switching not yet implemented: %d", desktop_num);
    return true;
  }

  // send to desktop (Super + Shift + 1-9)
  if (mod && shift && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
    int desktop_num = sym - XKB_KEY_1;
    send_to_desktop(desktop_num);
    return true;
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
    set_state(mon, mon->desk, n, STATE_TILED);
    wlr_log(WLR_INFO, "Window tiled");
  } else if (n->client->state == STATE_TILED) {
    n->client->floating_rectangle = n->rectangle;
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
    wlr_xdg_toplevel_set_fullscreen(n->client->toplevel->xdg_toplevel, false);
    wlr_log(WLR_INFO, "Fullscreen disabled");
  } else {
    set_state(mon, mon->desk, n, STATE_FULLSCREEN);
    wlr_xdg_toplevel_set_fullscreen(n->client->toplevel->xdg_toplevel, true);
    wlr_log(WLR_INFO, "Fullscreen enabled");
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
  // TODO: implement desktop transfer
  wlr_log(WLR_INFO, "Send to desktop %d not yet implemented", desktop_index);
}

void toggle_monocle(void) {
  if (mon == NULL || mon->desk == NULL)
    return;

  desktop_t *d = mon->desk;

  if (d->layout == LAYOUT_MONOCLE) {
    d->layout = d->user_layout;
    wlr_log(WLR_INFO, "Switched to tiled layout");
  } else {
    d->user_layout = d->layout;
    d->layout = LAYOUT_MONOCLE;
    wlr_log(WLR_INFO, "Switched to monocle layout");
  }

  arrange(mon, d);

  if (d->focus != NULL)
    focus_node(mon, d, d->focus);
}

void rotate_clockwise(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  rotate_tree(mon->desk->root, 90);
  arrange(mon, mon->desk);
  wlr_log(WLR_INFO, "Rotated tree clockwise");
}

void rotate_counterclockwise(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  rotate_tree(mon->desk->root, 270);
  arrange(mon, mon->desk);
  wlr_log(WLR_INFO, "Rotated tree counterclockwise");
}

void flip_horizontal(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  flip_tree(mon->desk->root, FLIP_HORIZONTAL);
  arrange(mon, mon->desk);
  wlr_log(WLR_INFO, "Flipped tree horizontally");
}

void flip_vertical(void) {
  if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
    return;

  flip_tree(mon->desk->root, FLIP_VERTICAL);
  arrange(mon, mon->desk);
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

void spawn_terminal(void) {
  if (fork() == 0) {
    execlp("foot", "foot", NULL);
    execlp("alacritty", "alacritty", NULL);
    execlp("kitty", "kitty", NULL);
    execlp("wterm", "wterm", NULL);
    execlp("weston-terminal", "weston-terminal", NULL);

    wlr_log(WLR_ERROR, "Failed to spawn terminal");
    exit(1);
  }

  wlr_log(WLR_INFO, "Spawned terminal");
}
