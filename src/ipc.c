#include "ipc.h"
#include "once.h"
#include "output.h"
#include "server.h"
#include "tree.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int ipc_socket_fd = -1;
static char socket_path[256];

subscriber_t *subscriber_head = NULL;
subscriber_t *subscriber_tail = NULL;

void remove_subscriber(subscriber_t *sb);

static bool ipc_write_all(int fd, const void *data, size_t len) {
	const char *buf = data;
	size_t written = 0;

	while (written < len) {
		ssize_t n = write(fd, buf + written, len - written);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false;
		written += (size_t)n;
	}

	return true;
}

static bool desktop_has_urgent(desktop_t *d) {
	if (!d || !d->root)
		return false;

	FOR_EACH_LEAF(n, d->root)
		if (n->client && n->client->flags.urgent)
			return true;

	return false;
}

int ipc_get_socket_fd(void) {
	return ipc_socket_fd;
}

static void send_response(int client_fd, bool success, const char *msg) {
	char buf[DOORS_BUFSIZ];
	size_t offset = 0;
	buf[offset++] = success ? '\0' : '\x01';

	if (msg) {
		size_t len = strlen(msg);
		if (len > sizeof(buf) - 1)
			len = sizeof(buf) - 1;
		memcpy(buf + offset, msg, len);
		offset += len;
	}

	wlr_log(WLR_DEBUG, "IPC: sending response: %.*s", (int)(offset - 1), buf + 1);
	ipc_write_all(client_fd, buf, offset);
}

void send_success(int client_fd, const char *msg) {
	send_response(client_fd, true, msg);
}

void send_failure(int client_fd, const char *msg) {
	send_response(client_fd, false, msg);
}

void ipc_handle_incoming(int client_fd) {
	char msg[DOORS_BUFSIZ];
	wlr_log(WLR_DEBUG, "IPC: handling incoming connection");
	ssize_t n = recv(client_fd, msg, sizeof(msg) - 1, 0);
	wlr_log(WLR_DEBUG, "IPC: received %zd bytes", n);

	if (n <= 0) {
		wlr_log(WLR_DEBUG, "IPC: no data received, closing");
		close(client_fd);
		return;
	}

	msg[n] = '\0';
	if (!process_ipc_message(msg, (int)n, client_fd))
		close(client_fd);
}

bool ipc_print_report(int fd) {
	char buf[DOORS_BUFSIZ];
	size_t offset = 0;

	for (output_t *m = mon_head; m; m = m->next) {
		char mon_flag = (server.focused_output == m) ? 'M' : 'm';
		offset += snprintf(buf + offset, sizeof(buf) - offset, "%c%s", mon_flag, m->name);

		for (desktop_t *d = m->desk_head; d != NULL; d = d->next) {
			char desk_flag;
			if (m->desk == d)
				desk_flag = d->focus ? 'O' : 'F';
			else
				desk_flag = d->focus ? 'o' : 'f';

			offset += snprintf(buf + offset, sizeof(buf) - offset, ":%c%s", desk_flag, d->name);
			if (desktop_has_urgent(d))
				offset += snprintf(buf + offset, sizeof(buf) - offset, ":U%s", d->name);
		}

		if (m->desk) {
			offset += snprintf(buf + offset, sizeof(buf) - offset, ":L%c", layout_to_char(m->desk->layout));

			if (m->desk->focus) {
				char state_char = 'T';
				client_state_t state = STATE_TILED;
				if (m->desk->focus->client) {
					state = m->desk->focus->client->state;
				}
				if (state == STATE_FLOATING)
					state_char = 'F';
				else if (state == STATE_FULLSCREEN)
					state_char = 'U';
				else if (state == STATE_PSEUDO_TILED)
					state_char = 'P';

				offset += snprintf(buf + offset, sizeof(buf) - offset, ":T%c", state_char);

				int i = 0;
				char flags[6] = {0};
				if (m->desk->focus->sticky)
					flags[i++] = 'S';
				if (m->desk->focus->private_node)
					flags[i++] = 'P';
				if (m->desk->focus->locked)
					flags[i++] = 'L';
				if (m->desk->focus->marked)
					flags[i++] = 'M';
				if (m->desk->focus->hidden)
					flags[i++] = 'H';
				if (i > 0)
					offset += snprintf(buf + offset, sizeof(buf) - offset, ":G%s", flags);
			}
		}

		if (m->next)
			offset += snprintf(buf + offset, sizeof(buf) - offset, "%s", ":");
	}

	offset += snprintf(buf + offset, sizeof(buf) - offset, "%s", "\n");
	return ipc_write_all(fd, buf, offset);
}

void ipc_put_status(subscriber_mask_t mask, const char *fmt, ...) {
	subscriber_t *sb = subscriber_head;
	char buf[DOORS_BUFSIZ];
	size_t len = 0;

	if (fmt) {
		va_list args;
		va_start(args, fmt);
		len = vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (len >= sizeof(buf))
			len = sizeof(buf) - 1;
	} while (sb != NULL) {
		subscriber_t *next = sb->next;
		if (sb->mask & mask) {
			if (sb->count > 0)
				sb->count--;

			bool ok = true;
			if (mask == SUB_MASK_REPORT) {
				ok = ipc_print_report(sb->client_fd);
			} else if (len > 0) {
				ok = ipc_write_all(sb->client_fd, buf, len);
			}

			if (!ok || sb->count == 0)
				remove_subscriber(sb);
		}
		sb = next;
	}
}

void ipc_init(void) {
	ONCE();
	struct sockaddr_un addr;
	socklen_t len;

	ipc_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc_socket_fd == -1) {
		wlr_log(WLR_ERROR, "Failed to create IPC socket");
		return;
	}

	fcntl(ipc_socket_fd, F_SETFD, FD_CLOEXEC);

	addr.sun_family = AF_UNIX;
	snprintf(socket_path, sizeof(socket_path), DOORS_SOCKET_PATH_TEMPLATE, getuid(), getpid());

	char *last_slash = strrchr(socket_path, '/');
	if (last_slash != NULL) {
		*last_slash = '\0';
		mkdir(socket_path, 0700);
		*last_slash = '/';
	}

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

	setenv(DOORS_SOCKET_ENV, socket_path, true);
	wlr_log(WLR_INFO, "IPC socket: %s", socket_path);
}

void ipc_fini(void) {
	ONCE();
	if (server.ipc_event_source) {
		wl_event_source_remove(server.ipc_event_source);
		server.ipc_event_source = NULL;
	}

	if (ipc_socket_fd != -1) {
		close(ipc_socket_fd);
		unlink(socket_path);
		ipc_socket_fd = -1;
	}

	subscriber_t *sb = subscriber_head;
	while (sb != NULL) {
		subscriber_t *next = sb->next;
		if (sb->event_source)
			wl_event_source_remove(sb->event_source);
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
