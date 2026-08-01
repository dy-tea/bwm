#include "config.h"
#include "global_shortcuts.h"
#include "keyboard.h"
#include "master_stack.h"
#include "once.h"
#include "server.h"
#include "tree.h"
#include "types.h"
#include "workspace.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#define DOORS_CONFIG_DIR "/.config/doors"
#define GLOBAL_CONFIG_DIR "/etc/doors"
#define CONFIG_PATH_MAX (PATH_MAX + sizeof(DOORSHKRC_NAME) + 2)
#define DOORSRC_NAME "doorsrc"
#define DOORSHKRC_NAME "doorshkrc"

static const char *custom_config_dir = NULL;

keybind_t keybinds[MAX_KEYBINDS];
size_t num_keybinds = 0;
keybind_t bell_bind = {0};
gesturebind_t gesture_bindings[MAX_GESTUREBINDS];
size_t num_gesturebinds = 0;
hotcornerbind_t hotcorner_bindings[MAX_HOTCORNERBINDS];
size_t num_hotcornerbinds = 0;
submap_t *active_submap = NULL;
bool minimize_to_scratchpad = false;
bool realtime_scheduling = false;

#define MAX_SUBMAPS 32
static submap_t submaps[MAX_SUBMAPS];
static size_t num_submaps = 0;
static submap_t *current_parsing_submap = NULL;

static keyboard_grouping_t keyboard_grouping = KEYBOARD_GROUP_DEFAULT;

static int hotkey_watch_fd = -1;
static char hotkey_config_path[PATH_MAX];
static struct wl_event_loop *hotkey_event_loop = NULL;
static struct wl_event_source *hotkey_event_source = NULL;
static void setup_inotify_watch(const char *config_path);
static void add_hotkey_listener_to_event_loop(void);

static const char *get_config_home(void) {
	if (custom_config_dir)
		return custom_config_dir;

	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0] != '\0')
		return xdg;
	static char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s%s", getenv("HOME") ? getenv("HOME") : "/root", DOORS_CONFIG_DIR);
	return buf;
}

uint32_t parse_modifiers(const char *mod_str) {
	uint32_t mods = 0;
	if (!mod_str || mod_str[0] == '\0')
		return 0;

	char *tmp = strdup(mod_str);
	if (!tmp) {
		wlr_log(WLR_ERROR, "allocation failed");
		return 0;
	}
	char *saveptr;
	char *token = strtok_r(tmp, "+", &saveptr);

	while (token) {
		while (*token == ' ')
			token++;
		char *end = token + strlen(token) - 1;
		while (end > token && *end == ' ')
			*end-- = '\0';

		if (strcmp(token, "super") == 0 || strcmp(token, "Mod4") == 0) {
			mods |= WLR_MODIFIER_LOGO;
		} else if (strcmp(token, "alt") == 0 || strcmp(token, "Mod1") == 0) {
			mods |= WLR_MODIFIER_ALT;
		} else if (strcmp(token, "ctrl") == 0 || strcmp(token, "Control") == 0) {
			mods |= WLR_MODIFIER_CTRL;
		} else if (strcmp(token, "shift") == 0 || strcmp(token, "Shift") == 0) {
			mods |= WLR_MODIFIER_SHIFT;
		} else if (strcmp(token, "mod4") == 0) {
			mods |= WLR_MODIFIER_LOGO;
		} else if (strcmp(token, "mod1") == 0) {
			mods |= WLR_MODIFIER_ALT;
		}

		token = strtok_r(NULL, "+", &saveptr);
	}

	free(tmp);
	return mods;
}

xkb_keysym_t parse_keysym(const char *name) {
	if (!name || name[0] == '\0')
		return XKB_KEY_NoSymbol;

	if (strlen(name) == 1)
		return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);

	return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
}

uint32_t parse_keycode(const char *name) {
	if (!name || name[0] == '\0')
		return 0;

	static const char *mouse_buttons[] = {
		"mouse_left",
		"mouse_right",
		"mouse_middle",
		"mouse_back",
		"mouse_forward"
	};

	for (size_t i = 0; i < sizeof(mouse_buttons) / sizeof(mouse_buttons[0]); i++)
		if (strcmp(name, mouse_buttons[i]) == 0)
			return 0x20000000 + i + 272;

	if (name[0] >= '1' && name[0] <= '9' && name[1] == '\0')
		return name[0] - '0' + 1;

	if (strcmp(name, "0") == 0)
		return 11;

	return 0;
}

typedef struct {
	const char *cmd;
	const char *subcmd;
	const char *subsubcmd;
	bind_action_t action;
} action_entry_t;

#define ENTRY(c, s, ss, a) { c, s, ss, a }
#define ENTRY2(c, s, a)    { c, s, NULL, a }
#define ENTRY1(c, a)       { c, NULL, NULL, a }

