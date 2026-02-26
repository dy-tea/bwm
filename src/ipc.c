#include "ipc.h"
#include "server.h"
#include "types.h"
#include "toplevel.h"
#include "transaction.h"
#include "workspace.h"
#include "tree.h"
#include "output_config.h"
#include "input.h"
#include "keyboard.h"
#include "rule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wayland-util.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>

static int ipc_socket_fd = -1;
static char socket_path[256];

const char *ipc_get_socket_path(void) {
  return socket_path;
}

static bool streq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

void ipc_init(void) {
  struct sockaddr_un addr;
  socklen_t len;

  ipc_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ipc_socket_fd == -1) {
    wlr_log(WLR_ERROR, "Failed to create IPC socket");
    return;
  }

  fcntl(ipc_socket_fd, F_SETFD, FD_CLOEXEC);

  addr.sun_family = AF_UNIX;
  snprintf(socket_path, sizeof(socket_path), BWM_SOCKET_PATH_TEMPLATE, getuid());
  unlink(socket_path);

  size_t path_len = strlen(socket_path);
  if (path_len >= sizeof(addr.sun_path)) {
    wlr_log(WLR_ERROR, "Socket path too long");
    close(ipc_socket_fd);
    ipc_socket_fd = -1;
    return;
  }
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

  len = sizeof(addr.sun_family) + path_len + 1;
  if (bind(ipc_socket_fd, (struct sockaddr *)&addr, len) == -1) {
    wlr_log(WLR_ERROR, "Failed to bind IPC socket: %s", socket_path);
    close(ipc_socket_fd);
    ipc_socket_fd = -1;
    return;
  }

  if (listen(ipc_socket_fd, SOMAXCONN) == -1) {
    wlr_log(WLR_ERROR, "Failed to listen on IPC socket");
    close(ipc_socket_fd);
    ipc_socket_fd = -1;
    return;
  }

  setenv(BWM_SOCKET_ENV, socket_path, true);
  wlr_log(WLR_INFO, "IPC socket: %s", socket_path);
}

int ipc_get_socket_fd(void) {
  return ipc_socket_fd;
}

static void send_response(int client_fd, bool success, const char *msg) {
  char buf[BWM_BUFSIZ];
  size_t offset = 0;
  buf[offset++] = success ? '\0' : '\x01';

  if (msg) {
    size_t len = strlen(msg);
    if (len > sizeof(buf) - 1)
      len = sizeof(buf) - 1;
    memcpy(buf + offset, msg, len);
    offset += len;
  }

  wlr_log(WLR_DEBUG, "IPC: sending response: %.*s", (int)(offset-1), buf+1);
  write(client_fd, buf, offset);
}

static void send_success(int client_fd, const char *msg) {
  send_response(client_fd, true, msg);
}

static void send_failure(int client_fd, const char *msg) {
  send_response(client_fd, false, msg);
}

static void ipc_cmd_quit(char **args, int num, int client_fd) {
  (void)args;
  (void)num;
  wlr_log(WLR_INFO, "Quit requested via IPC");
  wl_display_terminate(server.wl_display);
  send_success(client_fd, "quit\n");
}

