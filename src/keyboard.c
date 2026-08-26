#include "config.h"
#include "fallthrough.h"
#include "global_shortcuts.h"
#include "idle_power.h"
#include "input.h"
#include "input_method.h"
#include "ipc.h"
#include "keyboard.h"
#include "layout.h"
#include "master_stack.h"
#include "output.h"
#include "scroller.h"
#include "seat.h"
#include "server.h"
#include "tabs.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "workspace.h"
#include "xwayland.h"
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

extern struct server_t server;

extern keybind_t keybinds[MAX_KEYBINDS];
extern size_t num_keybinds;
extern submap_t *active_submap;

static void destroy_empty_wlr_keyboard_group(void *data) {
	keyboard_group_t *group = data;
	struct wlr_keyboard_group *wlr_group = group->wlr_group;
	wlr_keyboard_group_destroy(wlr_group);
	free(group);
}

static void parse_portal_shortcut_cmd(const char *cmd, char *app_id, size_t app_id_size,
		char *shortcut_id, size_t shortcut_id_size) {
	app_id[0] = '\0';
	shortcut_id[0] = '\0';
	if (!cmd || cmd[0] == '\0')
		return;
	const char *args = cmd + 16;
	const char *space = strchr(args, ' ');
	if (space) {
		size_t app_len = (size_t)(space - args);
		if (app_len >= app_id_size)
			app_len = app_id_size - 1;
		memcpy(app_id, args, app_len);
		app_id[app_len] = '\0';
		snprintf(shortcut_id, shortcut_id_size, "%s", space + 1);
	}
}

static bool find_and_send_portal_shortcut(uint32_t modifiers, uint32_t keycode,
		const xkb_keysym_t *syms, int nsyms, bool pressed) {
	keybind_t *arrays[2] = {
		NULL,
		keybinds
	};
	size_t sizes[2] = {
		0,
		num_keybinds
	};

	if (active_submap) {
		arrays[0] = active_submap->keybinds;
		sizes[0] = active_submap->num_keybinds;
	}

	for (int pass = 0; pass < 2; pass++) {
		if (!arrays[pass] || sizes[pass] == 0)
			continue;
		for (size_t i = 0; i < sizes[pass]; i++) {
			if (arrays[pass][i].action != BIND_GLOBAL_SHORTCUT)
				continue;
			bool matched = false;
			if (arrays[pass][i].use_keycode)
				matched = keybind_matches(&arrays[pass][i], modifiers, 0, keycode);
			else {
				for (int j = 0; j < nsyms && !matched; j++)
					matched = keybind_matches(&arrays[pass][i], modifiers, syms[j], 0);
			}
			if (matched) {
				char app_id[256], shortcut_id[256];
				parse_portal_shortcut_cmd(arrays[pass][i].external_cmd, app_id, sizeof(app_id), shortcut_id,
					sizeof(shortcut_id));
				if (app_id[0] && shortcut_id[0])
					global_shortcuts_send_by_app(app_id, shortcut_id, pressed);
				return true;
			}
		}
	}
	return false;
}

static output_t *find_monitor_for_desktop(desktop_t *d) {
	output_t *m = mon_head;
	while (m != NULL) {
		desktop_t *desk = m->desk_head;
		while (desk != NULL) {
			if (desk == d)
				return m;
			desk = desk->next;
		}
		m = m->next;
	}
	return NULL;
}

keyboard_t *keyboard_create(struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	keyboard_t *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) {
		wlr_log(WLR_ERROR, "allocation failed");
		return NULL;
	}
	keyboard->wlr_keyboard = wlr_keyboard;
	keyboard->seat = seat_default();

	input_config_t *config = input_config_get_for_device(device->name, INPUT_CONFIG_TYPE_KEYBOARD);
	if (config) {
		input_config_apply(config, device);
		input_config_destroy(config);
	} else {
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

		wlr_keyboard_set_keymap(wlr_keyboard, keymap);
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);

		wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);
	}

	keyboard->repeat_rate = wlr_keyboard->repeat_info.rate;
	keyboard->repeat_delay = wlr_keyboard->repeat_info.delay;

	keyboard->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wl_list_insert(&server.physical_keyboards, &keyboard->all_link);
	keyboard_group_add(keyboard);

	if (!server.seat->keyboard_state.keyboard)
		wlr_seat_set_keyboard(server.seat, keyboard->wlr_keyboard);

	wlr_log(WLR_INFO, "New keyboard configured: %s", device->name);
	return keyboard;
}

void keyboard_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
	keyboard_t *keyboard = wl_container_of(listener, keyboard, modifiers);
	seat_t *seat = keyboard->seat;
	if (!seat)
		return;
	struct wlr_seat *wlr_seat = seat->wlr_seat;

	wlr_seat_set_keyboard(wlr_seat, keyboard->wlr_keyboard);

	if (seat->input_method_relay &&
		!input_method_keyboard_grab_forward_modifiers(keyboard->wlr_keyboard, seat->input_method_relay))
		wlr_seat_keyboard_notify_modifiers(wlr_seat, &keyboard->wlr_keyboard->modifiers);
}

