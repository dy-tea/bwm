#include "ipc.h"
#include "server.h"
#include "types.h"
#include "toplevel.h"
#include "blur.h"
#include "transaction.h"
#include "workspace.h"
#include "tree.h"
#include "output_config.h"
#include "output.h"
#include "input.h"
#include "keyboard.h"
#include "rule.h"
#include "config.h"
#include "scroller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <sys/stat.h>
#include <wlr/util/box.h>
#include <wayland-server-core.h>

static int ipc_socket_fd = -1;
static char socket_path[256];

static bwm_subscriber_t *subscriber_head = NULL;
static bwm_subscriber_t *subscriber_tail = NULL;

static void ipc_cmd_subscribe(char **args, int num, int client_fd);
static monitor_t *find_monitor_by_name(const char *name);
void toplevel_map(struct wl_listener *listener, void *data);

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

  if (streq("list", *args) || streq("--list", *args) || streq("-l", *args)) {
    char buf[BWM_BUFSIZ];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[\n");
    bool first = true;
    struct bwm_output *output;
    wl_list_for_each(output, &server.outputs, link) {
      struct wlr_output *wo = output->wlr_output;
      if (!first)
        offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");
      first = false;
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  {\n"
        "    \"name\": \"%s\",\n"
        "    \"description\": \"%s\",\n"
        "    \"make\": \"%s\",\n"
        "    \"model\": \"%s\",\n"
        "    \"serial\": \"%s\",\n"
        "    \"width\": %d,\n"
        "    \"height\": %d,\n"
        "    \"refresh\": %d,\n"
        "    \"scale\": %.6g,\n"
        "    \"phys_width\": %d,\n"
        "    \"phys_height\": %d,\n"
        "    \"enabled\": %s\n"
        "  }",
        wo->name ? wo->name : "",
        wo->description ? wo->description : "",
        wo->make ? wo->make : "",
        wo->model ? wo->model : "",
        wo->serial ? wo->serial : "",
        wo->width, wo->height, wo->refresh,
        wo->scale,
        wo->phys_width, wo->phys_height,
        wo->enabled ? "true" : "false");
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "\n]\n");
    send_success(client_fd, buf);
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
  } else if (streq("color_profile", subcmd)) {
    if (num < 2) {
      send_failure(client_fd, "output color_profile: missing profile type (gamma22|srgb|icc <path>)\n");
      return;
    }
    args++;
    num--;

    struct wlr_color_transform *new_transform = NULL;
    if (streq("gamma22", *args)) {
    	// no transform
    } else if (streq("srgb", *args)) {
      new_transform = wlr_color_transform_init_linear_to_inverse_eotf(
          WLR_COLOR_TRANSFER_FUNCTION_SRGB);
      if (!new_transform) {
        send_failure(client_fd, "output color_profile: failed to create sRGB transform\n");
        return;
      }
    } else if (streq("icc", *args)) {
      if (num < 2) {
        send_failure(client_fd, "output color_profile icc: missing file path\n");
        return;
      }
      args++;
      num--;

      int fd = open(*args, O_RDONLY | O_NOCTTY | O_CLOEXEC);
      if (fd == -1) {
        send_failure(client_fd, "output color_profile icc: cannot open file\n");
        return;
      }
      struct stat info;
      if (fstat(fd, &info) == -1 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        close(fd);
        send_failure(client_fd, "output color_profile icc: invalid file\n");
        return;
      }
      void *icc_data = malloc(info.st_size);
      if (!icc_data) {
        close(fd);
        send_failure(client_fd, "output color_profile icc: out of memory\n");
        return;
      }
      size_t nread = 0;
      while (nread < (size_t)info.st_size) {
        ssize_t r = read(fd, (char *)icc_data + nread, (size_t)info.st_size - nread);
        if ((r == -1 && errno != EINTR) || r == 0) {
          free(icc_data);
          close(fd);
          send_failure(client_fd, "output color_profile icc: read error\n");
          return;
        }
        nread += (size_t)r;
      }
      close(fd);

      new_transform = wlr_color_transform_init_linear_to_icc(icc_data, (size_t)info.st_size);
      free(icc_data);
      if (!new_transform) {
        send_failure(client_fd, "output color_profile icc: failed to initialize ICC transform\n");
        return;
      }
    } else {
      send_failure(client_fd, "output color_profile: invalid type (gamma22|srgb|icc <path>)\n");
      return;
    }

    wlr_color_transform_unref(oc->color_transform);
    oc->color_transform = new_transform;
    output_config_apply(oc);
    send_success(client_fd, "output color_profile set\n");
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
    struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
    if (scene_tree)
      wlr_scene_node_set_enabled(&scene_tree->node, false);

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
          // Only directly enable windows that have already been configured
          // (positioned by a prior transaction). New unconfigured tiled windows
          // sit at (0,0); enabling them here causes a top-left flash before
          // arrange()/apply_node_state() positions and enables them atomically.
          bool already_configured = true;
          if (n_iter->client->toplevel)
            already_configured = n_iter->client->toplevel->configured;
          if (already_configured) {
            struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
            if (scene_tree)
              wlr_scene_node_set_enabled(&scene_tree->node, true);
          }
        }
      }
      arrange(m, target, true);
    } else {
      for (node_t *n_iter = first_extrema(target->root); n_iter != NULL; n_iter = next_leaf(n_iter, target->root)) {
        if (n_iter->client) {
          n_iter->client->shown = false;
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, false);
        }
      }
      arrange(m, target, false);
    }

    if (src_desk == m->desk) {
      for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, src_desk->root)) {
        if (n_iter->client) {
          n_iter->client->shown = true;
          bool already_configured = true;
          if (n_iter->client->toplevel)
            already_configured = n_iter->client->toplevel->configured;
          if (already_configured) {
            struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
            if (scene_tree)
              wlr_scene_node_set_enabled(&scene_tree->node, true);
          }
        }
      }
      arrange(m, src_desk, true);
    } else if (src_desk->root) {
      for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, src_desk->root)) {
        if (n_iter->client) {
          n_iter->client->shown = false;
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, false);
        }
      }
    }

    send_success(client_fd, "node sent to desktop\n");
  } else if (streq("-g", *args) || streq("--flag", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -g: missing flag argument\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -g: no focused desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -g: no focused node\n");
      return;
    }

    char *key = strtok(*args, "=");
    char *val = strtok(NULL, "=");

    bool set_value;
    bool has_value = false;

    if (val == NULL) {
      has_value = false;
    } else {
      has_value = true;
      if (strcmp(val, "true") == 0 || strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
        set_value = true;
      } else if (strcmp(val, "false") == 0 || strcmp(val, "off") == 0 || strcmp(val, "0") == 0) {
        set_value = false;
      } else {
        send_failure(client_fd, "node -g: invalid value\n");
        return;
      }
    }

    if (strcmp(key, "hidden") == 0) {
      n->hidden = has_value ? set_value : !n->hidden;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "sticky") == 0) {
      n->sticky = has_value ? set_value : !n->sticky;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "private") == 0) {
      n->private_node = has_value ? set_value : !n->private_node;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "locked") == 0) {
      n->locked = has_value ? set_value : !n->locked;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "marked") == 0) {
      n->marked = has_value ? set_value : !n->marked;
      transaction_commit_dirty();
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "blur") == 0) {
      if (!n->client) {
        send_failure(client_fd, "node -g: no client\n");
        return;
      }
      bool new_blur = has_value ? set_value : !n->client->blur;
      n->client->blur = new_blur;
      if (n->client->toplevel)
        toplevel_set_blur(n->client->toplevel, new_blur);
      send_success(client_fd, "flag changed\n");
    } else if (strcmp(key, "mica") == 0) {
      if (!n->client) {
        send_failure(client_fd, "node -g: no client\n");
        return;
      }
      bool new_val = has_value ? set_value : !n->client->mica;
      n->client->mica = new_val;
      if (n->client->toplevel)
        toplevel_set_mica(n->client->toplevel, new_val);
      send_success(client_fd, "flag changed\n");
    } else {
      send_failure(client_fd, "node -g: unknown flag\n");
      return;
    }
  } else if (streq("-v", *args) || streq("--move", *args)) {
    if (num < 3) {
      send_failure(client_fd, "node -v: missing delta arguments\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -v: no focused desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -v: no focused client\n");
      return;
    }

    int dx = 0, dy = 0;
    if (sscanf(*args, "%d", &dx) != 1) {
      send_failure(client_fd, "node -v: invalid dx\n");
      return;
    }
    args++;
    num--;

    if (sscanf(*args, "%d", &dy) != 1) {
      send_failure(client_fd, "node -v: invalid dy\n");
      return;
    }

    n->client->state = STATE_FLOATING;
    n->client->floating_rectangle.x += dx;
    n->client->floating_rectangle.y += dy;

    if (n->client->toplevel && n->client->toplevel->scene_tree) {
      wlr_scene_node_set_position(&n->client->toplevel->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
    } else if (n->client->xwayland_view && n->client->xwayland_view->scene_tree) {
      wlr_scene_node_set_position(&n->client->xwayland_view->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
    }

    transaction_commit_dirty();
    send_success(client_fd, "moved\n");
  } else if (streq("-z", *args) || streq("--resize", *args)) {
    if (num < 4) {
      send_failure(client_fd, "node -z: missing arguments\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -z: no focused desktop\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -z: no focused client\n");
      return;
    }

    char *handle = *args;
    int dx = 0, dy = 0;

    args++;
    num--;

    if (sscanf(*args, "%d", &dx) != 1) {
      send_failure(client_fd, "node -z: invalid dx\n");
      return;
    }
    args++;
    num--;

    if (sscanf(*args, "%d", &dy) != 1) {
      send_failure(client_fd, "node -z: invalid dy\n");
      return;
    }

    n->client->state = STATE_FLOATING;

    if (strcmp(handle, "northwest") == 0 || strcmp(handle, "nw") == 0 || strcmp(handle, "left") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.width -= dx;
      n->client->floating_rectangle.height -= dy;
    } else if (strcmp(handle, "north") == 0 || strcmp(handle, "n") == 0) {
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.height -= dy;
    } else if (strcmp(handle, "northeast") == 0 || strcmp(handle, "ne") == 0) {
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.width += dx;
      n->client->floating_rectangle.height -= dy;
    } else if (strcmp(handle, "east") == 0 || strcmp(handle, "e") == 0 || strcmp(handle, "right") == 0) {
      n->client->floating_rectangle.width += dx;
    } else if (strcmp(handle, "southeast") == 0 || strcmp(handle, "se") == 0) {
      n->client->floating_rectangle.width += dx;
      n->client->floating_rectangle.height += dy;
    } else if (strcmp(handle, "south") == 0 || strcmp(handle, "s") == 0) {
      n->client->floating_rectangle.height += dy;
    } else if (strcmp(handle, "southwest") == 0 || strcmp(handle, "sw") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.width -= dx;
      n->client->floating_rectangle.height += dy;
    } else if (strcmp(handle, "west") == 0 || strcmp(handle, "w") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.width -= dx;
    } else if (strcmp(handle, "center") == 0 || strcmp(handle, "c") == 0) {
      n->client->floating_rectangle.x += dx;
      n->client->floating_rectangle.y += dy;
      n->client->floating_rectangle.width += dx;
      n->client->floating_rectangle.height += dy;
    } else {
      send_failure(client_fd, "node -z: invalid resize handle\n");
      return;
    }

    if (n->client->floating_rectangle.width < 50)
      n->client->floating_rectangle.width = 50;
    if (n->client->floating_rectangle.height < 50)
      n->client->floating_rectangle.height = 50;

    if (n->client->toplevel && n->client->toplevel->scene_tree) {
      wlr_scene_node_set_position(&n->client->toplevel->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
      wlr_xdg_toplevel_set_size(n->client->toplevel->xdg_toplevel,
        n->client->floating_rectangle.width, n->client->floating_rectangle.height);
    } else if (n->client->xwayland_view && n->client->xwayland_view->scene_tree) {
      wlr_scene_node_set_position(&n->client->xwayland_view->scene_tree->node,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y);
      wlr_xwayland_surface_configure(n->client->xwayland_view->xwayland_surface,
        n->client->floating_rectangle.x, n->client->floating_rectangle.y,
        n->client->floating_rectangle.width, n->client->floating_rectangle.height);
    }

    transaction_commit_dirty();
    send_success(client_fd, "resized\n");
  } else if (streq("-a", *args) || streq("--activate", *args)) {
    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -a: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -a: no focused node\n");
      return;
    }
    activate_node(m, m->desk, n);
    send_success(client_fd, "activated\n");
  } else if (streq("-k", *args) || streq("--kill", *args)) {
    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -k: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -k: no focused node\n");
      return;
    }
    kill_node(m, m->desk, n);
    transaction_commit_dirty();
    send_success(client_fd, "killed\n");
  } else if (streq("-m", *args) || streq("--to-monitor", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -m: missing monitor name\n");
      return;
    }
    args++;
    num--;

    monitor_t *target = find_monitor_by_name(*args);
    if (!target) {
      send_failure(client_fd, "node -m: monitor not found\n");
      return;
    }

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -m: no focused desktop\n");
      return;
    }

    if (m == target) {
      send_failure(client_fd, "node -m: already on target monitor\n");
      return;
    }

    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -m: no client\n");
      return;
    }

    desktop_t *src_desk = m->desk;
    desktop_t *target_desk = target->desk ? target->desk : target->desk_head;

    n->destroying = false;
    n->ntxnrefs = 0;
    n->client->shown = false;
    struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
    if (scene_tree)
      wlr_scene_node_set_enabled(&scene_tree->node, false);

    remove_node(m, src_desk, n);

    if (src_desk->root) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus) {
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

    insert_node(target, target_desk, n, find_public(target_desk));
    target_desk->focus = n;

    for (node_t *n_iter = first_extrema(target_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, target_desk->root)) {
      if (n_iter->client) {
        n_iter->client->shown = true;
        bool already_configured = true;
        if (n_iter->client->toplevel)
          already_configured = n_iter->client->toplevel->configured;
        if (already_configured) {
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, true);
        }
      }
    }

    arrange(target, target_desk, true);
    arrange(m, src_desk, src_desk->root != NULL);

    send_success(client_fd, "node sent to monitor\n");
  } else if (streq("-n", *args) || streq("--to-node", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -n: missing target node\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -n: no focused desktop\n");
      return;
    }

    node_t *n1 = m->desk->focus;
    if (!n1 || !n1->client) {
      send_failure(client_fd, "node -n: no focused client\n");
      return;
    }

    node_t *n2 = NULL;
    int target_id = atoi(*args);
    if (target_id > 0) {
      for (node_t *n = first_extrema(m->desk->root); n != NULL; n = next_leaf(n, m->desk->root)) {
        if (n->id == (uint32_t)target_id) {
          n2 = n;
          break;
        }
      }
    }

    if (!n2) {
      send_failure(client_fd, "node -n: target node not found\n");
      return;
    }

    if (n1 == n2) {
      send_failure(client_fd, "node -n: cannot transfer to self\n");
      return;
    }

    desktop_t *src_desk = m->desk;
    desktop_t *target_desk = src_desk;

    n1->destroying = false;
    n1->ntxnrefs = 0;
    n1->client->shown = false;
    struct wlr_scene_tree *scene_tree = client_get_scene_tree(n1->client);
    if (scene_tree)
      wlr_scene_node_set_enabled(&scene_tree->node, false);

    remove_node(m, src_desk, n1);

    if (src_desk->root) {
      node_t *new_focus = first_extrema(src_desk->root);
      if (new_focus) {
        src_desk->focus = new_focus;
        focus_node(m, src_desk, new_focus);
      } else {
        src_desk->focus = NULL;
      }
    } else {
      src_desk->focus = NULL;
    }

    n1->destroying = false;
    n1->ntxnrefs = 0;

    if (n2->first_child) {
      n1->parent = n2;
      n2->second_child = n1;
    } else {
      n1->parent = n2;
      n2->first_child = n1;
    }

    target_desk->focus = n1;
    if (target_desk == m->desk)
      focus_node(m, target_desk, n1);

    for (node_t *n_iter = first_extrema(target_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter, target_desk->root)) {
      if (n_iter->client) {
        n_iter->client->shown = true;
        bool already_configured = true;
        if (n_iter->client->toplevel)
          already_configured = n_iter->client->toplevel->configured;
        if (already_configured) {
          struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
          if (scene_tree)
            wlr_scene_node_set_enabled(&scene_tree->node, true);
        }
      }
    }

    arrange(m, target_desk, true);
    if (src_desk != target_desk)
      arrange(m, src_desk, src_desk->root != NULL);

    send_success(client_fd, "node sent to node\n");
  } else if (streq("-l", *args) || streq("--layer", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -l: missing layer argument\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -l: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || !n->client) {
      send_failure(client_fd, "node -l: no client\n");
      return;
    }

    stack_layer_t layer;
    if (streq("below", *args)) {
      layer = LAYER_BELOW;
    } else if (streq("normal", *args)) {
      layer = LAYER_NORMAL;
    } else if (streq("above", *args)) {
      layer = LAYER_ABOVE;
    } else {
      send_failure(client_fd, "node -l: unknown layer (use below, normal, or above)\n");
      return;
    }

    n->client->layer = layer;
    transaction_commit_dirty();
    send_success(client_fd, "layer changed\n");
  } else if (streq("-y", *args) || streq("--type", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -y: missing type argument\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -y: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -y: no focused node\n");
      return;
    }

    if (streq("horizontal", *args) || streq("horizontal", *args)) {
      n->split_type = TYPE_HORIZONTAL;
    } else if (streq("vertical", *args) || streq("vertical", *args)) {
      n->split_type = TYPE_VERTICAL;
    } else {
      n->split_type = (n->split_type + 1) % 2;
    }

    transaction_commit_dirty();
    send_success(client_fd, "type changed\n");
  } else if (streq("-r", *args) || streq("--ratio", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -r: missing ratio argument\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -r: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -r: no focused node\n");
      return;
    }

    double rat;
    if ((*args)[0] == '+' || (*args)[0] == '-') {
      float delta;
      if (sscanf(*args, "%f", &delta) == 1) {
        if (delta > -1 && delta < 1) {
          rat = n->split_ratio + delta;
        } else {
          int max = (n->split_type == TYPE_HORIZONTAL) ? n->rectangle.height : n->rectangle.width;
          rat = ((max * n->split_ratio) + delta) / max;
        }
      } else {
        send_failure(client_fd, "node -r: invalid argument\n");
        return;
      }
    } else {
      if (sscanf(*args, "%lf", &rat) != 1) {
        send_failure(client_fd, "node -r: invalid argument\n");
        return;
      }
    }

    if (rat > 0 && rat < 1) {
      n->split_ratio = rat;
      transaction_commit_dirty();
      send_success(client_fd, "ratio changed\n");
    } else {
      send_failure(client_fd, "node -r: ratio out of range\n");
      return;
    }
  } else if (streq("-C", *args) || streq("--circulate", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -C: missing direction\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -C: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n) {
      send_failure(client_fd, "node -C: no focused node\n");
      return;
    }

    if (streq("forward", *args) || streq("f", *args)) {
      node_t *next = next_leaf(n, m->desk->root);
      if (next) {
        m->desk->focus = next;
        focus_node(m, m->desk, next);
      }
    } else if (streq("backward", *args) || streq("b", *args)) {
      node_t *prev = prev_leaf(n, m->desk->root);
      if (prev) {
        m->desk->focus = prev;
        focus_node(m, m->desk, prev);
      }
    } else {
      send_failure(client_fd, "node -C: unknown direction\n");
      return;
    }

    send_success(client_fd, "circulated\n");
  } else if (streq("-i", *args) || streq("--insert-receptacle", *args)) {
    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -i: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;

    node_t *receptacle = make_node(0);
    receptacle->vacant = true;
    receptacle->split_type = TYPE_VERTICAL;
    receptacle->split_ratio = 0.5;

    if (n && !is_leaf(n)) {
      if (n->first_child) {
        receptacle->parent = n;
        n->second_child->parent = receptacle;
        receptacle->first_child = n->second_child;
        n->second_child = receptacle;
      } else {
        n->first_child = receptacle;
        receptacle->parent = n;
      }
    } else if (n) {
      node_t *parent = n->parent;
      if (parent) {
        if (parent->first_child == n) {
          parent->first_child = receptacle;
        } else {
          parent->second_child = receptacle;
        }
        receptacle->parent = parent;

        if (n->split_type == TYPE_VERTICAL) {
          receptacle->first_child = n;
          n->parent = receptacle;
        } else {
          receptacle->second_child = n;
          n->parent = receptacle;
        }
      } else {
        m->desk->root = receptacle;
        receptacle->first_child = n;
        n->parent = receptacle;
      }
      receptacle->split_type = TYPE_VERTICAL;
    } else {
      m->desk->root = receptacle;
    }

    transaction_commit_dirty();
    send_success(client_fd, "receptacle inserted\n");
  } else if (streq("-p", *args) || streq("--presel-dir", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -p: missing direction\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -p: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || n->vacant) {
      send_failure(client_fd, "node -p: no valid node\n");
      return;
    }

    if (streq("cancel", *args)) {
      if (n->presel) {
        free(n->presel);
        n->presel = NULL;
      }
      send_success(client_fd, "presel cancelled\n");
      return;
    }

    direction_t dir;
    if (streq("west", *args) || streq("w", *args)) {
      dir = DIR_WEST;
    } else if (streq("east", *args) || streq("e", *args)) {
      dir = DIR_EAST;
    } else if (streq("north", *args) || streq("n", *args)) {
      dir = DIR_NORTH;
    } else if (streq("south", *args) || streq("s", *args)) {
      dir = DIR_SOUTH;
    } else {
      send_failure(client_fd, "node -p: unknown direction\n");
      return;
    }

    presel_dir(m, m->desk, n, dir);
    transaction_commit_dirty();
    send_success(client_fd, "presel set\n");
  } else if (streq("-o", *args) || streq("--presel-ratio", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -o: missing ratio\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -o: no focused desktop\n");
      return;
    }
    node_t *n = m->desk->focus;
    if (!n || n->vacant) {
      send_failure(client_fd, "node -o: no valid node\n");
      return;
    }

    double rat;
    if (sscanf(*args, "%lf", &rat) != 1 || rat <= 0 || rat >= 1) {
      send_failure(client_fd, "node -o: invalid ratio\n");
      return;
    }

    if (!n->presel) {
      n->presel = make_presel();
    }
    n->presel->split_ratio = rat;

    transaction_commit_dirty();
    send_success(client_fd, "presel ratio set\n");
  } else if (streq("-s", *args) || streq("--swap", *args)) {
    if (num < 2) {
      send_failure(client_fd, "node -s: missing target node\n");
      return;
    }
    args++;
    num--;

    monitor_t *m = server.focused_monitor;
    if (!m || !m->desk) {
      send_failure(client_fd, "node -s: no focused desktop\n");
      return;
    }

    node_t *n1 = m->desk->focus;
    if (!n1) {
      send_failure(client_fd, "node -s: no focused node\n");
      return;
    }

    node_t *n2 = NULL;
    int target_id = atoi(*args);
    if (target_id > 0) {
      for (node_t *n = first_extrema(m->desk->root); n != NULL; n = next_leaf(n, m->desk->root)) {
        if (n->id == (uint32_t)target_id) {
          n2 = n;
          break;
        }
      }
    }

    if (!n2) {
      send_failure(client_fd, "node -s: target node not found\n");
      return;
    }

    if (n1 == n2) {
      send_failure(client_fd, "node -s: cannot swap with self\n");
      return;
    }

    swap_nodes(m, m->desk, n1, m, m->desk, n2);
    m->desk->focus = n2;
    transaction_commit_dirty();
    send_success(client_fd, "swapped\n");
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
    args++;
    num--;
    if (num >= 1 && (streq("next", *args) || streq("next.local", *args))) {
      focus_next_desktop();
      send_success(client_fd, "focused\n");
    } else if (num >= 1 && (streq("prev", *args) || streq("prev.local", *args) || streq("previous", *args))) {
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
    } else if (streq("scroller", *args)) {
      desk->layout = LAYOUT_SCROLLER;
    } else {
      send_failure(client_fd, "desktop -l: unknown layout\n");
      return;
    }
    if (desk->root != NULL) {
      arrange(mon, desk, true);
      if (desk->focus != NULL)
        focus_node(mon, desk, desk->focus);
    } else {
      transaction_commit_dirty();
    }
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
  } else if (streq("-s", *args) || streq("--swap", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -s: missing target desktop\n");
      return;
    }
    args++;
    num--;

    desktop_t *target = find_desktop_by_name_in_monitor(mon, *args);
    if (!target) {
      send_failure(client_fd, "desktop -s: target desktop not found\n");
      return;
    }

    if (target == desk) {
      send_failure(client_fd, "desktop -s: cannot swap with self\n");
      return;
    }

    desktop_t *d0 = desk;
    desktop_t *d1 = target;
    monitor_t *m0 = d0->monitor;
    monitor_t *m1 = d1->monitor;

    if (m0 == m1) {
      desktop_t *prev0 = d0->prev;
      desktop_t *next0 = d0->next;
      desktop_t *prev1 = d1->prev;
      desktop_t *next1 = d1->next;

      if (next0 == d1) {
        d0->prev = d1;
        d0->next = next1;
        d1->prev = prev0;
        d1->next = d0;
        if (prev0) prev0->next = d1;
        if (next1) next1->next = d0;
      } else {
        d0->prev = prev1;
        d0->next = next1;
        d1->prev = prev0;
        d1->next = next0;
        if (prev0) prev0->next = d1;
        if (next0) next0->next = d1;
        if (prev1) prev1->next = d0;
        if (next1) next1->next = d0;
      }

      if (m0->desk == d0) m0->desk = d1;
      else if (m0->desk == d1) m0->desk = d0;
    } else {
      desktop_t *prev0 = d0->prev;
      desktop_t *next0 = d0->next;
      desktop_t *prev1 = d1->prev;
      desktop_t *next1 = d1->next;

      d0->prev = prev1;
      d0->next = next1;
      d1->prev = prev0;
      d1->next = next0;

      if (prev0) prev0->next = d1; else m0->desk = d1;
      if (next0) next0->next = d1;
      if (prev1) prev1->next = d0; else m1->desk = d0;
      if (next1) next1->next = d0;

      d0->monitor = m1;
      d1->monitor = m0;
    }

    if (mon->desk == d0) mon->desk = d1;
    else if (mon->desk == d1) mon->desk = d0;

    transaction_commit_dirty();
    send_success(client_fd, "swapped\n");
  } else if (streq("-r", *args) || streq("--remove", *args)) {
    if (!desk->prev && !desk->next) {
      send_failure(client_fd, "desktop -r: cannot remove the only desktop\n");
      return;
    }

    desktop_t *prev = desk->prev;
    desktop_t *next = desk->next;

    if (prev)
      prev->next = next;
    else if (mon->desk)
      mon->desk = next;

    if (next)
      next->prev = prev;

    if (mon->desk == desk) {
      mon->desk = next ? next : prev;
      if (mon->desk)
        focus_node(mon, mon->desk, mon->desk->focus);
    }

    free(desk);
    transaction_commit_dirty();
    send_success(client_fd, "removed\n");
  } else if (streq("-b", *args) || streq("--bubble", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -b: missing direction\n");
      return;
    }
    args++;
    num--;

    if (streq("up", *args) || streq("prev", *args)) {
      if (desk->prev) {
        desktop_t *prev = desk->prev;
        desktop_t *prev_prev = prev->prev;

        desk->prev = prev_prev;
        desk->next = prev;
        prev->prev = desk;
        prev->next = desk;

        if (prev_prev)
          prev_prev->next = desk;
        else
          mon->desk = desk;
      }
    } else if (streq("down", *args) || streq("next", *args)) {
      if (desk->next) {
        desktop_t *next = desk->next;
        desktop_t *next_next = next->next;

        desk->prev = next;
        desk->next = next_next;
        next->prev = desk;
        next->next = desk;

        if (next_next)
          next_next->prev = desk;
      }
    } else {
      send_failure(client_fd, "desktop -b: unknown direction\n");
      return;
    }

    transaction_commit_dirty();
    send_success(client_fd, "bubbled\n");
  } else if (streq("-m", *args) || streq("--to-monitor", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -m: missing monitor name\n");
      return;
    }
    args++;
    num--;

    desktop_t *desk = mon->desk;
    monitor_t *target = find_monitor_by_name(*args);
    if (!target) {
      send_failure(client_fd, "desktop -m: monitor not found\n");
      return;
    }

    if (desk->monitor == target) {
      send_failure(client_fd, "desktop -m: already on target monitor\n");
      return;
    }

    if (!target->desk && !target->desk_head) {
      send_failure(client_fd, "desktop -m: target monitor has no desktop\n");
      return;
    }

    monitor_t *src_mon = desk->monitor;

    if (desk->prev) {
      desk->prev->next = desk->next;
    } else {
      src_mon->desk = desk->next;
      if (src_mon->desk_head == desk) src_mon->desk_head = desk->next;
    }

    if (desk->next) {
      desk->next->prev = desk->prev;
    } else {
      if (src_mon->desk_tail == desk) src_mon->desk_tail = desk->prev;
    }

    desk->prev = target->desk_tail;
    desk->next = NULL;
    desk->monitor = target;

    if (target->desk_tail) {
      target->desk_tail->next = desk;
      target->desk_tail = desk;
    } else {
      target->desk = desk;
      target->desk_head = desk;
      target->desk_tail = desk;
    }

    if (src_mon->desk == desk) {
      src_mon->desk = src_mon->desk_head;
      if (src_mon->desk)
        focus_node(src_mon, src_mon->desk, src_mon->desk->focus);
    }

    transaction_commit_dirty();
    send_success(client_fd, "desktop moved to monitor\n");
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
  } else if (streq("-s", *args) || streq("--swap", *args)) {
    if (!has_target) {
      send_failure(client_fd, "monitor -s: no source monitor specified\n");
      return;
    }
    if (num < 2) {
      send_failure(client_fd, "monitor -s: missing target monitor\n");
      return;
    }
    args++;
    num--;

    monitor_t *target = find_monitor_by_name(*args);
    if (!target) {
      send_failure(client_fd, "monitor -s: target monitor not found\n");
      return;
    }

    if (target == mon) {
      send_failure(client_fd, "monitor -s: cannot swap with self\n");
      return;
    }

    monitor_t *m0 = mon;
    monitor_t *m1 = target;

    desktop_t *d0 = m0->desk;
    desktop_t *d1 = m1->desk;

    m0->desk = d1;
    m0->desk_head = d1 ? d1 : m0->desk_head;
    m0->desk_tail = d1 ? (d1->prev ? d1->prev : d1) : m0->desk_tail;

    m1->desk = d0;
    m1->desk_head = d0 ? d0 : m1->desk_head;
    m1->desk_tail = d0 ? (d0->prev ? d0->prev : d0) : m1->desk_tail;

    if (d0) {
      for (desktop_t *d = d0; d != NULL; d = d->next)
        d->monitor = m1;
      d0->prev = NULL;
      desktop_t *tail = d0;
      while (tail->next)
        tail = tail->next;
      tail->next = d1;
      if (d1) d1->prev = tail;
    }

    if (d1)
      for (desktop_t *d = d1; d != NULL; d = d->next)
        d->monitor = m0;

    if (server.focused_monitor == m0)
      server.focused_monitor = m1;
    else if (server.focused_monitor == m1)
      server.focused_monitor = m0;

    transaction_commit_dirty();
    send_success(client_fd, "swapped\n");
  } else if (streq("-r", *args) || streq("--remove", *args)) {
    if (!has_target) {
      send_failure(client_fd, "monitor -r: no monitor specified\n");
      return;
    }

    if (!mon->prev && !mon->next) {
      send_failure(client_fd, "monitor -r: cannot remove the only monitor\n");
      return;
    }

    if (mon->desk) {
      send_failure(client_fd, "monitor -r: cannot remove monitor with desktops\n");
      return;
    }

    monitor_t *prev = mon->prev;
    monitor_t *next = mon->next;

    if (prev)
      prev->next = next;
    else if (server.monitors == mon)
      server.monitors = next;

    if (next)
      next->prev = prev;

    if (server.focused_monitor == mon) {
      server.focused_monitor = next ? next : prev;
      if (server.focused_monitor) {
        focus_node(server.focused_monitor, server.focused_monitor->desk,
          server.focused_monitor->desk ? server.focused_monitor->desk->focus : NULL);
      }
    }

    free(mon);
    transaction_commit_dirty();
    send_success(client_fd, "removed\n");
  } else if (streq("-g", *args) || streq("--rectangle", *args)) {
    if (!has_target) {
      send_failure(client_fd, "monitor -g: no monitor specified\n");
      return;
    }
    if (num < 2) {
      send_failure(client_fd, "monitor -g: missing rectangle\n");
      return;
    }
    args++;
    num--;

    int x, y, width, height;
    if (sscanf(*args, "%dx%d:%d,%d", &width, &height, &x, &y) != 4 &&
        sscanf(*args, "%dx%d", &width, &height) == 2) {
      x = mon->rectangle.x;
      y = mon->rectangle.y;
    } else if (sscanf(*args, "%d,%d,%d,%d", &x, &y, &width, &height) != 4) {
      send_failure(client_fd, "monitor -g: invalid rectangle format\n");
      return;
    }

    mon->rectangle.x = x;
    mon->rectangle.y = y;
    mon->rectangle.width = width;
    mon->rectangle.height = height;

    transaction_commit_dirty();
    send_success(client_fd, "rectangle set\n");
  } else if (streq("-o", *args) || streq("--reorder-desktops", *args)) {
    if (!has_target) {
      send_failure(client_fd, "monitor -o: no monitor specified\n");
      return;
    }
    if (num < 2) {
      send_failure(client_fd, "monitor -o: missing desktop names\n");
      return;
    }
    args++;
    num--;

    desktop_t *d = mon->desk;
    while (d != NULL && num > 0) {
      desktop_t *next = d->next;
      for (int i = 0; i < num; i++) {
        if (strcmp(d->name, args[i]) == 0) {
          strncpy(d->name, args[i], SMALEN - 1);
          d->name[SMALEN - 1] = '\0';
          break;
        }
      }
      d = next;
    }

    transaction_commit_dirty();
    send_success(client_fd, "desktops reordered\n");
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

  // Parse optional selectors
  monitor_t *filter_mon = NULL;
  desktop_t *filter_desk = NULL;
  node_t *filter_node = NULL;
  bool use_names = false;

  while (num > 0 && (streq("-m", *args) || streq("--monitor", *args) ||
                     streq("-d", *args) || streq("--desktop", *args) ||
                     streq("-n", *args) || streq("--node", *args) ||
                     streq("--names", *args))) {
    if (streq("-m", *args) || streq("--monitor", *args)) {
      if (num < 2) {
        send_failure(client_fd, "query -m: missing monitor\n");
        return;
      }
      args++;
      num--;
      filter_mon = find_monitor_by_name(*args);
      if (!filter_mon) {
        send_failure(client_fd, "query -m: monitor not found\n");
        return;
      }
    } else if (streq("-d", *args) || streq("--desktop", *args)) {
      if (num < 2) {
        send_failure(client_fd, "query -d: missing desktop\n");
        return;
      }
      args++;
      num--;
      filter_desk = find_desktop_by_name(*args);
      if (!filter_desk) {
        send_failure(client_fd, "query -d: desktop not found\n");
        return;
      }
    } else if (streq("-n", *args) || streq("--node", *args)) {
      if (num < 2) {
        send_failure(client_fd, "query -n: missing node\n");
        return;
      }
      args++;
      num--;
      int node_id = atoi(*args);
      if (node_id <= 0) {
        send_failure(client_fd, "query -n: invalid node id\n");
        return;
      }
      struct bwm_toplevel *toplevel;
      wl_list_for_each(toplevel, &server.toplevels, link) {
        if (toplevel->node && toplevel->node->id == (uint32_t)node_id) {
          filter_node = toplevel->node;
          break;
        }
      }
      if (!filter_node) {
        send_failure(client_fd, "query -n: node not found\n");
        return;
      }
    } else if (streq("--names", *args)) {
      use_names = true;
    }
    args++;
    num--;
  }

  if (num < 1) {
    send_failure(client_fd, "query: missing query type\n");
    return;
  }

  if (streq("-T", *args) || streq("--tree", *args)) {
    offset += snprintf(buf + offset, sizeof(buf) - offset, "{\n");

    monitor_t *m_start = filter_mon ? filter_mon : server.monitors;
    monitor_t *m_end = filter_mon ? filter_mon->next : NULL;

    for (monitor_t *m = m_start; m != m_end; ) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  \"monitor\": {\"name\": \"%s\", \"id\": %u},\n",
        m->name, m->id);

      desktop_t *d_start = filter_desk ? filter_desk : m->desk;
      desktop_t *d_end = filter_desk ? filter_desk->next : NULL;

      for (desktop_t *d = d_start; d != d_end; ) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "  \"desktop\": {\"name\": \"%s\", \"id\": %u, \"layout\": %d},\n",
          d->name, d->id, d->layout);
        if (filter_desk) break;
        d = d->next;
      }

      if (filter_mon) break;
      m = m->next;
    }

    struct bwm_toplevel *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link) {
      bool include = true;
      if (filter_node && toplevel->node != filter_node)
        include = false;
      if (filter_desk && toplevel->node && toplevel->node->monitor &&
          toplevel->node->monitor->desk != filter_desk)
        include = false;
      if (filter_mon && toplevel->node && toplevel->node->monitor != filter_mon)
        include = false;

      if (include)
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "  \"toplevel\": {\"app_id\": \"%s\", \"title\": \"%s\", \"identifier\": \"%s\"}\n",
          toplevel->node && toplevel->node->client ? toplevel->node->client->app_id : "?",
          toplevel->node && toplevel->node->client ? toplevel->node->client->title : "?",
          toplevel->foreign_identifier ? toplevel->foreign_identifier : "?");
    }

    offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");
    send_success(client_fd, buf);
  } else if (streq("-M", *args) || streq("--monitors", *args)) {
    for (monitor_t *m = filter_mon ? filter_mon : server.monitors;
         m != NULL; m = filter_mon ? NULL : m->next) {
      if (use_names)
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", m->name);
      else
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n", m->id, m->name);
      if (filter_mon) break;
    }
    send_success(client_fd, buf);
  } else if (streq("-D", *args) || streq("--desktops", *args)) {
    monitor_t *m_start = filter_mon ? filter_mon : server.monitors;
    for (monitor_t *m = m_start; m != NULL; m = filter_mon ? NULL : m->next) {
      desktop_t *d_start = filter_desk ? filter_desk : m->desk;
      for (desktop_t *d = d_start; d != NULL; d = filter_desk ? NULL : d->next) {
        if (use_names)
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", d->name);
        else
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n", d->id, d->name);
        if (filter_desk) break;
      }
      if (filter_mon) break;
    }
    send_success(client_fd, buf);
  } else if (streq("-N", *args) || streq("--nodes", *args)) {
    struct bwm_toplevel *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link) {
      bool include = true;
      if (filter_node && toplevel->node != filter_node)
        include = false;
      if (filter_desk && toplevel->node && toplevel->node->monitor &&
          toplevel->node->monitor->desk != filter_desk)
        include = false;
      if (filter_mon && toplevel->node && toplevel->node->monitor != filter_mon)
        include = false;

      if (include) {
        if (use_names) {
          const char *name = "?";
          if (toplevel->node && toplevel->node->client && toplevel->node->client->title[0])
            name = toplevel->node->client->title;
          else if (toplevel->node && toplevel->node->client && toplevel->node->client->app_id[0])
            name = toplevel->node->client->app_id;
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", name);
        } else {
          offset += snprintf(buf + offset, sizeof(buf) - offset, "%u %s\n",
            toplevel->node ? toplevel->node->id : 0,
            toplevel->foreign_identifier ? toplevel->foreign_identifier : "?");
        }
      }
    }
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

    if (use_names) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "{\"monitor\": \"%s\", \"desktop\": \"%s\", \"node\": \"%s\", \"type\": %d, "
        "\"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}, "
        "\"client\": \"%s\", \"identifier\": \"%s\"}\n",
        m->name,
        m->desk->name,
        n->client && n->client->title[0] ? n->client->title :
        (n->client && n->client->app_id[0] ? n->client->app_id : "?"),
        n->split_type,
        n->rectangle.x,
        n->rectangle.y,
        n->rectangle.width,
        n->rectangle.height,
        n->client && n->client->app_id[0] ? n->client->app_id : "?",
        foreign_id);
    } else {
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
    }
    send_success(client_fd, buf);
  } else {
    send_failure(client_fd, "query: unknown command\n");
  }
}

