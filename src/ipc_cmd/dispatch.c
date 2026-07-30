#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include <stdlib.h>
#include <wlr/util/log.h>

typedef void (*cmd_handler)(char **args, int num, int client_fd);

typedef struct {
	const char *name;
	cmd_handler handler;
} cmd_entry_t;

bool ipc_cmd_subscribe(char **args, int num, int client_fd);

static const cmd_entry_t cmds[] = {
	{"balance", ipc_cmd_balance},
	{"bezier", ipc_cmd_bezier},
	{"config", ipc_cmd_config},
	{"desktop", ipc_cmd_desktop},
	{"env", ipc_cmd_env},
	{"equalize", ipc_cmd_equalize},
	{"flip", ipc_cmd_flip},
	{"focus", ipc_cmd_focus},
	{"globalshortcuts", ipc_cmd_globalshortcuts},
	{"hotkey", ipc_cmd_hotkey},
	{"input", ipc_cmd_input},
	{"keyboard_grouping", ipc_cmd_keyboard_grouping},
	{"master_stack", ipc_cmd_master_stack},
	{"node", ipc_cmd_node},
	{"output", ipc_cmd_output},
	{"presel", ipc_cmd_presel},
	{"query", ipc_cmd_query},
	{"quit", ipc_cmd_quit},
	{"resize", ipc_cmd_resize},
	{"rotate", ipc_cmd_rotate},
	{"rule", ipc_cmd_rule},
	{"scratchpad", ipc_cmd_scratchpad},
	{"scroller", ipc_cmd_scroller},
	{"seat", ipc_cmd_seat},
	{"send", ipc_cmd_send},
	{"spring", ipc_cmd_spring},
	{"swap", ipc_cmd_swap},
	{"toggle", ipc_cmd_toggle},
	{"wm", ipc_cmd_wm},
};

bool process_ipc_message(char *msg, int msg_len, int client_fd) {
	wlr_log(WLR_DEBUG, "IPC: processing message: %.*s", msg_len, msg);
	bool owns_client_fd = false;
	int cap = 16;
	int num = 0;
	char **args = calloc(cap, sizeof(char *));

	if (!args) {
		send_failure(client_fd, "memory error\n");
		return false;
	}

	for (int i = 0, j = 0; i < msg_len; i++) {
		if (num >= cap) {
			cap *= 2;
			char **new = realloc(args, cap * sizeof(char *));
			if (!new) {
				free(args);
				send_failure(client_fd, "memory error\n");
				return false;
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
		return false;
	}

	char **args_orig = args;

	if (streq("subscribe", *args)) {
		owns_client_fd = ipc_cmd_subscribe(++args, --num, client_fd);
	} else {
		bool found = false;
		for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
			if (streq(cmds[i].name, *args)) {
				cmds[i].handler(++args, --num, client_fd);
				found = true;
				break;
			}
		}
		if (!found)
			send_failure(client_fd, "unknown command\n");
	}

	free(args_orig);
	return owns_client_fd;
}