void keyboard_key(struct wl_listener *listener, void *data) {
	keyboard_t *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;
	seat_t *seat = keyboard->seat;
	if (!seat)
		return;
	struct wlr_seat *wlr_seat = seat->wlr_seat;

	wlr_idle_notifier_v1_notify_activity(server.idle_notifier, wlr_seat);
	idle_power_notify_activity();

	// get keysym
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

	// check keyboard shortcuts inhibitor
	bool shortcuts_inhibited = false;
	if (server.keyboard_shortcuts_inhibit_manager) {
		struct wlr_surface *focused = wlr_seat->keyboard_state.focused_surface;
		struct wlr_keyboard_shortcuts_inhibitor_v1 *inhib;
		wl_list_for_each(inhib, &server.keyboard_shortcuts_inhibit_manager->inhibitors, link) {
			if (inhib->active && inhib->surface == focused && inhib->seat == wlr_seat) {
				shortcuts_inhibited = true;
				break;
			}
		}
	}

	if (!shortcuts_inhibited && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		// first try raw keycode for number keys
		if (handle_keybind_raw(modifiers, event->keycode, true)) {
			handled = true;
		} else {
			// fallback to keysym
			for (int i = 0; i < nsyms; i++)
				if ((handled = handle_keybind(modifiers, syms[i])))
					break;
		}
	}

	// check portal global shortcuts (independent of compositor keybinds)
	if (global_shortcuts_handle_key(modifiers, keycode, syms, nsyms, event->state, event->time_msec))
		handled = true;

	// handle portal shortcut keybind release (send Deactivated to match press from execute_bind)
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && find_and_send_portal_shortcut(modifiers,
		event->keycode, syms, nsyms, false))
		handled = true;

	if (!handled) {
		// forward key to input method if grabbed
		if (!input_method_keyboard_grab_forward_key(keyboard->wlr_keyboard, event,
				seat->input_method_relay)) {
			// pass key to focused client
			wlr_seat_set_keyboard(wlr_seat, keyboard->wlr_keyboard);
			wlr_seat_keyboard_notify_key(wlr_seat, event->time_msec, event->keycode, event->state);
		}
	}

	if (server.focused_output)
		output_schedule_frame(server.focused_output);
}

void keyboard_destroy(struct wl_listener *listener, void *data) {
	keyboard_t *keyboard = wl_container_of(listener, keyboard, destroy);
	struct wlr_input_device *device = data;
	(void)device;

	bool is_standalone = (keyboard->group == NULL);

	if (keyboard->group)
		keyboard_group_remove(keyboard);

	if (keyboard->all_link.next || keyboard->all_link.prev)
		wl_list_remove(&keyboard->all_link);

	if (is_standalone)
		if (keyboard->active_link.next || keyboard->active_link.prev)
			wl_list_remove(&keyboard->active_link);

	if (keyboard->modifiers.link.next)
		wl_list_remove(&keyboard->modifiers.link);
	if (keyboard->key.link.next)
		wl_list_remove(&keyboard->key.link);
	if (keyboard->destroy.link.next)
		wl_list_remove(&keyboard->destroy.link);

	free(keyboard);
}

// keybind handling using raw keycode (for number keys 1-0)
keybind_t *handle_keybind_raw(uint32_t modifiers, uint32_t keycode, bool pressed) {
	if (!pressed)
		return NULL;

	keybind_t *matched_kb = find_keybind(modifiers, 0, keycode, true, true);
	if (!matched_kb)
		return NULL;

	if (is_interactive_bind(matched_kb))
		return matched_kb;

	execute_keybind(matched_kb);
	return matched_kb;
}

// keybind handling
bool handle_keybind(uint32_t modifiers, xkb_keysym_t sym) {
	// handle vt switch
	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (server.session) {
			wlr_session_change_vt(server.session, (unsigned int)(sym + 1 - XKB_KEY_XF86Switch_VT_1));
			return true;
		}
	}

	if (active_submap && sym == XKB_KEY_Escape) {
		exit_submap();
		return true;
	}

	// check if in submap
	if (active_submap) {
		wlr_log(WLR_DEBUG, "In submap '%s' with %zu keybinds, looking for keysym=%u mod=%u",
			active_submap->name, active_submap->num_keybinds, sym, modifiers);
		keybind_t *kb = find_keybind(modifiers, sym, 0, false, false);
		if (kb) {
			wlr_log(WLR_DEBUG, "  matched submap keybind: action=%d", kb->action);
			execute_keybind(kb);
			return true;
		}
		return false;
	}

	// check global user-defined keybinds
	keybind_t *kb = find_keybind(modifiers, sym, 0, false, true);
	if (!kb)
		return false;

	execute_keybind(kb);
	return true;
}

void focus_dir(direction_t dir) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	desktop_t *d = mon->desk;

	node_t *tab_anc = tabbed_ancestor(d->focus);
	if (tab_anc != NULL) {
		node_t *prev = tab_prev_leaf(tab_anc, d->focus);
		if (prev != NULL && prev != d->focus) {
			focus_node(mon, d, prev);
			arrange(mon, d, true);
		}
		return;
	}

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl && impl->focus)
		impl->focus(d, dir);
}

void swap_dir(direction_t dir) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	const layout_impl_t *impl = layout_get_impl(mon->desk->layout);
	if (impl && impl->swap)
		impl->swap(mon, mon->desk, dir);
}

void close_focused(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	kill_node(mon->desk, mon->desk->focus);

	wlr_log(WLR_INFO, "Closing focused window");
}

