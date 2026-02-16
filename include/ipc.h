#pragma once

#include <stdbool.h>
#include <wayland-server-core.h>
#include <sys/socket.h>
#include <sys/un.h>

#define BWM_SOCKET_ENV "BWM_SOCKET"
#define BWM_SOCKET_PATH_TEMPLATE "/tmp/bwm-%d.sock"
#define BWM_BUFSIZ 4096

void ipc_init(void);
int ipc_get_socket_fd(void);
void ipc_handle_incoming(int client_fd);
void ipc_cleanup(void);

const char *ipc_get_socket_path(void);
