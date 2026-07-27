#include "ipc.h"
#include "ipc_helpers.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static subscriber_t *make_subscriber(int client_fd, char *fifo_path, subscriber_mask_t mask,
		int count) {
	subscriber_t *sb = calloc(1, sizeof(*sb));
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

void remove_subscriber(subscriber_t *sb) {
	if (!sb)
		return;

	subscriber_t *a = sb->prev;
	subscriber_t *b = sb->next;

	if (a)
		a->next = b;
	else
		subscriber_head = b;

	if (b)
		b->prev = a;
	else
		subscriber_tail = a;

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

void add_subscriber(subscriber_t *sb) {
	if (subscriber_head == NULL) {
		subscriber_head = subscriber_tail = sb;
	} else {
		subscriber_tail->next = sb;
		sb->prev = subscriber_tail;
		subscriber_tail = sb;
	}

	int flags = fcntl(sb->client_fd, F_GETFD);
	fcntl(sb->client_fd, F_SETFD, flags & ~FD_CLOEXEC);

	if (sb->mask & SUB_MASK_REPORT) {
		if (!ipc_print_report(sb->client_fd)) {
			// remove dead subscriber
			remove_subscriber(sb);
		} else if (sb->count > 0 && --sb->count == 0) {
			// remove subscriber if event count hits 0
			remove_subscriber(sb);
		}
	}
}

bool ipc_cmd_subscribe(char **args, int num, int client_fd) {
	subscriber_mask_t mask = 0;
	int count = -1;
	char *fifo_path = NULL;
	bool explicit_fifo = false;

	while (num > 0) {
		if (streq("-c", *args) || streq("--count", *args)) {
			if (num < 2) {
				send_failure(client_fd, "subscribe -c: missing count\n");
				return false;
			}
			args++;
			num--;
			if (sscanf(*args, "%d", &count) != 1 || count < 1) {
				send_failure(client_fd, "subscribe -c: invalid count\n");
				return false;
			}
		} else if (streq("-f", *args) || streq("--fifo", *args)) {
			explicit_fifo = true;
		} else if (streq("report", *args) || streq("R", *args)) {
			mask |= SUB_MASK_REPORT;
		} else if (streq("monitor", *args) || streq("M", *args)) {
			mask |= SUB_MASK_MONITOR_ADD | SUB_MASK_MONITOR_REMOVE | SUB_MASK_MONITOR_FOCUS |
				SUB_MASK_MONITOR_CHANGE;
		} else if (streq("monitor_add", *args)) {
			mask |= SUB_MASK_MONITOR_ADD;
		} else if (streq("monitor_remove", *args)) {
			mask |= SUB_MASK_MONITOR_REMOVE;
		} else if (streq("monitor_focus", *args)) {
			mask |= SUB_MASK_MONITOR_FOCUS;
		} else if (streq("monitor_change", *args)) {
			mask |= SUB_MASK_MONITOR_CHANGE;
		} else if (streq("desktop", *args) || streq("D", *args)) {
			mask |= SUB_MASK_DESKTOP_ADD | SUB_MASK_DESKTOP_REMOVE | SUB_MASK_DESKTOP_FOCUS |
				SUB_MASK_DESKTOP_CHANGE | SUB_MASK_DESKTOP_LAYOUT;
		} else if (streq("desktop_add", *args)) {
			mask |= SUB_MASK_DESKTOP_ADD;
		} else if (streq("desktop_remove", *args)) {
			mask |= SUB_MASK_DESKTOP_REMOVE;
		} else if (streq("desktop_focus", *args)) {
			mask |= SUB_MASK_DESKTOP_FOCUS;
		} else if (streq("desktop_change", *args)) {
			mask |= SUB_MASK_DESKTOP_CHANGE;
		} else if (streq("desktop_layout", *args)) {
			mask |= SUB_MASK_DESKTOP_LAYOUT;
		} else if (streq("node", *args) || streq("N", *args)) {
			mask |= SUB_MASK_NODE_ADD | SUB_MASK_NODE_REMOVE | SUB_MASK_NODE_FOCUS | SUB_MASK_NODE_CHANGE |
				SUB_MASK_NODE_STATE | SUB_MASK_NODE_FLAG;
		} else if (streq("node_add", *args)) {
			mask |= SUB_MASK_NODE_ADD;
		} else if (streq("node_remove", *args)) {
			mask |= SUB_MASK_NODE_REMOVE;
		} else if (streq("node_focus", *args)) {
			mask |= SUB_MASK_NODE_FOCUS;
		} else if (streq("node_change", *args)) {
			mask |= SUB_MASK_NODE_CHANGE;
		} else if (streq("node_state", *args)) {
			mask |= SUB_MASK_NODE_STATE;
		} else if (streq("node_flag", *args)) {
			mask |= SUB_MASK_NODE_FLAG;
		} else if (streq("all", *args) || streq("A", *args)) {
			mask = SUB_MASK_ALL;
		} else {
			send_failure(client_fd, "subscribe: unknown argument\n");
			return false;
		}
		args++;
		num--;
	}

	if (mask == 0) {
		mask = SUB_MASK_REPORT;
	}

	if (!explicit_fifo) {
		char template[] = "/tmp/doors_fifo_XXXXXX";
		int fd = mkstemp(template);
		if (fd < 0) {
			send_failure(client_fd, "subscribe: failed to create fifo path\n");
			return false;
		}
		close(fd);
		unlink(template);
		fifo_path = strdup(template);
		if (!fifo_path) {
			send_failure(client_fd, "subscribe: memory error\n");
			return false;
		}
		if (mkfifo(fifo_path, 0666) == -1) {
			free(fifo_path);
			fifo_path = NULL;
		}
	}

	if (fifo_path) {
		int fifo_fd = open(fifo_path, O_RDWR);
		if (fifo_fd < 0) {
			free(fifo_path);
			send_failure(client_fd, "subscribe: failed to open fifo\n");
			return false;
		}
		char response[DOORS_BUFSIZ];
		size_t offset = 0;
		response[offset++] = '\0';
		size_t len = strlen(fifo_path);
		memcpy(response + offset, fifo_path, len);
		offset += len;
		response[offset++] = '\n';
		write(client_fd, response, offset);
		close(client_fd);
		client_fd = fifo_fd;
	}

	subscriber_t *sb = make_subscriber(client_fd, fifo_path, mask, count);
	if (!sb) {
		if (fifo_path)
			free(fifo_path);
		send_failure(client_fd, "subscribe: failed to create subscriber\n");
		return false;
	}

	add_subscriber(sb);
	return true;
}