static const action_entry_t action_table[] = {
	ENTRY2("focus", "west", BIND_FOCUS_WEST),
	ENTRY2("focus", "south", BIND_FOCUS_SOUTH),
	ENTRY2("focus", "north", BIND_FOCUS_NORTH),
	ENTRY2("focus", "east", BIND_FOCUS_EAST),
	ENTRY2("swap", "west", BIND_SWAP_WEST),
	ENTRY2("swap", "south", BIND_SWAP_SOUTH),
	ENTRY2("swap", "north", BIND_SWAP_NORTH),
	ENTRY2("swap", "east", BIND_SWAP_EAST),
	ENTRY2("presel", "west", BIND_PRESEL_WEST),
	ENTRY2("presel", "south", BIND_PRESEL_SOUTH),
	ENTRY2("presel", "north", BIND_PRESEL_NORTH),
	ENTRY2("presel", "east", BIND_PRESEL_EAST),
	ENTRY2("presel", "cancel", BIND_PRESEL_CANCEL),
	ENTRY2("resize", "left", BIND_RESIZE_LEFT),
	ENTRY2("resize", "right", BIND_RESIZE_RIGHT),
	ENTRY2("resize", "up", BIND_RESIZE_UP),
	ENTRY2("resize", "down", BIND_RESIZE_DOWN),
	ENTRY2("toggle", "floating", BIND_TOGGLE_FLOATING),
	ENTRY2("toggle", "fullscreen", BIND_TOGGLE_FULLSCREEN),
	ENTRY2("toggle", "pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED),
	ENTRY2("toggle", "monocle", BIND_TOGGLE_MONOCLE),
	ENTRY2("node", "-c", BIND_NODE_CLOSE),
	ENTRY2("node", "--close", BIND_NODE_CLOSE),
	ENTRY2("node", "-f", BIND_NODE_FOCUS),
	ENTRY2("node", "--focus", BIND_NODE_FOCUS),
	ENTRY2("node", "interactive_move", BIND_INTERACTIVE_MOVE),
	ENTRY2("node", "interactive_resize", BIND_INTERACTIVE_RESIZE),
	ENTRY2("node", "tiling_drag", BIND_TILING_DRAG),
	ENTRY("node", "-t", "tiled", BIND_NODE_STATE_TILED),
	ENTRY("node", "-t", "floating", BIND_NODE_STATE_FLOATING),
	ENTRY("node", "-t", "fullscreen", BIND_NODE_STATE_FULLSCREEN),
	ENTRY("node", "-t", "pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED),
	ENTRY("node", "--state", "tiled", BIND_NODE_STATE_TILED),
	ENTRY("node", "--state", "floating", BIND_NODE_STATE_FLOATING),
	ENTRY("node", "--state", "fullscreen", BIND_NODE_STATE_FULLSCREEN),
	ENTRY("node", "--state", "pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED),
	ENTRY("desktop", "-l", "tiled", BIND_DESKTOP_LAYOUT_TILED),
	ENTRY("desktop", "-l", "monocle", BIND_DESKTOP_LAYOUT_MONOCLE),
	ENTRY("desktop", "-l", "master_stack", BIND_DESKTOP_LAYOUT_MASTER_STACK),
	ENTRY("desktop", "--layout", "tiled", BIND_DESKTOP_LAYOUT_TILED),
	ENTRY("desktop", "--layout", "monocle", BIND_DESKTOP_LAYOUT_MONOCLE),
	ENTRY("desktop", "--layout", "master_stack", BIND_DESKTOP_LAYOUT_MASTER_STACK),
	ENTRY2("desktop", "last", BIND_DESKTOP_LAST),
	ENTRY1("quit", BIND_QUIT),
};


#undef ENTRY
#undef ENTRY2
#undef ENTRY1

static bind_action_t lookup_action(const char *args[3], int argc) {
	for (size_t i = 0; i < sizeof(action_table) / sizeof(action_table[0]); i++) {
		const action_entry_t *e = &action_table[i];
		if (strcmp(args[0], e->cmd) != 0)
			continue;
		if (e->subcmd == NULL)
			return e->action;
		if (argc < 2 || strcmp(args[1], e->subcmd) != 0)
			continue;
		if (e->subsubcmd == NULL)
			return e->action;
		if (argc >= 3 && strcmp(args[2], e->subsubcmd) == 0)
			return e->action;
	}
	return BIND_NONE;
}

bind_action_t parse_action(const char *cmd, int *desktop_index, char *submap_name) {
	*desktop_index = 0;
	if (submap_name)
		submap_name[0] = '\0';

	if (!cmd || cmd[0] == '\0')
		return BIND_NONE;

	if (cmd[0] == '@') {
		if (strcmp(cmd, "@exit") == 0)
			return BIND_EXIT_SUBMAP;
		if (submap_name)
			snprintf(submap_name, MAXLEN, "%s", cmd + 1);
		return BIND_ENTER_SUBMAP;
	}

	if (strncmp(cmd, "global-shortcut ", 16) == 0)
		return BIND_GLOBAL_SHORTCUT;

	if (strncmp(cmd, "doorsctl ", 9) != 0)
		return BIND_EXTERNAL;

	char buf[MAXLEN];
	snprintf(buf, sizeof(buf), "%s", cmd + 9);

	char *args[16];
	int argc = 0;
	char *saveptr;
	char *token = strtok_r(buf, " \t", &saveptr);
	while (token && argc < 16) {
		args[argc++] = token;
		token = strtok_r(NULL, " \t", &saveptr);
	}

	if (argc == 0)
		return BIND_NONE;

	// dispatch table lookup first
	bind_action_t action = lookup_action((const char **)args, argc);
	if (action != BIND_NONE)
		return action;

	// special cases that need extra logic
	if (strcmp(args[0], "node") == 0 && argc >= 2) {
		if (strcmp(args[1], "-d") == 0 || strcmp(args[1], "--to-desktop") == 0) {
			if (argc >= 3) {
				int d = atoi(args[2]);
				if (d >= 1 && d <= 10) {
					*desktop_index = d;
					return BIND_SEND_TO_DESKTOP_1;
				}
			}
			return BIND_NODE_TO_DESKTOP;
		}
	}

	if (strcmp(args[0], "desktop") == 0 && argc >= 2) {
		if (strcmp(args[1], "-f") == 0 || strcmp(args[1], "--focus") == 0)
			return (argc >= 3 && strcmp(args[2], "last") == 0) ? BIND_DESKTOP_LAST : BIND_DESKTOP_FOCUS;

		int d = atoi(args[1]);
		if (d >= 1 && d <= 10) {
			*desktop_index = d;
			return BIND_DESKTOP_1;
		}
	}

	return BIND_EXTERNAL;
}

// expansions
#define MAX_EXPANSIONS 64
typedef struct {
	char strings[MAX_EXPANSIONS][MAXLEN];
	size_t count;
} expansion_result_t;

static void expand_braces_recursive(const char *input, char *prefix, size_t prefix_len,
	expansion_result_t *result);

static void expand_braces(const char *input, expansion_result_t *result) {
	result->count = 0;
	char prefix[MAXLEN] = {0};
	expand_braces_recursive(input, prefix, 0, result);
}

static void expand_braces_recursive(const char *input, char *prefix, size_t prefix_len,
		expansion_result_t *result) {
	const char *brace_start = strchr(input, '{');
	if (!brace_start) {
		if (result->count < MAX_EXPANSIONS) {
			snprintf(result->strings[result->count], MAXLEN, "%s%s", prefix, input);
			result->count++;
		}
		return;
	}

	// copy prefix
	size_t pre_brace_len = brace_start - input;
	char new_prefix[MAXLEN];
	memcpy(new_prefix, prefix, prefix_len);
	memcpy(new_prefix + prefix_len, input, pre_brace_len);
	new_prefix[prefix_len + pre_brace_len] = '\0';

	// find closing brace
	const char *brace_end = brace_start + 1;
	int depth = 1;
	while (*brace_end && depth > 0) {
		if (*brace_end == '{')
			depth++;
		else if (*brace_end == '}')
			depth--;
		brace_end++;
	}

	if (depth != 0) {
		if (result->count < MAX_EXPANSIONS) {
			snprintf(new_prefix + strlen(new_prefix), MAXLEN - strlen(new_prefix), "%s", brace_start);
			snprintf(result->strings[result->count], MAXLEN, "%s", new_prefix);
			result->count++;
		}
		return;
	}

	// get content
	size_t content_len = (brace_end - 1) - (brace_start + 1);
	char content[MAXLEN];
	memcpy(content, brace_start + 1, content_len);
	content[content_len] = '\0';

	const char *rest = brace_end;

	int choice_depth = 0;
	size_t choice_start = 0;

	bool is_range = false;
	size_t range_len = content_len;
	if (range_len >= 3) {
		char first = content[0];
		char last = content[range_len - 1];
		bool is_dash = false;
		for (size_t di = 1; di < range_len - 1; di++) {
			if (content[di] == '-') {
				is_dash = true;
				break;
			}
		}
		if (is_dash && ((first >= '0' && first <= '9' && last >= '0' && last <= '9') || (first >= 'a' &&
			first <= 'z' && last >= 'a' && last <= 'z') || (first >= 'A' && first <= 'Z' && last >= 'A' &&
			last <= 'Z')))
			is_range = true;
	}

	if (is_range) {
		char first = content[0];
		char last = content[content_len - 1];
		for (char c = first; c <= last; c++) {
			char choice[2] = {
				c,
				'\0'
			};
			char full_input[MAXLEN * 2];
			snprintf(full_input, sizeof(full_input), "%s%s", choice, rest);
			expand_braces_recursive(full_input, new_prefix, strlen(new_prefix), result);
		}
	} else {
		// comma-separated
		for (size_t i = 0; i <= content_len; i++) {
			if (content[i] == '{') {
				choice_depth++;
			} else if (content[i] == '}') {
				choice_depth--;
			} else if (content[i] == ',' && choice_depth == 0) {
				char choice[MAXLEN];
				size_t choice_len = i - choice_start;
				if (choice_len < MAXLEN) {
					memcpy(choice, content + choice_start, choice_len);
					choice[choice_len] = '\0';
				} else {
					memcpy(choice, content + choice_start, MAXLEN - 1);
					choice[MAXLEN - 1] = '\0';
				}

				char full_input[MAXLEN * 2];
				snprintf(full_input, sizeof(full_input), "%s%s", choice, rest);

				expand_braces_recursive(full_input, new_prefix, strlen(new_prefix), result);

				choice_start = i + 1;
			}
		}

		// last choice
		if (choice_start <= content_len) {
			char choice[MAXLEN];
			size_t choice_len = content_len - choice_start;
			if (choice_len < MAXLEN) {
				memcpy(choice, content + choice_start, choice_len);
				choice[choice_len] = '\0';
			} else {
				memcpy(choice, content + choice_start, MAXLEN - 1);
				choice[MAXLEN - 1] = '\0';
			}

			char full_input[MAXLEN * 2];
			snprintf(full_input, sizeof(full_input), "%s%s", choice, rest);

			expand_braces_recursive(full_input, new_prefix, strlen(new_prefix), result);
		}
	}
}

static char *expand_sequence(const char *input, char *output, size_t out_size) {
	expansion_result_t result;
	expand_braces(input, &result);
	if (result.count > 0) {
		snprintf(output, out_size, "%s", result.strings[0]);
	} else {
		snprintf(output, out_size, "%s", input);
	}
	return output;
}

void add_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode, bool use_keycode,
		bind_action_t action, int desktop_index, const char *external_cmd, const char *submap_name) {
	size_t *num_ptr;
	keybind_t *kb_array;

	if (current_parsing_submap) {
		if (current_parsing_submap->num_keybinds >= MAX_KEYBINDS) {
			wlr_log(WLR_ERROR, "Maximum number of keybinds reached for submap %s",
				current_parsing_submap->name);
			return;
		}
		num_ptr = &current_parsing_submap->num_keybinds;
		kb_array = current_parsing_submap->keybinds;
	} else {
		if (num_keybinds >= MAX_KEYBINDS) {
			wlr_log(WLR_ERROR, "Maximum number of keybinds reached");
			return;
		}
		num_ptr = &num_keybinds;
		kb_array = keybinds;
	}

	keybind_t *kb = &kb_array[(*num_ptr)++];
	kb->modifiers = modifiers;
	kb->keysym = keysym;
	kb->keycode = keycode;
	kb->use_keycode = use_keycode;
	kb->action = action;
	kb->desktop_index = desktop_index;
	if (submap_name)
		snprintf(kb->submap_name, sizeof(kb->submap_name), "%s", submap_name);
	else
		kb->submap_name[0] = '\0';
	if (external_cmd)
		snprintf(kb->external_cmd, sizeof(kb->external_cmd), "%s", external_cmd);
	else
		kb->external_cmd[0] = '\0';

	wlr_log(WLR_DEBUG, "Added keybind: mod=%u keysym=%u keycode=%u action=%d index=%d submap=%s",
		modifiers, keysym, keycode, action, desktop_index, submap_name ? submap_name : "global");
}

static void add_gesturebind(enum gesture_type type, uint8_t fingers, uint32_t directions,
		const char *input, bind_action_t action, int desktop_index, const char *external_cmd) {
	if (num_gesturebinds >= MAX_GESTUREBINDS) {
		wlr_log(WLR_ERROR, "Maximum number of gesture binds reached");
		return;
	}

	gesturebind_t *gb = &gesture_bindings[num_gesturebinds++];
	gb->type = type;
	gb->fingers = fingers;
	gb->directions = directions;
	gb->input = input ? strdup(input) : NULL;
	gb->action = action;
	gb->desktop_index = desktop_index;
	if (external_cmd) {
		snprintf(gb->external_cmd, sizeof(gb->external_cmd), "%s", external_cmd);
	} else {
		gb->external_cmd[0] = '\0';
	}

	wlr_log(WLR_DEBUG, "Added gesturebind: type=%d fingers=%d dirs=%u action=%d", type, fingers,
		directions, action);
}

static void parse_gesture_hotkey_line(const char *gesture_str, const char *command_str) {
	char gesture_buf[MAXLEN];
	char command_buf[MAXLEN];

	snprintf(gesture_buf, sizeof(gesture_buf), "%s", gesture_str);
	snprintf(command_buf, sizeof(command_buf), "%s", command_str);

	char expanded_cmd[MAXLEN];
	expand_sequence(command_buf, expanded_cmd, sizeof(expanded_cmd));

	gesture_t gest;
	char *err = gesture_parse(gesture_buf, &gest);
	if (err) {
		wlr_log(WLR_ERROR, "Failed to parse gesture '%s': %s", gesture_buf, err);
		return;
	}

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(expanded_cmd, &desktop_index, submap_name);

	if (action != BIND_EXTERNAL) {
		add_gesturebind(gest.type, gest.fingers, gest.directions, NULL, action, desktop_index, NULL);
	} else {
		add_gesturebind(gest.type, gest.fingers, gest.directions, NULL, action, desktop_index,
			expanded_cmd);
	}
}

static void add_hotcornerbind(int corner_x, int corner_y, bind_action_t action, int desktop_index,
		const char *external_cmd) {
	if (num_hotcornerbinds >= MAX_HOTCORNERBINDS) {
		wlr_log(WLR_ERROR, "Maximum number of hotcorner binds reached");
		return;
	}

	hotcornerbind_t *hc = &hotcorner_bindings[num_hotcornerbinds++];
	hc->corner_x = corner_x;
	hc->corner_y = corner_y;
	if (corner_x == -1 && corner_y == -1)
		hc->corner = HOTCORNER_TOPLEFT;
	else if (corner_x == 1 && corner_y == -1)
		hc->corner = HOTCORNER_TOPRIGHT;
	else if (corner_x == -1 && corner_y == 1)
		hc->corner = HOTCORNER_BOTTOMLEFT;
	else if (corner_x == 1 && corner_y == 1)
		hc->corner = HOTCORNER_BOTTOMRIGHT;
	else
		hc->corner = HOTCORNER_NONE;
	hc->action = action;
	hc->desktop_index = desktop_index;
	if (external_cmd)
		snprintf(hc->external_cmd, sizeof(hc->external_cmd), "%s", external_cmd);
	else
		hc->external_cmd[0] = '\0';

	wlr_log(WLR_DEBUG, "Added hotcornerbind: corner=%d (%d,%d) action=%d", hc->corner, corner_x,
		corner_y, action);
}

static void parse_hotcorner_hotkey_line(const char *hotcorner_str, const char *command_str) {
	int corner_x = 0, corner_y = 0;

	// parse corner
	if (strcasecmp(hotcorner_str, "topleft") == 0) {
		corner_x = -1;
		corner_y = -1;
	} else if (strcasecmp(hotcorner_str, "topright") == 0) {
		corner_x = 1;
		corner_y = -1;
	} else if (strcasecmp(hotcorner_str, "bottomleft") == 0) {
		corner_x = -1;
		corner_y = 1;
	} else if (strcasecmp(hotcorner_str, "bottomright") == 0) {
		corner_x = 1;
		corner_y = 1;
	} else {
		wlr_log(WLR_ERROR, "Unknown hotcorner '%s'", hotcorner_str);
		return;
	}

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(command_str, &desktop_index, submap_name);

	if (action != BIND_EXTERNAL)
		add_hotcornerbind(corner_x, corner_y, action, desktop_index, NULL);
	else
		add_hotcornerbind(corner_x, corner_y, action, desktop_index, command_str);
}

static void parse_hotkey_line(const char *hotkey_str, const char *command_str) {
	if (strncmp(hotkey_str, "gesture ", 8) == 0) {
		parse_gesture_hotkey_line(hotkey_str + 8, command_str);
		return;
	}

	if (strncmp(hotkey_str, "hotcorner ", 10) == 0) {
		parse_hotcorner_hotkey_line(hotkey_str + 10, command_str);
		return;
	}

	if (strcmp(hotkey_str, "bell") == 0) {
		int desktop_index = 0;
		char submap_name[MAXLEN];
		submap_name[0] = '\0';
		bind_action_t action = parse_action(command_str, &desktop_index, submap_name);
		if (action != BIND_EXTERNAL) {
			bell_bind = (keybind_t){
				.action = action,
				.desktop_index = desktop_index,
			};
		} else {
			snprintf(bell_bind.external_cmd, sizeof(bell_bind.external_cmd), "%s", command_str);
			bell_bind.action = action;
		}
		wlr_log(WLR_DEBUG, "Parsed bell bind: action=%d cmd='%s'", action, command_str);
		return;
	}

	char hotkey_buf[MAXLEN];
	char command_buf[MAXLEN];

	snprintf(hotkey_buf, sizeof(hotkey_buf), "%s", hotkey_str);
	snprintf(command_buf, sizeof(command_buf), "%s", command_str);

	// expand braces
	expansion_result_t hk_expansions;
	expansion_result_t cmd_expansions;
	expand_braces(hotkey_buf, &hk_expansions);
	expand_braces(command_buf, &cmd_expansions);

	wlr_log(WLR_DEBUG, "parse_hotkey_line: hotkey=[%s] cmd=[%s] hk_expans=%zu cmd_expans=%zu",
		hotkey_buf, command_buf, hk_expansions.count, cmd_expansions.count);

	size_t num_pairs = 0;

	if (hk_expansions.count == 0 && cmd_expansions.count == 0) {
		num_pairs = 1;
	} else if (hk_expansions.count > 0 && cmd_expansions.count > 0) {
		num_pairs = (hk_expansions.count > cmd_expansions.count) ? hk_expansions.count :
			cmd_expansions.count;
	} else if (hk_expansions.count > 0) {
		num_pairs = hk_expansions.count;
	} else {
		num_pairs = cmd_expansions.count;
	}

	for (size_t i = 0; i < num_pairs; i++) {
		const char *single_hotkey = (hk_expansions.count > 0) ? hk_expansions.strings[i %
			hk_expansions.count] : hotkey_buf;
		const char *single_cmd = (cmd_expansions.count > 0) ? cmd_expansions.strings[i %
			cmd_expansions.count] : command_buf;

		wlr_log(WLR_DEBUG, "  pair %zu: hotkey=[%s] cmd=[%s]", i, single_hotkey, single_cmd);

		uint32_t modifiers = 0;
		xkb_keysym_t keysym = XKB_KEY_NoSymbol;
		uint32_t keycode = 0;
		bool use_keycode = false;

		char hotkey_copy[MAXLEN];
		snprintf(hotkey_copy, sizeof(hotkey_copy), "%s", single_hotkey);

		char *plus = strrchr(hotkey_copy, '+');
		if (plus) {
			*plus = '\0';
			modifiers = parse_modifiers(hotkey_copy);
			char *key_part = plus + 1;
			while (*key_part == ' ')
				key_part++;
			keysym = parse_keysym(key_part);
			keycode = parse_keycode(key_part);
			if (keycode > 0)
				use_keycode = true;
		} else {
			keysym = parse_keysym(single_hotkey);
			keycode = parse_keycode(single_hotkey);
			if (keycode > 0)
				use_keycode = true;
		}

		if (keysym == XKB_KEY_NoSymbol && keycode == 0) {
			wlr_log(WLR_ERROR, "Unknown keysym: %s", single_hotkey);
		} else {
			int desktop_index = 0;
			char submap_name[MAXLEN];
			submap_name[0] = '\0';
			bind_action_t action = parse_action(single_cmd, &desktop_index, submap_name);
			wlr_log(WLR_DEBUG, "Parsed action: %d for cmd: '%s' submap: '%s'", action, single_cmd,
				submap_name);
			if (action != BIND_EXTERNAL && action != BIND_GLOBAL_SHORTCUT)
				add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index, NULL,
					submap_name[0] ? submap_name : NULL);
			else
				add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index, single_cmd,
					submap_name[0] ? submap_name : NULL);
		}
	}
}