static void ipc_cmd_output(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "output: missing arguments\n");
    return;
  }

  char *output_name = *args;
  args++;
  num--;

  if (num < 1) {
    send_failure(client_fd, "output: missing subcommand\n");
    return;
  }

  struct output_config *oc = output_config_find(output_name);
  if (!oc) {
    oc = output_config_create(output_name);
    if (!oc) {
      send_failure(client_fd, "output: failed to create config\n");
      return;
    }
  }

  char *subcmd = *args;

  if (streq("enable", subcmd)) {
    oc->enable = OUTPUT_CONFIG_ENABLE;
    output_config_apply(oc);
    send_success(client_fd, "output enabled\n");
  } else if (streq("disable", subcmd)) {
    oc->enable = OUTPUT_CONFIG_DISABLE;
    output_config_apply(oc);
    send_success(client_fd, "output disabled\n");
  } else if (streq("mode", subcmd) || streq("resolution", subcmd) || streq("res", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output mode: missing resolution\n");
      return;
    }
    args++;
    num--;

    char *res = *args;
    int width, height;
    float refresh_rate = -1;

    char *at = strchr(res, '@');
    if (at) {
      *at = '\0';
      if (sscanf(at + 1, "%f", &refresh_rate) != 1) {
        send_failure(client_fd, "output mode: invalid refresh rate\n");
        return;
      }
    }

    if (sscanf(res, "%dx%d", &width, &height) != 2) {
      send_failure(client_fd, "output mode: invalid resolution format\n");
      return;
    }

    oc->width = width;
    oc->height = height;
    oc->refresh_rate = refresh_rate;
    output_config_apply(oc);
    send_success(client_fd, "output mode set\n");
  } else if (streq("position", subcmd) || streq("pos", subcmd)) {
    if (num < 3) {
      send_failure(client_fd, "output position: missing coordinates\n");
      return;
    }
    args++;
    num--;

    int x, y;
    if (sscanf(*args, "%d", &x) != 1) {
      send_failure(client_fd, "output position: invalid x\n");
      return;
    }
    args++;
    num--;

    if (sscanf(*args, "%d", &y) != 1) {
      send_failure(client_fd, "output position: invalid y\n");
      return;
    }

    oc->x = x;
    oc->y = y;
    output_config_apply(oc);
    send_success(client_fd, "output position set\n");
  } else if (streq("scale", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output scale: missing scale factor\n");
      return;
    }
    args++;
    num--;

    float scale;
    if (sscanf(*args, "%f", &scale) != 1) {
      send_failure(client_fd, "output scale: invalid scale\n");
      return;
    }

    oc->scale = scale;
    output_config_apply(oc);
    send_success(client_fd, "output scale set\n");
  } else if (streq("transform", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output transform: missing transform\n");
      return;
    }
    args++;
    num--;

    int transform = -1;
    if (streq("normal", *args) || streq("0", *args)) {
      transform = WL_OUTPUT_TRANSFORM_NORMAL;
    } else if (streq("90", *args)) {
      transform = WL_OUTPUT_TRANSFORM_90;
    } else if (streq("180", *args)) {
      transform = WL_OUTPUT_TRANSFORM_180;
    } else if (streq("270", *args)) {
      transform = WL_OUTPUT_TRANSFORM_270;
    } else if (streq("flipped", *args) || streq("flipped-180", *args)) {
      transform = WL_OUTPUT_TRANSFORM_FLIPPED_180;
    } else if (streq("flipped-90", *args)) {
      transform = WL_OUTPUT_TRANSFORM_FLIPPED_90;
    } else if (streq("flipped-270", *args)) {
      transform = WL_OUTPUT_TRANSFORM_FLIPPED_270;
    }

    if (transform < 0) {
      send_failure(client_fd, "output transform: invalid transform\n");
      return;
    }

    oc->transform = transform;
    output_config_apply(oc);
    send_success(client_fd, "output transform set\n");
  } else if (streq("dpms", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output dpms: missing state\n");
      return;
    }
    args++;
    num--;

    if (streq("on", *args)) {
      oc->dpms_state = OUTPUT_CONFIG_DPMS_ON;
    } else if (streq("off", *args)) {
      oc->dpms_state = OUTPUT_CONFIG_DPMS_OFF;
    } else {
      send_failure(client_fd, "output dpms: invalid state (on/off)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output dpms set\n");
  } else if (streq("adaptive_sync", subcmd) || streq("vrr", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output adaptive_sync: missing state\n");
      return;
    }
    args++;
    num--;

    if (streq("on", *args) || streq("enable", *args)) {
      oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED;
    } else if (streq("off", *args) || streq("disable", *args)) {
      oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_DISABLED;
    } else {
      send_failure(client_fd, "output adaptive_sync: invalid state (on/off)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output adaptive_sync set\n");
  } else if (streq("render_bit_depth", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output render_bit_depth: missing value\n");
      return;
    }
    args++;
    num--;

    if (streq("8", *args) || streq("8-bit", *args)) {
      oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_8;
    } else if (streq("10", *args) || streq("10-bit", *args)) {
      oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_10;
    } else {
      send_failure(client_fd, "output render_bit_depth: invalid value (8/10)\n");
      return;
    }

    output_config_apply(oc);
    send_success(client_fd, "output render_bit_depth set\n");
  } else {
    send_failure(client_fd, "output: unknown subcommand\n");
  }
}

static void ipc_cmd_input(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "input: missing arguments\n");
    return;
  }

  char *identifier = NULL;
  enum input_config_type type = INPUT_CONFIG_TYPE_ANY;
  if (num > 0 && strncmp(*args, "type:", 5) != 0) {
    identifier = *args;
    args++;
    num--;
  }

  if (num > 0 && strncmp(*args, "type:", 5) == 0) {
    char *type_str = *args + 5;
    while (*type_str == ' ') type_str++;

    if (streq(type_str, "keyboard")) {
      type = INPUT_CONFIG_TYPE_KEYBOARD;
    } else if (streq(type_str, "pointer")) {
      type = INPUT_CONFIG_TYPE_POINTER;
    } else if (streq(type_str, "touchpad")) {
      type = INPUT_CONFIG_TYPE_TOUCHPAD;
    } else if (streq(type_str, "touchscreen")) {
      type = INPUT_CONFIG_TYPE_TOUCH;
    } else if (streq(type_str, "tablet")) {
      type = INPUT_CONFIG_TYPE_TABLET;
    } else if (streq(type_str, "tablet_pad")) {
      type = INPUT_CONFIG_TYPE_TABLET_PAD;
    } else if (streq(type_str, "switch")) {
      type = INPUT_CONFIG_TYPE_SWITCH;
    } else if (streq(type_str, "any")) {
      type = INPUT_CONFIG_TYPE_ANY;
    } else {
      send_failure(client_fd, "input: unknown type\n");
      return;
    }

    args++;
    num--;
  }

  if (num < 1) {
    send_failure(client_fd, "input: missing property\n");
    return;
  }

  char *property = *args;
  args++;
  num--;

  char *value = "";
  if (num > 0)
    value = *args;

  input_config_t *config = NULL;
  for (size_t i = 0; i < num_input_configs; i++) {
    input_config_t *cfg = input_configs[i];
    if (identifier) {
      if (cfg->identifier && strcmp(cfg->identifier, identifier) == 0) {
        config = cfg;
        break;
      }
    } else if (cfg->type == type && cfg->identifier == NULL) {
      config = cfg;
      break;
    }
  }

  if (!config) {
    config = input_config_create(identifier);
    if (!config) {
      send_failure(client_fd, "input: failed to create config\n");
      return;
    }
    config->type = type;
    input_config_add(config);
  }

  if (!input_config_set_value(config, property, value)) {
    send_failure(client_fd, "input: unknown property\n");
    return;
  }

  if (!config) {
    send_failure(client_fd, "input: failed to create config\n");
    return;
  }
  config->type = type;

  if (!input_config_set_value(config, property, value)) {
    input_config_destroy(config);
    send_failure(client_fd, "input: unknown property\n");
    return;
  }

  input_apply_config_all_pointers();
  input_apply_config_all_keyboards();
}

