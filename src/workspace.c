#include "workspace.h"
#include "server.h"
#include "types.h"
#include "output.h"
#include "tree.h"
#include "transaction.h"
#include <stdint.h>
#include <string.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_scene.h>

extern struct bwm_server server;

static void handle_workspace_request(struct wl_listener *listener, void *data);

static struct wlr_ext_workspace_handle_v1 *find_workspace_by_name(const char *name) {
  struct wlr_ext_workspace_handle_v1 *workspace;
  wl_list_for_each(workspace, &server.workspace_manager->workspaces, link)
    if (strcmp(workspace->name, name) == 0)
      return workspace;
  return NULL;
}

struct desktop_t *find_desktop_by_name(const char *name) {
  if (!name || name[0] == '\0') return NULL;

  if (name[0] == '^' && name[1] >= '1' && name[1] <= '9') {
    int mon_idx = name[1] - '1';
    monitor_t *m = mon_head;
    for (int i = 0; m != NULL && i < mon_idx; m = m->next, i++)
    	;
    if (m && m->desk_head)
      return m->desk_head;
    return NULL;
  }

  monitor_t *m = mon_head;
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

void workspace_init(void) {
  server.workspace_manager = wlr_ext_workspace_manager_v1_create(
      server.wl_display, 1);
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

  struct bwm_output *output;
  wl_list_for_each(output, &server.outputs, link)
    wlr_ext_workspace_group_handle_v1_output_enter(group, output->wlr_output);

  desktop_t *d = mon_head->desk_tail;
  while (d != NULL) {
    struct wlr_ext_workspace_handle_v1 *workspace =
      wlr_ext_workspace_handle_v1_create(server.workspace_manager, NULL, 0);
    if (!workspace) {
      wlr_log(WLR_ERROR, "Failed to create workspace: %s", d->name);
      d = d->prev;
      continue;
    }

    wlr_ext_workspace_handle_v1_set_name(workspace, d->name);
    wlr_ext_workspace_handle_v1_set_group(workspace, group);

    d = d->prev;
  }

  struct wlr_ext_workspace_handle_v1 *active = find_workspace_by_name(mon_head->desk->name);
  if (active)
    wlr_ext_workspace_handle_v1_set_active(active, true);

  wlr_log(WLR_INFO, "Workspace manager initialized");
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

  desktop_t *d = mon_head->desk_tail;
  while (d != NULL) {
    struct wlr_ext_workspace_handle_v1 *workspace =
      wlr_ext_workspace_handle_v1_create(server.workspace_manager, NULL, 0);
    if (!workspace) {
      wlr_log(WLR_ERROR, "Failed to create workspace: %s", d->name);
      d = d->prev;
      continue;
    }

    wlr_ext_workspace_handle_v1_set_name(workspace, d->name);
    wlr_ext_workspace_handle_v1_set_group(workspace, group);

    d = d->prev;
  }

  struct wlr_ext_workspace_handle_v1 *active = find_workspace_by_name(mon_head->desk->name);
  if (active)
    wlr_ext_workspace_handle_v1_set_active(active, true);

  wlr_log(WLR_INFO, "Workspace manager synced");
}

void workspace_fini(void) {
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

static void update_all_toplevels_visibility(monitor_t *m, desktop_t *current_desktop) {
  int toplevel_count = 0;

  struct bwm_toplevel *toplevel;
  wl_list_for_each(toplevel, &server.toplevels, link) {
    if (!toplevel->mapped || !toplevel->scene_tree || !toplevel->node || !toplevel->node->client)
      continue;

    toplevel_count++;
    wlr_log(WLR_DEBUG, "  Checking toplevel #%d (node=%u, monitor=%p)",
            toplevel_count, toplevel->node->id, (void*)toplevel->node->monitor);

    monitor_t *toplevel_mon = toplevel->node->monitor;
    if (!toplevel_mon || toplevel_mon != m) {
      wlr_log(WLR_DEBUG, "    Skipping: wrong monitor");
      continue;
    }

    // show parent chain
    wlr_log(WLR_DEBUG, "    Parent chain: node=%u", toplevel->node->id);
    node_t *dbg_parent = toplevel->node->parent;
    int depth = 0;
    while (dbg_parent != NULL && depth < 10) {
      wlr_log(WLR_DEBUG, "      -> parent=%u", dbg_parent->id);
      dbg_parent = dbg_parent->parent;
      depth++;
    }

    bool should_show = false;
    bool found = false;
    desktop_t *d = m->desk_head;
    while (d != NULL) {
      wlr_log(WLR_DEBUG, "    Checking against desktop %s (root=%p)", d->name, (void*)d->root);
      if (d->root != NULL) {
        node_t *parent = toplevel->node;
        while (parent != NULL) {
          if (parent == d->root) {
            // this node belongs to desktop d
            should_show = (d == current_desktop);
            found = true;
            wlr_log(WLR_DEBUG, "      MATCH: Node's root matches desktop %s (current=%s, show=%d)",
                    d->name, current_desktop->name, should_show);
            goto found_desktop;
          }
          parent = parent->parent;
        }
      }
      d = d->next;
    }

    if (!found) {
      wlr_log(WLR_DEBUG, "    WARNING: Node %u not found in any desktop root! (orphaned node)", toplevel->node->id);
      should_show = false;
    }

found_desktop:
    wlr_log(WLR_DEBUG, "  Before update: shown=%d, should_show=%d",
            toplevel->node->client->shown, should_show);
    if (should_show && !toplevel->node->client->shown) {
      wlr_log(WLR_DEBUG, "  ACTION: Showing toplevel on desktop %s", current_desktop->name);
      toplevel->node->client->shown = true;
      wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    } else if (!should_show && toplevel->node->client->shown) {
      wlr_log(WLR_DEBUG, "  ACTION: Hiding toplevel (orphaned or not on desktop %s)", current_desktop->name);
      toplevel->node->client->shown = false;
      wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    } else {
      wlr_log(WLR_DEBUG, "  No change needed (shown=%d, should=%d)",
              toplevel->node->client->shown, should_show);
    }
  }
  wlr_log(WLR_DEBUG, "update_all_toplevels_visibility: Processed %d toplevels for desktop %s",
          toplevel_count, current_desktop->name);
}

void workspace_switch_to_desktop(const char *name) {
  if (!server.workspace_manager)
    return;

  struct wlr_ext_workspace_handle_v1 *workspace = find_workspace_by_name(name);
  if (!workspace) {
    wlr_log(WLR_ERROR, "Workspace not found: %s", name);
    return;
  }

  desktop_t *d = find_desktop_by_name(name);
  if (!d) {
    wlr_log(WLR_ERROR, "Desktop not found: %s", name);
    return;
  }

  desktop_t *old_desktop = server.focused_monitor->desk;

  server.focused_monitor->desk = d;

  wlr_log(WLR_DEBUG, "Switching from %s to %s",
          old_desktop ? old_desktop->name : "NULL", d->name);

  struct wlr_ext_workspace_handle_v1 *old = workspace_get_active();
  if (old)
    wlr_ext_workspace_handle_v1_set_active(old, false);

  wlr_ext_workspace_handle_v1_set_active(workspace, true);
  wlr_ext_workspace_handle_v1_set_hidden(workspace, false);

  update_all_toplevels_visibility(server.focused_monitor, d);

  if (d->root == NULL) {
    wlr_log(WLR_DEBUG, "Desktop %s has no root, skipping arrange/focus", name);
    wlr_log(WLR_INFO, "Switched to desktop: %s", name);
    return;
  }

  arrange(server.focused_monitor, d, true);

  if (d->focus != NULL)
    focus_node(server.focused_monitor, d, d->focus);

  wlr_log(WLR_INFO, "Switched to desktop: %s", name);
}

void workspace_switch_to_desktop_by_index(int index) {
  if (!server.workspace_manager || !server.focused_monitor)
    return;

  wlr_log(WLR_DEBUG, "Looking for desktop at index %d", index);
  desktop_t *target = server.focused_monitor->desk_head;
  for (int idx = 0; target != NULL && idx < index; target = target->next, ++idx)
    wlr_log(WLR_DEBUG, "Desktop at idx %d: %s", idx, target->name);

  if (!target) {
    int count = 0;
    target = server.focused_monitor->desk_head;
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
