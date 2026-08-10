#include "animation.h"
#include "ipc.h"
#include "once.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "workspace.h"
#include <stdint.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>

static void handle_workspace_request(struct wl_listener *listener, void *data);
static void update_all_toplevels_visibility(output_t *m, desktop_t *current_desktop);

static struct wlr_ext_workspace_handle_v1 *find_workspace_by_name(const char *name) {
	struct wlr_ext_workspace_handle_v1 *workspace;
	wl_list_for_each(workspace, &server.workspace_manager->workspaces, link)
		if (strcmp(workspace->name, name) == 0)
			return workspace;

	return NULL;
}

static struct wlr_box window_target_rect(node_t *n) {
	if (n && n->client && n->client->state == STATE_FLOATING)
		return n->client->floating_rectangle;
	return n ? n->client->tiled_rectangle : (struct wlr_box){
		0,
		0,
		0,
		0
	};
}

typedef struct {
	desktop_t *desk;
	node_t *tree_cursor;
	struct toplevel_t *tl_cursor;
	struct xwayland_toplevel_t *xw_cursor;
} desktop_window_iter_t;

static bool node_is_outside_tree(node_t *n, desktop_t *d) {
	for (node_t *p = n; p; p = p->parent)
		if (p == d->root)
			return false;
	return true;
}

static bool desktop_window_iter_advance(desktop_window_iter_t *it, node_t **out_node,
		struct wlr_scene_tree **out_tree) {
	if (!it)
		return false;

	while (true) {
		if (it->tree_cursor) {
			node_t *n = it->tree_cursor;
			it->tree_cursor = it->desk->root ? next_leaf(n, it->desk->root) : NULL;
			if (n && n->client) {
				struct wlr_scene_tree *tree = client_get_scene_tree(n->client);
				if (tree) {
					*out_node = n;
					*out_tree = tree;
					return true;
				}
			}
			continue;
		}

		if (it->tl_cursor) {
			struct toplevel_t *tl = it->tl_cursor;
			it->tl_cursor = (tl->link.next == &server.toplevels) ? NULL : wl_container_of(tl->link.next, tl,
				link);
			if (tl->mapped && tl->scene_tree && tl->node && tl->node->client &&
					tl->node->desktop == it->desk && node_is_outside_tree(tl->node, it->desk)) {
				*out_node = tl->node;
				*out_tree = tl->scene_tree;
				return true;
			}
			continue;
		}

		if (it->xw_cursor) {
			struct xwayland_toplevel_t *xw = it->xw_cursor;
			it->xw_cursor = (xw->link.next == &server.xwayland.views) ? NULL : wl_container_of(xw->link.next,
				xw, link);
			if (xw->mapped && xw->scene_tree && xw->node && xw->node->client &&
					xw->node->desktop == it->desk && node_is_outside_tree(xw->node, it->desk)) {
				*out_node = xw->node;
				*out_tree = xw->scene_tree;
				return true;
			}
			continue;
		}

		return false;
	}
}

static void desktop_window_iter_init(desktop_window_iter_t *it, desktop_t *d) {
	memset(it, 0, sizeof(*it));
	it->desk = d;
	if (d && d->root)
		it->tree_cursor = first_extrema(d->root);
	it->tl_cursor = wl_container_of(server.toplevels.next, (struct toplevel_t *)0, link);
	it->xw_cursor = wl_container_of(server.xwayland.views.next, (struct xwayland_toplevel_t *)0, link);
}

struct desktop_t *find_desktop_by_name(const char *name) {
	if (!name || name[0] == '\0')
		return NULL;

	if (name[0] == '^' && name[1] >= '1' && name[1] <= '9') {
		int mon_idx = name[1] - '1';
		output_t *m = mon_head;

		for (int i = 0; m != NULL && i < mon_idx; m = m->next, i++)
			;

		if (m && m->desk_head)
			return m->desk_head;

		return NULL;
	}

	output_t *m = mon_head;
	while (m != NULL) {
		desktop_t *d = m->desk_head;

		while (d != NULL) {
			if (strcmp(d->name, name) == 0)
				return d;

			d = d->next;
		}
		m = m->next;
	}
	return NULL;
}