void toggle_block_out_from_screenshare(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL) {
		wlr_log(WLR_DEBUG, "toggle_block_out: mon=%p desk=%p focus=%p", (void *)mon,
			mon ? (void *)mon->desk : NULL, (mon && mon->desk) ? (void *)mon->desk->focus : NULL);
		return;
	}

	node_t *n = mon->desk->focus;
	if (n->client == NULL) {
		wlr_log(WLR_DEBUG, "toggle_block_out: no client on focus");
		return;
	}

	n->client->flags.block_out_from_screenshare = !n->client->flags.block_out_from_screenshare;
	wlr_log(WLR_DEBUG, "toggle_block_out: %s -> %d", n->client->app_id,
		n->client->flags.block_out_from_screenshare);

	if (n->client->toplevel) {
		toplevel_t *tl = n->client->toplevel;

		if (n->client->flags.block_out_from_screenshare && tl->image_capture_surface) {
			wlr_scene_node_destroy(&tl->image_capture_surface->buffer->node);
			tl->image_capture_surface = NULL;
		} else if (!n->client->flags.block_out_from_screenshare && !tl->image_capture_surface) {
			tl->image_capture_surface = wlr_scene_surface_create(&tl->image_capture->tree,
				tl->xdg_toplevel->base->surface);
		}
	} else if (n->client->xwayland_view) {
		xwayland_toplevel_t *xw = n->client->xwayland_view;

		if (n->client->flags.block_out_from_screenshare && xw->image_capture_surface) {
			wlr_scene_node_destroy(&xw->image_capture_surface->buffer->node);
			xw->image_capture_surface = NULL;
		} else if (!n->client->flags.block_out_from_screenshare && !xw->image_capture_surface) {
			xw->image_capture_surface = wlr_scene_surface_create(&xw->image_capture->tree,
				xw->xwayland_surface->surface);
		}
	}

	wlr_log(WLR_INFO, "toggle block_out_from_screenshare: %s",
		n->client->flags.block_out_from_screenshare ? "on" : "off");
}

void toggle_floating(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	node_t *n = mon->desk->focus;
	if (n->client == NULL)
		return;

	struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
	if (!scene_tree) {
		wlr_log(WLR_ERROR, "Cannot toggle floating: no scene tree");
		return;
	}

	wlr_log(WLR_INFO, "toggle_floating: node=%u state=%d hidden=%d parent=%u root=%u focus=%u", n->id,
		n->client->state, n->hidden, n->parent ? n->parent->id : 0,
		mon->desk->root ? mon->desk->root->id : 0, mon->desk->focus ? mon->desk->focus->id : 0);

	if (n->client->state == STATE_FLOATING) {
		if (n->parent != NULL)
			wlr_log(WLR_ERROR, "toggle_floating: floating node %u has non-NULL parent %u, unexpected state",
				n->id, n->parent->id);

		n->hidden = false;
		wlr_scene_node_reparent(&scene_tree->node, server.tile_tree);

		n->client->last_state = n->client->state;
		n->client->state = STATE_TILED;

		node_t *ref = mon->desk->focus != n ? mon->desk->focus : NULL;
		insert_node(mon->desk, n, ref);

		arrange(mon, mon->desk, true);

		ipc_put_status(SUB_MASK_NODE_STATE, "node_state[%s,%s,%u,%c]\n",
			n->client->app_id[0] ? n->client->app_id : "?", n->client->title[0] ? n->client->title : "?",
			n->id,
			n->client->state == STATE_TILED ? 'T' : n->client->state == STATE_FLOATING ? 'F' :
			n->client->state == STATE_FULLSCREEN ? 'U' : '?');

		wlr_log(WLR_INFO, "toggle_floating: now tiled, node=%u parent=%u root=%u", n->id,
			n->parent ? n->parent->id : 0, mon->desk->root ? mon->desk->root->id : 0);
	} else if (n->client->state == STATE_TILED) {
		if (n->parent == NULL && mon->desk->root != n)
			wlr_log(WLR_ERROR,
				"toggle_floating: tiled node %u has no parent and is not root, already detached", n->id);

		if (n->client->toplevel) {
			toplevel_t *tl = n->client->toplevel;
			int off_x = (n->client->tiled_rectangle.width - tl->geometry.width) / 2;
			int off_y = (n->client->tiled_rectangle.height - tl->geometry.height) / 2;
			n->client->floating_rectangle = (struct wlr_box){
				.x = n->client->tiled_rectangle.x + (off_x > 0 ? off_x : 0),
				.y = n->client->tiled_rectangle.y + (off_y > 0 ? off_y : 0),
				.width = tl->geometry.width,
				.height = tl->geometry.height
			};
		} else
			n->client->floating_rectangle = n->client->tiled_rectangle;

		remove_node(mon->desk, n);
		n->hidden = true;

		struct wlr_scene_tree *st = client_get_scene_tree(n->client);
		if (st)
			wlr_scene_node_set_position(&st->node, n->client->floating_rectangle.x,
				n->client->floating_rectangle.y);

		wlr_scene_node_reparent(&scene_tree->node, server.float_tree);

		// restore focus
		focus_node(mon, mon->desk, n);

		set_state(mon, mon->desk, n, STATE_FLOATING);

		if (n->client->toplevel)
			toplevel_center_and_clip_surface(n->client->toplevel);

		node_set_dirty(n);
		transaction_commit_dirty();
		wlr_log(WLR_INFO, "toggle_floating: now floating, node=%u root=%u focus=%u", n->id,
			mon->desk->root ? mon->desk->root->id : 0, mon->desk->focus ? mon->desk->focus->id : 0);
	}
}

void tile_focused(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;
	node_t *n = mon->desk->focus;

	if (n->client == NULL)
		return;

	switch (n->client->state) {
	case STATE_FLOATING:
		toggle_floating();
		break;
	case STATE_PSEUDO_TILED:
		toggle_pseudo_tiled();
		break;
	case STATE_TILED:
		// xdd
		break;
	default:
		set_state(mon, mon->desk, n, STATE_TILED);
	}
}

