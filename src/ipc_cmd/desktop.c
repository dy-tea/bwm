#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "keyboard.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "transaction.h"
#include "tree.h"
#include "workspace.h"
#include <stdlib.h>
#include <string.h>

void ipc_cmd_desktop(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "desktop: Missing arguments\n");
		return;
	}

	output_t *mon = server.focused_output;
	if (!mon || !mon->desk) {
		send_failure(client_fd, "no desktop\n");
		return;
	}

	if (streq("next", *args)) {
		focus_next_desktop();
		send_success(client_fd, "focused\n");
		return;
	} else if (streq("last", *args)) {
		focus_last_desktop();
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
			char *end;
			long idx = strtol(*args, &end, 10);
			if (*end == '\0' && idx >= 1 && idx <= 10) {
				workspace_switch_to_desktop_by_index(idx - 1);
				send_success(client_fd, "focused\n");
				return;
			}
			send_failure(client_fd, "desktop: unknown desktop\n");
			return;
		}
		args++;
		num--;
	}

	if (num < 1) {
		workspace_switch_to_desktop(desk->name);
		send_success(client_fd, "focused\n");
		return;
	}

	if (streq("-f", *args) || streq("--focus", *args)) {
		args++;
		num--;
		if (num >= 1 && (streq("next", *args) || streq("next.local", *args))) {
			focus_next_desktop();
			send_success(client_fd, "focused\n");
		} else if (num >= 1 && streq("last", *args)) {
			focus_last_desktop();
			send_success(client_fd, "focused\n");
		} else if (num >= 1 && (streq("prev", *args) || streq("prev.local", *args) || streq("previous",
				*args))) {
			focus_prev_desktop();
			send_success(client_fd, "focused\n");
		} else {
			workspace_switch_to_desktop(desk->name);
			send_success(client_fd, "focused\n");
		}
	} else if (streq("-l", *args) || streq("--layout", *args)) {
		if (num < 2) {
			send_failure(client_fd, "desktop -l: missing layout argument\n");
			return;
		}
		args++;
		num--;
		layout_t layout;
		if (streq("tiled", *args)) {
			layout = LAYOUT_TILED;
		} else if (streq("monocle", *args)) {
			layout = LAYOUT_MONOCLE;
		} else if (streq("scroller", *args)) {
			layout = LAYOUT_SCROLLER;
		} else if (streq("master_stack", *args)) {
			layout = LAYOUT_MASTER_STACK;
		} else {
			send_failure(client_fd, "desktop -l: unknown layout\n");
			return;
		}

		if (num > 1 && streq("--all", args[1])) {
			for (output_t *m = mon_head; m != NULL; m = m->next) {
				for (desktop_t *d = m->desk_head; d != NULL; d = d->next) {
					layout_set(d, layout);
					arrange(m, d, true);
					ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", d->name,
						layout_to_char(d->layout));
				}
			}
		} else {
			layout_set(desk, layout);
			arrange(mon, desk, true);
			if (desk->focus != NULL)
				focus_node(mon, desk, desk->focus);
			ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", desk->name,
				layout_to_char(desk->layout));
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
		ipc_put_status(SUB_MASK_DESKTOP_CHANGE, "desktop_change[%s]\n", desk->name);
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
		output_t *m0 = d0->output;
		output_t *m1 = d1->output;

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
				if (prev0)
					prev0->next = d1;
				if (next1)
					next1->next = d0;
			} else {
				d0->prev = prev1;
				d0->next = next1;
				d1->prev = prev0;
				d1->next = next0;
				if (prev0)
					prev0->next = d1;
				if (next0)
					next0->next = d1;
				if (prev1)
					prev1->next = d0;
				if (next1)
					next1->next = d0;
			}

			if (m0->desk == d0)
				m0->desk = d1;
			else if (m0->desk == d1)
				m0->desk = d0;
		} else {
			desktop_t *prev0 = d0->prev;
			desktop_t *next0 = d0->next;
			desktop_t *prev1 = d1->prev;
			desktop_t *next1 = d1->next;

			d0->prev = prev1;
			d0->next = next1;
			d1->prev = prev0;
			d1->next = next0;

			if (prev0)
				prev0->next = d1;
			else
				m0->desk = d1;
			if (next0)
				next0->next = d1;
			if (prev1)
				prev1->next = d0;
			else
				m1->desk = d0;
			if (next1)
				next1->next = d0;

			d0->output = m1;
			d1->output = m0;
		}

		if (mon->desk == d0)
			mon->desk = d1;
		else if (mon->desk == d1)
			mon->desk = d0;

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
		if (mon->last_desk == desk)
			mon->last_desk = next ? next : prev;

		ipc_put_status(SUB_MASK_DESKTOP_REMOVE, "desktop_remove[%s]\n", desk->name);
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
		output_t *target = find_output_by_name(*args);
		if (!target) {
			send_failure(client_fd, "desktop -m: monitor not found\n");
			return;
		}

		if (desk->output == target) {
			send_failure(client_fd, "desktop -m: already on target monitor\n");
			return;
		}

		if (!target->desk && !target->desk_head) {
			send_failure(client_fd, "desktop -m: target monitor has no desktop\n");
			return;
		}

		output_t *src_mon = desk->output;

		if (desk->prev) {
			desk->prev->next = desk->next;
		} else {
			src_mon->desk = desk->next;
			if (src_mon->desk_head == desk)
				src_mon->desk_head = desk->next;
		}

		if (desk->next) {
			desk->next->prev = desk->prev;
		} else {
			if (src_mon->desk_tail == desk)
				src_mon->desk_tail = desk->prev;
		}

		desk->prev = target->desk_tail;
		desk->next = NULL;
		desk->output = target;

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