static desktop_t *find_desktop_by_name_in_monitor(monitor_t *mon, const char *name) {
  desktop_t *d = mon->desk;
  while (d) {
    if (strcmp(d->name, name) == 0)
      return d;
    d = d->next;
  }
  return NULL;
}

static void ipc_cmd_node(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "node: Missing arguments\n");
    return;
  }

  struct bwm_toplevel *toplevel = server.toplevels.next ?
    (struct bwm_toplevel *)((char *)&server.toplevels.next - offsetof(struct bwm_toplevel, link)) : NULL;

  if (streq("-f", *args) || streq("--focus", *args)) {
    if (toplevel) {
      focus_toplevel(toplevel);
      send_success(client_fd, "focused\n");
    } else {
      send_failure(client_fd, "no toplevel to focus\n");
    }
  } else if (streq("-c", *args) || streq("--close", *args)) {
    if (toplevel && toplevel->xdg_toplevel) {
      wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
      send_success(client_fd, "closed\n");
    } else {
      send_failure(client_fd, "no toplevel to close\n");
    }
  } else if (streq("-t", *args) || streq("--state", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -t: missing state argument\n");
      return;
    }
    args++;
    num--;
    client_state_t state;
    if (streq("tiled", *args)) {
      state = STATE_TILED;
    } else if (streq("floating", *args)) {
      state = STATE_FLOATING;
    } else if (streq("fullscreen", *args)) {
      state = STATE_FULLSCREEN;
    } else {
      send_failure(client_fd, "node -t: unknown state\n");
      return;
    }

    if (toplevel && toplevel->node) {
      toplevel->node->client->state = state;
      transaction_commit_dirty();
      send_success(client_fd, "state changed\n");
    } else {
      send_failure(client_fd, "no toplevel\n");
    }
  } else if (streq("-d", *args) || streq("--to-desktop", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -d: missing desktop name\n");
      return;
    }
    args++;
    num--;
    char *desk_name = *args;
    desktop_t *target = find_desktop_by_name(desk_name);
    if (!target) {
      send_failure(client_fd, "node -d: desktop not found\n");
      return;
    }

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk || !m->desk->focus) {
      send_failure(client_fd, "node -d: no focused node\n");
      return;
    }

    if (m->desk == target) {
      send_failure(client_fd, "node -d: already on target desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (n == NULL || n->client == NULL) {
      send_failure(client_fd, "node -d: no client\n");
      return;
    }

    desktop_t *src_desk = m->desk;

    n->destroying = false;
    n->ntxnrefs = 0;

    n->client->shown = false;
    wlr_scene_node_set_enabled(&n->client->toplevel->scene_tree->node, false);

    remove_node(m, src_desk, n);

    if (src_desk != target && src_desk->root != NULL) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus != NULL) {
        src_desk->focus = new_focus;
        focus_node(m, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }

    n->destroying = false;
    n->ntxnrefs = 0;

    insert_node(m, target, n, find_public(target));

    target->focus = n;
    if (target == m->desk && target->focus == n) {
      focus_node(m, target, n);
    }

    if (target == m->desk) {
      for (node_t *n_iter = first_extrema(target->root); n_iter != NULL; n_iter = next_leaf(n_iter, target->root)) {
        if (n_iter->client) {
          n_iter->client->shown = true;
          wlr_scene_node_set_enabled(&n_iter->client->toplevel->scene_tree->node, true);
        }
      }
      arrange(m, target, true);
    } else {
      for (node_t *n_iter = first_extrema(target->root); n_iter != NULL; n_iter = next_leaf(n_iter, target->root)) {
        if (n_iter->client) {
          n_iter->client->shown = false;
          wlr_scene_node_set_enabled(&n_iter->client->toplevel->scene_tree->node, false);
        }
      }
      arrange(m, target, false);
    }

    if (src_desk == m->desk) {
      for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, src_desk->root)) {
        if (n_iter->client) {
          n_iter->client->shown = true;
          wlr_scene_node_set_enabled(&n_iter->client->toplevel->scene_tree->node, true);
        }
      }
      arrange(m, src_desk, true);
    } else if (src_desk->root) {
      for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, src_desk->root)) {
        if (n_iter->client) {
          n_iter->client->shown = false;
          wlr_scene_node_set_enabled(&n_iter->client->toplevel->scene_tree->node, false);
        }
      }
    }

    send_success(client_fd, "node sent to desktop\n");
  } else {
    send_failure(client_fd, "node: unknown command\n");
  }
}