void toggle_fullscreen(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	mon->desk->fullscreen_recreate_pending_window_id = 0;
	node_t *n = mon->desk->focus;
	if (n->client == NULL)
		return;

	struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
	if (!scene_tree) {
		wlr_log(WLR_ERROR, "Cannot toggle fullscreen: no scene tree");
		return;
	}

	if (n->client->state == STATE_FULLSCREEN) {
		client_state_t restore = n->client->last_state;
		// fallback to tiled if node should probably be
		if (restore == STATE_FULLSCREEN)
			restore = STATE_TILED;
		if (restore == STATE_FLOATING && n->parent != NULL)
			restore = STATE_TILED;

		if (restore == STATE_FLOATING)
			wlr_scene_node_reparent(&scene_tree->node, server.float_tree);
		else
			wlr_scene_node_reparent(&scene_tree->node, server.tile_tree);

		if (n->client->toplevel && n->client->toplevel->xdg_toplevel)
			wlr_xdg_toplevel_set_fullscreen(n->client->toplevel->xdg_toplevel, false);
		else if (n->client->xwayland_view)
			wlr_xwayland_surface_set_fullscreen(n->client->xwayland_view->xwayland_surface, false);

		set_state(mon, mon->desk, n, restore);
		n->hidden = (restore == STATE_FLOATING);
		wlr_log(WLR_INFO, "Fullscreen disabled");
	} else {
		wlr_scene_node_reparent(&scene_tree->node, server.full_tree);

		if (n->client->toplevel && n->client->toplevel->xdg_toplevel)
			wlr_xdg_toplevel_set_fullscreen(n->client->toplevel->xdg_toplevel, true);
		else if (n->client->xwayland_view)
			wlr_xwayland_surface_set_fullscreen(n->client->xwayland_view->xwayland_surface, true);

		set_state(mon, mon->desk, n, STATE_FULLSCREEN);
		wlr_log(WLR_INFO, "Fullscreen enabled");
	}

	if (n->client->toplevel)
		update_foreign_toplevel_state(n->client->toplevel);
}

void toggle_pseudo_tiled(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	node_t *n = mon->desk->focus;
	if (n->client == NULL)
		return;

	if (n->client->state == STATE_PSEUDO_TILED) {
		set_state(mon, mon->desk, n, STATE_TILED);
		wlr_log(WLR_INFO, "Window tiled");
	} else {
		struct wlr_box base_rect = {0};

		if (n->client->toplevel && n->client->toplevel->xdg_toplevel)
			base_rect = n->client->toplevel->xdg_toplevel->base->geometry;
		else if (n->client->xwayland_view)
			base_rect = n->client->xwayland_view->geometry;
		else
			base_rect = n->rectangle;

		n->client->floating_rectangle = (struct wlr_box){
			.x = 0,
			.y = 0,
			.width = base_rect.width,
			.height = base_rect.height
		};
		set_state(mon, mon->desk, n, STATE_PSEUDO_TILED);
		wlr_log(WLR_INFO, "Window pseudo_tiled");
	}
}

void focus_next_desktop(void) {
	if (!server.focused_output || !server.focused_output->desk)
		return;

	desktop_t *next = server.focused_output->desk->next;
	if (next != NULL)
		workspace_switch_to_desktop(next->name);
}

void focus_prev_desktop(void) {
	if (!server.focused_output || !server.focused_output->desk)
		return;

	desktop_t *prev = server.focused_output->desk->prev;
	if (prev != NULL) {
		wlr_log(WLR_DEBUG, "Focus prev desktop - %s", prev->name);
		workspace_switch_to_desktop(prev->name);
	}
}

void focus_last_desktop(void) {
	workspace_switch_to_last_desktop();
}

void send_to_desktop(int desktop_index) {
	if (mon == NULL)
		return;

	// get index of desktop
	desktop_t *target = mon->desk_head;
	int idx = 0;
	for (; target != NULL && idx < desktop_index; target = target->next, ++idx)
		;

	if (!target) {
		wlr_log(WLR_ERROR, "Desktop not found at index: %d", desktop_index);
		return;
	}

	if (mon->desk == target)
		return;
	if (mon->desk == NULL || mon->desk->focus == NULL)
		return;

	node_t *n = mon->desk->focus;
	if (n == NULL || n->client == NULL)
		return;

	desktop_t *src_desk = mon->desk;

	n->destroying = false;
	n->ntxnrefs = 0;

	n->client->flags.shown = false;
	struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
	if (scene_tree)
		wlr_scene_node_set_enabled(&scene_tree->node, false);

	// remove from source desktop
	remove_node(src_desk, n);

	// update focus on source desktop
	if (src_desk->focus == n) {
		if (src_desk->root != NULL) {
			node_t *new_focus = first_extrema(src_desk->root);
			if (new_focus != NULL) {
				src_desk->focus = new_focus;
				focus_node(mon, src_desk, new_focus);
			} else {
				src_desk->focus = NULL;
			}
		} else {
			src_desk->focus = NULL;
		}
	}

	output_t *target_mon = find_monitor_for_desktop(target);
	if (target_mon == NULL) {
		wlr_log(WLR_ERROR, "Could not find monitor for desktop: %s", target->name);
		return;
	}

	insert_node(target, n, find_public(target));
	target->focus = n;

	arrange(mon, src_desk, true);
	arrange(target_mon, target, false);

	n->output = target_mon;

	wlr_log(WLR_INFO, "Sent window to desktop: %s (n->output=%p target_mon=%p)", target->name,
		(void *)n->output, (void *)target_mon);
}

