#include "config.h"
#include "server.h"
#include "keyboard.h"
#include "types.h"
#include "workspace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include <limits.h>
#include <ctype.h>
#include <sys/inotify.h>

#define BWM_CONFIG_DIR "/.config/bwm"
#define BWMRC_NAME "bwmrc"
#define BWMHKRC_NAME "bwmhkrc"

static const char *custom_config_dir = NULL;

keybind_t keybinds[MAX_KEYBINDS];
size_t num_keybinds = 0;
gesturebind_t gesture_bindings[MAX_GESTUREBINDS];
size_t num_gesturebinds = 0;
submap_t *active_submap = NULL;

#define MAX_SUBMAPS 32
static submap_t submaps[MAX_SUBMAPS];
static size_t num_submaps = 0;
static submap_t *current_parsing_submap = NULL;

static keyboard_grouping_t keyboard_grouping = KEYBOARD_GROUP_DEFAULT;

static int hotkey_watch_fd = -1;
static char hotkey_config_path[PATH_MAX];
static void setup_inotify_watch(const char *config_path);

static const char *get_config_home(void) {
  if (custom_config_dir)
    return custom_config_dir;

  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0] != '\0')
    return xdg;
  static char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s%s", getenv("HOME") ? getenv("HOME") : "/root", BWM_CONFIG_DIR);
  return buf;
}

