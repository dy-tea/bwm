#pragma once

#include <wayland-server.h>
#include <wlr/types/wlr_input_device.h>
#include <xkbcommon/xkbcommon.h>

struct bwm_keyboard {
  struct wl_list link;
  struct wlr_keyboard *wlr_keyboard;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
};

// keyboard lifecycle
void handle_new_keyboard(struct wlr_input_device *device);
void keyboard_modifiers(struct wl_listener *listener, void *data);
void keyboard_key(struct wl_listener *listener, void *data);
void keyboard_destroy(struct wl_listener *listener, void *data);

// window navigation
void focus_west(void);
void focus_east(void);
void focus_north(void);
void focus_south(void);

// window manipulation
void close_focused(void);
void toggle_floating(void);
void toggle_fullscreen(void);
void toggle_pseudo_tiled(void);

// window swapping
void swap_west(void);
void swap_east(void);
void swap_north(void);
void swap_south(void);

// desktop/monitor
void focus_next_desktop(void);
void focus_prev_desktop(void);
void send_to_desktop(int desktop_index);
void send_to_desktop_by_name(const char *name);
void send_to_next_desktop(void);
void send_to_prev_desktop(void);
void send_all_to_desktop(int desktop_index);
void toggle_monocle(void);

// layout
void rotate_clockwise(void);
void rotate_counterclockwise(void);
void flip_horizontal(void);
void flip_vertical(void);

// preselection
void presel_west(void);
void presel_east(void);
void presel_north(void);
void presel_south(void);
void cancel_presel(void);

// keybind processing
bool handle_keybind(uint32_t modifiers, xkb_keysym_t sym);
