#include "animation.h"
#include "effects.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "keyboard.h"
#include "output.h"
#include "scratchpad.h"
#include "server.h"
#include "tabs.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "workspace.h"
#include "xwayland.h"
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

void toplevel_map(struct wl_listener *listener, void *data);

static void hide_node_client(node_t *n) {
	n->client->flags.shown = false;
	struct wlr_scene_tree *st = client_get_scene_tree(n->client);
	if (st)
		wlr_scene_node_set_enabled(&st->node, false);
}

static void unhide_leaves(desktop_t *desk) {
	for (node_t *ni = first_extrema(desk->root); ni; ni = next_leaf(ni, desk->root)) {
		if (!ni->client)
			continue;

		ni->client->flags.shown = true;
		bool configured = true;
		if (ni->client->toplevel)
			configured = ni->client->toplevel->configured;

		if (!configured)
			continue;

		struct wlr_scene_tree *st = client_get_scene_tree(ni->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, true);
	}
}

static void hide_leaves(desktop_t *desk) {
	for (node_t *ni = first_extrema(desk->root); ni; ni = next_leaf(ni, desk->root)) {
		if (!ni->client)
			continue;

		ni->client->flags.shown = false;
		struct wlr_scene_tree *st = client_get_scene_tree(ni->client);
		if (st)
			wlr_scene_node_set_enabled(&st->node, false);
	}
}

static void unlink_and_refocus(node_t *n, desktop_t *src, output_t *mon) {
	hide_node_client(n);
	remove_node(src, n);
	if (src->root) {
		node_t *nf = first_extrema(src->root);
		if (nf) {
			src->focus = nf;
			focus_node(mon, src, nf);
		} else {
			src->focus = NULL;
		}
	} else {
		src->focus = NULL;
	}
	n->destroying = false;
	n->ntxnrefs = 0;
}