static uint32_t parse_modifiers(const char *mod_str) {
  uint32_t mods = 0;
  if (!mod_str || mod_str[0] == '\0')
    return 0;

  char *tmp = strdup(mod_str);
  char *saveptr;
  char *token = strtok_r(tmp, "+", &saveptr);

  while (token) {
    while (*token == ' ') token++;
    char *end = token + strlen(token) - 1;
    while (end > token && *end == ' ') *end-- = '\0';

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

static xkb_keysym_t parse_keysym(const char *name) {
  if (!name || name[0] == '\0')
    return XKB_KEY_NoSymbol;

  if (strlen(name) == 1)
    return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);

  return xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
}

static uint32_t parse_keycode(const char *name) {
  if (!name || name[0] == '\0')
    return 0;

  if (name[0] >= '1' && name[0] <= '9' && name[1] == '\0')
    return name[0] - '0' + 1;

  if (strcmp(name, "0") == 0)
    return 11;

  return 0;
}

static bind_action_t parse_action(const char *cmd, int *desktop_index, char *submap_name) {
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

  if (strncmp(cmd, "bmsg ", 5) == 0) {
    char buf[MAXLEN];
    snprintf(buf, sizeof(buf), "%s", cmd + 5);

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

    if (strcmp(args[0], "quit") == 0)
      return BIND_QUIT;

    if (strcmp(args[0], "focus") == 0 && argc >= 2) {
      if (strcmp(args[1], "west") == 0) return BIND_FOCUS_WEST;
      if (strcmp(args[1], "south") == 0) return BIND_FOCUS_SOUTH;
      if (strcmp(args[1], "north") == 0) return BIND_FOCUS_NORTH;
      if (strcmp(args[1], "east") == 0) return BIND_FOCUS_EAST;
    }

    if (strcmp(args[0], "swap") == 0 && argc >= 2) {
      if (strcmp(args[1], "west") == 0) return BIND_SWAP_WEST;
      if (strcmp(args[1], "south") == 0) return BIND_SWAP_SOUTH;
      if (strcmp(args[1], "north") == 0) return BIND_SWAP_NORTH;
      if (strcmp(args[1], "east") == 0) return BIND_SWAP_EAST;
    }

    if (strcmp(args[0], "presel") == 0 && argc >= 2) {
      if (strcmp(args[1], "west") == 0) return BIND_PRESEL_WEST;
      if (strcmp(args[1], "south") == 0) return BIND_PRESEL_SOUTH;
      if (strcmp(args[1], "north") == 0) return BIND_PRESEL_NORTH;
      if (strcmp(args[1], "east") == 0) return BIND_PRESEL_EAST;
      if (strcmp(args[1], "cancel") == 0) return BIND_PRESEL_CANCEL;
    }

    if (strcmp(args[0], "node") == 0 && argc >= 2) {
      if (strcmp(args[1], "-c") == 0 || strcmp(args[1], "--close") == 0)
        return BIND_NODE_CLOSE;

      if (strcmp(args[1], "-f") == 0 || strcmp(args[1], "--focus") == 0)
        return BIND_NODE_FOCUS;

      if ((strcmp(args[1], "-t") == 0 || strcmp(args[1], "--state") == 0) && argc >= 3) {
        if (strcmp(args[2], "tiled") == 0) return BIND_NODE_STATE_TILED;
        if (strcmp(args[2], "floating") == 0) return BIND_NODE_STATE_FLOATING;
        if (strcmp(args[2], "fullscreen") == 0) return BIND_NODE_STATE_FULLSCREEN;
        if (strcmp(args[2], "pseudo_tiled") == 0) return BIND_TOGGLE_PSEUDO_TILED;
      }

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
        return BIND_DESKTOP_FOCUS;

      if ((strcmp(args[1], "-l") == 0 || strcmp(args[1], "--layout") == 0) && argc >= 3) {
        if (strcmp(args[2], "tiled") == 0) return BIND_DESKTOP_LAYOUT_TILED;
        if (strcmp(args[2], "monocle") == 0) return BIND_DESKTOP_LAYOUT_MONOCLE;
      }

      int d = atoi(args[1]);
      if (d >= 1 && d <= 10) {
        *desktop_index = d;
        return BIND_DESKTOP_1;
      }
    }
  }

  return BIND_EXTERNAL;
}

static char *expand_sequence(const char *input, char *output, size_t out_size) {
  size_t in_len = strlen(input);
  size_t out_idx = 0;

  for (size_t i = 0; i < in_len && out_idx < out_size - 1; i++) {
    if (input[i] == '{') {
      size_t end = i + 1;
      while (end < in_len && input[end] != '}') end++;
      if (end > i + 1) {
        char choices[256];
        size_t choice_len = end - i - 1;
        if (choice_len < sizeof(choices)) {
          memcpy(choices, input + i + 1, choice_len);
          choices[choice_len] = '\0';

          char *choice_save;
          char *choice = strtok_r(choices, ",", &choice_save);
          while (choice && out_idx < out_size - 1) {
            output[out_idx++] = *choice;
            choice = strtok_r(NULL, ",", &choice_save);
          }
        }
      }
      i = end;
    } else {
      output[out_idx++] = input[i];
    }
  }
  output[out_idx] = '\0';
  return output;
}

static void add_keybind(uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode, bool use_keycode, bind_action_t action, int desktop_index, const char *external_cmd, const char *submap_name) {
  size_t *num_ptr;
  keybind_t *kb_array;

  if (current_parsing_submap) {
    if (current_parsing_submap->num_keybinds >= MAX_KEYBINDS) {
      wlr_log(WLR_ERROR, "Maximum number of keybinds reached for submap %s", current_parsing_submap->name);
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

  wlr_log(WLR_DEBUG, "Added gesturebind: type=%d fingers=%d dirs=%u action=%d",
      type, fingers, directions, action);
}

static void parse_gesture_hotkey_line(const char *gesture_str, const char *command_str) {
  char gesture_buf[MAXLEN];
  char command_buf[MAXLEN];

  snprintf(gesture_buf, sizeof(gesture_buf), "%s", gesture_str);
  snprintf(command_buf, sizeof(command_buf), "%s", command_str);

  char expanded_cmd[MAXLEN];
  expand_sequence(command_buf, expanded_cmd, sizeof(expanded_cmd));

  struct gesture gest;
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
    add_gesturebind(gest.type, gest.fingers, gest.directions, NULL, action, desktop_index, expanded_cmd);
  }
}

static void parse_hotkey_line(const char *hotkey_str, const char *command_str) {
  if (strncmp(hotkey_str, "gesture ", 8) == 0) {
    parse_gesture_hotkey_line(hotkey_str + 8, command_str);
    return;
  }

  char hotkey_buf[MAXLEN];
  char command_buf[MAXLEN];

  snprintf(hotkey_buf, sizeof(hotkey_buf), "%s", hotkey_str);
  snprintf(command_buf, sizeof(command_buf), "%s", command_str);

  char expanded_hotkey[MAXLEN];
  char expanded_cmd[MAXLEN];

  expand_sequence(hotkey_buf, expanded_hotkey, sizeof(expanded_hotkey));
  expand_sequence(command_buf, expanded_cmd, sizeof(expanded_cmd));

  wlr_log(WLR_DEBUG, "parse_hotkey_line: hotkey=[%s] cmd=[%s]", expanded_hotkey, expanded_cmd);

  char single_hotkey[MAXLEN];
  char single_cmd[MAXLEN];

  char *saveptr;
  char *hotkey_token = strtok_r(expanded_hotkey, "\t", &saveptr);
  while (hotkey_token) {
    snprintf(single_hotkey, sizeof(single_hotkey), "%s", hotkey_token);

    char *cmd_token = strtok_r(expanded_cmd, "\t", &saveptr);

    while (cmd_token) {
      snprintf(single_cmd, sizeof(single_cmd), "%s", cmd_token);

      uint32_t modifiers = 0;
      xkb_keysym_t keysym = XKB_KEY_NoSymbol;
      uint32_t keycode = 0;
      bool use_keycode = false;

      char *plus = strrchr(single_hotkey, '+');
      if (plus) {
        *plus = '\0';
        modifiers = parse_modifiers(single_hotkey);
        char *key_part = plus + 1;
        while (*key_part == ' ') key_part++;
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
        wlr_log(WLR_DEBUG, "Parsed action: %d for cmd: '%s' submap: '%s'", action, single_cmd, submap_name);
        if (action != BIND_EXTERNAL)
          add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index, NULL, submap_name[0] ? submap_name : NULL);
        else
          add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index, single_cmd, submap_name[0] ? submap_name : NULL);
      }

      cmd_token = strtok_r(NULL, "\t", &saveptr);
    }

    hotkey_token = strtok_r(NULL, "\t", &saveptr);
  }
}

