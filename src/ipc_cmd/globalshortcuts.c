#include "global_shortcuts.h"
#include "ipc.h"
#include "ipc_cmd.h"

void ipc_cmd_globalshortcuts(char **args, int num, int client_fd) {
	(void)args;
	(void)num;
	char buf[DOORS_BUFSIZ];
	global_shortcuts_list(buf, sizeof(buf));
	send_success(client_fd, buf);
}
