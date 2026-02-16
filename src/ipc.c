#define WLR_USE_UNSTABLE
#include "ipc.h"
#include "server.h"
#include "types.h"
#include "toplevel.h"
#include "transaction.h"
#include "workspace.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <wlr/types/wlr_xdg_shell.h>

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

  if (streq("-f", *args) || streq("--focus", *args)) {
    focus_node(mon, mon->desk, mon->desk->focus);
    send_success(client_fd, "focused\n");
  } else if (streq("-l", *args) || streq("--layout", *args)) {
    if (num < 2) {
      send_failure(client_fd, "desktop -l: missing layout argument\n");
      return;
    }
    args++;
    if (streq("tiled", *args)) {
      mon->desk->layout = LAYOUT_TILED;
    } else if (streq("monocle", *args)) {
      mon->desk->layout = LAYOUT_MONOCLE;
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
    strncpy(mon->desk->name, *args, SMALEN - 1);
    mon->desk->name[SMALEN - 1] = '\0';
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
      ;	m = m->next, offset += snprintf(buf + offset, sizeof(buf) - offset,
		  		"  \"monitor\": {\"name\": \"%s\", \"id\": %u},\n",
		     	m->name, m->id))
     	for (desktop_t *d = m->desk;
        ; d = d->next,  offset += snprintf(buf + offset, sizeof(buf) - offset,
	          "  \"desktop\": {\"name\": \"%s\", \"id\": %u, \"layout\": %d},\n",
	          d->name, d->id, d->layout))
       	;

    struct bwm_toplevel *toplevel;
    wl_list_for_each(toplevel, &server.toplevels, link)
      offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  \"toplevel\": {\"app_id\": \"%s\", \"title\": \"%s\"}\n",
        toplevel->node && toplevel->node->client ? toplevel->node->client->app_id : "?",
        toplevel->node && toplevel->node->client ? toplevel->node->client->title : "?");

    offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");
    send_success(client_fd, buf);
  } else if (streq("-M", *args) || streq("--monitors", *args)) {
    for (monitor_t *m = server.monitors
      ; m != NULL
      ; m = m->next)
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
      offset += snprintf(buf + offset, sizeof(buf) - offset, "%u\n",
        toplevel->node ? toplevel->node->id : 0);
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