void workspace_sync(void) {
	if (!server.workspace_manager)
		return;

	struct wlr_ext_workspace_group_handle_v1 *group = NULL;
	if (!wl_list_empty(&server.workspace_manager->groups))
		group = wl_container_of(server.workspace_manager->groups.next, group, link);

	struct wlr_ext_workspace_handle_v1 *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &server.workspace_manager->workspaces, link)
		wlr_ext_workspace_handle_v1_destroy(workspace);

	if (!mon_head)
		return;

	output_t *m = mon_head;
	while (m) {
		desktop_t *d = m->desk_head;
		while (d != NULL) {
			struct wlr_ext_workspace_handle_v1 *workspace =
				wlr_ext_workspace_handle_v1_create(server.workspace_manager, NULL, 0);
			if (!workspace) {
				wlr_log(WLR_ERROR, "Failed to create workspace: %s", d->name);
				d = d->next;
				continue;
			}

			wlr_ext_workspace_handle_v1_set_name(workspace, d->name);
			wlr_ext_workspace_handle_v1_set_group(workspace, group);

			d = d->next;
		}
		m = m->next;
	}

	struct wlr_ext_workspace_handle_v1 *active = find_workspace_by_name(mon_head->desk->name);
	if (active)
		wlr_ext_workspace_handle_v1_set_active(active, true);

	wlr_log(WLR_INFO, "Workspace manager synced");
}

void workspace_create_desktop(const char *name) {
	if (!server.workspace_manager)
		return;

	if (find_workspace_by_name(name)) {
		wlr_log(WLR_DEBUG, "Workspace already exists: %s", name);
		return;
	}

	struct wlr_ext_workspace_group_handle_v1 *group = NULL;
	if (!wl_list_empty(&server.workspace_manager->groups))
		group = wl_container_of(server.workspace_manager->groups.next, group, link);

	struct wlr_ext_workspace_handle_v1 *workspace =
		wlr_ext_workspace_handle_v1_create(server.workspace_manager, NULL, 0);
	if (!workspace) {
		wlr_log(WLR_ERROR, "Failed to create workspace: %s", name);
		return;
	}

	wlr_ext_workspace_handle_v1_set_name(workspace, name);
	wlr_ext_workspace_handle_v1_set_group(workspace, group);
	wlr_ext_workspace_handle_v1_set_hidden(workspace, true);

	wlr_log(WLR_INFO, "Created workspace: %s", name);
}

static void update_window_visibility(node_t *node, output_t *m, desktop_t *current_desktop,
		int *count) {
	if (!node)
		return;

	client_t *c = node->client;
	if (!c || (uintptr_t)c == 0xDEADBEEF)
		return;

	struct wlr_scene_tree *scene_tree = client_get_scene_tree(c);
	if (!scene_tree)
		return;

	(*count)++;

	output_t *node_mon = node->output;
	if (!node_mon || node_mon != m)
		return;

	bool should_show = false;
	bool found = false;
	desktop_t *d = m->desk_head;
	while (d != NULL) {
		if (d->root != NULL) {
			node_t *parent = node;
			while (parent != NULL) {
				if (parent == d->root) {
					// this node belongs to desktop d
					should_show = (d == current_desktop);
					found = true;
					goto found_desktop;
				}
				parent = parent->parent;
			}
		}
		d = d->next;
	}

	if (!found) {
		if (node->scratchpad) {
			should_show = (node->desktop == current_desktop);
			found = true;
		} else if (node->client && (node->client->state == STATE_FLOATING ||
				node->client->state == STATE_FULLSCREEN) && node->desktop != NULL) {
			should_show = (node->desktop == current_desktop);
			found = true;
		} else {
			should_show = false;
		}
	}

found_desktop:
	// in monocle layout, only the focused node should be visible
	if (should_show && current_desktop->layout == LAYOUT_MONOCLE)
		should_show = (node == current_desktop->focus);

	if (should_show) {
		node->client->flags.shown = true;
		bool already_configured = true;
		if (node->client->toplevel)
			already_configured = node->client->toplevel->configured;
		if (already_configured)
			wlr_scene_node_set_enabled(&scene_tree->node, true);
	} else {
		node->client->flags.shown = false;
		wlr_scene_node_set_enabled(&scene_tree->node, false);
	}
}