static char config_path[CONFIG_PATH_MAX];
static char hotkey_init_path[CONFIG_PATH_MAX];
static bool config_ran = false;

void run_config(const char *config_path_arg) {
	if (!config_path_arg || access(config_path_arg, R_OK) != 0)
		return;

	snprintf(config_path, sizeof(config_path), "%s", config_path_arg);
}

void run_config_idle(void *data) {
	(void)data;
	if (config_ran || config_path[0] == '\0')
		return;

	config_ran = true;

	wlr_log(WLR_INFO, "Running config: %s", config_path);
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", config_path, NULL);
		_exit(1);
	}
}

void load_hotkeys_idle(void *data) {
	(void)data;
	if (hotkey_init_path[0] == '\0')
		return;

	load_hotkeys(hotkey_init_path);
	setup_inotify_watch(hotkey_init_path);
}

static submap_t *find_or_create_submap(const char *name) {
	for (size_t i = 0; i < num_submaps; i++)
		if (strcmp(submaps[i].name, name) == 0)
			return &submaps[i];

	if (num_submaps >= MAX_SUBMAPS) {
		wlr_log(WLR_ERROR, "Maximum number of submaps reached");
		return NULL;
	}

	submap_t *sm = &submaps[num_submaps++];
	snprintf(sm->name, sizeof(sm->name), "%s", name);
	sm->num_keybinds = 0;
	sm->parent = NULL;
	return sm;
}