static void ipc_cmd_desktop(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "desktop: Missing arguments\n");
    return;
  }

  monitor_t *mon = server.focused_monitor;
  if (!mon || !mon->desk) {
    send_failure(client_fd, "no desktop\n");
    return;
  }

  if (streq("next", *args)) {
    focus_next_desktop();
    send_success(client_fd, "focused\n");
    return;
  } else if (streq("prev", *args) || streq("previous", *args)) {
    focus_prev_desktop();
    send_success(client_fd, "focused\n");
    return;
  }

  desktop_t *desk = mon->desk;
  if ((*args)[0] != '-') {
    desk = find_desktop_by_name_in_monitor(mon, *args);
    if (!desk) {
      send_failure(client_fd, "desktop: unknown desktop\n");
      return;
    }
    args++;
    num--;
  }

  if (num < 1) {
    send_failure(client_fd, "desktop: Missing command\n");
    return;
  }

  if (streq("-f", *args) || streq("--focus", *args)) {
    if (num >= 2 && (streq("next", *args) || streq("next.local", *args))) {
      focus_next_desktop();
      send_success(client_fd, "focused\n");
    } else if (num >= 2 && (streq("prev", *args) || streq("prev.local", *args) || streq("previous", *args))) {
      focus_prev_desktop();
      send_success(client_fd, "focused\n");
    } else {
      focus_node(mon, desk, desk->focus);
      send_success(client_fd, "focused\n");
    }
  } else if (streq("-l", *args) || streq("--layout", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -l: missing layout argument\n");
      return;
    }
    args++;
    if (streq("tiled", *args)) {
      desk->layout = LAYOUT_TILED;
    } else if (streq("monocle", *args)) {
      desk->layout = LAYOUT_MONOCLE;
    } else {
      send_failure(client_fd, "desktop -l: unknown layout\n");
      return;
    }
    transaction_commit_dirty();
    send_success(client_fd, "layout changed\n");
  } else if (streq("-n", *args) || streq("--rename", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -n: missing name argument\n");
      return;
    }
    args++;
    strncpy(desk->name, *args, SMALEN - 1);
    desk->name[SMALEN - 1] = '\0';
    transaction_commit_dirty();
    send_success(client_fd, "renamed\n");
  } else {
    send_failure(client_fd, "desktop: unknown command\n");
  }
}

static monitor_t *find_monitor_by_name(const char *name) {
  for (monitor_t *m = server.monitors; m != NULL; m = m->next)
    if (strcmp(m->name, name) == 0)
      return m;
  return NULL;
}

