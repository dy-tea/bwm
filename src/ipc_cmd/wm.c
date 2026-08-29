#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "output.h"
#include "output_config.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void toplevel_map(struct wl_listener *listener, void *data);

void ipc_cmd_wm(char **args, int num, int client_fd) {
	char buf[DOORS_BUFSIZ];
	size_t offset = 0;

	if (num < 1) {
		send_failure(client_fd, "wm: missing command\n");
		return;
	}

	if (streq("-d", *args) || streq("--dump-state", *args)) {
		// dump current state as JSON
		offset += snprintf(buf + offset, sizeof(buf) - offset, "{\n");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "  \"monitors\": [\n");

		bool first_mon = true;
		output_t *out;
		wl_list_for_each(out, &mon_list, link) {
			if (!first_mon)
				offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");
			first_mon = false;
			offset += snprintf(buf + offset, sizeof(buf) - offset,
				"    {\"name\": \"%s\", \"id\": %u, \"rect\": {\"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d}}", out->name, out->id, out->rectangle.x, out->rectangle.y, out->rectangle.width, out->rectangle.height);
		}
		offset += snprintf(buf + offset, sizeof(buf) - offset, "\n  ],\n");

		offset += snprintf(buf + offset, sizeof(buf) - offset, "  \"settings\": {\n");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"border_width\": %d,\n",
			settings.border_width);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"window_gap\": %d,\n",
			settings.window_gap);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"split_ratio\": %.2f,\n",
			settings.split_ratio);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"single_monocle\": %s,\n",
			settings.single_monocle ? "true" : "false");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"automatic_scheme\": %d,\n",
			settings.automatic_scheme);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"smart_gaps\": %s,\n",
			settings.smart_gaps ? "true" : "false");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"smart_borders\": %s,\n",
			settings.smart_borders ? "true" : "false");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"respect_tiled_min_size\": %s,\n",
			settings.respect_tiled_min_size ? "true" : "false");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"focus_wrapping\": %s,\n",
			settings.focus_wrapping ? "true" : "false");

		const char *m = "focus";
		if (settings.focus_on_activate == FOCUS_ON_ACTIVATE_NONE)
			m = "none";
		else if (settings.focus_on_activate == FOCUS_ON_ACTIVATE_SMART)
			m = "smart";
		else if (settings.focus_on_activate == FOCUS_ON_ACTIVATE_URGENT)
			m = "urgent";
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"focus_on_activate\": \"%s\",\n", m);

		const char *ffm = "no";
		if (settings.focus_follows_mouse == FOLLOWS_YES)
			ffm = "yes";
		else if (settings.focus_follows_mouse == FOLLOWS_ALWAYS)
			ffm = "always";
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"focus_follows_mouse\": \"%s\",\n",
			ffm);

		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"record_history\": %s,\n",
			settings.record_history ? "true" : "false");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"shadow_size\": %.1f,\n",
			settings.shadow_size);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"shadow_offset_x\": %.1f,\n",
			settings.shadow_offset_x);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "    \"shadow_offset_y\": %.1f,\n",
			settings.shadow_offset_y);
		offset += snprintf(buf + offset, sizeof(buf) - offset,
			"    \"shadow_color\": [%.3f, %.3f, %.3f, %.3f]\n", settings.shadow_color[0],
			settings.shadow_color[1], settings.shadow_color[2], settings.shadow_color[3]);
		offset += snprintf(buf + offset, sizeof(buf) - offset, "  }\n");
		offset += snprintf(buf + offset, sizeof(buf) - offset, "}\n");

		send_success(client_fd, buf);
	} else if (streq("-l", *args) || streq("--load-state", *args)) {
		send_success(client_fd, "load-state: not implemented\n");
	} else if (streq("-a", *args) || streq("--add-monitor", *args)) {
		if (num < 2) {
			send_failure(client_fd, "wm -a: missing monitor name\n");
			return;
		}
		args++;
		num--;

		if (find_output_by_name(*args)) {
			send_failure(client_fd, "wm -a: monitor already exists\n");
			return;
		}

		struct output_config *oc = output_config_create(*args);
		if (oc) {
			send_success(client_fd, "monitor config added\n");
		} else {
			send_failure(client_fd, "wm -a: failed to add monitor\n");
		}
	} else if (streq("-O", *args) || streq("--reorder-monitors", *args)) {
		if (num < 2) {
			send_failure(client_fd, "wm -O: missing monitor list\n");
			return;
		}
		args++;
		num--;

		send_success(client_fd, "unimplemented\n");
	} else if (streq("-o", *args) || streq("--adopt-orphans", *args)) {
		struct toplevel_t *toplevel, *tmp;
		int adopted = 0;
		wl_list_for_each_safe(toplevel, tmp, &server.toplevels, link) {
			if (!toplevel->node && toplevel->xdg_toplevel && toplevel->mapped) {
				toplevel_map(NULL, toplevel);
				adopted++;
			}
		}
		offset += snprintf(buf + offset, sizeof(buf) - offset, "adopted %d orphans\n", adopted);
		send_success(client_fd, buf);
	} else if (streq("-g", *args) || streq("--get-status", *args)) {
		int output_count = 0;
		output_t *m;
		wl_list_for_each(m, &mon_list, link)
			output_count++;

		offset += snprintf(buf + offset, sizeof(buf) - offset, "status: running\n"
			"monitors: %d\n", output_count);

		m = server.focused_output;
		if (m && m->desk) {
			offset += snprintf(buf + offset, sizeof(buf) - offset, "focused_monitor: %s\n"
				"focused_desktop: %s\n", m->name, m->desk->name);
			if (m->desk->focus) {
				offset += snprintf(buf + offset, sizeof(buf) - offset, "focused_node: %u\n",
					m->desk->focus->id);
			}
		}

		send_success(client_fd, buf);
	} else if (streq("-h", *args) || streq("--record-history", *args)) {
		if (num >= 2) {
			if (streq("true", args[1]) || streq("on", args[1]) || streq("1", args[1])) {
				settings.record_history = true;
				send_success(client_fd, "record-history enabled\n");
			} else if (streq("false", args[1]) || streq("off", args[1]) || streq("0", args[1])) {
				settings.record_history = false;
				send_success(client_fd, "record-history disabled\n");
			} else {
				send_failure(client_fd, "wm -h: invalid value (use true/false)\n");
			}
		} else {
			send_success(client_fd, settings.record_history ? "true\n" : "false\n");
		}
	} else if (streq("-r", *args) || streq("--restart", *args)) {
		server_restart();
		send_success(client_fd, "restarting\n");
	} else {
		send_failure(client_fd, "wm: unknown command\n");
	}
}