void load_hotkeys(const char *config_path) {
	wlr_log(WLR_DEBUG, "load_hotkeys called with path: %s", config_path);

	FILE *f = fopen(config_path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "No hotkey config found at %s", config_path);
		return;
	}

	num_keybinds = 0;
	num_submaps = 0;
	num_hotcornerbinds = 0;
	num_gesturebinds = 0;
	active_submap = NULL;
	current_parsing_submap = NULL;
	bell_bind = (keybind_t){0};

	char line[MAXLEN * 2];
	char hotkey[MAXLEN * 2];
	char command[MAXLEN * 2];
	int offset = 0;
	hotkey[0] = '\0';
	command[0] = '\0';
	char pending_hotkey[MAXLEN * 2];
	char pending_command[MAXLEN * 2];
	pending_hotkey[0] = '\0';
	pending_command[0] = '\0';
	size_t pending_hotkey_indent = 0;

	while (fgets(line, sizeof(line), f)) {
		wlr_log(WLR_DEBUG, "Config line: [%s]", line);
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';

		size_t indent = 0;
		while (line[indent] == ' ' || line[indent] == '\t')
			indent++;

		char *ptr = line + indent;
		char *content_start = ptr;
		while (*content_start == ' ' || *content_start == '\t')
			content_start++;
		char first_char = *content_start;

		if (line[0] == '#' || line[0] == '\0')
			continue;

		if (first_char == '@') {
			size_t flush_indent = pending_hotkey_indent;
			if (pending_hotkey[0] != '\0') {
				snprintf(hotkey, sizeof(hotkey), "%s", pending_hotkey);
				snprintf(command, sizeof(command), "%s", pending_command);
				parse_hotkey_line(hotkey, command);
				pending_hotkey[0] = '\0';
				pending_command[0] = '\0';
			}
			if (current_parsing_submap == NULL) {
				pending_hotkey_indent = 0;
				char *submap_name = content_start + 1;
				current_parsing_submap = find_or_create_submap(submap_name);
			} else {
				snprintf(pending_command, sizeof(pending_command), "%s", content_start);
				pending_hotkey_indent = flush_indent;
				offset = 0;
			}
			continue;
		}

		if (indent == 0 && strstr(ptr, "->")) {
			char *arrow = strstr(ptr, "->");
			if (arrow) {
				if (pending_hotkey[0] != '\0') {
					snprintf(hotkey, sizeof(hotkey), "%s", pending_hotkey);
					snprintf(command, sizeof(command), "%s", pending_command);
					parse_hotkey_line(hotkey, command);
				}
				size_t key_part_len = arrow - ptr;
				while (key_part_len > 0 && ptr[key_part_len - 1] == ' ')
					key_part_len--;
				if (key_part_len < sizeof(pending_hotkey)) {
					strncpy(pending_hotkey, ptr, key_part_len);
					pending_hotkey[key_part_len] = '\0';
				}
				char *cmd_part = arrow + 2;
				while (*cmd_part == ' ')
					cmd_part++;
				snprintf(pending_command, sizeof(pending_command), "%s", cmd_part);
				continue;
			}
		}

		if (isgraph((unsigned char)first_char) || (first_char != '\0' &&
				!isspace((unsigned char)first_char))) {
			if (pending_hotkey[0] != '\0') {
				if (indent == 0 || indent <= pending_hotkey_indent) {
					snprintf(hotkey, sizeof(hotkey), "%s", pending_hotkey);
					snprintf(command, sizeof(command), "%s", pending_command);
					parse_hotkey_line(hotkey, command);
					pending_hotkey[0] = '\0';
					pending_command[0] = '\0';
					pending_hotkey_indent = 0;
				} else {
					snprintf(pending_command, sizeof(pending_command), "%s", content_start);
					offset = 0;
					continue;
				}
			}
			snprintf(pending_hotkey, sizeof(pending_hotkey), "%s", content_start);
			pending_command[0] = '\0';
			pending_hotkey_indent = indent;
			offset = 0;
		} else if (pending_hotkey[0] != '\0') {
			if (pending_command[0] != '\0' && offset > 0 && pending_command[offset - 1] != ' ') {
				pending_command[offset++] = ' ';
			}
			bool last_was_space = false;
			for (size_t i = 0; ptr[i] && offset < (int)sizeof(pending_command) - 1; i++) {
				if (isspace((unsigned char)ptr[i])) {
					if (!last_was_space && offset > 0 && pending_command[offset - 1] != ' ') {
						pending_command[offset++] = ' ';
						last_was_space = true;
					}
				} else {
					pending_command[offset++] = ptr[i];
					last_was_space = false;
				}
			}
			pending_command[offset] = '\0';
		}
	}

	if (pending_hotkey[0] != '\0') {
		snprintf(hotkey, sizeof(hotkey), "%s", pending_hotkey);
		snprintf(command, sizeof(command), "%s", pending_command);
		parse_hotkey_line(hotkey, command);
	}

	current_parsing_submap = NULL;

	fclose(f);
	wlr_log(WLR_INFO,
		"Loaded %zu keybinds, %zu gesturebinds, %zu hotcornerbinds and %zu submaps from %s", num_keybinds,
		num_gesturebinds, num_hotcornerbinds, num_submaps, config_path);
}