static void ipc_cmd_monitor(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "monitor: Missing arguments\n");
    return;
  }

  monitor_t *mon = server.focused_monitor;
  bool has_target = false;

  if ((*args)[0] != '-') {
    mon = find_monitor_by_name(*args);
    if (!mon) {
      send_failure(client_fd, "monitor: unknown monitor\n");
      return;
    }
    has_target = true;
    args++;
    num--;
  }

  if (num < 1) {
    send_failure(client_fd, "monitor: Missing command\n");
    return;
  }

  if (streq("-f", *args) || streq("--focus", *args)) {
    if (mon) {
      server.focused_monitor = mon;
      focus_node(mon, mon->desk, mon->desk ? mon->desk->focus : NULL);
      send_success(client_fd, "focused\n");
    } else {
      send_failure(client_fd, "no monitor\n");
    }
  } else if (streq("-n", *args) || streq("--rename", *args)) {
    if (!has_target) {
      send_failure(client_fd, "monitor -n: no monitor specified\n");
      return;
    }
    if (num < 2) {
      send_failure(client_fd, "monitor -n: missing name argument\n");
      return;
    }
    args++;
    strncpy(mon->name, *args, SMALEN - 1);
    mon->name[SMALEN - 1] = '\0';
    transaction_commit_dirty();
    send_success(client_fd, "renamed\n");
  } else if (streq("-a", *args) || streq("--add-desktops", *args)) {
    if (!has_target)
      mon = server.focused_monitor;
    if (!mon) {
      send_failure(client_fd, "monitor -a: no monitor available\n");
      return;
    }
    if (num < 2) {
      send_failure(client_fd, "monitor -a: missing desktop names\n");
      return;
    }
    args++;
    num--;
    while (num > 0) {
      desktop_t *d = (desktop_t *)calloc(1, sizeof(desktop_t));
      d->id = next_desktop_id++;
      strncpy(d->name, *args, SMALEN - 1);
      d->name[SMALEN - 1] = '\0';
      d->layout = LAYOUT_TILED;
      d->user_layout = LAYOUT_TILED;
      d->window_gap = window_gap;
      d->border_width = border_width;
      d->padding = (padding_t){0};
      d->root = NULL;
      d->focus = NULL;

      if (mon->desk_tail) {
        d->prev = mon->desk_tail;
        mon->desk_tail->next = d;
        mon->desk_tail = d;
      } else {
        mon->desk = d;
        mon->desk_head = d;
        mon->desk_tail = d;
      }
      d->monitor = mon;

      workspace_create_desktop(d->name);

      args++;
      num--;
    }
    transaction_commit_dirty();
    send_success(client_fd, "desktops added\n");
  } else if (streq("-d", *args) || streq("--reset-desktops", *args)) {
    if (!has_target)
      mon = server.focused_monitor;
    if (!mon) {
      send_failure(client_fd, "monitor -d: no monitor available\n");
      return;
    }
    if (num < 2) {
      send_failure(client_fd, "monitor -d: missing desktop names\n");
      return;
    }

    desktop_t *d = mon->desk;
    args++;
    num--;

    while (num > 0 && d != NULL) {
      wlr_log(WLR_DEBUG, "IPC: renaming desktop to %s", *args);
      strncpy(d->name, *args, SMALEN - 1);
      d->name[SMALEN - 1] = '\0';
      wlr_log(WLR_DEBUG, "IPC: calling workspace_create_desktop for %s", d->name);
      workspace_create_desktop(d->name);
      d = d->next;
      args++;
      num--;
    }

    while (num > 0) {
      desktop_t *newd = (desktop_t *)calloc(1, sizeof(desktop_t));
      newd->id = next_desktop_id++;
      strncpy(newd->name, *args, SMALEN - 1);
      newd->name[SMALEN - 1] = '\0';
      newd->layout = LAYOUT_TILED;
      newd->user_layout = LAYOUT_TILED;
      newd->window_gap = window_gap;
      newd->border_width = border_width;
      newd->padding = (padding_t){0};
      newd->root = NULL;
      newd->focus = NULL;

      if (mon->desk_tail) {
        newd->prev = mon->desk_tail;
        mon->desk_tail->next = newd;
        mon->desk_tail = newd;
      } else {
        mon->desk = newd;
        mon->desk_head = newd;
        mon->desk_tail = newd;
      }
      newd->monitor = mon;

      wlr_log(WLR_DEBUG, "IPC: creating workspace for new desktop %s", newd->name);
      workspace_create_desktop(newd->name);
      args++;
      num--;
    }

    while (d != NULL) {
      if (d == mon->desk) {
        mon->desk = d->next;
        if (mon->desk)
          mon->desk->prev = NULL;
        if (mon->desk_head == d)
          mon->desk_head = d->next;
        if (mon->desk_tail == d)
          mon->desk_tail = d->prev;
        if (mon->desk)
          focus_node(mon, mon->desk, mon->desk->focus);
      } else {
        if (d->prev)
          d->prev->next = d->next;
        if (d->next)
          d->next->prev = d->prev;
        if (mon->desk_tail == d)
          mon->desk_tail = d->prev;
      }
      desktop_t *next = d->next;
     	d = next;
      free(d);
    }

    transaction_commit_dirty();
    workspace_sync();

    if (mon->desk) {
      focus_node(mon, mon->desk, mon->desk->focus);
    }

    send_success(client_fd, "desktops reset\n");
  } else if (streq("-d", *args) || streq("--desktops", *args)) {
    if (!has_target) {
      send_failure(client_fd, "monitor -d: no monitor specified\n");
      return;
    }
    char buf[BWM_BUFSIZ];
    size_t offset = 0;

    for (desktop_t *d = mon->desk; d != NULL; d = d->next) {
      offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", d->name);
    }
    send_success(client_fd, buf);
  } else if (streq("-l", *args) || streq("--list", *args)) {
    char buf[BWM_BUFSIZ];
    size_t offset = 0;
    for (monitor_t *m = server.monitors; m != NULL; m = m->next) {
      offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", m->name);
    }
    send_success(client_fd, buf);
  } else {
    send_failure(client_fd, "monitor: unknown command\n");
  }
}

static void ipc_cmd_query(char **args, int num, int client_fd) {
  char buf[BWM_BUFSIZ];
  size_t offset = 0;

  if (num < 1) {
    send_failure(client_fd, "query: Missing arguments\n");
    return;
  }

  if (streq("-T", *args) || streq("--tree", *args)) {
    offset += snprintf(buf + offset, sizeof(buf) - offset, "{\n");

    for (monitor_t *m = server.monitors;
      m != NULL;
      m = m->next) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  \"monitor\": {\"name\": \"%s\", \"id\": %u},\n",
        m->name, m->id);
      for (desktop_t *d = m->desk; d != NULL; d = d->next)
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "  \"desktop\": {\"name\": \"%s\", \"id\": %u, \"layout\": %d},\n",
          d->name, d->id, d->layout);
    }

    struct bwm_toplevel *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link)
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  \"toplevel\": {\"app_id\": \"%s\", \"title\": \"%s\", \"identifier\": \"%s\"}\n",
        toplevel->node && toplevel->node->client ? toplevel->node->client->app_id : "?",
        toplevel->node && toplevel->node->client ? toplevel->node->client->title : "?",
        toplevel->foreign_identifier ? toplevel->foreign_identifier : "?");

    offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");
    send_success(client_fd, buf);
  } else if (streq("-M", *args) || streq("--monitors", *args)) {
    for (monitor_t *m = server.monitors; m != NULL; m = m->next)
     	offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", m->name);
    send_success(client_fd, buf);
  } else if (streq("-D", *args) || streq("--desktops", *args)) {
    for (monitor_t *m = server.monitors; m != NULL; m = m->next)
      for (desktop_t *d = m->desk; d != NULL; d = d->next)
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", d->name);
    send_success(client_fd, buf);
  } else if (streq("-N", *args) || streq("--nodes", *args)) {
    struct bwm_toplevel *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link)
      offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n",
        toplevel->node ? toplevel->node->id : 0,
        toplevel->foreign_identifier ? toplevel->foreign_identifier : "?");
    send_success(client_fd, buf);
  } else if (streq("-f", *args) || streq("--focused", *args)) {
    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "no focused node\n");
      return;
    }
    char *foreign_id = "?";
    struct bwm_toplevel *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link)
      if (toplevel->node == n) {
        foreign_id = toplevel->foreign_identifier ? toplevel->foreign_identifier : "?";
        break;
      }
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "{\"monitor\": \"%s\", \"desktop\": \"%s\", \"id\": %u, \"type\": %d, "
      "\"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}, "
      "\"client\": \"%s\", \"identifier\": \"%s\"}\n",
      m->name,
      m->desk->name,
      n->id,
      n->split_type,
      n->rectangle.x,
      n->rectangle.y,
      n->rectangle.width,
      n->rectangle.height,
      n->client && n->client->app_id[0] ? n->client->app_id : "?",
      foreign_id);
    send_success(client_fd, buf);
  } else {
    send_failure(client_fd, "query: unknown command\n");
  }
}