void send_to_desktop_by_name(const char *name) {
	if (mon == NULL)
		return;

	desktop_t *target = find_desktop_by_name(name);
	if (!target) {
		wlr_log(WLR_ERROR, "Desktop not found: %s", name);
		return;
	}

	if (mon->desk == target)
		return;
	if (mon->desk == NULL || mon->desk->focus == NULL)
		return;

	node_t *n = mon->desk->focus;
	if (n == NULL || n->client == NULL)
		return;

	desktop_t *src_desk = mon->desk;

	output_t *target_mon = find_monitor_for_desktop(target);
	if (target_mon == NULL) {
		wlr_log(WLR_ERROR, "Could not find monitor for desktop: %s", target->name);
		return;
	}

	n->destroying = false;
	n->ntxnrefs = 0;

	n->client->flags.shown = false;
	struct wlr_scene_tree *scene_tree = client_get_scene_tree(n->client);
	if (scene_tree)
		wlr_scene_node_set_enabled(&scene_tree->node, false);

	remove_node(src_desk, n);

	if (src_desk->focus == n) {
		if (src_desk->root != NULL) {
			node_t *new_focus = first_extrema(src_desk->root);
			if (new_focus != NULL) {
				src_desk->focus = new_focus;
				focus_node(mon, src_desk, new_focus);
			} else {
				src_desk->focus = NULL;
			}
		} else {
			src_desk->focus = NULL;
		}
	}

	// add to target desktop
	insert_node(target, n, find_public(target));
	target->focus = n;

	// ensure the moved node respects initial_polarity
	// in spiral mode, insert_node may place it as second_child regardless
	if (n->parent != NULL) {
		if (initial_polarity == FIRST_CHILD && n->parent->second_child == n) {
			// node is second child but should be first, swap them
			node_t *p = n->parent;
			node_t *sibling = p->first_child;
			p->first_child = n;
			p->second_child = sibling;
			n->parent = p;

			if (sibling != NULL)
				sibling->parent = p;
		} else if (initial_polarity == SECOND_CHILD && n->parent->first_child == n) {
			// node is first child but should be second, swap them
			node_t *p = n->parent;
			node_t *sibling = p->second_child;
			p->second_child = n;
			p->first_child = sibling;
			n->parent = p;

			if (sibling != NULL)
				sibling->parent = p;
		}
	}

	arrange(mon, src_desk, true);
	arrange(target_mon, target, false);

	n->output = target_mon;

	wlr_log(WLR_INFO, "Sent window to desktop: %s (n->output=%p target_mon=%p)", target->name,
		(void *)n->output, (void *)target_mon);
}

void send_to_next_desktop(void) {
	if (mon == NULL || mon->desk == NULL)
		return;

	desktop_t *next = mon->desk->next;
	if (next != NULL) {
		send_to_desktop_by_name(next->name);
		wlr_log(WLR_INFO, "Sent window to next desktop");
	}
}

void send_to_prev_desktop(void) {
	if (mon == NULL || mon->desk == NULL)
		return;

	desktop_t *prev = mon->desk->prev;
	if (prev != NULL) {
		send_to_desktop_by_name(prev->name);
		wlr_log(WLR_INFO, "Sent window to previous desktop");
	}
}

void toggle_monocle(void) {
	if (mon == NULL || mon->desk == NULL)
		return;

	desktop_t *d = mon->desk;

	if (d->layout == LAYOUT_MONOCLE) {
		layout_set(d, d->user_layout);

		if (d->root) {
			FOR_EACH_LEAF(n, d->root) {
				if (n->client && n->client->toplevel && n->client->state != STATE_FULLSCREEN) {
					n->client->toplevel->client_maximized = false;
					wlr_xdg_toplevel_set_maximized(n->client->toplevel->xdg_toplevel, false);
				}
			}
		}
	} else {
		layout_toggle(d, LAYOUT_MONOCLE);

		if (d->root) {
			FOR_EACH_LEAF(n, d->root) {
				if (n->client && n->client->toplevel && n->client->state != STATE_FULLSCREEN) {
					n->client->toplevel->client_maximized = true;
					wlr_xdg_toplevel_set_maximized(n->client->toplevel->xdg_toplevel, true);
				}
			}
		}
	}

	arrange(mon, d, true);
	ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", d->name,
		layout_to_char(d->layout));

	if (d->focus != NULL)
		focus_node(mon, d, d->focus);
}

void set_tiled_layout(void) {
	if (mon == NULL || mon->desk == NULL)
		return;
	desktop_t *d = mon->desk;

	layout_set(d, LAYOUT_TILED);
	d->user_layout = LAYOUT_TILED;

	if (d->root) {
		FOR_EACH_LEAF(n, d->root) {
			if (n->client && n->client->toplevel && n->client->state != STATE_FULLSCREEN) {
				n->client->toplevel->client_maximized = false;
				wlr_xdg_toplevel_set_maximized(n->client->toplevel->xdg_toplevel, false);
			}
		}
	}

	arrange(mon, d, true);
	ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", d->name,
		layout_to_char(d->layout));

	if (d->focus != NULL)
		focus_node(mon, d, d->focus);
}

