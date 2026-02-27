#pragma once

#include <stdint.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server.h>

#define MAX_KEYBINDS 256
#define MAXLEN 256

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
    BIND_FOCUS_WEST,
    BIND_FOCUS_SOUTH,
    BIND_FOCUS_NORTH,
    BIND_FOCUS_EAST,
    BIND_SWAP_WEST,
    BIND_SWAP_SOUTH,
    BIND_SWAP_NORTH,
    BIND_SWAP_EAST,
    BIND_PRESEL_WEST,
    BIND_PRESEL_SOUTH,
    BIND_PRESEL_NORTH,
    BIND_PRESEL_EAST,
    BIND_PRESEL_CANCEL,
    BIND_TOGGLE_FLOATING,
    BIND_TOGGLE_FULLSCREEN,
    BIND_TOGGLE_PSEUDO_TILED,
    BIND_TOGGLE_MONOCLE,
    BIND_ROTATE_CW,
    BIND_ROTATE_CCW,
    BIND_FLIP_HORIZONTAL,
    BIND_FLIP_VERTICAL,
    BIND_DESKTOP_NEXT,
    BIND_DESKTOP_PREV,
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
    BIND_EXTERNAL,
} bind_action_t;

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

struct submap {
    char name[MAXLEN];
    keybind_t keybinds[MAX_KEYBINDS];
    size_t num_keybinds;
    submap_t *parent;
};

extern keybind_t keybinds[MAX_KEYBINDS];
extern size_t num_keybinds;
extern submap_t *active_submap;

void config_init(void);
void config_init_with_config_dir(const char *config_dir);
void config_fini(void);
void run_config(const char *config_path);
void run_config_idle(void *data);
void load_hotkeys_idle(void *data);
void load_hotkeys(const char *config_path);
void reload_hotkeys(void);
bool keybind_matches(keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode);
void execute_keybind(keybind_t *kb);
int get_hotkey_watch_fd(void);
void enter_submap(const char *name);
void exit_submap(void);