static void ipc_cmd_config(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "config: Missing arguments\n");
    return;
  }

  if (streq("border_width", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      border_width = val;
      for (monitor_t *m = server.monitors; m != NULL; m = m->next) {
        m->border_width = border_width;
        for (desktop_t *d = m->desk; d != NULL; d = d->next)
          d->border_width = border_width;
      }
      transaction_commit_dirty();
      send_success(client_fd, "border_width set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", border_width);
      send_success(client_fd, buf);
    }
  } else if (streq("window_gap", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      window_gap = val;
      for (monitor_t *m = server.monitors; m != NULL; m = m->next) {
        m->window_gap = window_gap;
        for (desktop_t *d = m->desk; d != NULL; d = d->next)
          d->window_gap = window_gap;
      }
      transaction_commit_dirty();
      send_success(client_fd, "window_gap set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", window_gap);
      send_success(client_fd, buf);
    }
  } else if (streq("single_monocle", *args)) {
    if (num >= 2) {
      single_monocle = (strcmp(args[1], "true") == 0);
      transaction_commit_dirty();
      send_success(client_fd, "single_monocle set\n");
    } else {
      send_success(client_fd, single_monocle ? "true\n" : "false\n");
    }
  } else if (streq("borderless_monocle", *args)) {
    if (num >= 2) {
      borderless_monocle = (strcmp(args[1], "true") == 0);
      transaction_commit_dirty();
      send_success(client_fd, "borderless_monocle set\n");
    } else {
      send_success(client_fd, borderless_monocle ? "true\n" : "false\n");
    }
  } else if (streq("borderless_singleton", *args)) {
    if (num >= 2) {
      borderless_singleton = (strcmp(args[1], "true") == 0);
      transaction_commit_dirty();
      send_success(client_fd, "borderless_singleton set\n");
    } else {
      send_success(client_fd, borderless_singleton ? "true\n" : "false\n");
    }
  } else if (streq("gapless_monocle", *args)) {
    if (num >= 2) {
      gapless_monocle = (strcmp(args[1], "true") == 0);
      transaction_commit_dirty();
      send_success(client_fd, "gapless_monocle set\n");
    } else {
      send_success(client_fd, gapless_monocle ? "true\n" : "false\n");
    }
  } else {
    send_failure(client_fd, "config: unknown setting\n");
  }
}

static void ipc_cmd_focus(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "focus: missing direction\n");
    return;
  }

  if (streq("west", *args) || streq("w", *args)) {
    focus_west();
    send_success(client_fd, "focused\n");
  } else if (streq("east", *args) || streq("e", *args)) {
    focus_east();
    send_success(client_fd, "focused\n");
  } else if (streq("north", *args) || streq("n", *args)) {
    focus_north();
    send_success(client_fd, "focused\n");
  } else if (streq("south", *args) || streq("s", *args)) {
    focus_south();
    send_success(client_fd, "focused\n");
  } else {
    send_failure(client_fd, "focus: unknown direction\n");
  }
}

static void ipc_cmd_swap(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "swap: missing direction\n");
    return;
  }

  if (streq("west", *args) || streq("w", *args)) {
    swap_west();
    send_success(client_fd, "swapped\n");
  } else if (streq("east", *args) || streq("e", *args)) {
    swap_east();
    send_success(client_fd, "swapped\n");
  } else if (streq("north", *args) || streq("n", *args)) {
    swap_north();
    send_success(client_fd, "swapped\n");
  } else if (streq("south", *args) || streq("s", *args)) {
    swap_south();
    send_success(client_fd, "swapped\n");
  } else {
    send_failure(client_fd, "swap: unknown direction\n");
  }
}

