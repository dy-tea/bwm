#pragma once

#include "types.h"

#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-server-core.h>

#define DOORS_SOCKET_ENV "DOORS_SOCKET"
#define DOORS_SOCKET_PATH_TEMPLATE "/run/user/%d/doors-%d.sock"
#define DOORS_BUFSIZ 4096

#define DOORS_FIFO_TEMPLATE "doors_fifo.XXXXXX"

typedef struct output_t output_t;

typedef enum {
	SUB_MASK_REPORT = 1 << 0,
	SUB_MASK_MONITOR_ADD = 1 << 1,
	SUB_MASK_MONITOR_REMOVE = 1 << 2,
	SUB_MASK_MONITOR_FOCUS = 1 << 3,
	SUB_MASK_MONITOR_CHANGE = 1 << 4,
	SUB_MASK_DESKTOP_ADD = 1 << 5,
	SUB_MASK_DESKTOP_REMOVE = 1 << 6,
	SUB_MASK_DESKTOP_FOCUS = 1 << 7,
	SUB_MASK_DESKTOP_CHANGE = 1 << 8,
	SUB_MASK_DESKTOP_LAYOUT = 1 << 9,
	SUB_MASK_NODE_ADD = 1 << 10,
	SUB_MASK_NODE_REMOVE = 1 << 11,
	SUB_MASK_NODE_FOCUS = 1 << 12,
	SUB_MASK_NODE_CHANGE = 1 << 13,
	SUB_MASK_NODE_STATE = 1 << 14,
	SUB_MASK_NODE_FLAG = 1 << 15,
	SUB_MASK_ALL = (1 << 16) - 1
} subscriber_mask_t;

typedef struct subscriber_t {
	int client_fd;
	char *fifo_path;
	subscriber_mask_t mask;
	int count;
	struct wl_event_source *event_source;
	struct subscriber_t *prev, *next;
} subscriber_t;

extern subscriber_t *subscriber_head;
extern subscriber_t *subscriber_tail;

void ipc_init(void);
int ipc_get_socket_fd(void);
void ipc_handle_incoming(int client_fd);
void ipc_fini(void);
const char *ipc_get_socket_path(void);

void ipc_put_status(subscriber_mask_t mask, const char *fmt, ...);
bool ipc_print_report(int fd);

void send_success(int client_fd, const char *msg);
void send_failure(int client_fd, const char *msg);

/* Returns true when the command takes ownership of client_fd. */
bool process_ipc_message(char *msg, int msg_len, int client_fd);