void toggle_master_stack(void) {
	if (mon == NULL || mon->desk == NULL)
		return;

	desktop_t *d = mon->desk;

	if (d->layout == LAYOUT_MASTER_STACK) {
		layout_set(d, d->user_layout);
	} else {
		layout_toggle(d, LAYOUT_MASTER_STACK);
	}

	if (d->root) {
		FOR_EACH_LEAF(n, d->root) {
			if (n->client && n->client->toplevel && n->client->state != STATE_FULLSCREEN) {
				n->client->toplevel->client_maximized = false;
				wlr_xdg_toplevel_set_maximized(n->client->toplevel->xdg_toplevel, false);
			}
		}
	}

	arrange(mon, d, true);
	ipc_put_status(SUB_MASK_DESKTOP_LAYOUT, "desktop_layout[%s,%c]\n", d->name,
		layout_to_char(d->layout));

	if (d->focus != NULL)
		focus_node(mon, d, d->focus);
}

void rotate_clockwise(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
		return;

	rotate_tree(mon->desk->root, 90);
	arrange(mon, mon->desk, true);
	wlr_log(WLR_INFO, "Rotated tree clockwise");
}

void rotate_counterclockwise(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->root == NULL)
		return;

	rotate_tree(mon->desk->root, 270);
	arrange(mon, mon->desk, true);
	wlr_log(WLR_INFO, "Rotated tree counterclockwise");
}

#define RESIZE_AMOUNT 0.05

static bool resize_master_stack_ratio(bool horizontal, float delta) {
	if (mon->desk->layout != LAYOUT_MASTER_STACK)
		return false;

	bool horizontal_split = master_stack_orientation == MASTER_LEFT ||
		master_stack_orientation == MASTER_RIGHT || master_stack_orientation == MASTER_CENTER;
	if (horizontal != horizontal_split)
		return true;

	if (master_stack_orientation == MASTER_RIGHT || master_stack_orientation == MASTER_BOTTOM)
		delta = -delta;

	if (master_stack_adjust_ratio(delta))
		arrange(mon, mon->desk, true);
	return true;
}

void resize_left(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL) {
		wlr_log(WLR_ERROR, "resize_left: invalid state mon=%p desk=%p focus=%p", (void *)mon,
			mon ? (void *)mon->desk : NULL, mon && mon->desk ? (void *)mon->desk->focus : NULL);
		return;
	}
	if (resize_master_stack_ratio(true, -RESIZE_AMOUNT))
		return;
	if (mon->desk->layout == LAYOUT_SCROLLER) {
		if (scroller_resize_width(mon->desk, -RESIZE_AMOUNT)) {
			arrange(mon, mon->desk, true);
			wlr_log(WLR_INFO, "Resized left (scroller)");
		}
		return;
	}

	node_t *n = mon->desk->focus;
	if (n->parent == NULL) {
		wlr_log(WLR_ERROR, "resize_left: no parent node");
		return;
	}

	// walk up to find nearest ancestor with VERTICAL split
	node_t *child = n;
	node_t *p = n->parent;
	for (child = p; p != NULL && p->split_type != TYPE_VERTICAL; p = p->parent)
		;

	wlr_log(WLR_INFO, "resize_left: node=%u ancestor=%u split_type=%d is_first=%d ratio_before=%f",
		n->id, p ? p->id : 0, p ? (int)p->split_type : -1, p ? is_first_child(child) : -1,
		p ? p->split_ratio : 0.0f);

	if (p != NULL) {
		p->split_ratio -= RESIZE_AMOUNT;
		if (p->split_ratio < 0.1)
			p->split_ratio = 0.1;

		// sync with pending and current state
		p->pending.split_ratio = p->split_ratio;
		p->current.split_ratio = p->split_ratio;
		wlr_log(WLR_INFO, "resize_left: ratio_after=%f", p->split_ratio);
		arrange(mon, mon->desk, true);
		wlr_log(WLR_INFO, "Resized left");
	} else {
		wlr_log(WLR_ERROR, "resize_left: no VERTICAL ancestor found");
	}
}

void resize_right(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL) {
		wlr_log(WLR_ERROR, "resize_right: invalid state");
		return;
	}
	if (resize_master_stack_ratio(true, RESIZE_AMOUNT))
		return;
	if (mon->desk->layout == LAYOUT_SCROLLER) {
		if (scroller_resize_width(mon->desk, RESIZE_AMOUNT)) {
			arrange(mon, mon->desk, true);
			wlr_log(WLR_INFO, "Resized right (scroller)");
		}
		return;
	}

	node_t *n = mon->desk->focus;
	if (n->parent == NULL) {
		wlr_log(WLR_ERROR, "resize_right: no parent node");
		return;
	}

	// walk up to find earest ancestor with VERTICAL split
	node_t *child = n;
	node_t *p = n->parent;
	for (child = p; p != NULL && p->split_type != TYPE_VERTICAL; p = p->parent)
		;

	wlr_log(WLR_INFO, "resize_right: node=%u ancestor=%u split_type=%d is_first=%d ratio_before=%f",
		n->id, p ? p->id : 0, p ? (int)p->split_type : -1, p ? is_first_child(child) : -1,
		p ? p->split_ratio : 0.0f);

	if (p != NULL) {
		p->split_ratio += RESIZE_AMOUNT;
		if (p->split_ratio > 0.9)
			p->split_ratio = 0.9;

		// sync with pending and current state
		p->pending.split_ratio = p->split_ratio;
		p->current.split_ratio = p->split_ratio;
		wlr_log(WLR_INFO, "resize_right: ratio_after=%f", p->split_ratio);
		arrange(mon, mon->desk, true);
		wlr_log(WLR_INFO, "Resized right");
	} else {
		wlr_log(WLR_ERROR, "resize_right: no VERTICAL ancestor found");
	}
}