static void setup_inotify_watch(const char *config_path) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}

	if (hotkey_watch_fd >= 0) {
		close(hotkey_watch_fd);
		hotkey_watch_fd = -1;
	}

	hotkey_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (hotkey_watch_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to initialize inotify");
		return;
	}

	strcpy(hotkey_config_path, config_path);

	char dir_path[PATH_MAX];
	strcpy(dir_path, config_path);
	char *last_slash = strrchr(dir_path, '/');
	uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
	if (last_slash) {
		*last_slash = '\0';
		inotify_add_watch(hotkey_watch_fd, dir_path, mask);
	} else {
		inotify_add_watch(hotkey_watch_fd, ".", mask);
	}

	add_hotkey_listener_to_event_loop();
}

void config_init(void) {
	config_init_with_config_dir(NULL);
}

void config_init_with_config_dir(const char *config_dir) {
	ONCE();
	custom_config_dir = config_dir;

	const char *config_home = get_config_home();
	char doorsrc_path[CONFIG_PATH_MAX];
	const char *config_source = config_home;

	if (!custom_config_dir) {
		char test_path[CONFIG_PATH_MAX];
		snprintf(test_path, sizeof(test_path), "%s/%s", config_home, DOORSRC_NAME);
		if (access(test_path, R_OK) != 0) {
			snprintf(test_path, sizeof(test_path), "%s/%s", config_home, DOORSHKRC_NAME);
			if (access(test_path, R_OK) != 0)
				config_source = GLOBAL_CONFIG_DIR;
		}
	}

	snprintf(doorsrc_path, sizeof(doorsrc_path), "%s/%s", config_source, DOORSRC_NAME);
	run_config(doorsrc_path);

	snprintf(hotkey_init_path, sizeof(hotkey_init_path), "%s/%s", config_source, DOORSHKRC_NAME);

	refresh_border_color_cache();
}