static void ipc_cmd_wm(char **args, int num, int client_fd) {
  char buf[BWM_BUFSIZ];
  size_t offset = 0;

  if (num < 1) {
    send_failure(client_fd, "wm: missing command\n");
    return;
  }

  if (streq("-d", *args) || streq("--dump-state", *args)) {
    // dump current state as JSON
    offset += snprintf(buf + offset, sizeof(buf) - offset, "{\n");
    offset += snprintf(buf + offset, sizeof(buf) - offset, "  \"monitors\": [\n");

    bool first_mon = true;
    for (monitor_t *m = server.monitors; m != NULL; m = m->next) {
      if (!first_mon) offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");
      first_mon = false;
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "    {\"name\": \"%s\", \"id\": %u, \"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}}",
        m->name, m->id, m->rectangle.x, m->rectangle.y, m->rectangle.width, m->rectangle.height);
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "\n  ],\n");

    offset += snprintf(buf + offset, sizeof(buf) - offset, "  \"settings\": {\n");
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"border_width\": %d,\n", border_width);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"window_gap\": %d,\n", window_gap);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"split_ratio\": %.2f,\n", split_ratio);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"single_monocle\": %s,\n", single_monocle ? "true" : "false");
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"automatic_scheme\": %d,\n", automatic_scheme);
    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "    \"record_history\": %s\n", record_history ? "true" : "false");
    offset += snprintf(buf + offset, sizeof(buf) - offset, "  }\n");
    offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");

    send_success(client_fd, buf);
  } else if (streq("-l", *args) || streq("--load-state", *args)) {
    send_success(client_fd, "load-state: not implemented\n");
  } else if (streq("-a", *args) || streq("--add-monitor", *args)) {
    if (num < 2) {
      send_failure(client_fd, "wm -a: missing monitor name\n");
      return;
    }
    args++;
    num--;

    if (find_monitor_by_name(*args)) {
      send_failure(client_fd, "wm -a: monitor already exists\n");
      return;
    }

    struct output_config *oc = output_config_create(*args);
    if (oc) {
      send_success(client_fd, "monitor config added\n");
    } else {
      send_failure(client_fd, "wm -a: failed to add monitor\n");
    }
  } else if (streq("-O", *args) || streq("--reorder-monitors", *args)) {
    if (num < 2) {
      send_failure(client_fd, "wm -O: missing monitor list\n");
      return;
    }
    args++;
    num--;

    send_success(client_fd, "unimplemented\n");
  } else if (streq("-o", *args) || streq("--adopt-orphans", *args)) {
    struct bwm_toplevel *toplevel, *tmp;
    int adopted = 0;
    wl_list_for_each_safe(toplevel, tmp, &server.toplevels, link) {
      if (!toplevel->node && toplevel->xdg_toplevel && toplevel->mapped) {
        toplevel_map(NULL, toplevel);
        adopted++;
      }
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset, "adopted %d orphans\n", adopted);
    send_success(client_fd, buf);
  } else if (streq("-g", *args) || streq("--get-status", *args)) {
    int output_count = 0;
    struct bwm_output *output;
    wl_list_for_each(output, &server.outputs, link)
      output_count++;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
      "status: running\n"
      "monitors: %d\n", output_count);

    monitor_t *m = server.focused_monitor;
    if (m && m->desk) {
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "focused_monitor: %s\n"
        "focused_desktop: %s\n",
        m->name, m->desk->name);
      if (m->desk->focus) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
          "focused_node: %u\n", m->desk->focus->id);
      }
    }

    send_success(client_fd, buf);
  } else if (streq("-h", *args) || streq("--record-history", *args)) {
    if (num >= 2) {
      if (streq("true", args[1]) || streq("on", args[1]) || streq("1", args[1])) {
        record_history = true;
        send_success(client_fd, "record-history enabled\n");
      } else if (streq("false", args[1]) || streq("off", args[1]) || streq("0", args[1])) {
        record_history = false;
        send_success(client_fd, "record-history disabled\n");
      } else {
        send_failure(client_fd, "wm -h: invalid value (use true/false)\n");
      }
    } else {
      send_success(client_fd, record_history ? "true\n" : "false\n");
    }
  } else if (streq("-r", *args) || streq("--restart", *args)) {
    server_restart();
    send_success(client_fd, "restarting\n");
  } else {
    send_failure(client_fd, "wm: unknown command\n");
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
  } else if (streq("edge_scroller_pointer_focus", *args)) {
    if (num >= 2) {
      edge_scroller_pointer_focus = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "edge_scroller_pointer_focus set\n");
    } else {
      send_success(client_fd, edge_scroller_pointer_focus ? "true\n" : "false\n");
    }
  } else if (streq("scroller_default_proportion", *args)) {
    if (num >= 2) {
      scroller_default_proportion = atof(args[1]);
      if (scroller_default_proportion < 0.1f) scroller_default_proportion = 0.1f;
      if (scroller_default_proportion > 1.0f) scroller_default_proportion = 1.0f;
      send_success(client_fd, "scroller_default_proportion set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.2f\n", scroller_default_proportion);
      send_success(client_fd, buf);
    }
  } else if (streq("scroller_proportion_preset", *args)) {
    if (num >= 2) {
      // Parse comma-separated list of proportions
      char *value = args[1];
      int count = 1;
      for (char *p = value; *p; p++)
        if (*p == ',') count++;

      // Free old presets
      if (scroller_proportion_preset)
        free(scroller_proportion_preset);

      // Allocate new array
      scroller_proportion_preset = malloc(count * sizeof(float));
      if (!scroller_proportion_preset) {
        send_failure(client_fd, "memory allocation failed\n");
        return;
      }

      // Parse values
      char *token = strtok(value, ",");
      int i = 0;
      while (token && i < count) {
        float val = atof(token);
        if (val < 0.1f) val = 0.1f;
        if (val > 1.0f) val = 1.0f;
        scroller_proportion_preset[i++] = val;
        token = strtok(NULL, ",");
      }
      scroller_proportion_preset_count = i;

      send_success(client_fd, "scroller_proportion_preset set\n");
    } else {
      char buf[512];
      int offset = 0;
      for (int i = 0; i < scroller_proportion_preset_count && offset < 500; i++) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%.2f%s",
                          scroller_proportion_preset[i],
                          i < scroller_proportion_preset_count - 1 ? "," : "\n");
      }
      send_success(client_fd, buf);
    }
  } else if (streq("scroller_default_proportion_single", *args)) {
    if (num >= 2) {
      scroller_default_proportion_single = atof(args[1]);
      if (scroller_default_proportion_single < 0.1f) scroller_default_proportion_single = 0.1f;
      if (scroller_default_proportion_single > 1.0f) scroller_default_proportion_single = 1.0f;
      send_success(client_fd, "scroller_default_proportion_single set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.2f\n", scroller_default_proportion_single);
      send_success(client_fd, buf);
    }
  } else if (streq("scroller_focus_center", *args)) {
    if (num >= 2) {
      scroller_focus_center = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "scroller_focus_center set\n");
    } else {
      send_success(client_fd, scroller_focus_center ? "true\n" : "false\n");
    }
  } else if (streq("scroller_prefer_center", *args)) {
    if (num >= 2) {
      scroller_prefer_center = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "scroller_prefer_center set\n");
    } else {
      send_success(client_fd, scroller_prefer_center ? "true\n" : "false\n");
    }
  } else if (streq("scroller_prefer_overspread", *args)) {
    if (num >= 2) {
      scroller_prefer_overspread = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "scroller_prefer_overspread set\n");
    } else {
      send_success(client_fd, scroller_prefer_overspread ? "true\n" : "false\n");
    }
  } else if (streq("scroller_ignore_proportion_single", *args)) {
    if (num >= 2) {
      scroller_ignore_proportion_single = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "scroller_ignore_proportion_single set\n");
    } else {
      send_success(client_fd, scroller_ignore_proportion_single ? "true\n" : "false\n");
    }
  } else if (streq("scroller_structs", *args)) {
    if (num >= 2) {
      scroller_structs = atoi(args[1]);
      if (scroller_structs < 0) scroller_structs = 0;
      send_success(client_fd, "scroller_structs set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", scroller_structs);
      send_success(client_fd, buf);
    }
  } else if (streq("focus_follows_pointer", *args)) {
    if (num >= 2) {
      focus_follows_pointer = (strcmp(args[1], "true") == 0);
      transaction_commit_dirty();
      send_success(client_fd, "focus_follows_pointer set\n");
    } else {
      send_success(client_fd, focus_follows_pointer ? "true\n" : "false\n");
    }
  } else if (streq("pointer_follows_focus", *args)) {
    if (num >= 2) {
      pointer_follows_focus = (strcmp(args[1], "true") == 0);
      transaction_commit_dirty();
      send_success(client_fd, "pointer_follows_focus set\n");
    } else {
      send_success(client_fd, pointer_follows_focus ? "true\n" : "false\n");
    }
  } else if (streq("split_ratio", *args)) {
    if (num >= 2) {
      double val = atof(args[1]);
      if (val > 0 && val < 1) {
        split_ratio = val;
        transaction_commit_dirty();
        send_success(client_fd, "split_ratio set\n");
      } else {
        send_failure(client_fd, "config split_ratio: invalid value\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%f\n", split_ratio);
      send_success(client_fd, buf);
    }
  } else if (streq("top_padding", *args)) {
    if (num >= 2) {
      padding.top = atoi(args[1]);
      transaction_commit_dirty();
      send_success(client_fd, "top_padding set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", padding.top);
      send_success(client_fd, buf);
    }
  } else if (streq("right_padding", *args)) {
    if (num >= 2) {
      padding.right = atoi(args[1]);
      transaction_commit_dirty();
      send_success(client_fd, "right_padding set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", padding.right);
      send_success(client_fd, buf);
    }
  } else if (streq("bottom_padding", *args)) {
    if (num >= 2) {
      padding.bottom = atoi(args[1]);
      transaction_commit_dirty();
      send_success(client_fd, "bottom_padding set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", padding.bottom);
      send_success(client_fd, buf);
    }
  } else if (streq("left_padding", *args)) {
    if (num >= 2) {
      padding.left = atoi(args[1]);
      transaction_commit_dirty();
      send_success(client_fd, "left_padding set\n");
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", padding.left);
      send_success(client_fd, buf);
    }
  } else if (streq("normal_border_color", *args)) {
    if (num >= 2) {
      strncpy(normal_border_color, args[1], sizeof(normal_border_color) - 1);
      normal_border_color[sizeof(normal_border_color) - 1] = '\0';
      transaction_commit_dirty();
      send_success(client_fd, "normal_border_color set\n");
    } else {
      send_success(client_fd, normal_border_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("active_border_color", *args)) {
    if (num >= 2) {
      strncpy(active_border_color, args[1], sizeof(active_border_color) - 1);
      active_border_color[sizeof(active_border_color) - 1] = '\0';
      transaction_commit_dirty();
      send_success(client_fd, "active_border_color set\n");
    } else {
      send_success(client_fd, active_border_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("focused_border_color", *args)) {
    if (num >= 2) {
      strncpy(focused_border_color, args[1], sizeof(focused_border_color) - 1);
      focused_border_color[sizeof(focused_border_color) - 1] = '\0';
      transaction_commit_dirty();
      send_success(client_fd, "focused_border_color set\n");
    } else {
      send_success(client_fd, focused_border_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("presel_feedback_color", *args)) {
    if (num >= 2) {
      strncpy(presel_feedback_color, args[1], sizeof(presel_feedback_color) - 1);
      presel_feedback_color[sizeof(presel_feedback_color) - 1] = '\0';
      transaction_commit_dirty();
      send_success(client_fd, "presel_feedback_color set\n");
    } else {
      send_success(client_fd, presel_feedback_color);
      send_success(client_fd, "\n");
    }
  } else if (streq("automatic_scheme", *args)) {
    if (num >= 2) {
      if (streq("longest_side", args[1]) || streq("longest-side", args[1])) {
        automatic_scheme = SCHEME_LONGEST_SIDE;
      } else if (streq("alternate", args[1])) {
        automatic_scheme = SCHEME_ALTERNATE;
      } else if (streq("spiral", args[1])) {
        automatic_scheme = SCHEME_SPIRAL;
      } else {
        send_failure(client_fd, "config automatic_scheme: invalid value\n");
        return;
      }
      transaction_commit_dirty();
      send_success(client_fd, "automatic_scheme set\n");
    } else {
      char scheme_buf[64];
      const char *scheme_str = "spiral";
      if (automatic_scheme == SCHEME_LONGEST_SIDE) scheme_str = "longest_side";
      else if (automatic_scheme == SCHEME_ALTERNATE) scheme_str = "alternate";
      snprintf(scheme_buf, sizeof(scheme_buf), "%s\n", scheme_str);
      send_success(client_fd, scheme_buf);
    }
  } else if (streq("initial_polarity", *args)) {
    if (num >= 2) {
      if (streq("first_child", args[1]) || streq("first-child", args[1])) {
        initial_polarity = FIRST_CHILD;
      } else if (streq("second_child", args[1]) || streq("second-child", args[1])) {
        initial_polarity = SECOND_CHILD;
      } else {
        send_failure(client_fd, "config initial_polarity: invalid value\n");
        return;
      }
      send_success(client_fd, "initial_polarity set\n");
    } else {
      send_success(client_fd, initial_polarity == FIRST_CHILD ? "first_child\n" : "second_child\n");
    }
  } else if (streq("directional_focus_tightness", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val >= 0 && val <= 100) {
        directional_focus_tightness = val;
        send_success(client_fd, "directional_focus_tightness set\n");
      } else {
        send_failure(client_fd, "config directional_focus_tightness: invalid value\n");
        return;
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", directional_focus_tightness);
      send_success(client_fd, buf);
    }
  } else if (streq("mapping_events_count", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val >= 0) {
        mapping_events_count = val;
        send_success(client_fd, "mapping_events_count set\n");
      } else {
        send_failure(client_fd, "config mapping_events_count: invalid value\n");
        return;
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", mapping_events_count);
      send_success(client_fd, buf);
    }
  } else if (streq("ignore_ewmh_fullscreen", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val >= 0 && val <= 2) {
        ignore_ewmh_fullscreen = val;
        send_success(client_fd, "ignore_ewmh_fullscreen set\n");
      } else {
        send_failure(client_fd, "config ignore_ewmh_fullscreen: invalid value (0-2)\n");
        return;
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", ignore_ewmh_fullscreen);
      send_success(client_fd, buf);
    }
  } else if (streq("click_to_focus", *args)) {
    if (num >= 2) {
      click_to_focus = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "click_to_focus set\n");
    } else {
      send_success(client_fd, click_to_focus ? "true\n" : "false\n");
    }
  } else if (streq("record_history", *args)) {
    if (num >= 2) {
      record_history = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "record_history set\n");
    } else {
      send_success(client_fd, record_history ? "true\n" : "false\n");
    }
  } else if (streq("blur_enabled", *args)) {
    if (num >= 2) {
      blur_enabled = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "blur_enabled set\n");
    } else {
      send_success(client_fd, blur_enabled ? "true\n" : "false\n");
    }
  } else if (streq("blur_algorithm", *args)) {
    if (num >= 2) {
      enum blur_algorithm algo = blur_algorithm_from_str(args[1]);
      if (algo == BLUR_ALGORITHM_NONE && strcmp(args[1], "none") != 0) {
        send_failure(client_fd, "config blur_algorithm: unknown algorithm\n");
      } else {
        blur_algorithm = algo;
        send_success(client_fd, "blur_algorithm set\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s\n", blur_algorithm_to_str(blur_algorithm));
      send_success(client_fd, buf);
    }
  } else if (streq("blur_passes", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val >= 1 && val <= 10) {
        blur_passes = val;
        send_success(client_fd, "blur_passes set\n");
      } else {
        send_failure(client_fd, "config blur_passes: value must be 1-10\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", blur_passes);
      send_success(client_fd, buf);
    }
  } else if (streq("blur_radius", *args)) {
    if (num >= 2) {
      float val = atof(args[1]);
      if (val > 0.0f) {
        blur_radius = val;
        send_success(client_fd, "blur_radius set\n");
      } else {
        send_failure(client_fd, "config blur_radius: value must be > 0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.2f\n", blur_radius);
      send_success(client_fd, buf);
    }
  } else if (streq("blur_downsample", *args)) {
    if (num >= 2) {
      int val = atoi(args[1]);
      if (val >= 1 && val <= 8) {
        blur_downsample = val;
        for (monitor_t *m = mon_head; m; m = m->next) {
          if (m->output && m->output->blur_ctx)
            blur_output_resize(m->output->blur_ctx, m->output->width, m->output->height);
        }
        send_success(client_fd, "blur_downsample set\n");
      } else {
        send_failure(client_fd, "config blur_downsample: value must be 1-8\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d\n", blur_downsample);
      send_success(client_fd, buf);
    }
  } else if (streq("mica_enabled", *args)) {
    if (num >= 2) {
      mica_enabled = (strcmp(args[1], "true") == 0);
      send_success(client_fd, "mica_enabled set\n");
    } else {
      send_success(client_fd, mica_enabled ? "true\n" : "false\n");
    }
  } else if (streq("mica_tint_strength", *args)) {
    if (num >= 2) {
      float val = atof(args[1]);
      if (val >= 0.0f && val <= 1.0f) {
        mica_tint_strength = val;
        send_success(client_fd, "mica_tint_strength set\n");
      } else {
        send_failure(client_fd, "config mica_tint_strength: value must be 0.0-1.0\n");
      }
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.3f\n", mica_tint_strength);
      send_success(client_fd, buf);
    }
  } else if (streq("mica_tint", *args)) {
    if (num >= 2) {
      float r, g, b, a = 1.0f;
      int n = sscanf(args[1], "%f %f %f %f", &r, &g, &b, &a);
      if (n >= 3) {
        mica_tint[0] = r; mica_tint[1] = g;
        mica_tint[2] = b; mica_tint[3] = a;
        send_success(client_fd, "mica_tint set\n");
      } else {
        send_failure(client_fd, "config mica_tint: expected \"R G B [A]\"\n");
      }
    } else {
      char buf[128];
      snprintf(buf, sizeof(buf), "%.3f %.3f %.3f %.3f\n",
      	mica_tint[0], mica_tint[1], mica_tint[2], mica_tint[3]);
    send_success(client_fd, buf);
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
  } else if (streq("pseudo_tiled", *args)) {
    toggle_pseudo_tiled();
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

static void ipc_cmd_equalize(char **args, int num, int client_fd) {
  (void)args;
  (void)num;

  monitor_t *m = server.focused_monitor;
  if (!m || !m->desk) {
    send_failure(client_fd, "equalize: no focused desktop\n");
    return;
  }

  if (!m->desk->root) {
    send_failure(client_fd, "equalize: no tree\n");
    return;
  }

  equalize_tree(m->desk->root);
  transaction_commit_dirty();
  send_success(client_fd, "equalized\n");
}

static void ipc_cmd_balance(char **args, int num, int client_fd) {
  (void)args;
  (void)num;

  monitor_t *m = server.focused_monitor;
  if (!m || !m->desk) {
    send_failure(client_fd, "balance: no focused desktop\n");
    return;
  }

  if (!m->desk->root) {
    send_failure(client_fd, "balance: no tree\n");
    return;
  }

  balance_tree(m->desk->root);
  transaction_commit_dirty();
  send_success(client_fd, "balanced\n");
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
      } else if (strncmp(arg, "scroller_proportion=", 20) == 0) {
        float val = atof(arg + 20);
        if (val >= 0.1f && val <= 1.0f) {
          r->consequence.scroller_proportion = val;
          r->consequence.has_scroller_proportion = true;
        }
      } else if (strncmp(arg, "scroller_proportion_single=", 27) == 0) {
        float val = atof(arg + 27);
        if (val >= 0.1f && val <= 1.0f) {
          r->consequence.scroller_proportion_single = val;
          r->consequence.has_scroller_proportion_single = true;
        }
      } else if (streq("blur=on", arg)) {
        r->consequence.blur = true;
        r->consequence.has_blur = true;
      } else if (streq("blur=off", arg)) {
        r->consequence.blur = false;
        r->consequence.has_blur = true;
      } else if (streq("mica=on", arg)) {
        r->consequence.mica = true;
        r->consequence.has_mica = true;
      } else if (streq("mica=off", arg)) {
        r->consequence.mica = false;
        r->consequence.has_mica = true;
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

static void ipc_cmd_keyboard_grouping(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "keyboard_grouping: missing argument\n");
    return;
  }

  char *mode = *args;
  keyboard_grouping_t grouping;
  if (streq("none", mode)) {
    grouping = KEYBOARD_GROUP_NONE;
  } else if (streq("smart", mode)) {
    grouping = KEYBOARD_GROUP_SMART;
  } else if (streq("default", mode)) {
    grouping = KEYBOARD_GROUP_DEFAULT;
  } else {
    send_failure(client_fd, "keyboard_grouping: invalid mode (use none, smart, default)\n");
    return;
  }

  set_keyboard_grouping(grouping);
  keyboard_reapply_grouping();
  send_success(client_fd, "keyboard_grouping set\n");
}

static void ipc_cmd_scroller(char **args, int num, int client_fd) {
  if (num < 1) {
    send_failure(client_fd, "scroller: missing arguments\n");
    return;
  }

  monitor_t *mon = server.focused_monitor;
  if (!mon || !mon->desk) {
    send_failure(client_fd, "no desktop\n");
    return;
  }

  desktop_t *desk = mon->desk;

  if (streq("proportion", *args)) {
    if (num < 2) {
      send_failure(client_fd, "scroller proportion: missing value\n");
      return;
    }

    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller proportion: no focused client\n");
      return;
    }

    float value = atof(args[1]);
    if (value < 0.1f || value > 1.0f) {
      send_failure(client_fd, "scroller proportion: value must be between 0.1 and 1.0\n");
      return;
    }

    client_t *head = scroller_get_stack_head(desk->focus->client);
    head->scroller_proportion = value;
    arrange(mon, desk, true);
    send_success(client_fd, "proportion set\n");

  } else if (streq("stack", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller stack: no focused client\n");
      return;
    }

    // Find another client to stack with (for now, just stack with previous leaf)
    node_t *target = prev_leaf(desk->focus, desk->root);
    if (!target || !target->client) {
      send_failure(client_fd, "scroller stack: no target to stack with\n");
      return;
    }

    client_t *head = scroller_get_stack_head(target->client);
    scroller_stack_push(head, desk->focus->client);
    arrange(mon, desk, true);
    send_success(client_fd, "stacked\n");

  } else if (streq("unstack", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller unstack: no focused client\n");
      return;
    }

    scroller_stack_remove(desk->focus->client);
    arrange(mon, desk, true);
    send_success(client_fd, "unstacked\n");

  } else if (streq("resize", *args)) {
    if (num < 2) {
      send_failure(client_fd, "scroller resize: missing delta\n");
      return;
    }

    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller resize: no focused client\n");
      return;
    }

    float delta = atof(args[1]);
    scroller_resize_width(desk->focus->client, delta);
    arrange(mon, desk, true);
    send_success(client_fd, "resized\n");

  } else if (streq("set_proportion", *args)) {
    if (num < 2) {
      send_failure(client_fd, "scroller set_proportion: missing value\n");
      return;
    }

    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller set_proportion: no focused client\n");
      return;
    }

    float value = atof(args[1]);
    scroller_set_proportion(desk->focus->client, value);
    arrange(mon, desk, true);
    send_success(client_fd, "proportion set\n");

  } else if (streq("cycle_preset", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller cycle_preset: no focused client\n");
      return;
    }

    if (scroller_proportion_preset_count == 0) {
      send_failure(client_fd, "scroller cycle_preset: no presets configured\n");
      return;
    }

    scroller_cycle_proportion_preset(desk->focus->client);
    arrange(mon, desk, true);
    send_success(client_fd, "cycled to next preset\n");

  } else if (streq("center", *args)) {
    if (!desk->focus || !desk->focus->client) {
      send_failure(client_fd, "scroller center: no focused client\n");
      return;
    }

    scroller_center_window(desk, desk->focus->client);
    send_success(client_fd, "window centered\n");

  } else {
    send_failure(client_fd, "scroller: unknown subcommand\n");
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
  } else if (streq("wm", *args)) {
    ipc_cmd_wm(++args, --num, client_fd);
  } else if (streq("config", *args)) {
    ipc_cmd_config(++args, --num, client_fd);
  } else if (streq("quit", *args)) {
    ipc_cmd_quit(++args, --num, client_fd);
  } else if (streq("subscribe", *args)) {
    ipc_cmd_subscribe(++args, --num, client_fd);
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
  } else if (streq("equalize", *args)) {
    ipc_cmd_equalize(++args, --num, client_fd);
  } else if (streq("balance", *args)) {
    ipc_cmd_balance(++args, --num, client_fd);
  } else if (streq("send", *args)) {
    ipc_cmd_send(++args, --num, client_fd);
   } else if (streq("rule", *args)) {
     ipc_cmd_rule(++args, --num, client_fd);
   } else if (streq("keyboard_grouping", *args)) {
     ipc_cmd_keyboard_grouping(++args, --num, client_fd);
   } else if (streq("scroller", *args)) {
     ipc_cmd_scroller(++args, --num, client_fd);
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

  bwm_subscriber_t *sb = subscriber_head;
  while (sb != NULL) {
    bwm_subscriber_t *next = sb->next;
    if (sb->event_source) {
      wl_event_source_remove(sb->event_source);
    }
    close(sb->client_fd);
    if (sb->fifo_path) {
      unlink(sb->fifo_path);
      free(sb->fifo_path);
    }
    free(sb);
    sb = next;
  }
  subscriber_head = subscriber_tail = NULL;
}

static bwm_subscriber_t *make_subscriber(int client_fd, char *fifo_path, bwm_subscriber_mask_t mask, int count) {
  bwm_subscriber_t *sb = calloc(1, sizeof(bwm_subscriber_t));
  if (!sb) {
    return NULL;
  }
  sb->client_fd = client_fd;
  sb->fifo_path = fifo_path;
  sb->mask = mask;
  sb->count = count;
  sb->prev = sb->next = NULL;
  sb->event_source = NULL;
  return sb;
}

static void remove_subscriber(bwm_subscriber_t *sb) {
  if (!sb) return;

  bwm_subscriber_t *a = sb->prev;
  bwm_subscriber_t *b = sb->next;

  if (a) a->next = b;
  else subscriber_head = b;

  if (b) b->prev = a;
  else subscriber_tail = a;

  if (sb->event_source) {
    wl_event_source_remove(sb->event_source);
  }

  close(sb->client_fd);
  if (sb->fifo_path) {
    unlink(sb->fifo_path);
    free(sb->fifo_path);
  }
  free(sb);
}

static void add_subscriber(bwm_subscriber_t *sb) {
  if (subscriber_head == NULL) {
    subscriber_head = subscriber_tail = sb;
  } else {
    subscriber_tail->next = sb;
    sb->prev = subscriber_tail;
    subscriber_tail = sb;
  }

  int flags = fcntl(sb->client_fd, F_GETFD);
  fcntl(sb->client_fd, F_SETFD, flags & ~FD_CLOEXEC);

  if (sb->mask & BWM_MASK_REPORT) {
    ipc_print_report(sb->client_fd);
    if (sb->count-- == 1) {
      remove_subscriber(sb);
    }
  }
}

void ipc_print_report(int fd) {
  char buf[BWM_BUFSIZ];
  size_t offset = 0;

  for (monitor_t *m = server.monitors; m != NULL; m = m->next) {
    char mon_flag = (server.focused_monitor == m) ? 'M' : 'm';
    offset += snprintf(buf + offset, sizeof(buf) - offset, "%c%s", mon_flag, m->name);

    for (desktop_t *d = m->desk; d != NULL; d = d->next) {
      char desk_flag;
      if (m->desk == d) {
        if (d->focus) {
          desk_flag = 'O';
        } else {
          desk_flag = 'F';
        }
      } else {
        if (d->focus) {
          desk_flag = 'o';
        } else {
          desk_flag = 'f';
        }
      }
      offset += snprintf(buf + offset, sizeof(buf) - offset, ":%c%s", desk_flag, d->name);
    }

    if (m->desk) {
      offset += snprintf(buf + offset, sizeof(buf) - offset, ":L%c",
        m->desk->layout == LAYOUT_TILED ? 'T' :
        m->desk->layout == LAYOUT_MONOCLE ? 'M' : 'S');

      if (m->desk->focus) {
        char state_char = 'T';
        client_state_t state = STATE_TILED;
        if (m->desk->focus->client) {
          state = m->desk->focus->client->state;
        }
        if (state == STATE_FLOATING) state_char = 'F';
        else if (state == STATE_FULLSCREEN) state_char = 'U';
        else if (state == STATE_PSEUDO_TILED) state_char = 'P';

        offset += snprintf(buf + offset, sizeof(buf) - offset, ":T%c", state_char);

        int i = 0;
        char flags[6] = {0};
        if (m->desk->focus->sticky) flags[i++] = 'S';
        if (m->desk->focus->private_node) flags[i++] = 'P';
        if (m->desk->focus->locked) flags[i++] = 'L';
        if (m->desk->focus->marked) flags[i++] = 'M';
        if (m->desk->focus->hidden) flags[i++] = 'H';
        if (i > 0) {
          offset += snprintf(buf + offset, sizeof(buf) - offset, ":G%s", flags);
        }
      }
    }

    if (m->next) {
      offset += snprintf(buf + offset, sizeof(buf) - offset, "%s", ":");
    }
  }

  offset += snprintf(buf + offset, sizeof(buf) - offset, "%s", "\n");
  write(fd, buf, offset);
}

void ipc_put_status(bwm_subscriber_mask_t mask, const char *fmt, ...) {
  bwm_subscriber_t *sb = subscriber_head;
  char buf[BWM_BUFSIZ];
  size_t len = 0;

  if (mask == BWM_MASK_REPORT) {
    ipc_print_report(-1);
    return;
  }

  if (fmt) {
    va_list args;
    va_start(args, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
  }

  while (sb != NULL) {
    bwm_subscriber_t *next = sb->next;
    if (sb->mask & mask) {
      if (sb->count > 0) {
        sb->count--;
      }

      if (mask == BWM_MASK_REPORT) {
        ipc_print_report(sb->client_fd);
      } else if (len > 0) {
        write(sb->client_fd, buf, len);
      }

      if (sb->count == 0) {
        remove_subscriber(sb);
      }
    }
    sb = next;
  }
}

static void ipc_cmd_subscribe(char **args, int num, int client_fd) {
  bwm_subscriber_mask_t mask = 0;
  int count = -1;
  char *fifo_path = NULL;
  bool explicit_fifo = false;

  while (num > 0) {
    if (streq("-c", *args) || streq("--count", *args)) {
      if (num < 2) {
        send_failure(client_fd, "subscribe -c: missing count\n");
        return;
      }
      args++;
      num--;
      if (sscanf(*args, "%d", &count) != 1 || count < 1) {
        send_failure(client_fd, "subscribe -c: invalid count\n");
        return;
      }
    } else if (streq("-f", *args) || streq("--fifo", *args)) {
      explicit_fifo = true;
    } else if (streq("report", *args) || streq("R", *args)) {
      mask |= BWM_MASK_REPORT;
    } else if (streq("monitor", *args) || streq("M", *args)) {
      mask |= BWM_MASK_MONITOR_ADD | BWM_MASK_MONITOR_REMOVE | BWM_MASK_MONITOR_FOCUS | BWM_MASK_MONITOR_CHANGE;
    } else if (streq("monitor_add", *args)) {
      mask |= BWM_MASK_MONITOR_ADD;
    } else if (streq("monitor_remove", *args)) {
      mask |= BWM_MASK_MONITOR_REMOVE;
    } else if (streq("monitor_focus", *args)) {
      mask |= BWM_MASK_MONITOR_FOCUS;
    } else if (streq("monitor_change", *args)) {
      mask |= BWM_MASK_MONITOR_CHANGE;
    } else if (streq("desktop", *args) || streq("D", *args)) {
      mask |= BWM_MASK_DESKTOP_ADD | BWM_MASK_DESKTOP_REMOVE | BWM_MASK_DESKTOP_FOCUS | BWM_MASK_DESKTOP_CHANGE | BWM_MASK_DESKTOP_LAYOUT;
    } else if (streq("desktop_add", *args)) {
      mask |= BWM_MASK_DESKTOP_ADD;
    } else if (streq("desktop_remove", *args)) {
      mask |= BWM_MASK_DESKTOP_REMOVE;
    } else if (streq("desktop_focus", *args)) {
      mask |= BWM_MASK_DESKTOP_FOCUS;
    } else if (streq("desktop_change", *args)) {
      mask |= BWM_MASK_DESKTOP_CHANGE;
    } else if (streq("desktop_layout", *args)) {
      mask |= BWM_MASK_DESKTOP_LAYOUT;
    } else if (streq("node", *args) || streq("N", *args)) {
      mask |= BWM_MASK_NODE_ADD | BWM_MASK_NODE_REMOVE | BWM_MASK_NODE_FOCUS | BWM_MASK_NODE_CHANGE | BWM_MASK_NODE_STATE | BWM_MASK_NODE_FLAG;
    } else if (streq("node_add", *args)) {
      mask |= BWM_MASK_NODE_ADD;
    } else if (streq("node_remove", *args)) {
      mask |= BWM_MASK_NODE_REMOVE;
    } else if (streq("node_focus", *args)) {
      mask |= BWM_MASK_NODE_FOCUS;
    } else if (streq("node_change", *args)) {
      mask |= BWM_MASK_NODE_CHANGE;
    } else if (streq("node_state", *args)) {
      mask |= BWM_MASK_NODE_STATE;
    } else if (streq("node_flag", *args)) {
      mask |= BWM_MASK_NODE_FLAG;
    } else if (streq("all", *args) || streq("A", *args)) {
      mask = BWM_MASK_ALL;
    } else {
      send_failure(client_fd, "subscribe: unknown argument\n");
      return;
    }
    args++;
    num--;
  }

  if (mask == 0) {
    mask = BWM_MASK_REPORT;
  }

  if (!explicit_fifo) {
    char template[] = "/tmp/bwm_fifo_XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
      send_failure(client_fd, "subscribe: failed to create fifo path\n");
      return;
    }
    close(fd);
    unlink(template);
    fifo_path = strdup(template);
    if (!fifo_path) {
      send_failure(client_fd, "subscribe: memory error\n");
      return;
    }
    if (mkfifo(fifo_path, 0666) == -1) {
      free(fifo_path);
      fifo_path = NULL;
    }
  }

  if (fifo_path) {
    int fifo_fd = open(fifo_path, O_WRONLY);
    if (fifo_fd < 0) {
      free(fifo_path);
      send_failure(client_fd, "subscribe: failed to open fifo\n");
      return;
    }
    char response[BWM_BUFSIZ];
    snprintf(response, sizeof(response), "%s\n", fifo_path);
    write(client_fd, response, strlen(response));
    close(client_fd);
    client_fd = fifo_fd;
  }

  bwm_subscriber_t *sb = make_subscriber(client_fd, fifo_path, mask, count);
  if (!sb) {
    if (fifo_path) free(fifo_path);
    send_failure(client_fd, "subscribe: failed to create subscriber\n");
    return;
  }

  add_subscriber(sb);
}