static void update_all_toplevels_visibility(output_t *m, desktop_t *current_desktop) {
	int window_count = 0;

	struct toplevel_t *toplevel;
	wl_list_for_each(toplevel, &server.toplevels, link) {
		if (!toplevel->mapped || !toplevel->scene_tree || !toplevel->node)
			continue;
		update_window_visibility(toplevel->node, m, current_desktop, &window_count);
	}

	desktop_t *d = m->desk_head;
	while (d != NULL) {
		if (d->root != NULL) {
			node_t *n = first_extrema(d->root);
			while (n != NULL) {
				if (n->client && n->client->xwayland_view)
					update_window_visibility(n, m, current_desktop, &window_count);
				n = next_leaf(n, d->root);
			}
		}
		d = d->next;
	}

	struct xwayland_toplevel_t *xwayland_view;
	wl_list_for_each(xwayland_view, &server.xwayland.views, link) {
		if (!xwayland_view->mapped || !xwayland_view->scene_tree || !xwayland_view->node)
			continue;

		client_state_t st = xwayland_view->node->client ? xwayland_view->node->client->state :
			STATE_TILED;
		if (st == STATE_FLOATING || st == STATE_FULLSCREEN)
			update_window_visibility(xwayland_view->node, m, current_desktop, &window_count);
	}
}

static void workspace_switch_animate(output_t *output, desktop_t *old_desk, desktop_t *new_desk) {
	int dx = 0, dy = 0;
	int slide_dist;
	bool forward = true;

	// skip if animations are disabled
	if (old_desk == new_desk || !enable_animations)
		return;

	int num_steps = 0;
	desktop_t *walk = old_desk;
	while (walk) {
		if (walk == new_desk) {
			forward = false;
			break;
		}
		walk = walk->next;
		num_steps++;
	}

	if (forward) {
		num_steps = 0;
		walk = old_desk;
		while (walk) {
			if (walk == new_desk)
				break;
			walk = walk->prev;
			num_steps++;
		}
	}

	if (workspace_anim_direction == WORKSPACE_ANIM_VERTICAL) {
		slide_dist = output->height;
		dy = forward ? slide_dist : -slide_dist;
		if (workspace_anim_slide_up)
			dy = -dy;
	} else {
		slide_dist = output->width;
		dx = forward ? slide_dist : -slide_dist;
		if (workspace_anim_slide_up)
			dx = -dx;
	}

	// create slide-out animations for old desktop windows
	{
		desktop_window_iter_t it;
		desktop_window_iter_init(&it, old_desk);
		node_t *n;
		struct wlr_scene_tree *tree;
		while (desktop_window_iter_advance(&it, &n, &tree)) {
			if (!tree || !tree->node.enabled)
				continue;
			struct wlr_box from = {
				tree->node.x,
				tree->node.y,
				0,
				0
			};
			struct wlr_box to = {
				from.x + num_steps * dx,
				from.y + num_steps * dy,
				0,
				0
			};
			animation_start_workspace_slide(output, n, tree, from, to, true);
		}
	}

	output->desk = new_desk;
	ipc_put_status(SUB_MASK_REPORT, NULL);
	ipc_put_status(SUB_MASK_DESKTOP_FOCUS, "desktop_focus[%s]\n", new_desk->name);

	// update visibility
	update_all_toplevels_visibility(output, new_desk);

	// re-enable old desktop windows for slide-out animation
	{
		desktop_window_iter_t it;
		desktop_window_iter_init(&it, old_desk);
		node_t *n;
		struct wlr_scene_tree *tree;
		while (desktop_window_iter_advance(&it, &n, &tree)) {
			if (!tree)
				continue;
			n->client->flags.shown = true;
			wlr_scene_node_set_enabled(&tree->node, true);
		}
	}

	// enable and animate intermediate desktop windows (between old and new)
	int k = 1;
	desktop_t *intermediate = forward ? old_desk->prev : old_desk->next;
	while (intermediate && intermediate != new_desk) {
		arrange(output, intermediate, true);
		desktop_window_iter_t it;
		desktop_window_iter_init(&it, intermediate);
		node_t *n;
		struct wlr_scene_tree *tree;
		while (desktop_window_iter_advance(&it, &n, &tree)) {
			if (!tree)
				continue;
			n->client->flags.shown = true;
			wlr_scene_node_set_enabled(&tree->node, true);
			struct wlr_box target = window_target_rect(n);
			struct wlr_box from = {
				target.x - k * dx,
				target.y - k * dy,
				0,
				0
			};
			struct wlr_box to = {
				target.x + (num_steps - k) * dx,
				target.y + (num_steps - k) * dy,
				0,
				0
			};
			wlr_scene_node_set_position(&tree->node, from.x, from.y);
			animation_start_workspace_slide(output, n, tree, from, to, true);
		}
		k++;
		intermediate = forward ? intermediate->prev : intermediate->next;
	}

	{
		desktop_window_iter_t it;
		desktop_window_iter_init(&it, new_desk);
		node_t *n;
		struct wlr_scene_tree *tree;
		while (desktop_window_iter_advance(&it, &n, &tree)) {
			if (!tree)
				continue;
			wlr_scene_node_set_enabled(&tree->node, true);
			wlr_scene_node_set_position(&tree->node, tree->node.x - num_steps * dx,
				tree->node.y - num_steps * dy);
		}
	}

	// arrange new desktop
	if (new_desk->root) {
		arrange(output, new_desk, true);
		if (new_desk->focus)
			focus_node(output, new_desk, new_desk->focus);
	}

	// override transaction's animation for new windows
	{
		desktop_window_iter_t it;
		desktop_window_iter_init(&it, new_desk);
		node_t *n;
		struct wlr_scene_tree *tree;
		while (desktop_window_iter_advance(&it, &n, &tree)) {
			if (!tree)
				continue;
			struct wlr_box target = window_target_rect(n);
			struct wlr_box from = {
				target.x - num_steps * dx,
				target.y - num_steps * dy,
				0,
				0
			};
			wlr_scene_node_set_position(&tree->node, from.x, from.y);
			animation_start_workspace_slide(output, n, tree, from, target, false);
		}
	}

	// update workspace protocol
	struct wlr_ext_workspace_handle_v1 *old_ws = workspace_get_active();
	if (old_ws)
		wlr_ext_workspace_handle_v1_set_active(old_ws, false);

	struct wlr_ext_workspace_handle_v1 *new_ws = find_workspace_by_name(new_desk->name);
	if (new_ws) {
		wlr_ext_workspace_handle_v1_set_active(new_ws, true);
		wlr_ext_workspace_handle_v1_set_hidden(new_ws, false);
	}

	output_schedule_frame(output);
	wlr_log(WLR_DEBUG, "Switched from %s to %s (animated slide %s)", old_desk ? old_desk->name : "NULL",
		new_desk->name, workspace_anim_direction == WORKSPACE_ANIM_VERTICAL ? "vertical" : "horizontal");
}