void config_fini(void) {
	ONCE();
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}
	if (hotkey_watch_fd >= 0) {
		close(hotkey_watch_fd);
		hotkey_watch_fd = -1;
	}
	num_keybinds = 0;
	num_gesturebinds = 0;
}

void reload_hotkeys(void) {
	if (hotkey_config_path[0] != '\0') {
		wlr_log(WLR_INFO, "Reloading hotkeys from %s", hotkey_config_path);
		load_hotkeys(hotkey_config_path);
	}
}

static struct wl_event_source *hotkey_debounce_timer = NULL;

static int hotkey_debounce_handler(void *data) {
	(void)data;
	reload_hotkeys();
	hotkey_debounce_timer = NULL;
	return 0;
}

static int hotkey_reload_handler(int fd, uint32_t mask, void *data) {
	(void)data;
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	ssize_t len = read(fd, buf, sizeof(buf));
	if (len <= 0)
		return 0;

	struct inotify_event *event = (struct inotify_event *)buf;

	bool match = false;
	if (event->len > 0 && event->name[0] != '\0') {
		const char *config_file_name = strrchr(hotkey_config_path, '/');
		config_file_name = config_file_name ? config_file_name + 1 : hotkey_config_path;
		match = (strcmp(event->name, config_file_name) == 0);
	} else {
		match = true;
	}

	if (!match)
		return 0;

	if (hotkey_debounce_timer) {
		wl_event_source_timer_update(hotkey_debounce_timer, 100);
	} else if (hotkey_event_loop) {
		hotkey_debounce_timer = wl_event_loop_add_timer(hotkey_event_loop, hotkey_debounce_handler, NULL);
		if (hotkey_debounce_timer)
			wl_event_source_timer_update(hotkey_debounce_timer, 100);
	}
	return 0;
}

bool keybind_matches(const keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym,
		uint32_t keycode) {
	if (!kb)
		return false;

	if (kb->use_keycode)
		return (kb->modifiers == modifiers) && (kb->keycode == keycode);
	else {
		if (kb->modifiers == modifiers && kb->keysym == keysym)
			return true;
		if (kb->modifiers == modifiers) {
			xkb_keysym_t lower = xkb_keysym_to_lower(keysym);
			if (kb->keysym == lower)
				return true;
		}
		return false;
	}
}