void ipc_cmd_node(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "node: Missing arguments\n");
		return;
	}

	if (streq("-f", *args) || streq("--focus", *args)) {
		output_t *m = server.focused_output;
		if (m && m->desk && m->desk->focus && m->desk->focus->client) {
			focus_node(m, m->desk, m->desk->focus);
			send_success(client_fd, "focused\n");
		} else {
			send_failure(client_fd, "no focused node\n");
		}
	} else if (streq("-c", *args) || streq("--close", *args)) {
		output_t *m = server.focused_output;
		if (m && m->desk && m->desk->focus && m->desk->focus->client) {
			kill_node(m->desk, m->desk->focus);
			send_success(client_fd, "closed\n");
		} else {
			send_failure(client_fd, "no focused node to close\n");
		}
	} else if (streq("-t", *args) || streq("--state", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -t: missing state argument\n");
			return;
		}
		args++;
		num--;
		if (streq("tiled", *args)) {
			tile_focused();
		} else if (streq("floating", *args)) {
			toggle_floating();
		} else if (streq("fullscreen", *args)) {
			toggle_fullscreen();
		} else {
			send_failure(client_fd, "node -t: unknown state\n");
			return;
		}

		output_t *m = server.focused_output;
		if (m && m->desk && m->desk->focus && m->desk->focus->client) {
			send_success(client_fd, "state changed\n");
		} else {
			send_failure(client_fd, "no focused node\n");
		}
	} else if (streq("-d", *args) || streq("--to-desktop", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -d: missing desktop name\n");
			return;
		}
		args++;
		num--;
		char *desk_name = *args;
		desktop_t *target = find_desktop_by_name(desk_name);
		if (!target) {
			send_failure(client_fd, "node -d: desktop not found\n");
			return;
		}

		output_t *m = server.focused_output;
		if (!m || !m->desk || !m->desk->focus) {
			send_failure(client_fd, "node -d: no focused node\n");
			return;
		}

		if (m->desk == target) {
			send_failure(client_fd, "node -d: already on target desktop\n");
			return;
		}

		node_t *n = m->desk->focus;
		if (n == NULL || n->client == NULL) {
			send_failure(client_fd, "node -d: no client\n");
			return;
		}

		desktop_t *src_desk = m->desk;

		unlink_and_refocus(n, src_desk, m);

		insert_node(target, n, find_public(target));

		target->focus = n;
		if (target == m->desk)
			focus_node(m, target, n);

		if (target == m->desk) {
			unhide_leaves(target);
			arrange(m, target, true);
		} else {
			hide_leaves(target);
			arrange(m, target, false);
		}

		if (src_desk == m->desk) {
			for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter,
					src_desk->root)) {
				if (n_iter->client) {
					n_iter->client->flags.shown = true;
					bool already_configured = true;
					if (n_iter->client->toplevel)
						already_configured = n_iter->client->toplevel->configured;
					if (already_configured) {
						struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
						if (scene_tree)
							wlr_scene_node_set_enabled(&scene_tree->node, true);
					}
				}
			}
			arrange(m, src_desk, true);
		} else if (src_desk->root) {
			for (node_t *n_iter = first_extrema(src_desk->root); n_iter != NULL; n_iter = next_leaf(n_iter,
					src_desk->root)) {
				if (n_iter->client) {
					n_iter->client->flags.shown = false;
					struct wlr_scene_tree *scene_tree = client_get_scene_tree(n_iter->client);
					if (scene_tree)
						wlr_scene_node_set_enabled(&scene_tree->node, false);
				}
			}
		}

		send_success(client_fd, "node sent to desktop\n");
	} else if (streq("-g", *args) || streq("--flag", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -g: missing flag argument\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -g: no focused desktop\n");
			return;
		}

		node_t *n = m->desk->focus;
		if (!n) {
			send_failure(client_fd, "node -g: no focused node\n");
			return;
		}

		char *key = strtok(*args, "=");
		char *val = strtok(NULL, "=");

		bool set_value = false;
		bool has_value = false;

		if (val == NULL) {
			has_value = false;
		} else {
			has_value = true;
			if (strcmp(val, "true") == 0 || strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
				set_value = true;
			} else if (strcmp(val, "false") == 0 || strcmp(val, "off") == 0 || strcmp(val, "0") == 0) {
				set_value = false;
			}
		}

		if (strcmp(key, "hidden") == 0) {
			n->hidden = has_value ? set_value : !n->hidden;
			transaction_commit_dirty();
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "sticky") == 0) {
			n->sticky = has_value ? set_value : !n->sticky;
			transaction_commit_dirty();
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "private") == 0) {
			n->private_node = has_value ? set_value : !n->private_node;
			transaction_commit_dirty();
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "locked") == 0) {
			n->locked = has_value ? set_value : !n->locked;
			transaction_commit_dirty();
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "marked") == 0) {
			n->marked = has_value ? set_value : !n->marked;
			transaction_commit_dirty();
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "blur") == 0) {
			if (!n->client) {
				send_failure(client_fd, "node -g: no client\n");
				return;
			}
			bool new_blur = has_value ? set_value : !n->client->flags.blur;
			n->client->flags.blur = new_blur;
			surface_client_set_effect(n->client, EFFECT_BLUR, new_blur);
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "mica") == 0) {
			if (!n->client) {
				send_failure(client_fd, "node -g: no client\n");
				return;
			}
			bool new_val = has_value ? set_value : !n->client->flags.mica;
			n->client->flags.mica = new_val;
			surface_client_set_effect(n->client, EFFECT_MICA, new_val);
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "acrylic") == 0) {
			if (!n->client) {
				send_failure(client_fd, "node -g: no client\n");
				return;
			}
			bool new_val = has_value ? set_value : !n->client->flags.acrylic;
			n->client->flags.acrylic = new_val;
			surface_client_set_effect(n->client, EFFECT_ACRYLIC, new_val);
			send_success(client_fd, "flag changed\n");
		} else if (strcmp(key, "shadow") == 0) {
			if (!n->client) {
				send_failure(client_fd, "node -g: no client\n");
				return;
			}
			bool new_val = has_value ? set_value : !n->client->flags.shadow;
			n->client->flags.shadow = new_val;
			n->client->shadow_size = shadow_size;
			n->client->shadow_offset_x = shadow_offset_x;
			n->client->shadow_offset_y = shadow_offset_y;
			memcpy(n->client->shadow_color, shadow_color, sizeof(shadow_color));
			surface_client_set_shadow(n->client, new_val);
			send_success(client_fd, "flag changed\n");
		} else if (strncmp(key, "border_radius", 13) == 0) {
			if (!n->client) {
				send_failure(client_fd, "node -g: no client\n");
				return;
			}
			float r = atof(key + 14);
			surface_client_set_border_radius(n->client, r);
			send_success(client_fd, "border_radius set\n");
		} else if (strncmp(key, "opacity", 7) == 0) {
			if (!n->client) {
				send_failure(client_fd, "node -g: no client\n");
				return;
			}
			float o = atof(key + 8);

			if (o < 0 || o > 1.0) {
				send_failure(client_fd, "node -g: opacity out of range 0.0-1.0\n");
				return;
			}

			n->client->opacity = o;

			struct wlr_scene_node *node = NULL;
			if (n->client->toplevel)
				node = &n->client->toplevel->scene_tree->node;
			if (n->client->xwayland_view)
				node = &n->client->xwayland_view->scene_tree->node;

			if (node) {
				surface_set_opacity(node, o);
				send_success(client_fd, "opacity set\n");
			} else {
				send_failure(client_fd, "node -g: no toplevel or xwayland view\n");
			}
		} else {
			send_failure(client_fd, "node -g: unknown flag\n");
			return;
		}
	} else if (streq("-S", *args) || streq("--scratchpad", *args)) {
		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -S: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n || !n->client) {
			send_failure(client_fd, "node -S: no focused client\n");
			return;
		}
		scratchpad_add(n);
		send_success(client_fd, "sent to scratchpad\n");
	} else if (streq("-v", *args) || streq("--move", *args)) {
		if (num < 3) {
			send_failure(client_fd, "node -v: missing delta arguments\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -v: no focused desktop\n");
			return;
		}

		node_t *n = m->desk->focus;
		if (!n || !n->client) {
			send_failure(client_fd, "node -v: no focused client\n");
			return;
		}

		int dx = 0, dy = 0;
		if (sscanf(*args, "%d", &dx) != 1) {
			send_failure(client_fd, "node -v: invalid dx\n");
			return;
		}
		args++;
		num--;

		if (sscanf(*args, "%d", &dy) != 1) {
			send_failure(client_fd, "node -v: invalid dy\n");
			return;
		}

		n->client->state = STATE_FLOATING;
		n->client->floating_rectangle.x += dx;
		n->client->floating_rectangle.y += dy;

		struct wlr_scene_tree *st = client_get_scene_tree(n->client);
		if (st)
			wlr_scene_node_set_position(&st->node, n->client->floating_rectangle.x,
				n->client->floating_rectangle.y);

		transaction_commit_dirty();
		send_success(client_fd, "moved\n");
	} else if (streq("-z", *args) || streq("--resize", *args)) {
		if (num < 4) {
			send_failure(client_fd, "node -z: missing arguments\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -z: no focused desktop\n");
			return;
		}

		node_t *n = m->desk->focus;
		if (!n || !n->client) {
			send_failure(client_fd, "node -z: no focused client\n");
			return;
		}

		char *handle = *args;
		int dx = 0, dy = 0;

		args++;
		num--;

		if (sscanf(*args, "%d", &dx) != 1) {
			send_failure(client_fd, "node -z: invalid dx\n");
			return;
		}
		args++;
		num--;

		if (sscanf(*args, "%d", &dy) != 1) {
			send_failure(client_fd, "node -z: invalid dy\n");
			return;
		}

		n->client->state = STATE_FLOATING;

		if (strcmp(handle, "northwest") == 0 || strcmp(handle, "nw") == 0 || strcmp(handle,
				"left") == 0) {
			n->client->floating_rectangle.x += dx;
			n->client->floating_rectangle.y += dy;
			n->client->floating_rectangle.width -= dx;
			n->client->floating_rectangle.height -= dy;
		} else if (strcmp(handle, "north") == 0 || strcmp(handle, "n") == 0) {
			n->client->floating_rectangle.y += dy;
			n->client->floating_rectangle.height -= dy;
		} else if (strcmp(handle, "northeast") == 0 || strcmp(handle, "ne") == 0) {
			n->client->floating_rectangle.y += dy;
			n->client->floating_rectangle.width += dx;
			n->client->floating_rectangle.height -= dy;
		} else if (strcmp(handle, "east") == 0 || strcmp(handle, "e") == 0 || strcmp(handle,
				"right") == 0) {
			n->client->floating_rectangle.width += dx;
		} else if (strcmp(handle, "southeast") == 0 || strcmp(handle, "se") == 0) {
			n->client->floating_rectangle.width += dx;
			n->client->floating_rectangle.height += dy;
		} else if (strcmp(handle, "south") == 0 || strcmp(handle, "s") == 0) {
			n->client->floating_rectangle.height += dy;
		} else if (strcmp(handle, "southwest") == 0 || strcmp(handle, "sw") == 0) {
			n->client->floating_rectangle.x += dx;
			n->client->floating_rectangle.width -= dx;
			n->client->floating_rectangle.height += dy;
		} else if (strcmp(handle, "west") == 0 || strcmp(handle, "w") == 0) {
			n->client->floating_rectangle.x += dx;
			n->client->floating_rectangle.width -= dx;
		} else if (strcmp(handle, "center") == 0 || strcmp(handle, "c") == 0) {
			n->client->floating_rectangle.x += dx;
			n->client->floating_rectangle.y += dy;
			n->client->floating_rectangle.width += dx;
			n->client->floating_rectangle.height += dy;
		} else {
			send_failure(client_fd, "node -z: invalid resize handle\n");
			return;
		}

		if (n->client->floating_rectangle.width < 50)
			n->client->floating_rectangle.width = 50;
		if (n->client->floating_rectangle.height < 50)
			n->client->floating_rectangle.height = 50;

		struct wlr_scene_tree *st = client_get_scene_tree(n->client);
		if (st) {
			wlr_scene_node_set_position(&st->node, n->client->floating_rectangle.x,
				n->client->floating_rectangle.y);
			if (n->client->toplevel)
				wlr_xdg_toplevel_set_size(n->client->toplevel->xdg_toplevel, n->client->floating_rectangle.width,
					n->client->floating_rectangle.height);
			else if (n->client->xwayland_view)
				wlr_xwayland_surface_configure(n->client->xwayland_view->xwayland_surface,
					n->client->floating_rectangle.x, n->client->floating_rectangle.y,
					n->client->floating_rectangle.width, n->client->floating_rectangle.height);
		}

		transaction_commit_dirty();
		send_success(client_fd, "resized\n");
	} else if (streq("-a", *args) || streq("--activate", *args)) {
		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -a: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n) {
			send_failure(client_fd, "node -a: no focused node\n");
			return;
		}
		activate_node(m, m->desk, n);
		send_success(client_fd, "activated\n");
	} else if (streq("-k", *args) || streq("--kill", *args)) {
		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -k: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n) {
			send_failure(client_fd, "node -k: no focused node\n");
			return;
		}
		kill_node(m->desk, n);
		transaction_commit_dirty();
		send_success(client_fd, "killed\n");
	} else if (streq("-m", *args) || streq("--to-monitor", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -m: missing monitor name\n");
			return;
		}
		args++;
		num--;

		bool set_focus = false;
		char *target_name = NULL;
		while (num > 0) {
			if (streq("--follow", *args)) {
				set_focus = true;
			} else if (target_name == NULL) {
				target_name = *args;
			} else {
				send_failure(client_fd, "node -m: unexpected argument\n");
				return;
			}
			args++;
			num--;
		}

		if (target_name == NULL) {
			send_failure(client_fd, "node -m: missing monitor name\n");
			return;
		}

		output_t *target = find_output_by_name(target_name);
		if (!target) {
			send_failure(client_fd, "node -m: monitor not found\n");
			return;
		}

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -m: no focused desktop\n");
			return;
		}

		if (m == target) {
			send_failure(client_fd, "node -m: already on target monitor\n");
			return;
		}

		node_t *n = m->desk->focus;
		if (!n || !n->client) {
			send_failure(client_fd, "node -m: no client\n");
			return;
		}

		desktop_t *src_desk = m->desk;
		desktop_t *target_desk = target->desk ? target->desk : (wl_list_empty(&target->desk_list) ? NULL :
			wl_container_of(target->desk_list.next, target_desk, link));

		if (!target_desk) {
			send_failure(client_fd, "node -m: target monitor has no desktops");
			return;
		}

		unlink_and_refocus(n, src_desk, m);

		insert_node(target_desk, n, find_public(target_desk));
		target_desk->focus = n;

		unhide_leaves(target_desk);

		arrange(target, target_desk, true);
		arrange(m, src_desk, src_desk->root != NULL);
		if (set_focus)
			focus_node(target, target_desk, n);

		send_success(client_fd, "node sent to monitor\n");
	} else if (streq("-n", *args) || streq("--to-node", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -n: missing target node\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -n: no focused desktop\n");
			return;
		}

		node_t *n1 = m->desk->focus;
		if (!n1 || !n1->client) {
			send_failure(client_fd, "node -n: no focused client\n");
			return;
		}

		node_t *n2 = NULL;
		int target_id = atoi(*args);
		if (target_id > 0) {
			for (node_t *n = first_extrema(m->desk->root); n != NULL; n = next_leaf(n, m->desk->root)) {
				if (n->id == (uint32_t)target_id) {
					n2 = n;
					break;
				}
			}
		}

		if (!n2) {
			send_failure(client_fd, "node -n: target node not found\n");
			return;
		}

		if (n1 == n2) {
			send_failure(client_fd, "node -n: cannot transfer to self\n");
			return;
		}

		desktop_t *src_desk = m->desk;
		desktop_t *target_desk = src_desk;

		unlink_and_refocus(n1, src_desk, m);

		if (n2->first_child) {
			n1->parent = n2;
			n2->second_child = n1;
		} else {
			n1->parent = n2;
			n2->first_child = n1;
		}

		target_desk->focus = n1;
		if (target_desk == m->desk)
			focus_node(m, target_desk, n1);

		unhide_leaves(target_desk);

		arrange(m, target_desk, true);
		if (src_desk != target_desk)
			arrange(m, src_desk, src_desk->root != NULL);

		send_success(client_fd, "node sent to node\n");
	} else if (streq("-l", *args) || streq("--layer", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -l: missing layer argument\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -l: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n || !n->client) {
			send_failure(client_fd, "node -l: no client\n");
			return;
		}

		stack_layer_t layer;
		if (streq("below", *args)) {
			layer = LAYER_BELOW;
		} else if (streq("normal", *args)) {
			layer = LAYER_NORMAL;
		} else if (streq("above", *args)) {
			layer = LAYER_ABOVE;
		} else {
			send_failure(client_fd, "node -l: unknown layer (use below, normal, or above)\n");
			return;
		}

		n->client->layer = layer;
		transaction_commit_dirty();
		send_success(client_fd, "layer changed\n");
	} else if (streq("-y", *args) || streq("--type", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -y: missing type argument\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -y: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n) {
			send_failure(client_fd, "node -y: no focused node\n");
			return;
		}

		if (streq("next_tab", *args) || streq("next.tab", *args)) {
			node_t *t = tabbed_ancestor(n);
			if (t == NULL) {
				send_failure(client_fd, "node -y: focused node not in tab group\n");
				return;
			}
			node_t *next = tab_next_leaf(t, n);
			if (next != NULL) {
				focus_node(m, m->desk, next);
				arrange(m, m->desk, true);
			}
			send_success(client_fd, "next tab\n");
			return;
		}
		if (streq("prev_tab", *args) || streq("prev.tab", *args)) {
			node_t *t = tabbed_ancestor(n);
			if (t == NULL) {
				send_failure(client_fd, "node -y: focused node not in tab group\n");
				return;
			}
			node_t *prev = tab_prev_leaf(t, n);
			if (prev != NULL) {
				focus_node(m, m->desk, prev);
				arrange(m, m->desk, true);
			}
			send_success(client_fd, "prev tab\n");
			return;
		}
		if (streq("tabbed", *args) || streq("horizontal", *args) || streq("vertical", *args)) {
			node_t *target = n->parent;
			if (target == NULL) {
				send_failure(client_fd, "node -y: focused node has no parent\n");
				return;
			}
			split_type_t prev_st = target->split_type;
			split_type_t st = TYPE_HORIZONTAL;
			if (streq("tabbed", *args))
				st = TYPE_TABBED;
			else if (streq("vertical", *args))
				st = TYPE_VERTICAL;
			target->split_type = st;
			target->pending.split_type = st;
			target->current.split_type = st;

			if (prev_st == TYPE_TABBED && st != TYPE_TABBED) {
				tabs_destroy(target);
				for (node_t *leaf = first_extrema(target); leaf != NULL && leaf != target; leaf = next_leaf(leaf,
						target)) {
					if (leaf->client == NULL)
						continue;
					if (leaf->client->state == STATE_FLOATING)
						continue;
					leaf->client->flags.shown = true;
					struct wlr_scene_tree *stree = client_get_scene_tree(leaf->client);
					if (stree)
						wlr_scene_node_set_enabled(&stree->node, true);
				}
			}

			arrange(m, m->desk, true);

			// reapply decoration mode for all leaves
			for (node_t *leaf = first_extrema(target); leaf != NULL && leaf != target; leaf = next_leaf(leaf,
					target)) {
				if (leaf->client && leaf->client->toplevel)
					toplevel_apply_decoration_mode(leaf->client->toplevel);
			}

			if (m->desk->focus != NULL)
				focus_node(m, m->desk, m->desk->focus);
			send_success(client_fd, "type changed\n");
			return;
		}
		n->split_type = (n->split_type + 1) % 2;

		transaction_commit_dirty();
		send_success(client_fd, "type changed\n");
	} else if (streq("-r", *args) || streq("--ratio", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -r: missing ratio argument\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -r: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n) {
			send_failure(client_fd, "node -r: no focused node\n");
			return;
		}

		double rat;
		if ((*args)[0] == '+' || (*args)[0] == '-') {
			float delta;
			if (sscanf(*args, "%f", &delta) == 1) {
				if (delta > -1 && delta < 1) {
					rat = n->split_ratio + delta;
				} else {
					int max = (n->split_type == TYPE_HORIZONTAL) ? n->rectangle.height : n->rectangle.width;
					rat = ((max * n->split_ratio) + delta) / max;
				}
			} else {
				send_failure(client_fd, "node -r: invalid argument\n");
				return;
			}
		} else {
			if (sscanf(*args, "%lf", &rat) != 1) {
				send_failure(client_fd, "node -r: invalid argument\n");
				return;
			}
		}

		if (rat > 0 && rat < 1) {
			n->split_ratio = rat;
			transaction_commit_dirty();
			send_success(client_fd, "ratio changed\n");
		} else {
			send_failure(client_fd, "node -r: ratio out of range\n");
			return;
		}
	} else if (streq("-C", *args) || streq("--circulate", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -C: missing direction\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -C: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n) {
			send_failure(client_fd, "node -C: no focused node\n");
			return;
		}

		if (streq("forward", *args) || streq("f", *args)) {
			node_t *next = next_leaf(n, m->desk->root);
			if (next) {
				m->desk->focus = next;
				focus_node(m, m->desk, next);
			}
		} else if (streq("backward", *args) || streq("b", *args)) {
			node_t *prev = prev_leaf(n, m->desk->root);
			if (prev) {
				m->desk->focus = prev;
				focus_node(m, m->desk, prev);
			}
		} else {
			send_failure(client_fd, "node -C: unknown direction\n");
			return;
		}

		send_success(client_fd, "circulated\n");
	} else if (streq("-i", *args) || streq("--insert-receptacle", *args)) {
		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -i: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;

		node_t *receptacle = make_node(0);
		receptacle->vacant = true;
		receptacle->split_type = TYPE_VERTICAL;
		receptacle->split_ratio = 0.5;

		if (n && !is_leaf(n)) {
			if (n->first_child) {
				receptacle->parent = n;
				n->second_child->parent = receptacle;
				receptacle->first_child = n->second_child;
				n->second_child = receptacle;
			} else {
				n->first_child = receptacle;
				receptacle->parent = n;
			}
		} else if (n) {
			node_t *parent = n->parent;
			if (parent) {
				if (parent->first_child == n) {
					parent->first_child = receptacle;
				} else {
					parent->second_child = receptacle;
				}
				receptacle->parent = parent;

				if (n->split_type == TYPE_VERTICAL) {
					receptacle->first_child = n;
					n->parent = receptacle;
				} else {
					receptacle->second_child = n;
					n->parent = receptacle;
				}
			} else {
				m->desk->root = receptacle;
				receptacle->first_child = n;
				n->parent = receptacle;
			}
			receptacle->split_type = TYPE_VERTICAL;
		} else {
			m->desk->root = receptacle;
		}

		transaction_commit_dirty();
		send_success(client_fd, "receptacle inserted\n");
	} else if (streq("-p", *args) || streq("--presel-dir", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -p: missing direction\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -p: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n || n->vacant) {
			send_failure(client_fd, "node -p: no valid node\n");
			return;
		}

		if (streq("cancel", *args)) {
			if (n->presel) {
				free(n->presel);
				n->presel = NULL;
			}
			send_success(client_fd, "presel cancelled\n");
			return;
		}

		direction_t dir;
		if (streq("west", *args) || streq("w", *args)) {
			dir = DIR_WEST;
		} else if (streq("east", *args) || streq("e", *args)) {
			dir = DIR_EAST;
		} else if (streq("north", *args) || streq("n", *args)) {
			dir = DIR_NORTH;
		} else if (streq("south", *args) || streq("s", *args)) {
			dir = DIR_SOUTH;
		} else {
			send_failure(client_fd, "node -p: unknown direction\n");
			return;
		}

		presel_dir(n, dir);
		transaction_commit_dirty();
		send_success(client_fd, "presel set\n");
	} else if (streq("-o", *args) || streq("--presel-ratio", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -o: missing ratio\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -o: no focused desktop\n");
			return;
		}
		node_t *n = m->desk->focus;
		if (!n || n->vacant) {
			send_failure(client_fd, "node -o: no valid node\n");
			return;
		}

		double rat;
		if (sscanf(*args, "%lf", &rat) != 1 || rat <= 0 || rat >= 1) {
			send_failure(client_fd, "node -o: invalid ratio\n");
			return;
		}

		if (!n->presel) {
			n->presel = make_presel();
		}
		n->presel->split_ratio = rat;

		transaction_commit_dirty();
		send_success(client_fd, "presel ratio set\n");
	} else if (streq("-s", *args) || streq("--swap", *args)) {
		if (num < 2) {
			send_failure(client_fd, "node -s: missing target node\n");
			return;
		}
		args++;
		num--;

		output_t *m = server.focused_output;
		if (!m || !m->desk) {
			send_failure(client_fd, "node -s: no focused desktop\n");
			return;
		}

		node_t *n1 = m->desk->focus;
		if (!n1) {
			send_failure(client_fd, "node -s: no focused node\n");
			return;
		}

		node_t *n2 = NULL;
		int target_id = atoi(*args);
		if (target_id > 0) {
			for (node_t *n = first_extrema(m->desk->root); n != NULL; n = next_leaf(n, m->desk->root)) {
				if (n->id == (uint32_t)target_id) {
					n2 = n;
					break;
				}
			}
		}

		if (!n2) {
			send_failure(client_fd, "node -s: target node not found\n");
			return;
		}

		if (n1 == n2) {
			send_failure(client_fd, "node -s: cannot swap with self\n");
			return;
		}

		swap_nodes(m, m->desk, n1, m, m->desk, n2);
		m->desk->focus = n2;
		transaction_commit_dirty();
		send_success(client_fd, "swapped\n");
	} else {
		send_failure(client_fd, "node: unknown command\n");
	}
}