void workspace_switch_to_desktop(const char *name) {
	if (!server.workspace_manager)
		return;

	struct wlr_ext_workspace_handle_v1 *workspace = find_workspace_by_name(name);
	if (!workspace) {
		wlr_log(WLR_ERROR, "Workspace not found: %s", name);
		return;
	}

	output_t *output = server.focused_output;
	desktop_t *d = NULL;
	for (desktop_t *local = output->desk_head; local; local = local->next) {
		if (strcmp(local->name, name) == 0) {
			d = local;
			break;
		}
	}
	if (!d) {
		d = find_desktop_by_name(name);
		if (!d) {
			wlr_log(WLR_ERROR, "Desktop not found: %s", name);
			return;
		}
		if (d->output)
			output = d->output;
	}

	desktop_t *old_desktop = output->desk;
	if (old_desktop && old_desktop != d)
		output->last_desk = old_desktop;

	if (enable_animations && old_desktop && old_desktop != d && output) {
		workspace_switch_animate(output, old_desktop, d);
		return;
	}

	output->desk = d;
	ipc_put_status(SUB_MASK_REPORT, NULL);
	ipc_put_status(SUB_MASK_DESKTOP_FOCUS, "desktop_focus[%s]\n", name);

	wlr_log(WLR_DEBUG, "Switching from %s to %s", old_desktop ? old_desktop->name : "NULL", d->name);

	update_all_toplevels_visibility(output, d);

	struct wlr_ext_workspace_handle_v1 *old = workspace_get_active();

	if (d->root == NULL) {
		wlr_log(WLR_DEBUG, "Desktop %s has no root, skipping arrange/focus", name);
		wlr_log(WLR_INFO, "Switched to desktop: %s", name);
		goto finish;
	}

	arrange(output, d, true);

	if (d->focus != NULL)
		focus_node(output, d, d->focus);

	wlr_log(WLR_INFO, "Switched to desktop: %s", name);

finish:
	if (old)
		wlr_ext_workspace_handle_v1_set_active(old, false);

	wlr_ext_workspace_handle_v1_set_active(workspace, true);
	wlr_ext_workspace_handle_v1_set_hidden(workspace, false);
}