void resize_up(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL) {
		wlr_log(WLR_ERROR, "resize_up: invalid state");
		return;
	}
	if (resize_master_stack_ratio(false, -RESIZE_AMOUNT))
		return;
	if (mon->desk->layout == LAYOUT_SCROLLER) {
		float delta = -RESIZE_AMOUNT;
		if (mon->desk->scroller_state)
			delta *= (float)mon->desk->scroller_state->working_area.height;
		if (scroller_resize_stack(mon->desk, delta)) {
			arrange(mon, mon->desk, true);
			wlr_log(WLR_INFO, "Resized up (scroller)");
		}
		return;
	}

	node_t *n = mon->desk->focus;
	if (n->parent == NULL) {
		wlr_log(WLR_ERROR, "resize_up: no parent node");
		return;
	}

	// walk up to find nearest ancestor with HORIZONTAL split
	node_t *child = n;
	node_t *p = n->parent;
	for (child = p; p != NULL && p->split_type != TYPE_HORIZONTAL; p = p->parent)
		;

	wlr_log(WLR_INFO, "resize_up: node=%u ancestor=%u split_type=%d is_first=%d ratio_before=%f", n->id,
		p ? p->id : 0, p ? (int)p->split_type : -1, p ? is_first_child(child) : -1,
		p ? p->split_ratio : 0.0f);

	if (p != NULL) {
		p->split_ratio -= RESIZE_AMOUNT;
		if (p->split_ratio < 0.1)
			p->split_ratio = 0.1;

		// sync with pending and current state
		p->pending.split_ratio = p->split_ratio;
		p->current.split_ratio = p->split_ratio;
		wlr_log(WLR_INFO, "resize_up: ratio_after=%f", p->split_ratio);
		arrange(mon, mon->desk, true);
		wlr_log(WLR_INFO, "Resized up");
	} else {
		wlr_log(WLR_ERROR, "resize_up: no HORIZONTAL ancestor found");
	}
}

void resize_down(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL) {
		wlr_log(WLR_ERROR, "resize_down: invalid state");
		return;
	}
	if (resize_master_stack_ratio(false, RESIZE_AMOUNT))
		return;
	if (mon->desk->layout == LAYOUT_SCROLLER) {
		float delta = RESIZE_AMOUNT;
		if (mon->desk->scroller_state)
			delta *= (float)mon->desk->scroller_state->working_area.height;
		if (scroller_resize_stack(mon->desk, delta)) {
			arrange(mon, mon->desk, true);
			wlr_log(WLR_INFO, "Resized down (scroller)");
		}
		return;
	}

	node_t *n = mon->desk->focus;
	if (n->parent == NULL) {
		wlr_log(WLR_ERROR, "resize_down: no parent node");
		return;
	}

	// walk up to find nearest ancestor with HORIZONTAL split
	node_t *child = n;
	node_t *p = n->parent;
	for (child = p; p != NULL && p->split_type != TYPE_HORIZONTAL; p = p->parent)
		;

	wlr_log(WLR_INFO, "resize_down: node=%u ancestor=%u split_type=%d is_first=%d ratio_before=%f",
		n->id, p ? p->id : 0, p ? (int)p->split_type : -1, p ? is_first_child(child) : -1,
		p ? p->split_ratio : 0.0f);

	if (p != NULL) {
		p->split_ratio += RESIZE_AMOUNT;
		if (p->split_ratio > 0.9)
			p->split_ratio = 0.9;

		// sync with pending and current state
		p->pending.split_ratio = p->split_ratio;
		p->current.split_ratio = p->split_ratio;
		wlr_log(WLR_INFO, "resize_down: ratio_after=%f", p->split_ratio);
		arrange(mon, mon->desk, true);
		wlr_log(WLR_INFO, "Resized down");
	} else {
		wlr_log(WLR_ERROR, "resize_down: no HORIZONTAL ancestor found");
	}
}

void cancel_presel(void) {
	if (mon == NULL || mon->desk == NULL || mon->desk->focus == NULL)
		return;

	presel_cancel(mon->desk->focus);
	wlr_log(WLR_INFO, "Cancelled preselection");
}

static bool repeat_info_match(struct wlr_keyboard *a, struct wlr_keyboard *b) {
	return a->repeat_info.rate == b->repeat_info.rate && a->repeat_info.delay == b->repeat_info.delay;
}

// remove a keyboard from its group (if any)
void keyboard_group_remove(keyboard_t *keyboard) {
	if (!keyboard->group)
		return;

	keyboard_group_t *group = keyboard->group;
	struct wlr_keyboard_group *wlr_group = group->wlr_group;

	wlr_log(WLR_DEBUG, "Removing keyboard %p from group %p", (void *)keyboard, (void *)wlr_group);

	wlr_keyboard_group_remove_keyboard(wlr_group, keyboard->wlr_keyboard);
	keyboard->group = NULL;

	if (wl_list_empty(&wlr_group->devices)) {
		wlr_log(WLR_DEBUG, "Destroying empty keyboard group %p", (void *)wlr_group);

		if (server.seat->keyboard_state.keyboard == group->representative->wlr_keyboard)
			wlr_seat_set_keyboard(server.seat, NULL);

		if (group->representative) {
			wl_list_remove(&group->representative->active_link);
			wl_list_remove(&group->representative->modifiers.link);
			wl_list_remove(&group->representative->key.link);
			free(group->representative);
			group->representative = NULL;
		}

		wl_list_remove(&group->link);

		struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
		wl_event_loop_add_idle(event_loop, destroy_empty_wlr_keyboard_group, group);
	}
}

