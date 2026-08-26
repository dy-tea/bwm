#pragma once

#include "gesture.h"
#include "types.h"

#include <stdint.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_KEYBINDS 256
#define MAX_GESTUREBINDS 64
#define MAX_HOTCORNERBINDS 16

typedef struct submap submap_t;

typedef enum {
	BIND_NONE = 0,
	BIND_QUIT,
	BIND_ENTER_SUBMAP,
	BIND_EXIT_SUBMAP,
	BIND_NODE_FOCUS,
	BIND_NODE_CLOSE,
	BIND_NODE_STATE_TILED,
	BIND_NODE_STATE_FLOATING,
	BIND_NODE_STATE_FULLSCREEN,
	BIND_NODE_TO_DESKTOP,
	BIND_DESKTOP_FOCUS,
	BIND_DESKTOP_LAYOUT_TILED,
	BIND_DESKTOP_LAYOUT_MONOCLE,
	BIND_FOCUS_NORTH,
	BIND_FOCUS_WEST,
	BIND_FOCUS_SOUTH,
	BIND_FOCUS_EAST,
	BIND_SWAP_NORTH,
	BIND_SWAP_WEST,
	BIND_SWAP_SOUTH,
	BIND_SWAP_EAST,
	BIND_PRESEL_NORTH,
	BIND_PRESEL_WEST,
	BIND_PRESEL_SOUTH,
	BIND_PRESEL_EAST,
	BIND_PRESEL_CANCEL,
	BIND_TOGGLE_FLOATING,
	BIND_TOGGLE_FULLSCREEN,
	BIND_TOGGLE_PSEUDO_TILED,
	BIND_TOGGLE_MONOCLE,
	BIND_TOGGLE_MASTER_STACK,
	BIND_DESKTOP_LAYOUT_MASTER_STACK,
	BIND_MASTER_STACK_INC,
	BIND_MASTER_STACK_DEC,
	BIND_MASTER_STACK_FLIP,
	BIND_MASTER_STACK_CYCLE_ORIENTATION,
	BIND_MASTER_STACK_CYCLE_STACK_LAYOUT,
	BIND_ROTATE_CW,
	BIND_ROTATE_CCW,
	BIND_FLIP_HORIZONTAL,
	BIND_FLIP_VERTICAL,
	BIND_DESKTOP_NEXT,
	BIND_DESKTOP_PREV,
	BIND_DESKTOP_LAST,
	BIND_SEND_TO_DESKTOP_NEXT,
	BIND_SEND_TO_DESKTOP_PREV,
	BIND_SEND_TO_DESKTOP_1,
	BIND_SEND_TO_DESKTOP_2,
	BIND_SEND_TO_DESKTOP_3,
	BIND_SEND_TO_DESKTOP_4,
	BIND_SEND_TO_DESKTOP_5,
	BIND_SEND_TO_DESKTOP_6,
	BIND_SEND_TO_DESKTOP_7,
	BIND_SEND_TO_DESKTOP_8,
	BIND_SEND_TO_DESKTOP_9,
	BIND_SEND_TO_DESKTOP_10,
	BIND_DESKTOP_1,
	BIND_DESKTOP_2,
	BIND_DESKTOP_3,
	BIND_DESKTOP_4,
	BIND_DESKTOP_5,
	BIND_DESKTOP_6,
	BIND_DESKTOP_7,
	BIND_DESKTOP_8,
	BIND_DESKTOP_9,
	BIND_DESKTOP_10,
	BIND_RESIZE_LEFT,
	BIND_RESIZE_RIGHT,
	BIND_RESIZE_UP,
	BIND_RESIZE_DOWN,
	BIND_INTERACTIVE_MOVE,
	BIND_INTERACTIVE_RESIZE,
	BIND_TILING_DRAG,
	BIND_EXTERNAL,
	BIND_GLOBAL_SHORTCUT,
} bind_action_t;

