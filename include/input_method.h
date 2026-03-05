#pragma once

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wayland-server.h>

struct bwm_ime_relay {
	struct wl_list text_inputs;
	struct wlr_input_method_v2 *input_method;
	struct wlr_surface *focused_surface;

	struct wlr_keyboard_modifiers forwarded_modifiers;

	struct bwm_ime_text *active_text_input;

	struct wl_list popups;
	struct wlr_scene_tree *popup_tree;

	struct wl_listener new_text_input;
	struct wl_listener new_input_method;

	struct wl_listener input_method_commit;
	struct wl_listener input_method_grab_keyboard;
	struct wl_listener input_method_destroy;
	struct wl_listener input_method_new_popup_surface;

	struct wl_listener keyboard_grab_destroy;
	struct wl_listener focused_surface_destroy;
};

struct bwm_ime_popup {
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *scene_surface;
	struct bwm_ime_relay *relay;
	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener commit;
};

struct bwm_ime_text {
	struct bwm_ime_relay *relay;
	struct wlr_text_input_v3 *input;
	struct wl_list link;

	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
};

struct bwm_ime_relay *input_method_relay_create(void);
void input_method_relay_finish(struct bwm_ime_relay *relay);
void input_method_relay_set_focus(struct bwm_ime_relay *relay, struct wlr_surface *surface);

bool input_method_keyboard_grab_forward_key(struct wlr_keyboard *keyboard, struct wlr_keyboard_key_event *event);
bool input_method_keyboard_grab_forward_modifiers(struct wlr_keyboard *keyboard);