void keyboard_group_remove_invalid(keyboard_t *keyboard) {
	if (!keyboard->group)
		return;

	keyboard_group_t *group = keyboard->group;
	keyboard_grouping_t grouping = get_keyboard_grouping();

	bool should_remove = false;
	switch (grouping) {
	case KEYBOARD_GROUP_NONE:
		should_remove = true;
		break;
	case KEYBOARD_GROUP_DEFAULT:
		fallthrough;
	case KEYBOARD_GROUP_SMART: {
		if (!wlr_keyboard_keymaps_match(keyboard->wlr_keyboard->keymap,
				group->wlr_group->keyboard.keymap) || !repeat_info_match(keyboard->wlr_keyboard,
				&group->wlr_group->keyboard)) {
			should_remove = true;
		}
		break;
	}
	}

	if (should_remove)
		keyboard_group_remove(keyboard);
}

void keyboard_group_add(keyboard_t *keyboard) {
	keyboard_grouping_t grouping = get_keyboard_grouping();

	if (grouping == KEYBOARD_GROUP_NONE) {
		keyboard->modifiers.notify = keyboard_modifiers;
		wl_signal_add(&keyboard->wlr_keyboard->events.modifiers, &keyboard->modifiers);
		keyboard->key.notify = keyboard_key;
		wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);
		wl_list_insert(&server.keyboards, &keyboard->active_link);
		return;
	}

	keyboard_group_t *group;
	wl_list_for_each(group, &server.keyboard_groups, link) {
		if (wlr_keyboard_keymaps_match(keyboard->wlr_keyboard->keymap,
				group->wlr_group->keyboard.keymap) && repeat_info_match(keyboard->wlr_keyboard,
				&group->wlr_group->keyboard)) {
			wlr_log(WLR_DEBUG, "Adding keyboard %p to existing group %p", (void *)keyboard,
				(void *)group->wlr_group);
			wlr_keyboard_group_add_keyboard(group->wlr_group, keyboard->wlr_keyboard);
			keyboard->group = group;
			if (server.seat->keyboard_state.keyboard == keyboard->wlr_keyboard)
				wlr_seat_set_keyboard(server.seat, group->representative->wlr_keyboard);
			return;
		}
	}

	keyboard_group_t *new_group = calloc(1, sizeof(*new_group));
	if (!new_group) {
		wlr_log(WLR_ERROR, "Failed to allocate keyboard group");
		return;
	}

	new_group->wlr_group = wlr_keyboard_group_create();
	if (!new_group->wlr_group) {
		wlr_log(WLR_ERROR, "Failed to create wlr_keyboard_group");
		free(new_group);
		return;
	}
	new_group->wlr_group->data = new_group;

	wlr_keyboard_set_keymap(&new_group->wlr_group->keyboard, keyboard->wlr_keyboard->keymap);
	wlr_keyboard_set_repeat_info(&new_group->wlr_group->keyboard,
		keyboard->wlr_keyboard->repeat_info.rate, keyboard->wlr_keyboard->repeat_info.delay);

	keyboard_t *rep = calloc(1, sizeof(*rep));
	if (!rep) {
		wlr_log(WLR_ERROR, "Failed to allocate group representative keyboard");
		wlr_keyboard_group_destroy(new_group->wlr_group);
		free(new_group);
		return;
	}
	rep->wlr_keyboard = &new_group->wlr_group->keyboard;
	rep->group = NULL;
	rep->is_representative = true;
	rep->seat = seat_default();

	// listeners
	rep->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&rep->wlr_keyboard->events.modifiers, &rep->modifiers);
	rep->key.notify = keyboard_key;
	wl_signal_add(&rep->wlr_keyboard->events.key, &rep->key);

	// add to list
	wl_list_insert(&server.keyboards, &rep->active_link);

	new_group->representative = rep;

	wl_list_insert(&server.keyboard_groups, &new_group->link);

	wlr_keyboard_group_add_keyboard(new_group->wlr_group, keyboard->wlr_keyboard);
	keyboard->group = new_group;

	if (server.seat->keyboard_state.keyboard == keyboard->wlr_keyboard)
		wlr_seat_set_keyboard(server.seat, new_group->representative->wlr_keyboard);

	wlr_log(WLR_DEBUG, "Created new keyboard group %p for keyboard %p", (void *)new_group,
		(void *)keyboard);
}

void keyboard_reapply_grouping(void) {
	keyboard_t *keyboard, *tmp;
	wl_list_for_each_safe(keyboard, tmp, &server.physical_keyboards, all_link) {
		if (keyboard->group) {
			keyboard_group_remove(keyboard);
		} else if (!keyboard->is_representative) {
			if (keyboard->active_link.next || keyboard->active_link.prev)
				wl_list_remove(&keyboard->active_link);
			if (keyboard->modifiers.link.next)
				wl_list_remove(&keyboard->modifiers.link);
			if (keyboard->key.link.next)
				wl_list_remove(&keyboard->key.link);
		}
		keyboard_group_add(keyboard);
	}

	if (!server.seat->keyboard_state.keyboard && !wl_list_empty(&server.keyboards)) {
		keyboard_t *first = wl_container_of(server.keyboards.next, first, active_link);
		if (first)
			wlr_seat_set_keyboard(server.seat, first->wlr_keyboard);
	}
}