static void ipc_cmd_presel(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "presel: missing direction\n");
    return;
  }

  if (streq("west", *args) || streq("w", *args)) {
    presel_west();
    send_success(client_fd, "presel set\n");
  } else if (streq("east", *args) || streq("e", *args)) {
    presel_east();
    send_success(client_fd, "presel set\n");
  } else if (streq("north", *args) || streq("n", *args)) {
    presel_north();
    send_success(client_fd, "presel set\n");
  } else if (streq("south", *args) || streq("s", *args)) {
    presel_south();
    send_success(client_fd, "presel set\n");
  } else if (streq("cancel", *args)) {
    cancel_presel();
    send_success(client_fd, "presel cancelled\n");
  } else {
    send_failure(client_fd, "presel: unknown direction\n");
  }
}

static void ipc_cmd_toggle(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "toggle: missing property\n");
    return;
  }

  if (streq("floating", *args)) {
    toggle_floating();
    send_success(client_fd, "toggled\n");
  } else if (streq("fullscreen", *args)) {
    toggle_fullscreen();
    send_success(client_fd, "toggled\n");
  } else if (streq("monocle", *args)) {
    toggle_monocle();
    send_success(client_fd, "toggled\n");
  } else {
    send_failure(client_fd, "toggle: unknown property\n");
  }
}

static void ipc_cmd_rotate(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "rotate: missing direction\n");
    return;
  }

  if (streq("clockwise", *args) || streq("cw", *args)) {
    rotate_clockwise();
    send_success(client_fd, "rotated\n");
  } else if (streq("counterclockwise", *args) || streq("ccw", *args)) {
    rotate_counterclockwise();
    send_success(client_fd, "rotated\n");
  } else {
    send_failure(client_fd, "rotate: unknown direction\n");
  }
}

static void ipc_cmd_flip(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "flip: missing direction\n");
    return;
  }

  if (streq("horizontal", *args) || streq("h", *args)) {
    flip_horizontal();
    send_success(client_fd, "flipped\n");
  } else if (streq("vertical", *args) || streq("v", *args)) {
    flip_vertical();
    send_success(client_fd, "flipped\n");
  } else {
    send_failure(client_fd, "flip: unknown direction\n");
  }
}

static void ipc_cmd_send(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "send: missing direction\n");
    return;
  }

  if (streq("next", *args)) {
    send_to_next_desktop();
    send_success(client_fd, "sent\n");
  } else if (streq("prev", *args) || streq("previous", *args)) {
    send_to_prev_desktop();
    send_success(client_fd, "sent\n");
  } else {
    send_failure(client_fd, "send: unknown direction\n");
  }
}

static void ipc_cmd_rule(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "rule: missing arguments\n");
    return;
  }

  char *subcmd = *args;

  if (streq("-a", subcmd) || streq("--add", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "rule -a: missing app_id\n");
      return;
    }

    args++;
    num--;

    rule_t *r = make_rule();
    if (!r) {
      send_failure(client_fd, "rule: failed to create rule\n");
      return;
    }

    char *app_id = NULL;
    char *title = NULL;
    r->consequence.follow = true;
    r->consequence.has_follow = true;
    r->consequence.focus = true;
    r->consequence.has_focus = true;
    r->consequence.manage = true;
    r->consequence.has_manage = true;

    while (num > 0) {
      char *arg = *args;

      if (arg[0] != '-' && app_id == NULL) {
        app_id = arg;
        strncpy(r->match.app_id, app_id, MAXLEN - 1);
        r->match.app_id[MAXLEN - 1] = '\0';
      } else if (streq("title=", arg) || strncmp(arg, "title=", 6) == 0) {
        title = arg + 6;
        strncpy(r->match.title, title, MAXLEN - 1);
        r->match.title[MAXLEN - 1] = '\0';
      } else if (streq("state=tiled", arg)) {
        r->consequence.state = STATE_TILED;
        r->consequence.has_state = true;
      } else if (streq("state=floating", arg)) {
        r->consequence.state = STATE_FLOATING;
        r->consequence.has_state = true;
      } else if (streq("state=fullscreen", arg)) {
        r->consequence.state = STATE_FULLSCREEN;
        r->consequence.has_state = true;
      } else if (streq("state=pseudo_tiled", arg)) {
        r->consequence.state = STATE_PSEUDO_TILED;
        r->consequence.has_state = true;
      } else if (streq("desktop=^", arg) || (strlen(arg) > 8 && strncmp(arg, "desktop=", 8) == 0)) {
        char *desk = arg + 8;
        strncpy(r->consequence.desktop, desk, SMALEN - 1);
        r->consequence.desktop[SMALEN - 1] = '\0';
        r->consequence.has_desktop = true;
      } else if (streq("follow=on", arg)) {
        r->consequence.follow = true;
        r->consequence.has_follow = true;
      } else if (streq("follow=off", arg)) {
        r->consequence.follow = false;
        r->consequence.has_follow = true;
      } else if (streq("focus=on", arg)) {
        r->consequence.focus = true;
        r->consequence.has_focus = true;
      } else if (streq("focus=off", arg)) {
        r->consequence.focus = false;
        r->consequence.has_focus = true;
      } else if (streq("manage=on", arg)) {
        r->consequence.manage = true;
        r->consequence.has_manage = true;
      } else if (streq("manage=off", arg)) {
        r->consequence.manage = false;
        r->consequence.has_manage = true;
      } else if (streq("locked=on", arg)) {
        r->consequence.locked = true;
        r->consequence.has_locked = true;
      } else if (streq("locked=off", arg)) {
        r->consequence.locked = false;
        r->consequence.has_locked = true;
      } else if (streq("hidden=on", arg)) {
        r->consequence.hidden = true;
        r->consequence.has_hidden = true;
      } else if (streq("hidden=off", arg)) {
        r->consequence.hidden = false;
        r->consequence.has_hidden = true;
      } else if (streq("sticky=on", arg)) {
        r->consequence.sticky = true;
        r->consequence.has_sticky = true;
      } else if (streq("sticky=off", arg)) {
        r->consequence.sticky = false;
        r->consequence.has_sticky = true;
      } else if (streq("one_shot", arg)) {
        r->match.one_shot = true;
      }

      args++;
      num--;
    }

    if (!app_id && !title) {
      free(r);
      send_failure(client_fd, "rule -a: must specify app_id or title\n");
      return;
    }

    add_rule(r);
    send_success(client_fd, "rule added\n");

  } else if (streq("-r", subcmd) || streq("--remove", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "rule -r: missing index\n");
      return;
    }
    args++;
    int idx = atoi(*args);
    if (remove_rule_by_index(idx))
      send_success(client_fd, "rule removed\n");
    else
   		send_failure(client_fd, "rule -r: invalid index\n");

  } else if (streq("-l", subcmd) || streq("--list", subcmd)) {
    char buf[BWM_BUFSIZ];
    list_rules(buf, sizeof(buf));
    send_success(client_fd, buf);
  } else {
    send_failure(client_fd, "rule: unknown subcommand (use -a, -r, or -l)\n");
  }
}

