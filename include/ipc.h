#pragma once

#include <stdbool.h>
#include <wayland-server-core.h>
#include <sys/socket.h>
#include <sys/un.h>

#define BWM_SOCKET_ENV "BWM_SOCKET"
#define BWM_SOCKET_PATH_TEMPLATE "/tmp/bwm-%d.sock"
#define BWM_BUFSIZ 4096

#define BWM_FIFO_TEMPLATE "bwm_fifo.XXXXXX"

typedef enum {
  BWM_MASK_REPORT = 1 << 0,
  BWM_MASK_MONITOR_ADD = 1 << 1,
  BWM_MASK_MONITOR_REMOVE = 1 << 2,
  BWM_MASK_MONITOR_FOCUS = 1 << 3,
  BWM_MASK_MONITOR_CHANGE = 1 << 4,
  BWM_MASK_DESKTOP_ADD = 1 << 5,
  BWM_MASK_DESKTOP_REMOVE = 1 << 6,
  BWM_MASK_DESKTOP_FOCUS = 1 << 7,
  BWM_MASK_DESKTOP_CHANGE = 1 << 8,
  BWM_MASK_DESKTOP_LAYOUT = 1 << 9,
  BWM_MASK_NODE_ADD = 1 << 10,
  BWM_MASK_NODE_REMOVE = 1 << 11,
  BWM_MASK_NODE_FOCUS = 1 << 12,
  BWM_MASK_NODE_CHANGE = 1 << 13,
  BWM_MASK_NODE_STATE = 1 << 14,
  BWM_MASK_NODE_FLAG = 1 << 15,
  BWM_MASK_ALL = (1 << 16) - 1
} bwm_subscriber_mask_t;

typedef struct bwm_subscriber {
  int client_fd;
  char *fifo_path;
  bwm_subscriber_mask_t mask;
  int count;
  struct wl_event_source *event_source;
  struct bwm_subscriber *prev;
  struct bwm_subscriber *next;
} bwm_subscriber_t;

void ipc_init(void);
int ipc_get_socket_fd(void);
void ipc_handle_incoming(int client_fd);
void ipc_cleanup(void);
const char *ipc_get_socket_path(void);

void ipc_put_status(bwm_subscriber_mask_t mask, const char *fmt, ...);
void ipc_print_report(int fd);
