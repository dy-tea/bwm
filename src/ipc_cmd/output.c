#include "animation.h"
#include "effects.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "output.h"
#include "output_config.h"
#include "server.h"
#include "tabs.h"
#include "transaction.h"
#include "tree.h"
#include "workspace.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

static void create_output(struct wlr_backend *backend, void *data) {
	bool *done = data;
	if (*done)
		return;

	do {
		if (wlr_backend_is_wl(backend)) {
			wlr_wl_output_create(backend);
			break;
		} else if (wlr_backend_is_headless(backend)) {
			wlr_headless_add_output(backend, 1920, 1080);
			break;
		} else if (wlr_backend_is_x11(backend)) {
			wlr_x11_output_create(backend);
			break;
		}

		return;
	} while (0);

	*done = true;
}

void ipc_cmd_output(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "output: missing arguments\n");
		return;
	}

	if (streq("list", *args) || streq("--list", *args) || streq("-l", *args)) {
		char buf[DOORS_BUFSIZ];
		int offset = 0;
		offset += snprintf(buf + offset, sizeof(buf) - offset, "[\n");
		bool first = true;
		output_t *output;
		wl_list_for_each(output, &mon_list, link) {
			struct wlr_output *wo = output->wlr_output;
			if (!first)
				offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");
			first = false;
			offset += snprintf(buf + offset, sizeof(buf) - offset, "  {\n"
				"    \"name\": \"%s\",\n"
				"    \"description\": \"%s\",\n"
				"    \"make\": \"%s\",\n"
				"    \"model\": \"%s\",\n"
				"    \"serial\": \"%s\",\n"
				"    \"width\": %d,\n"
				"    \"height\": %d,\n"
				"    \"refresh\": %.3f,\n"
				"    \"scale\": %.6g,\n"
				"    \"phys_width\": %d,\n"
				"    \"phys_height\": %d,\n"
				"    \"hdr\": %s,\n"
				"    \"allow_tearing\": %s,\n"
				"    \"enabled\": %s\n"
				"  }", wo->name ? wo->name : "", wo->description ? wo->description : "",
					wo->make ? wo->make : "", wo->model ? wo->model : "", wo->serial ? wo->serial : "", wo->width,
					wo->height, wo->refresh / 1000.0f, wo->scale, wo->phys_width, wo->phys_height,
					output->hdr ? "true" : "false", output->allow_tearing ? "true" : "false",
					wo->enabled ? "true" : "false");
		}
		offset += snprintf(buf + offset, sizeof(buf) - offset, "\n]\n");
		send_success(client_fd, buf);
		return;
	} else if (streq("create", *args)) {
		bool done = false;
		wlr_multi_for_each_backend(server.backend, create_output, &done);

		if (done)
			send_success(client_fd, "created output\n");
		else
			send_failure(client_fd,
				"output create: can only create outputs for wayland, x11 or headless backends");

		return;
	}

	char *output_name = *args;
	args++;
	num--;

	if (num < 1) {
		send_failure(client_fd, "output: missing subcommand\n");
		return;
	}

	struct output_config *oc = output_config_find(output_name);
	bool oc_new = false;
	if (!oc) {
		oc = output_config_create(output_name);
		if (!oc) {
			send_failure(client_fd, "output: failed to create config\n");
			return;
		}
		oc_new = true;
	}

	if (oc_new)
		output_config_add(oc);

	output_t *mon = find_output_by_name(output_name);
	char *subcmd = *args;

	if (streq("enable", subcmd)) {
		oc->enable = OUTPUT_CONFIG_ENABLE;
		output_config_apply(oc);
		send_success(client_fd, "output enabled\n");
	} else if (streq("disable", subcmd)) {
		oc->enable = OUTPUT_CONFIG_DISABLE;
		output_config_apply(oc);
		send_success(client_fd, "output disabled\n");
	} else if (streq("mode", subcmd) || streq("resolution", subcmd) || streq("res", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output mode: missing resolution\n");
			return;
		}
		args++;
		num--;

		char *res = *args;
		int width, height;
		float refresh_rate = -1;

		char *at = strchr(res, '@');
		if (at) {
			*at = '\0';
			if (sscanf(at + 1, "%f", &refresh_rate) != 1) {
				send_failure(client_fd, "output mode: invalid refresh rate\n");
				return;
			}
		}

		if (sscanf(res, "%dx%d", &width, &height) != 2) {
			send_failure(client_fd, "output mode: invalid resolution format\n");
			return;
		}

		oc->width = width;
		oc->height = height;
		oc->refresh_rate = refresh_rate;
		output_config_apply(oc);
		send_success(client_fd, "output mode set\n");
	} else if (streq("modes", subcmd)) {
		output_t *target = NULL;
		output_t *o;
		wl_list_for_each(o, &mon_list, link) {
			if (streq(o->name, output_name) || streq(o->wlr_output->name, output_name)) {
				target = o;
				break;
			}
		}

		if (!target) {
			send_failure(client_fd, "output modes: no such output\n");
			return;
		}

		struct wlr_output *wo = target->wlr_output;
		char buf[DOORS_BUFSIZ];
		int offset = 0;
		offset += snprintf(buf + offset, sizeof(buf) - offset, "[\n");
		bool first = true;

		struct wlr_output_mode *mode;
		wl_list_for_each(mode, &wo->modes, link) {
			if (!first)
				offset += snprintf(buf + offset, sizeof(buf) - offset, ",\n");

			first = false;
			offset += snprintf(buf + offset, sizeof(buf) - offset, "  {\n"
				"    \"width\": %d,\n"
				"    \"height\": %d,\n"
				"    \"refresh\": %.3f\n"
				"  }", mode->width, mode->height, mode->refresh / 1000.0f);
		}

		offset += snprintf(buf + offset, sizeof(buf) - offset, "\n]\n");
		send_success(client_fd, buf);
	} else if (streq("position", subcmd) || streq("pos", subcmd)) {
		if (num < 3) {
			send_failure(client_fd, "output position: missing coordinates\n");
			return;
		}
		args++;
		num--;

		int x, y;
		if (sscanf(*args, "%d", &x) != 1) {
			send_failure(client_fd, "output position: invalid x\n");
			return;
		}
		args++;
		num--;

		if (sscanf(*args, "%d", &y) != 1) {
			send_failure(client_fd, "output position: invalid y\n");
			return;
		}

		oc->x = x;
		oc->y = y;
		output_config_apply(oc);
		send_success(client_fd, "output position set\n");
	} else if (streq("scale", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output scale: missing scale factor\n");
			return;
		}
		args++;
		num--;

		float scale;
		if (sscanf(*args, "%f", &scale) != 1) {
			send_failure(client_fd, "output scale: invalid scale\n");
			return;
		}

		oc->scale = scale;
		output_config_apply(oc);
		send_success(client_fd, "output scale set\n");
	} else if (streq("transform", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output transform: missing transform\n");
			return;
		}
		args++;
		num--;

		int transform = -1;
		if (streq("normal", *args) || streq("0", *args)) {
			transform = WL_OUTPUT_TRANSFORM_NORMAL;
		} else if (streq("90", *args)) {
			transform = WL_OUTPUT_TRANSFORM_90;
		} else if (streq("180", *args)) {
			transform = WL_OUTPUT_TRANSFORM_180;
		} else if (streq("270", *args)) {
			transform = WL_OUTPUT_TRANSFORM_270;
		} else if (streq("flipped", *args) || streq("flipped-180", *args)) {
			transform = WL_OUTPUT_TRANSFORM_FLIPPED_180;
		} else if (streq("flipped-90", *args)) {
			transform = WL_OUTPUT_TRANSFORM_FLIPPED_90;
		} else if (streq("flipped-270", *args)) {
			transform = WL_OUTPUT_TRANSFORM_FLIPPED_270;
		}

		if (transform < 0) {
			send_failure(client_fd, "output transform: invalid transform\n");
			return;
		}

		oc->transform = transform;
		output_config_apply(oc);
		send_success(client_fd, "output transform set\n");
	} else if (streq("dpms", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output dpms: missing state\n");
			return;
		}
		args++;
		num--;

		if (streq("on", *args)) {
			oc->dpms_state = OUTPUT_CONFIG_DPMS_ON;
		} else if (streq("off", *args)) {
			oc->dpms_state = OUTPUT_CONFIG_DPMS_OFF;
		} else {
			send_failure(client_fd, "output dpms: invalid state (on/off)\n");
			return;
		}

		output_config_apply(oc);
		send_success(client_fd, "output dpms set\n");
	} else if (streq("adaptive_sync", subcmd) || streq("vrr", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output adaptive_sync: missing state\n");
			return;
		}
		args++;
		num--;

		if (streq("on", *args) || streq("enable", *args)) {
			oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED;
		} else if (streq("off", *args) || streq("disable", *args)) {
			oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_DISABLED;
		} else {
			send_failure(client_fd, "output adaptive_sync: invalid state (on/off)\n");
			return;
		}

		output_config_apply(oc);
		send_success(client_fd, "output adaptive_sync set\n");
	} else if (streq("render_bit_depth", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output render_bit_depth: missing value\n");
			return;
		}
		args++;
		num--;

		if (streq("8", *args) || streq("8-bit", *args)) {
			oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_8;
		} else if (streq("10", *args) || streq("10-bit", *args)) {
			oc->render_bit_depth = OUTPUT_CONFIG_RENDER_BIT_DEPTH_10;
		} else {
			send_failure(client_fd, "output render_bit_depth: invalid value (8/10)\n");
			return;
		}

		output_config_apply(oc);
		send_success(client_fd, "output render_bit_depth set\n");
	} else if (streq("color_profile", subcmd)) {
		if (num < 2) {
			send_failure(client_fd,
				"output color_profile: missing profile type (gamma22|srgb|icc <path>)\n");
			return;
		}
		args++;
		num--;

		struct wlr_color_transform *new_transform = NULL;
		if (streq("gamma22", *args)) {
			// no transform
		} else if (streq("srgb", *args)) {
			new_transform =
				wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
			if (!new_transform) {
				send_failure(client_fd, "output color_profile: failed to create sRGB transform\n");
				return;
			}
		} else if (streq("icc", *args)) {
			if (num < 2) {
				send_failure(client_fd, "output color_profile icc: missing file path\n");
				return;
			}
			args++;
			num--;

			int fd = open(*args, O_RDONLY | O_NOCTTY | O_CLOEXEC);
			if (fd == -1) {
				send_failure(client_fd, "output color_profile icc: cannot open file\n");
				return;
			}
			struct stat info;
			if (fstat(fd, &info) == -1 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
				close(fd);
				send_failure(client_fd, "output color_profile icc: invalid file\n");
				return;
			}
			void *icc_data = malloc(info.st_size);
			if (!icc_data) {
				close(fd);
				send_failure(client_fd, "output color_profile icc: out of memory\n");
				return;
			}
			size_t nread = 0;
			while (nread < (size_t)info.st_size) {
				ssize_t r = read(fd, (char *)icc_data + nread, (size_t)info.st_size - nread);
				if ((r == -1 && errno != EINTR) || r == 0) {
					free(icc_data);
					close(fd);
					send_failure(client_fd, "output color_profile icc: read error\n");
					return;
				}
				nread += (size_t)r;
			}
			close(fd);

			new_transform = wlr_color_transform_init_linear_to_icc(icc_data, (size_t)info.st_size);
			free(icc_data);
			if (!new_transform) {
				send_failure(client_fd, "output color_profile icc: failed to initialize ICC transform\n");
				return;
			}
		} else {
			send_failure(client_fd, "output color_profile: invalid type (gamma22|srgb|icc <path>)\n");
			return;
		}

		wlr_color_transform_unref(oc->color_transform);
		oc->color_transform = new_transform;
		output_config_apply(oc);
		send_success(client_fd, "output color_profile set\n");
	} else if (streq("hdr", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output hdr: missing state (on/off)\n");
			return;
		}
		args++;
		num--;

		if (streq("on", *args) || streq("enable", *args) || streq("true", *args)) {
			oc->hdr_enabled = true;
		} else if (streq("off", *args) || streq("disable", *args) || streq("false", *args)) {
			oc->hdr_enabled = false;
		} else {
			send_failure(client_fd, "output hdr: invalid state (on/off)\n");
			return;
		}

		output_config_apply(oc);
		send_success(client_fd, "output hdr set\n");
	} else if (streq("tearing", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output tearing: missing state\n");
			return;
		}
		args++;
		num--;

		if (streq("on", *args) || streq("enable", *args) || streq("true", *args)) {
			oc->allow_tearing = true;
		} else if (streq("off", *args) || streq("disable", *args) || streq("false", *args)) {
			oc->allow_tearing = false;
		} else {
			send_failure(client_fd, "output tearing: invalid state (on/off)\n");
			return;
		}

		output_config_apply(oc);
		send_success(client_fd, "output tearing set\n");
	} else if (streq("max_render_time", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output max_render_time: missing value\n");
			return;
		}
		args++;
		num--;

		if (streq("off", *args)) {
			oc->max_render_time = 0;
		} else {
			char *end;
			int val = (int)strtol(*args, &end, 10);
			if (*end || val <= 0) {
				send_failure(client_fd, "output max_render_time: invalid value (off or <ms>)\n");
				return;
			}
			oc->max_render_time = val;
		}

		output_config_apply(oc);
		send_success(client_fd, "output max_render_time set\n");
	} else if (streq("focus", subcmd) || streq("-f", subcmd) || streq("--focus", subcmd)) {
		if (!mon) {
			send_failure(client_fd, "output focus: no such output\n");
			return;
		}
		server.focused_output = mon;
		focus_node(mon, mon->desk, mon->desk ? mon->desk->focus : NULL);
		send_success(client_fd, "focused\n");
	} else if (streq("rename", subcmd) || streq("-n", subcmd) || streq("--rename", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output rename: missing name argument\n");
			return;
		}
		args++;
		num--;
		strncpy(mon->name, *args, SMALEN - 1);
		mon->name[SMALEN - 1] = '\0';
		transaction_commit_dirty();
		send_success(client_fd, "renamed\n");
	} else if (streq("add-desktops", subcmd) || streq("-a", subcmd) || streq("--add-desktops",
			subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output add-desktops: missing desktop names\n");
			return;
		}
		args++;
		num--;
		while (num > 0) {
			desktop_t *d = (desktop_t *)calloc(1, sizeof(desktop_t));
			if (!d) {
				wlr_log(WLR_ERROR, "allocation failed");
				return;
			}
			d->id = next_desktop_id++;
			strncpy(d->name, *args, SMALEN - 1);
			d->name[SMALEN - 1] = '\0';
			d->layout = LAYOUT_TILED;
			d->user_layout = LAYOUT_TILED;
			d->window_gap = settings.window_gap;
			d->master_stack_count = 1;
			d->padding = (padding_t){0};
			d->root = NULL;
			d->focus = NULL;

			if (wl_list_empty(&mon->desk_list))
				mon->desk = d;
			wl_list_init(&d->link);
			wl_list_insert(mon->desk_list.prev, &d->link);
			d->output = mon;

			workspace_create_desktop(d->name);
			ipc_put_status(SUB_MASK_DESKTOP_ADD, "desktop_add[%s]\n", d->name);

			args++;
			num--;
		}
		transaction_commit_dirty();
		send_success(client_fd, "desktops added\n");
	} else if (streq("desktops", subcmd) || streq("-d", subcmd) || streq("--desktops", subcmd)) {
		if (!mon) {
			send_failure(client_fd, "output desktops: no such output\n");
			return;
		}
		if (num < 2) {
			char buf[DOORS_BUFSIZ];
			size_t offset = 0;
			desktop_t *d;
			wl_list_for_each(d, &mon->desk_list, link) {
				offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", d->name);
			}
			send_success(client_fd, buf);
		} else {
			args++;
			num--;

			desktop_t *d = wl_list_empty(&mon->desk_list) ? NULL : wl_container_of(mon->desk_list.next, d,
				link);
			for (; num > 0 &&
					d != NULL; d = d->link.next == &mon->desk_list ? NULL : wl_container_of(d->link.next, d,
					link)) {
				strncpy(d->name, *args, SMALEN - 1);
				d->name[SMALEN - 1] = '\0';
				workspace_create_desktop(d->name);
				args++;
				num--;
			} while (num > 0) {
				desktop_t *newd = (desktop_t *)calloc(1, sizeof(desktop_t));
				if (!newd) {
					wlr_log(WLR_ERROR, "allocation failed");
					return;
				}
				newd->id = next_desktop_id++;
				strncpy(newd->name, *args, SMALEN - 1);
				newd->name[SMALEN - 1] = '\0';
				newd->layout = LAYOUT_TILED;
				newd->user_layout = LAYOUT_TILED;
				newd->window_gap = settings.window_gap;
				newd->master_stack_count = 1;
				newd->padding = (padding_t){0};
				newd->root = NULL;
				newd->focus = NULL;

				if (wl_list_empty(&mon->desk_list))
					mon->desk = newd;
				wl_list_init(&newd->link);
				wl_list_insert(mon->desk_list.prev, &newd->link);
				newd->output = mon;

				workspace_create_desktop(newd->name);
				args++;
				num--;
			} while (d != NULL) {
				desktop_t *next = d->link.next == &mon->desk_list ? NULL : wl_container_of(d->link.next, d,
					link);
				if (d == mon->desk) {
					mon->desk = next;
					if (mon->desk)
						focus_node(mon, mon->desk, mon->desk->focus);
				}
				wl_list_remove(&d->link);
				free(d);
				d = next;
			}

			transaction_commit_dirty();
			workspace_sync();

			if (mon->desk) {
				focus_node(mon, mon->desk, mon->desk->focus);
			}

			send_success(client_fd, "desktops reset\n");
		}
	} else if (streq("swap-desktops", subcmd) || streq("-s", subcmd) || streq("--swap", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output swap-desktops: missing target output\n");
			return;
		}
		args++;
		num--;

		output_t *target = find_output_by_name(*args);
		if (!target) {
			send_failure(client_fd, "output swap-desktops: target output not found\n");
			return;
		}

		if (target == mon) {
			send_failure(client_fd, "output swap-desktops: cannot swap with self\n");
			return;
		}

		output_t *m0 = mon;
		output_t *m1 = target;

		desktop_t *d0 = m0->desk;
		desktop_t *d1 = m1->desk;

		struct wl_list *a = &m0->desk_list;
		struct wl_list *b = &m1->desk_list;

		if (wl_list_empty(a)) {
			wl_list_insert_list(a, b);
			wl_list_init(b);
		} else if (wl_list_empty(b)) {
			wl_list_insert_list(b, a);
			wl_list_init(a);
		} else {
			struct wl_list *a_first = a->next;
			struct wl_list *a_last = a->prev;
			struct wl_list *b_first = b->next;
			struct wl_list *b_last = b->prev;
			a->next = b_first;
			b_first->prev = a;
			a->prev = b_last;
			b_last->next = a;
			b->next = a_first;
			a_first->prev = b;
			b->prev = a_last;
			a_last->next = b;
		}

		m0->desk = d1;
		m1->desk = d0;

		desktop_t *d;
		wl_list_for_each(d, &m0->desk_list, link)
			d->output = m0;
		wl_list_for_each(d, &m1->desk_list, link)
			d->output = m1;

		if (server.focused_output == m0)
			server.focused_output = m1;
		else if (server.focused_output == m1)
			server.focused_output = m0;

		transaction_commit_dirty();
		send_success(client_fd, "swapped\n");
	} else if (streq("remove", subcmd) || streq("-r", subcmd) || streq("--remove", subcmd)) {
		if (wl_list_length(&mon_list) == 1) {
			send_failure(client_fd, "output remove: cannot remove the only output\n");
			return;
		}

		if (mon->desk) {
			send_failure(client_fd, "output remove: cannot remove output with desktops\n");
			return;
		}

		output_t *remove_next = mon->link.next != &mon_list ? wl_container_of(mon->link.next, mon,
			link) : NULL;
		output_t *remove_prev = mon->link.prev != &mon_list ? wl_container_of(mon->link.prev, mon,
			link) : NULL;
		wl_list_remove(&mon->link);

		if (server.focused_output == mon) {
			server.focused_output = remove_next ? remove_next : remove_prev;
			if (server.focused_output) {
				focus_node(server.focused_output, server.focused_output->desk,
					server.focused_output->desk ? server.focused_output->desk->focus : NULL);
			}
		}

		ipc_put_status(SUB_MASK_MONITOR_REMOVE, "monitor_remove[%s]\n", mon->name);
		free(mon);
		transaction_commit_dirty();
		send_success(client_fd, "removed\n");
	} else if (streq("adaptive_sync", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output adaptive_sync: missing argument (on/off)\n");
			return;
		}
		args++;
		num--;

		if (streq("on", *args) || streq("true", *args) || streq("1", *args)) {
			oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_ENABLED;
		} else if (streq("off", *args) || streq("false", *args) || streq("0", *args)) {
			oc->adaptive_sync = OUTPUT_CONFIG_ADAPTIVE_SYNC_DISABLED;
		} else {
			send_failure(client_fd, "output adaptive_sync: expected \"on\" or \"off\"\n");
			return;
		}
		output_config_apply(oc);
		send_success(client_fd, "adaptive_sync set\n");
	} else if (streq("rectangle", subcmd) || streq("-g", subcmd) || streq("--rectangle", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output rectangle: missing rectangle\n");
			return;
		}
		args++;
		num--;

		int x, y, width, height;
		if (sscanf(*args, "%dx%d:%d,%d", &width, &height, &x, &y) != 4 && sscanf(*args, "%dx%d", &width,
				&height) == 2) {
			x = mon->rectangle.x;
			y = mon->rectangle.y;
		} else if (sscanf(*args, "%d,%d,%d,%d", &x, &y, &width, &height) != 4) {
			send_failure(client_fd, "output rectangle: invalid rectangle format\n");
			return;
		}

		mon->rectangle.x = x;
		mon->rectangle.y = y;
		mon->rectangle.width = width;
		mon->rectangle.height = height;

		ipc_put_status(SUB_MASK_MONITOR_CHANGE, "monitor_change[%s]\n", mon->name);
		transaction_commit_dirty();
		send_success(client_fd, "rectangle set\n");
	} else if (streq("reorder-desktops", subcmd) || streq("-o", subcmd) || streq("--reorder-desktops",
			subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "output reorder-desktops: missing desktop names\n");
			return;
		}
		args++;
		num--;

		desktop_t *d = mon->desk;
		while (d != NULL && num > 0) {
			desktop_t *next = d->link.next == &mon->desk_list ? NULL : wl_container_of(d->link.next, d,
				link);
			for (int i = 0; i < num; i++) {
				if (strcmp(d->name, args[i]) == 0) {
					strncpy(d->name, args[i], SMALEN - 1);
					d->name[SMALEN - 1] = '\0';
					break;
				}
			}
			d = next;
		}

		transaction_commit_dirty();
		send_success(client_fd, "desktops reordered\n");
	} else {
		send_failure(client_fd, "output: unknown subcommand\n");
	}
}