void workspace_switch_to_last_desktop(void) {
	if (!server.focused_output || !server.focused_output->desk || !server.focused_output->last_desk)
		return;

	output_t *output = server.focused_output;
	desktop_t *target = output->last_desk;
	if (target == output->desk)
		return;

	for (desktop_t *d = output->desk_head; d != NULL; d = d->next) {
		if (d == target) {
			workspace_switch_to_desktop(target->name);
			return;
		}
	}

	output->last_desk = NULL;
}

void workspace_switch_to_desktop_by_index(int index) {
	if (!server.workspace_manager || !server.focused_output)
		return;

	wlr_log(WLR_DEBUG, "Looking for desktop at index %d", index);
	desktop_t *target = server.focused_output->desk_head;
	for (int idx = 0; target != NULL && idx < index; target = target->next, ++idx)
		wlr_log(WLR_DEBUG, "Desktop at idx %d: %s", idx, target->name);

	if (!target) {
		int count = 0;
		target = server.focused_output->desk_head;

		for (; target != NULL; target = target->next, ++count)
			wlr_log(WLR_DEBUG, "Desktop %d: %s", count, target->name);

		wlr_log(WLR_ERROR, "Desktop not found at index: %d (total: %d)", index, count);
		return;
	}

	wlr_log(WLR_DEBUG, "Switching to desktop: %s", target->name);
	workspace_switch_to_desktop(target->name);
}

struct wlr_ext_workspace_handle_v1 *workspace_get_active(void) {
	if (!server.workspace_manager)
		return NULL;

	struct wlr_ext_workspace_handle_v1 *workspace;
	wl_list_for_each(workspace, &server.workspace_manager->workspaces, link)
		if (workspace->state == 1)
			return workspace;

	return NULL;
}

static void handle_workspace_request(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *request;

	wl_list_for_each(request, event->requests, link) {
		switch (request->type) {
		case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE:
			workspace_create_desktop(request->create_workspace.name);
			break;
		case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE:
			if (request->activate.workspace)
				workspace_switch_to_desktop(request->activate.workspace->name);
			break;
		case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE:
			break;
		case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
			break;
		case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
			break;
		}
	}
}

void workspace_init(void) {
	ONCE();
	server.workspace_manager = wlr_ext_workspace_manager_v1_create(server.wl_display, 1);
	if (!server.workspace_manager) {
		wlr_log(WLR_ERROR, "Failed to create workspace manager");
		return;
	}

	server.workspace_commit.notify = handle_workspace_request;
	wl_signal_add(&server.workspace_manager->events.commit, &server.workspace_commit);

	struct wlr_ext_workspace_group_handle_v1 *group =
		wlr_ext_workspace_group_handle_v1_create(server.workspace_manager, 0);
	if (!group) {
		wlr_log(WLR_ERROR, "Failed to create workspace group");
		return;
	}

	wlr_log(WLR_INFO, "Workspace manager initialized");
}

void workspace_fini(void) {
	ONCE();
	if (!server.workspace_manager)
		return;

	struct wlr_ext_workspace_handle_v1 *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &server.workspace_manager->workspaces, link)
		wlr_ext_workspace_handle_v1_destroy(workspace);

	struct wlr_ext_workspace_group_handle_v1 *group, *tmp_group;
	wl_list_for_each_safe(group, tmp_group, &server.workspace_manager->groups, link)
		wlr_ext_workspace_group_handle_v1_destroy(group);

	wl_list_remove(&server.workspace_commit.link);
}