typedef enum {
	KEYBOARD_GROUP_DEFAULT,
	KEYBOARD_GROUP_NONE,
	KEYBOARD_GROUP_SMART,
} keyboard_grouping_t;

typedef struct {
	bind_action_t action;
	int desktop_index;
	const char *submap_name, *external_cmd;
} bind_t;

#define MOUSE_BUTTON_SYNTH_KEYCODE_BASE 0x20000000
#define MOUSE_BUTTON_KEYCODE_OFFSET 272
#define mouse_button_to_keycode(button_index) \
	(MOUSE_BUTTON_SYNTH_KEYCODE_BASE + ((button_index) + MOUSE_BUTTON_KEYCODE_OFFSET))
#define keycode_to_mouse_button_index(keycode) \
	((int)(keycode) - MOUSE_BUTTON_SYNTH_KEYCODE_BASE - MOUSE_BUTTON_KEYCODE_OFFSET)

typedef struct {
	uint32_t modifiers;
	xkb_keysym_t keysym;
	uint32_t keycode;
	bool use_keycode;
	bind_action_t action;
	int desktop_index;
	char submap_name[MAXLEN];
	char external_cmd[MAXLEN];
} keybind_t;

typedef struct {
	enum gesture_type type;
	uint8_t fingers;
	uint32_t directions;
	char *input;
	bind_action_t action;
	int desktop_index;
	char external_cmd[MAXLEN];
} gesturebind_t;

typedef struct {
	enum hotcorner corner;
	int corner_x, corner_y;
	bind_action_t action;
	int desktop_index;
	char external_cmd[MAXLEN];
} hotcornerbind_t;

struct submap {
	char name[MAXLEN];
	keybind_t keybinds[MAX_KEYBINDS];
	size_t num_keybinds;
	submap_t *parent;
};

extern keybind_t keybinds[MAX_KEYBINDS];
extern size_t num_keybinds;
extern keybind_t bell_bind;
extern gesturebind_t gesture_bindings[MAX_GESTUREBINDS];
extern size_t num_gesturebinds;
extern hotcornerbind_t hotcorner_bindings[MAX_HOTCORNERBINDS];
extern size_t num_hotcornerbinds;
extern submap_t *active_submap;
extern bool minimize_to_scratchpad;

void config_init(void);
void config_init_with_config_dir(const char *config_dir);
void config_fini(void);
void run_config(const char *config_path);
void run_config_idle(void *data);
void load_hotkeys_idle(void *data);
void load_hotkeys(const char *config_path);
void reload_hotkeys(void);
bool keybind_matches(const keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym,
	uint32_t keycode);
keybind_t *find_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode,
	bool prefer_keycode, bool fallback_to_global);
bool is_interactive_bind(const keybind_t *kb);
void execute_keybind(const keybind_t *kb);
void execute_bell_bind(void);
void setup_hotkey_event_listener(struct wl_event_loop *event_loop);
void enter_submap(const char *name);
void exit_submap(void);

keyboard_grouping_t get_keyboard_grouping(void);
void set_keyboard_grouping(keyboard_grouping_t grouping);

bool gesturebind_matches(const gesturebind_t *gb, enum gesture_type type, uint8_t fingers);
void execute_gesturebind(const gesturebind_t *gb);

bool hotcornerbind_matches(const hotcornerbind_t *hc, int corner_x, int corner_y);
hotcornerbind_t *hotcorner_bind_match(int corner_x, int corner_y);
void execute_hotcornerbind(const hotcornerbind_t *hc);

void execute_bind(bind_t b);
void execute_bind_action(bind_action_t action);

bind_action_t bind_action_from_name(const char *name);
const char *bind_action_name(bind_action_t action);

uint32_t parse_modifiers(const char *mod_str);
xkb_keysym_t parse_keysym(const char *name);
uint32_t parse_keycode(const char *name);
bind_action_t parse_action(const char *cmd, int *desktop_index, char *submap_name);
void add_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode, bool use_keycode,
	bind_action_t action, int desktop_index, const char *external_cmd, const char *submap_name);