static void process_ipc_message(char *msg, int msg_len, int client_fd) {
  wlr_log(WLR_DEBUG, "IPC: processing message: %.*s", msg_len, msg);
  int cap = 16;
  int num = 0;
  char **args = calloc(cap, sizeof(char *));

  if (!args) {
    send_failure(client_fd, "memory error\n");
    return;
  }

  for (int i = 0, j = 0; i < msg_len; i++) {
    if (num >= cap) {
      cap *= 2;
      char **new = realloc(args, cap * sizeof(char *));
      if (!new) {
        free(args);
        send_failure(client_fd, "memory error\n");
        return;
      }
      args = new;
    }
    if (msg[i] == '\0') {
      args[num++] = msg + j;
      j = i + 1;
    }
  }

  if (num < 1) {
    free(args);
    send_failure(client_fd, "no arguments\n");
    return;
  }

  char **args_orig = args;

  if (streq("node", *args)) {
    ipc_cmd_node(++args, --num, client_fd);
  } else if (streq("desktop", *args)) {
    ipc_cmd_desktop(++args, --num, client_fd);
  } else if (streq("monitor", *args)) {
    ipc_cmd_monitor(++args, --num, client_fd);
  } else if (streq("query", *args)) {
    ipc_cmd_query(++args, --num, client_fd);
  } else if (streq("config", *args)) {
    ipc_cmd_config(++args, --num, client_fd);
  } else if (streq("quit", *args)) {
    ipc_cmd_quit(++args, --num, client_fd);
  } else if (streq("output", *args)) {
    ipc_cmd_output(++args, --num, client_fd);
  } else if (streq("input", *args)) {
    ipc_cmd_input(++args, --num, client_fd);
  } else if (streq("focus", *args)) {
    ipc_cmd_focus(++args, --num, client_fd);
  } else if (streq("swap", *args)) {
    ipc_cmd_swap(++args, --num, client_fd);
  } else if (streq("presel", *args)) {
    ipc_cmd_presel(++args, --num, client_fd);
  } else if (streq("toggle", *args)) {
    ipc_cmd_toggle(++args, --num, client_fd);
  } else if (streq("rotate", *args)) {
    ipc_cmd_rotate(++args, --num, client_fd);
  } else if (streq("flip", *args)) {
    ipc_cmd_flip(++args, --num, client_fd);
  } else if (streq("send", *args)) {
    ipc_cmd_send(++args, --num, client_fd);
  } else if (streq("rule", *args)) {
    ipc_cmd_rule(++args, --num, client_fd);
  } else {
    send_failure(client_fd, "unknown command\n");
  }

  free(args_orig);
}

void ipc_handle_incoming(int client_fd) {
  char msg[BWM_BUFSIZ];
  wlr_log(WLR_DEBUG, "IPC: handling incoming connection");
  ssize_t n = recv(client_fd, msg, sizeof(msg) - 1, 0);
  wlr_log(WLR_DEBUG, "IPC: received %zd bytes", n);

  if (n <= 0) {
    wlr_log(WLR_DEBUG, "IPC: no data received, closing");
    close(client_fd);
    return;
  }

  msg[n] = '\0';
  process_ipc_message(msg, (int)n, client_fd);
  close(client_fd);
}

void ipc_cleanup(void) {
  if (ipc_socket_fd != -1) {
    close(ipc_socket_fd);
    unlink(socket_path);
    ipc_socket_fd = -1;
  }
}
