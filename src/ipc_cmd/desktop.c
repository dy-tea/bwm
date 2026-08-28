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

static void swap_desktops(desktop_t *d0, desktop_t *d1) {
	struct wl_list *a = &d0->link;
	struct wl_list *b = &d1->link;

	if (a == b)
		return;

	struct wl_list *a_prev = a->prev;
	struct wl_list *a_next = a->next;
	struct wl_list *b_prev = b->prev;
	struct wl_list *b_next = b->next;

	if (a->next == b) {
		a_prev->next = b;
		b->prev = a_prev;
		b->next = a;
		a->prev = b;
		a->next = b_next;
		b_next->prev = a;
	} else if (b->next == a) {
		b_prev->next = a;
		a->prev = b_prev;
		a->next = b;
		b->prev = a;
		b->next = a_next;
		a_next->prev = b;
	} else {
		a_prev->next = b;
		b->prev = a_prev;
		a_next->prev = b;
		b->next = a_next;
		b_prev->next = a;
		a->prev = b_prev;
		b_next->prev = a;
		a->next = b_next;
	}
}

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
			output_t *m;
			wl_list_for_each(m, &mon_list, link) {
				desktop_t *d;
				wl_list_for_each(d, &m->desk_list, link) {
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

		swap_desktops(d0, d1);

		if (m0 != m1) {
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
		if (wl_list_length(&mon->desk_list) == 1) {
			send_failure(client_fd, "desktop -r: cannot remove the only desktop\n");
			return;
		}

		desktop_t *next = desk->link.next != &mon->desk_list ? wl_container_of(desk->link.next, desk,
			link) : NULL;
		desktop_t *prev = desk->link.prev != &mon->desk_list ? wl_container_of(desk->link.prev, desk,
			link) : NULL;

		if (desk->link.prev == &mon->desk_list && mon->desk)
			mon->desk = next;

		wl_list_remove(&desk->link);

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
			if (desk->link.prev != &mon->desk_list) {
				desktop_t *prev = wl_container_of(desk->link.prev, desk, link);
				swap_desktops(desk, prev);
			}
		} else if (streq("down", *args) || streq("next", *args)) {
			if (desk->link.next != &mon->desk_list) {
				desktop_t *next = wl_container_of(desk->link.next, desk, link);
				swap_desktops(desk, next);
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

		if (wl_list_empty(&target->desk_list)) {
			send_failure(client_fd, "desktop -m: target monitor has no desktop\n");
			return;
		}

		output_t *src_mon = desk->output;

		wl_list_remove(&desk->link);
		wl_list_insert(target->desk_list.prev, &desk->link);
		desk->output = target;

		if (src_mon->desk == desk) {
			src_mon->desk = wl_list_empty(&src_mon->desk_list) ? NULL :
				wl_container_of(src_mon->desk_list.next, desk, link);
			if (src_mon->desk)
				focus_node(src_mon, src_mon->desk, src_mon->desk->focus);
		}

		transaction_commit_dirty();
		send_success(client_fd, "desktop moved to monitor\n");
	} else {
		send_failure(client_fd, "desktop: unknown command\n");
	}
}