void execute_bind(bind_t b) {
	switch (b.action) {
	case BIND_QUIT:
		wl_display_terminate(server.wl_display);
		break;
	case BIND_NODE_FOCUS:
		break;
	case BIND_NODE_CLOSE:
		close_focused();
		break;
	case BIND_NODE_STATE_TILED:
		tile_focused();
		break;
	case BIND_NODE_STATE_FLOATING:
		toggle_floating();
		break;
	case BIND_NODE_STATE_FULLSCREEN:
		toggle_fullscreen();
		break;
	case BIND_NODE_TO_DESKTOP:
		if (b.desktop_index > 0) {
			send_to_desktop(b.desktop_index - 1);
		}
		break;
	case BIND_DESKTOP_FOCUS:
		if (b.desktop_index > 0) {
			workspace_switch_to_desktop_by_index(b.desktop_index - 1);
		}
		break;
	case BIND_DESKTOP_LAYOUT_TILED:
		set_tiled_layout();
		break;
	case BIND_DESKTOP_LAYOUT_MONOCLE:
		toggle_monocle();
		break;
	case BIND_FOCUS_WEST:
		focus_west();
		break;
	case BIND_FOCUS_SOUTH:
		focus_south();
		break;
	case BIND_FOCUS_NORTH:
		focus_north();
		break;
	case BIND_FOCUS_EAST:
		focus_east();
		break;
	case BIND_SWAP_WEST:
		swap_west();
		break;
	case BIND_SWAP_SOUTH:
		swap_south();
		break;
	case BIND_SWAP_NORTH:
		swap_north();
		break;
	case BIND_SWAP_EAST:
		swap_east();
		break;
	case BIND_PRESEL_WEST:
		presel_west();
		break;
	case BIND_PRESEL_SOUTH:
		presel_south();
		break;
	case BIND_PRESEL_NORTH:
		presel_north();
		break;
	case BIND_PRESEL_EAST:
		presel_east();
		break;
	case BIND_PRESEL_CANCEL:
		cancel_presel();
		break;
	case BIND_TOGGLE_FLOATING:
		toggle_floating();
		break;
	case BIND_TOGGLE_FULLSCREEN:
		toggle_fullscreen();
		break;
	case BIND_TOGGLE_PSEUDO_TILED:
		toggle_pseudo_tiled();
		break;
	case BIND_TOGGLE_MONOCLE:
		toggle_monocle();
		break;
	case BIND_TOGGLE_MASTER_STACK:
		toggle_master_stack();
		break;
	case BIND_DESKTOP_LAYOUT_MASTER_STACK:
		toggle_master_stack();
		break;
	case BIND_MASTER_STACK_INC:
		if (mon && mon->desk) {
			master_stack_increment(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_MASTER_STACK_DEC:
		if (mon && mon->desk) {
			master_stack_decrement(mon->desk);
			if (mon->desk->layout == LAYOUT_MASTER_STACK)
				arrange(mon, mon->desk, true);
		}
		break;
	case BIND_MASTER_STACK_FLIP:
		master_stack_flip_orientation();
		if (mon && mon->desk && mon->desk->layout == LAYOUT_MASTER_STACK)
			arrange(mon, mon->desk, true);
		break;
	case BIND_MASTER_STACK_CYCLE_ORIENTATION:
		master_stack_cycle_orientation();
		if (mon && mon->desk && mon->desk->layout == LAYOUT_MASTER_STACK)
			arrange(mon, mon->desk, true);
		break;
	case BIND_MASTER_STACK_CYCLE_STACK_LAYOUT:
		master_stack_cycle_stack_layout();
		if (mon && mon->desk && mon->desk->layout == LAYOUT_MASTER_STACK)
			arrange(mon, mon->desk, true);
		break;
	case BIND_ROTATE_CW:
		rotate_clockwise();
		break;
	case BIND_ROTATE_CCW:
		rotate_counterclockwise();
		break;
	case BIND_FLIP_HORIZONTAL:
		flip_horizontal();
		break;
	case BIND_FLIP_VERTICAL:
		flip_vertical();
		break;
	case BIND_RESIZE_LEFT:
		resize_left();
		break;
	case BIND_RESIZE_RIGHT:
		resize_right();
		break;
	case BIND_RESIZE_UP:
		resize_up();
		break;
	case BIND_RESIZE_DOWN:
		resize_down();
		break;
	case BIND_DESKTOP_NEXT:
		focus_next_desktop();
		break;
	case BIND_DESKTOP_PREV:
		focus_prev_desktop();
		break;
	case BIND_DESKTOP_LAST:
		focus_last_desktop();
		break;
	case BIND_SEND_TO_DESKTOP_NEXT:
		send_to_next_desktop();
		break;
	case BIND_SEND_TO_DESKTOP_PREV:
		send_to_prev_desktop();
		break;
	case BIND_DESKTOP_1:
	case BIND_DESKTOP_2:
	case BIND_DESKTOP_3:
	case BIND_DESKTOP_4:
	case BIND_DESKTOP_5:
	case BIND_DESKTOP_6:
	case BIND_DESKTOP_7:
	case BIND_DESKTOP_8:
	case BIND_DESKTOP_9:
	case BIND_DESKTOP_10:
		workspace_switch_to_desktop_by_index(b.desktop_index - 1);
		break;
	case BIND_SEND_TO_DESKTOP_1:
	case BIND_SEND_TO_DESKTOP_2:
	case BIND_SEND_TO_DESKTOP_3:
	case BIND_SEND_TO_DESKTOP_4:
	case BIND_SEND_TO_DESKTOP_5:
	case BIND_SEND_TO_DESKTOP_6:
	case BIND_SEND_TO_DESKTOP_7:
	case BIND_SEND_TO_DESKTOP_8:
	case BIND_SEND_TO_DESKTOP_9:
	case BIND_SEND_TO_DESKTOP_10:
		send_to_desktop(b.desktop_index - 1);
		break;
	case BIND_EXTERNAL:
	case BIND_NONE:
		if (b.external_cmd[0] != '\0') {
			if (fork() == 0) {
				execl("/bin/sh", "/bin/sh", "-c", b.external_cmd, NULL);
				_exit(1);
			}
		}
		break;
	case BIND_ENTER_SUBMAP:
		enter_submap(b.submap_name);
		break;
	case BIND_EXIT_SUBMAP:
		exit_submap();
		break;
	case BIND_INTERACTIVE_MOVE:
	case BIND_INTERACTIVE_RESIZE:
	case BIND_TILING_DRAG:
		break;
	case BIND_GLOBAL_SHORTCUT:
		if (b.external_cmd[0] != '\0') {
			char app_id[256], shortcut_id[256];
			const char *args = b.external_cmd + 16;
			const char *space = strchr(args, ' ');
			if (space) {
				size_t app_len = (size_t)(space - args);
				if (app_len >= sizeof(app_id))
					app_len = sizeof(app_id) - 1;
				memcpy(app_id, args, app_len);
				app_id[app_len] = '\0';
				snprintf(shortcut_id, sizeof(shortcut_id), "%s", space + 1);
				global_shortcuts_send_by_app(app_id, shortcut_id, true);
			}
		}
		break;
	}
}

void execute_keybind(const keybind_t *kb) {
	if (!kb)
		return;
	bind_t bind = {
		.action = kb->action,
		.desktop_index = kb->desktop_index,
		.submap_name = kb->submap_name,
		.external_cmd = kb->external_cmd,
	};
	execute_bind(bind);
}

void execute_bell_bind(void) {
	if (bell_bind.action != BIND_NONE || bell_bind.external_cmd[0] != '\0')
		execute_keybind(&bell_bind);
}

int get_hotkey_watch_fd(void) {
	return hotkey_watch_fd;
}

void setup_hotkey_event_listener(struct wl_event_loop *event_loop) {
	hotkey_event_loop = event_loop;
	add_hotkey_listener_to_event_loop();
}

static void add_hotkey_listener_to_event_loop(void) {
	if (hotkey_event_source) {
		wl_event_source_remove(hotkey_event_source);
		hotkey_event_source = NULL;
	}

	if (hotkey_event_loop && hotkey_watch_fd >= 0)
		hotkey_event_source = wl_event_loop_add_fd(hotkey_event_loop, hotkey_watch_fd, WL_EVENT_READABLE,
			hotkey_reload_handler, NULL);
}

static submap_t *find_submap(const char *name) {
	for (size_t i = 0; i < num_submaps; i++)
		if (strcmp(submaps[i].name, name) == 0)
			return &submaps[i];
	return NULL;
}

void enter_submap(const char *name) {
	submap_t *sm = find_submap(name);
	if (sm) {
		active_submap = sm;
		wlr_log(WLR_INFO, "Entered submap: %s", name);
	} else {
		wlr_log(WLR_ERROR, "Submap not found: %s", name);
	}
}

void exit_submap(void) {
	if (active_submap) {
		wlr_log(WLR_INFO, "Exited submap: %s", active_submap->name);
		active_submap = NULL;
	}
}

keyboard_grouping_t get_keyboard_grouping(void) {
	return keyboard_grouping;
}

void set_keyboard_grouping(keyboard_grouping_t grouping) {
	if (keyboard_grouping != grouping) {
		keyboard_grouping = grouping;
		wlr_log(WLR_INFO, "Keyboard grouping set to %d", (int)grouping);
		extern void keyboard_reapply_grouping(void);
		keyboard_reapply_grouping();
	}
}

bool gesturebind_matches(const gesturebind_t *gb, enum gesture_type type, uint8_t fingers) {
	if (!gb)
		return false;
	if (gb->type != type)
		return false;
	if (gb->fingers != GESTURE_FINGERS_ANY && gb->fingers != fingers)
		return false;

	return true;
}

void execute_gesturebind(const gesturebind_t *gb) {
	if (!gb)
		return;
	if (gb->action == BIND_ENTER_SUBMAP)
		return; // submap entry from gestures not supported
	bind_t bind = {
		.action = gb->action,
		.desktop_index = gb->desktop_index,
		.external_cmd = gb->external_cmd,
		.submap_name = NULL
	};
	execute_bind(bind);
}

void reload_gesturebinds(void) {
	num_gesturebinds = 0;
}

bool hotcornerbind_matches(const hotcornerbind_t *hc, int corner_x, int corner_y) {
	if (!hc)
		return false;
	return hc->corner_x == corner_x && hc->corner_y == corner_y;
}

hotcornerbind_t *hotcorner_bind_match(int corner_x, int corner_y) {
	for (size_t i = 0; i < num_hotcornerbinds; i++)
		if (hotcornerbind_matches(&hotcorner_bindings[i], corner_x, corner_y))
			return &hotcorner_bindings[i];

	return NULL;
}

void execute_hotcornerbind(const hotcornerbind_t *hc) {
	if (!hc)
		return;
	if (hc->action == BIND_ENTER_SUBMAP)
		return; // submap entry from hotcorners not supported
	bind_t bind = {
		.action = hc->action,
		.desktop_index = hc->desktop_index,
		.external_cmd = hc->external_cmd,
		.submap_name = NULL
	};
	execute_bind(bind);
}

void reload_hotcornerbinds(void) {
	num_hotcornerbinds = 0;
}

const char *bind_action_name(bind_action_t action) {
	static const char *names[] = {
		"none",
		"quit",
		"enter_submap",
		"exit_submap",
		"node_focus",
		"node_close",
		"node_state_tiled",
		"node_state_floating",
		"node_state_fullscreen",
		"node_to_desktop",
		"desktop_focus",
		"desktop_layout_tiled",
		"desktop_layout_monocle",
		"focus_west",
		"focus_south",
		"focus_north",
		"focus_east",
		"swap_west",
		"swap_south",
		"swap_north",
		"swap_east",
		"presel_west",
		"presel_south",
		"presel_north",
		"presel_east",
		"presel_cancel",
		"toggle_floating",
		"toggle_fullscreen",
		"toggle_pseudo_tiled",
		"toggle_monocle",
		"toggle_master_stack",
		"desktop_layout_master_stack",
		"master_stack_inc",
		"master_stack_dec",
		"master_stack_flip",
		"master_stack_cycle_orientation",
		"master_stack_cycle_stack_layout",
		"rotate_cw",
		"rotate_ccw",
		"flip_horizontal",
		"flip_vertical",
		"desktop_next",
		"desktop_prev",
		"desktop_last",
		"send_to_desktop_next",
		"send_to_desktop_prev",
		"send_to_desktop_1",
		"send_to_desktop_2",
		"send_to_desktop_3",
		"send_to_desktop_4",
		"send_to_desktop_5",
		"send_to_desktop_6",
		"send_to_desktop_7",
		"send_to_desktop_8",
		"send_to_desktop_9",
		"send_to_desktop_10",
		"desktop_1",
		"desktop_2",
		"desktop_3",
		"desktop_4",
		"desktop_5",
		"desktop_6",
		"desktop_7",
		"desktop_8",
		"desktop_9",
		"desktop_10",
		"resize_left",
		"resize_right",
		"resize_up",
		"resize_down",
		"interactive_move",
		"interactive_resize",
		"tiling_drag",
		"external",
		"portal_shortcut"
	};
	if (action >= 0 && action < (int)(sizeof(names) / sizeof(names[0])))
		return names[action];

	return "unknown";
}

void execute_bind_action(bind_action_t action) {
	bind_t b = {
		.action = action,
		.desktop_index = 0
	};
	execute_bind(b);
}

bind_action_t bind_action_from_name(const char *name) {
	if (!name)
		return BIND_NONE;

#define ACTION_IF_MATCH(n, val)                                                                                        \
	if (strcmp(name, n) == 0)                                                                                            \
	return val

	ACTION_IF_MATCH("west", BIND_FOCUS_WEST);
	ACTION_IF_MATCH("w", BIND_FOCUS_WEST);
	ACTION_IF_MATCH("east", BIND_FOCUS_EAST);
	ACTION_IF_MATCH("e", BIND_FOCUS_EAST);
	ACTION_IF_MATCH("north", BIND_FOCUS_NORTH);
	ACTION_IF_MATCH("n", BIND_FOCUS_NORTH);
	ACTION_IF_MATCH("south", BIND_FOCUS_SOUTH);
	ACTION_IF_MATCH("s", BIND_FOCUS_SOUTH);

	ACTION_IF_MATCH("swap_west", BIND_SWAP_WEST);
	ACTION_IF_MATCH("swap_east", BIND_SWAP_EAST);
	ACTION_IF_MATCH("swap_north", BIND_SWAP_NORTH);
	ACTION_IF_MATCH("swap_south", BIND_SWAP_SOUTH);

	ACTION_IF_MATCH("presel_west", BIND_PRESEL_WEST);
	ACTION_IF_MATCH("presel_east", BIND_PRESEL_EAST);
	ACTION_IF_MATCH("presel_north", BIND_PRESEL_NORTH);
	ACTION_IF_MATCH("presel_south", BIND_PRESEL_SOUTH);
	ACTION_IF_MATCH("presel_cancel", BIND_PRESEL_CANCEL);

	ACTION_IF_MATCH("resize_left", BIND_RESIZE_LEFT);
	ACTION_IF_MATCH("left", BIND_RESIZE_LEFT);
	ACTION_IF_MATCH("resize_right", BIND_RESIZE_RIGHT);
	ACTION_IF_MATCH("right", BIND_RESIZE_RIGHT);
	ACTION_IF_MATCH("resize_up", BIND_RESIZE_UP);
	ACTION_IF_MATCH("up", BIND_RESIZE_UP);
	ACTION_IF_MATCH("resize_down", BIND_RESIZE_DOWN);
	ACTION_IF_MATCH("down", BIND_RESIZE_DOWN);

	ACTION_IF_MATCH("toggle_floating", BIND_TOGGLE_FLOATING);
	ACTION_IF_MATCH("toggle_fullscreen", BIND_TOGGLE_FULLSCREEN);
	ACTION_IF_MATCH("toggle_pseudo_tiled", BIND_TOGGLE_PSEUDO_TILED);
	ACTION_IF_MATCH("toggle_monocle", BIND_TOGGLE_MONOCLE);
	ACTION_IF_MATCH("toggle_master_stack", BIND_TOGGLE_MASTER_STACK);
	ACTION_IF_MATCH("master_stack_inc", BIND_MASTER_STACK_INC);
	ACTION_IF_MATCH("master_stack_dec", BIND_MASTER_STACK_DEC);
	ACTION_IF_MATCH("master_stack_flip", BIND_MASTER_STACK_FLIP);
	ACTION_IF_MATCH("master_stack_cycle_orientation", BIND_MASTER_STACK_CYCLE_ORIENTATION);
	ACTION_IF_MATCH("master_stack_cycle_stack_layout", BIND_MASTER_STACK_CYCLE_STACK_LAYOUT);

	ACTION_IF_MATCH("rotate_cw", BIND_ROTATE_CW);
	ACTION_IF_MATCH("rotate_ccw", BIND_ROTATE_CCW);

	ACTION_IF_MATCH("flip_horizontal", BIND_FLIP_HORIZONTAL);
	ACTION_IF_MATCH("flip_vertical", BIND_FLIP_VERTICAL);

	ACTION_IF_MATCH("desktop_next", BIND_DESKTOP_NEXT);
	ACTION_IF_MATCH("next", BIND_DESKTOP_NEXT);
	ACTION_IF_MATCH("desktop_prev", BIND_DESKTOP_PREV);
	ACTION_IF_MATCH("desktop_last", BIND_DESKTOP_LAST);

	ACTION_IF_MATCH("quit", BIND_QUIT);

	ACTION_IF_MATCH("send_to_desktop_next", BIND_SEND_TO_DESKTOP_NEXT);
	ACTION_IF_MATCH("send_to_desktop_prev", BIND_SEND_TO_DESKTOP_PREV);

	ACTION_IF_MATCH("send_next", BIND_SEND_TO_DESKTOP_NEXT);
	ACTION_IF_MATCH("send_prev", BIND_SEND_TO_DESKTOP_PREV);
	ACTION_IF_MATCH("send", BIND_SEND_TO_DESKTOP_NEXT);

	ACTION_IF_MATCH("clockwise", BIND_ROTATE_CW);
	ACTION_IF_MATCH("cw", BIND_ROTATE_CW);
	ACTION_IF_MATCH("counterclockwise", BIND_ROTATE_CCW);
	ACTION_IF_MATCH("ccw", BIND_ROTATE_CCW);
	ACTION_IF_MATCH("horizontal", BIND_FLIP_HORIZONTAL);
	ACTION_IF_MATCH("h", BIND_FLIP_HORIZONTAL);
	ACTION_IF_MATCH("vertical", BIND_FLIP_VERTICAL);
	ACTION_IF_MATCH("v", BIND_FLIP_VERTICAL);

#undef ACTION_IF_MATCH

	return BIND_NONE;
}