static char config_path[PATH_MAX];
static char hotkey_init_path[PATH_MAX];
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
  num_keybinds = 0;
  num_submaps = 0;
  active_submap = NULL;
  current_parsing_submap = NULL;

  wlr_log(WLR_DEBUG, "load_hotkeys called with path: %s", config_path);

  FILE *f = fopen(config_path, "r");
  if (!f) {
    wlr_log(WLR_INFO, "No hotkey config found at %s", config_path);
    return;
  }

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
    if (len > 0 && line[len-1] == '\n')
      line[len-1] = '\0';

    size_t indent = 0;
    while (line[indent] == ' ' || line[indent] == '\t') indent++;

    char *ptr = line + indent;
    char *content_start = ptr;
    while (*content_start == ' ' || *content_start == '\t') content_start++;
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
        while (key_part_len > 0 && ptr[key_part_len - 1] == ' ') key_part_len--;
        if (key_part_len < sizeof(pending_hotkey)) {
          strncpy(pending_hotkey, ptr, key_part_len);
          pending_hotkey[key_part_len] = '\0';
        }
        char *cmd_part = arrow + 2;
        while (*cmd_part == ' ') cmd_part++;
        snprintf(pending_command, sizeof(pending_command), "%s", cmd_part);
        continue;
      }
    }

    if (isgraph((unsigned char)first_char) || (first_char != '\0' && !isspace((unsigned char)first_char))) {
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
      if (pending_command[0] != '\0' && offset > 0 && pending_command[offset-1] != ' ') {
        pending_command[offset++] = ' ';
      }
      bool last_was_space = false;
      for (size_t i = 0; ptr[i] && offset < (int)sizeof(pending_command) - 1; i++) {
        if (isspace((unsigned char)ptr[i])) {
          if (!last_was_space && offset > 0 && pending_command[offset-1] != ' ') {
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
  wlr_log(WLR_INFO, "Loaded %zu keybinds and %zu submaps from %s", num_keybinds, num_submaps, config_path);
}

static void setup_inotify_watch(const char *config_path) {
  if (hotkey_watch_fd >= 0)
    close(hotkey_watch_fd);

  hotkey_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (hotkey_watch_fd < 0) {
    wlr_log(WLR_ERROR, "Failed to initialize inotify");
    return;
  }

  char dir_path[PATH_MAX];
  snprintf(dir_path, sizeof(dir_path), "%s", config_path);
  char *last_slash = strrchr(dir_path, '/');
  if (last_slash) {
    *last_slash = '\0';
    inotify_add_watch(hotkey_watch_fd, dir_path, IN_MODIFY);
  } else {
    inotify_add_watch(hotkey_watch_fd, ".", IN_MODIFY);
  }

  snprintf(hotkey_config_path, sizeof(hotkey_config_path), "%s", config_path);
}

void config_init(void) {
  config_init_with_config_dir(NULL);
}

void config_init_with_config_dir(const char *config_dir) {
  custom_config_dir = config_dir;

  const char *config_home = get_config_home();

  char bwmrc_path[PATH_MAX];
  snprintf(bwmrc_path, sizeof(bwmrc_path), "%s/%s", config_home, BWMRC_NAME);
  run_config(bwmrc_path);

  char hotkey_path[PATH_MAX];
  snprintf(hotkey_path, sizeof(hotkey_path), "%s/%s", config_home, BWMHKRC_NAME);
  snprintf(hotkey_init_path, sizeof(hotkey_init_path), "%s", hotkey_path);
}

void config_fini(void) {
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

bool keybind_matches(keybind_t *kb, uint32_t modifiers, xkb_keysym_t keysym, uint32_t keycode) {
  if (!kb) return false;

  if (kb->use_keycode)
    return (kb->modifiers == modifiers) && (kb->keycode == keycode);
  else {
    if (kb->modifiers == modifiers && kb->keysym == keysym)
      return true;
    if (kb->modifiers == (modifiers | WLR_MODIFIER_SHIFT) && kb->keysym == keysym)
      return true;
    if (kb->modifiers == modifiers) {
      xkb_keysym_t lower = xkb_keysym_to_lower(keysym);
      if (kb->keysym == lower)
        return true;
    }
    return false;
  }
}

void execute_keybind(keybind_t *kb) {
  if (!kb) return;

  switch (kb->action) {
    case BIND_QUIT:
      wl_display_terminate(server.wl_display);
      break;
    case BIND_NODE_FOCUS:
      break;
    case BIND_NODE_CLOSE:
      close_focused();
      break;
    case BIND_NODE_STATE_TILED:
      break;
    case BIND_NODE_STATE_FLOATING:
      toggle_floating();
      break;
    case BIND_NODE_STATE_FULLSCREEN:
      toggle_fullscreen();
      break;
    case BIND_NODE_TO_DESKTOP:
      if (kb->desktop_index > 0) {
        send_to_desktop(kb->desktop_index - 1);
      }
      break;
    case BIND_DESKTOP_FOCUS:
      if (kb->desktop_index > 0) {
        workspace_switch_to_desktop_by_index(kb->desktop_index - 1);
      }
      break;
    case BIND_DESKTOP_LAYOUT_TILED:
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
    case BIND_DESKTOP_NEXT:
      focus_next_desktop();
      break;
    case BIND_DESKTOP_PREV:
      focus_prev_desktop();
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
      workspace_switch_to_desktop_by_index(kb->desktop_index - 1);
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
      send_to_desktop(kb->desktop_index - 1);
      break;
    case BIND_EXTERNAL:
    case BIND_NONE:
      if (kb->external_cmd[0] != '\0') {
        if (fork() == 0) {
          execl("/bin/sh", "/bin/sh", "-c", kb->external_cmd, NULL);
          _exit(1);
        }
      }
      break;
    case BIND_ENTER_SUBMAP:
      enter_submap(kb->submap_name);
      break;
    case BIND_EXIT_SUBMAP:
      exit_submap();
      break;
  }
}

int get_hotkey_watch_fd(void) {
  return hotkey_watch_fd;
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

bool gesturebind_matches(gesturebind_t *gb, enum gesture_type type, uint8_t fingers) {
  if (!gb) return false;

  if (gb->type != type) {
    return false;
  }

  if (gb->fingers != GESTURE_FINGERS_ANY && gb->fingers != fingers) {
    return false;
  }

  return true;
}

void execute_gesturebind(gesturebind_t *gb) {
  if (!gb) return;

  switch (gb->action) {
    case BIND_QUIT:
      wl_display_terminate(server.wl_display);
      break;
    case BIND_NODE_FOCUS:
      break;
    case BIND_NODE_CLOSE:
      close_focused();
      break;
    case BIND_NODE_STATE_TILED:
      break;
    case BIND_NODE_STATE_FLOATING:
      toggle_floating();
      break;
    case BIND_NODE_STATE_FULLSCREEN:
      toggle_fullscreen();
      break;
    case BIND_NODE_TO_DESKTOP:
      if (gb->desktop_index > 0) {
        send_to_desktop(gb->desktop_index - 1);
      }
      break;
    case BIND_DESKTOP_FOCUS:
      if (gb->desktop_index > 0) {
        workspace_switch_to_desktop_by_index(gb->desktop_index - 1);
      }
      break;
    case BIND_DESKTOP_LAYOUT_TILED:
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
    case BIND_DESKTOP_NEXT:
      focus_next_desktop();
      break;
    case BIND_DESKTOP_PREV:
      focus_prev_desktop();
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
      workspace_switch_to_desktop_by_index(gb->action - BIND_DESKTOP_1);
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
      send_to_desktop(gb->action - BIND_SEND_TO_DESKTOP_1);
      break;
    case BIND_EXTERNAL:
      if (gb->external_cmd[0] != '\0') {
        if (fork() == 0) {
          execl("/bin/sh", "/bin/sh", "-c", gb->external_cmd, NULL);
          _exit(1);
        }
      }
      break;
    case BIND_NONE:
    case BIND_ENTER_SUBMAP:
    case BIND_EXIT_SUBMAP:
      break;
  }
}

void reload_gesturebinds(void) {
  num_gesturebinds = 0;
}
