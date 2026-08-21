#include "animation.h"
#include "config.h"
#include "copy_capture.h"
#include "cursor.h"
#include "effects.h"
#include "input_method.h"
#include "ipc.h"
#include "keyboard.h"
#include "output.h"
#include "popup.h"
#include "render_unfocused.h"
#include "rule.h"
#include "scratchpad.h"
#include "scroller.h"
#include "seat.h"
#include "server.h"
#include "surface.h"
#include "tablet.h"
#include "tabs.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include "workspace.h"
#include "xwayland.h"
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_ext_background_effect_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/log.h>

static struct wlr_fbox clamp_scene_buffer_source_box(struct wlr_scene_buffer *buffer,
		struct wlr_fbox box) {
	if (!buffer || !buffer->buffer)
		return (struct wlr_fbox){0};

	if (box.x < 0.0f) {
		box.width += box.x;
		box.x = 0.0f;
	}
	if (box.y < 0.0f) {
		box.height += box.y;
		box.y = 0.0f;
	}

	float max_w = (float)buffer->buffer->width;
	float max_h = (float)buffer->buffer->height;

	if (max_w < 1.0f || max_h < 1.0f)
		return (struct wlr_fbox){
			0,
			0,
			1,
			1
		};

	if (box.x > max_w)
		box.x = max_w;
	if (box.y > max_h)
		box.y = max_h;
	if (box.x + box.width > max_w)
		box.width = max_w - box.x;
	if (box.y + box.height > max_h)
		box.height = max_h - box.y;

	if (box.width < 1.0f)
		box.width = 1.0f;
	if (box.height < 1.0f)
		box.height = 1.0f;
	if (box.x + box.width > max_w)
		box.x = max_w - box.width;
	if (box.y + box.height > max_h)
		box.y = max_h - box.height;
	if (box.x < 0.0f)
		box.x = 0.0f;
	if (box.y < 0.0f)
		box.y = 0.0f;

	return box;
}

static void handle_foreign_activate_request(struct wl_listener *listener, void *data);
static void handle_foreign_fullscreen_request(struct wl_listener *listener, void *data);
static void handle_foreign_close_request(struct wl_listener *listener, void *data);
static void handle_foreign_destroy(struct wl_listener *listener, void *data);
static void handle_outputs_update(struct wl_listener *listener, void *data);

static bool toplevel_should_use_server_decorations(toplevel_t *tl) {
	if (!tl || !tl->node)
		return false;

	switch (decoration_mode) {
	case DECORATION_NONE:
	case DECORATION_TABS:
		return true;
	case DECORATION_ALWAYS:
		return tabbed_ancestor(tl->node) != NULL;
	case DECORATION_CSD:
		return false;
	}
	return false;
}

void toplevel_apply_decoration_mode(struct toplevel_t *tl) {
	if (!tl || !tl->xdg_decoration || !tl->xdg_toplevel || !tl->xdg_toplevel->base)
		return;

	if (!tl->xdg_toplevel->base->initialized)
		return;

	enum wlr_xdg_toplevel_decoration_v1_mode mode = toplevel_should_use_server_decorations(tl) ?
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;

	wlr_xdg_toplevel_decoration_v1_set_mode(tl->xdg_decoration, mode);
}

static void update_ext_foreign_toplevel(toplevel_t *toplevel) {
	if (!toplevel->ext_foreign_toplevel || !toplevel->node || !toplevel->node->client)
		return;

	struct wlr_ext_foreign_toplevel_handle_v1_state state = {0};
	client_t *c = toplevel->node->client;

	if (c->title[0] != '\0')
		state.title = c->title;
	if (c->app_id[0] != '\0')
		state.app_id = c->app_id;

	wlr_ext_foreign_toplevel_handle_v1_update_state(toplevel->ext_foreign_toplevel, &state);
}

void update_foreign_toplevel_state(toplevel_t *toplevel) {
	if (!toplevel->foreign_toplevel || !toplevel->node || !toplevel->node->client)
		return;

	client_t *c = toplevel->node->client;
	bool maximized = (c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED);
	bool fullscreen = (c->state == STATE_FULLSCREEN);

	wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign_toplevel, fullscreen);
	wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign_toplevel, maximized);
	wlr_foreign_toplevel_handle_v1_set_minimized(toplevel->foreign_toplevel, false);
}

static void handle_foreign_activate_request(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, foreign_activate_request);

	if (!toplevel->node || !toplevel->node->client)
		return;

	output_t *m = toplevel->node->output;
	if (!m)
		return;

	desktop_t *toplevel_desk = toplevel->node->desktop;
	if (!toplevel_desk)
		return;

	if (m->desk != toplevel_desk)
		workspace_switch_to_desktop(toplevel_desk->name);

	if (toplevel_desk->focus == toplevel->node)
		return;

	node_t *prev = toplevel_desk->focus;

	if (prev && prev->client && prev->client->toplevel) {
		struct toplevel_t *prev_toplevel = prev->client->toplevel;
		wlr_xdg_toplevel_set_activated(prev_toplevel->xdg_toplevel, false);
		if (prev_toplevel->foreign_toplevel)
			wlr_foreign_toplevel_handle_v1_set_activated(prev_toplevel->foreign_toplevel, false);
	}

	activate_node(m, toplevel_desk, toplevel->node);
}

static void handle_foreign_fullscreen_request(struct wl_listener *listener, void *data) {
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, foreign_fullscreen_request);

	if (toplevel->node == NULL || toplevel->node->client == NULL)
		return;

	output_t *m = toplevel->node->output;
	desktop_t *d = m ? m->desk : NULL;

	if (event->fullscreen) {
		set_state(m, d, toplevel->node, STATE_FULLSCREEN);
		wlr_scene_node_reparent(&toplevel->scene_tree->node, server.full_tree);
	} else {
		client_state_t last = toplevel->node->client->last_state;
		if (last == STATE_FLOATING) {
			set_state(m, d, toplevel->node, STATE_FLOATING);
			wlr_scene_node_reparent(&toplevel->scene_tree->node, server.float_tree);
		} else {
			set_state(m, d, toplevel->node, STATE_TILED);
			wlr_scene_node_reparent(&toplevel->scene_tree->node, server.tile_tree);
		}
	}

	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, event->fullscreen);
}

static void handle_foreign_close_request(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, foreign_close_request);
	wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void handle_foreign_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, foreign_destroy);

	wl_list_remove(&toplevel->foreign_activate_request.link);
	wl_list_remove(&toplevel->foreign_fullscreen_request.link);
	wl_list_remove(&toplevel->foreign_close_request.link);
	wl_list_remove(&toplevel->foreign_destroy.link);
}

static void handle_outputs_update(struct wl_listener *listener, void *data) {
	toplevel_t *toplevel = wl_container_of(listener, toplevel, outputs_update);
	struct wlr_scene_outputs_update_event *event = data;

	if (toplevel->foreign_toplevel) {
		struct wlr_foreign_toplevel_handle_v1_output *toplevel_output, *tmp;
		wl_list_for_each_safe(toplevel_output, tmp, &toplevel->foreign_toplevel->outputs, link) {
			bool active = false;
			for (size_t i = 0; i < event->size; i++) {
				struct wlr_scene_output *scene_output = event->active[i];
				if (scene_output->output == toplevel_output->output) {
					active = true;
					break;
				}
			}

			if (!active) {
				wlr_log(WLR_DEBUG, "Toplevel output leave: %s", toplevel_output->output->name);
				wlr_foreign_toplevel_handle_v1_output_leave(toplevel->foreign_toplevel,
					toplevel_output->output);
			}
		}

		for (size_t i = 0; i < event->size; i++) {
			struct wlr_scene_output *scene_output = event->active[i];
			wlr_log(WLR_DEBUG, "Toplevel output enter: %s", scene_output->output->name);
			wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign_toplevel, scene_output->output);
		}
	}
}

static bool toplevel_output_handler_point_accepts_input(struct wlr_scene_buffer *buffer, double *x,
		double *y) {
	(void)buffer;
	(void)x;
	(void)y;
	return false;
}

void toplevel_center_and_clip_surface(toplevel_t *toplevel) {
	if (!toplevel || !toplevel->content_tree || !toplevel->node || !toplevel->node->client)
		return;

	client_t *c = toplevel->node->client;
	bool floating = (c->state == STATE_FLOATING);
	bool fullscreen = (c->state == STATE_FULLSCREEN);
	bool tiled = IS_TILED(c);
	int x = 0, y = 0;
	struct wlr_box *container_rect = NULL;
	bool clip_to_geometry = true;

	// center floating, fullscreen, and undersized tiled surfaces
	if (floating || fullscreen || tiled) {
		if (floating) {
			container_rect = &c->floating_rectangle;
		} else if (fullscreen) {
			output_t *m = toplevel->node->output;
			container_rect = m ? &m->rectangle : &c->tiled_rectangle;
		} else {
			container_rect = &c->tiled_rectangle;
		}

		if (container_rect && toplevel->geometry.width > 0 && toplevel->geometry.height > 0) {
			int center_x = (container_rect->width - toplevel->geometry.width) / 2;
			int center_y = (container_rect->height - toplevel->geometry.height) / 2;

			x = center_x > 0 ? center_x : 0;
			y = center_y > 0 ? center_y : 0;

			if ((floating || fullscreen) && (x != 0 || y != 0)) {
				wlr_log(WLR_DEBUG, "Centering surface: %dx%d at offset (%d,%d) in container %dx%d",
					toplevel->geometry.width, toplevel->geometry.height, x, y, container_rect->width,
					container_rect->height);
				clip_to_geometry = false;
			}
		}
	}

	wlr_scene_node_set_position(&toplevel->content_tree->node, x, y);

	// when tiled or floating surface is smaller than its container, update borders
	// to wrap the actual surface instead of the full allocated space
	if ((tiled || floating) && toplevel->border_tree) {
		unsigned int bw = effective_border_width(toplevel->node->desktop);
		if (bw > 0) {
			if (tiled && (x > 0 || y > 0)) {
				int border_w = (int)toplevel->geometry.width < container_rect->width ?
					(int)toplevel->geometry.width : container_rect->width;
				int border_h = (int)toplevel->geometry.height < container_rect->height ?
					(int)toplevel->geometry.height : container_rect->height;
				struct wlr_box content_geo = {
					0,
					0,
					border_w,
					border_h
				};
				update_borders(toplevel->border_tree, toplevel->border_rects, content_geo, bw);
				wlr_scene_node_set_position(&toplevel->border_tree->node, (int)x - (int)bw, (int)y - (int)bw);
				update_border_colors(c);
				if (toplevel->rounded && toplevel->rounded->border_shader_node && (c->border_radius > 0.0f ||
						toplevel->rounded->gradient_count >= 2)) {
					rounded_mark_border_size(toplevel->rounded, border_w, border_h, (int)bw,
						toplevel->node && toplevel->node->output ? toplevel->node->output->wlr_output->scale : 1.0f);
					if (border_w + 2 * (int)bw > 0)
						wlr_scene_buffer_set_dest_size(toplevel->rounded->border_shader_node, border_w + 2 * (int)bw,
							border_h + 2 * (int)bw);
				}
			} else if (container_rect) {
				struct wlr_box full_geo = {
					0,
					0,
					container_rect->width,
					container_rect->height
				};
				update_borders(toplevel->border_tree, toplevel->border_rects, full_geo, bw);
				update_border_colors(c);
				if (toplevel->rounded && toplevel->rounded->border_shader_node && (c->border_radius > 0.0f ||
						toplevel->rounded->gradient_count >= 2)) {
					rounded_mark_border_size(toplevel->rounded, container_rect->width, container_rect->height,
						(int)bw,
						toplevel->node && toplevel->node->output ? toplevel->node->output->wlr_output->scale : 1.0f);
					if (container_rect->width + 2 * (int)bw > 0)
						wlr_scene_buffer_set_dest_size(toplevel->rounded->border_shader_node,
							container_rect->width + 2 * (int)bw, container_rect->height + 2 * (int)bw);
				}
			}
		} else if (toplevel->border_tree->node.enabled) {
			wlr_scene_node_set_enabled(&toplevel->border_tree->node, false);
		}
	}

	if (!wl_list_empty(&toplevel->content_tree->children) && container_rect) {
		int clip_w = container_rect->width;
		int clip_h = container_rect->height;
		if (tiled && toplevel->geometry.width > 0 && toplevel->geometry.height > 0) {
			if ((int)toplevel->geometry.width < container_rect->width)
				clip_w = toplevel->geometry.width;
			else if ((int)toplevel->geometry.width > container_rect->width)
				clip_w = container_rect->width;
			if ((int)toplevel->geometry.height < container_rect->height)
				clip_h = toplevel->geometry.height;
			else if ((int)toplevel->geometry.height > container_rect->height)
				clip_h = container_rect->height;
		}

		struct wlr_box clip = {
			.x = toplevel->geometry.x,
			.y = toplevel->geometry.y,
			.width = clip_w,
			.height = clip_h
		};

		if (clip_to_geometry) {
			wlr_scene_subsurface_tree_set_clip(&toplevel->content_tree->node, &clip);
		} else {
			wlr_scene_subsurface_tree_set_clip(&toplevel->content_tree->node, NULL);
		}
	}

	if (toplevel->shadow)
		toplevel->shadow->shadow_geometry_dirty = true;
}

bool toplevel_get_surface_offset(toplevel_t *tl, int *ox, int *oy) {
	if (!tl || !tl->scene_tree || !tl->content_tree)
		return false;

	int x = tl->content_tree->node.x - tl->geometry.x;
	int y = tl->content_tree->node.y - tl->geometry.y;
	if (ox)
		*ox = x;
	if (oy)
		*oy = y;
	return true;
}

static void toplevel_set_floating(toplevel_t *toplevel, node_t *n, output_t *output) {
	wlr_scene_node_reparent(&toplevel->scene_tree->node, server.float_tree);
	struct wlr_box mon_rect = output->rectangle;
	struct wlr_box base_rect = n->client->toplevel->xdg_toplevel->base->geometry;
	n->client->floating_rectangle = (struct wlr_box){
		.x = mon_rect.x + (mon_rect.width - base_rect.width) / 2,
		.y = mon_rect.y + (mon_rect.height - base_rect.height) / 2,
		.width = base_rect.width,
		.height = base_rect.height
	};
	wlr_scene_node_set_position(&toplevel->scene_tree->node, n->client->floating_rectangle.x,
		n->client->floating_rectangle.y);
	effects_dirty_corner_masks(output);
	n->client->last_state = STATE_TILED;
	n->client->state = STATE_FLOATING;
	n->hidden = true;
	n->client->flags.shown = true;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
}

void toplevel_map(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, map);

	wlr_log(WLR_INFO, "Toplevel mapped");

	toplevel->mapped = true;
	toplevel->configured = false;

	output_t *m = server.focused_output;
	if (m == NULL) {
		wlr_log(WLR_ERROR, "No monitor available for toplevel");
		return;
	}

	desktop_t *d = m->desk;
	if (d == NULL) {
		// try to find a valid output with a desk
		output_t *valid_output = output_get_valid();
		if (valid_output) {
			m = valid_output;
			d = m->desk;
			server.focused_output = m;
		} else {
			wlr_log(WLR_ERROR, "No desktop available for toplevel");
			return;
		}
	}

	node_t *n = make_node(0);
	if (n == NULL) {
		wlr_log(WLR_ERROR, "Failed to create node for toplevel");
		return;
	}

	n->client = make_client();
	if (n->client == NULL) {
		wlr_log(WLR_ERROR, "Failed to create client for toplevel");
		free_node(n);
		return;
	}

	// link client and toplevel
	n->client->toplevel = toplevel;
	toplevel->node = n;

	// populate constraints from xdg_toplevel protocol state
	struct wlr_xdg_toplevel_state *toplevel_state = &toplevel->xdg_toplevel->current;
	if (toplevel_state->min_width > 0)
		n->constraints.min_width = toplevel_state->min_width;
	if (toplevel_state->min_height > 0)
		n->constraints.min_height = toplevel_state->min_height;

	// set initial app_id and title
	const char *app_id = toplevel->xdg_toplevel->app_id;
	const char *title = toplevel->xdg_toplevel->title;

	if (app_id) {
		strncpy(n->client->app_id, app_id, MAXLEN - 1);
		n->client->app_id[MAXLEN - 1] = '\0';
	}

	if (title) {
		strncpy(n->client->title, title, MAXLEN - 1);
		n->client->title[MAXLEN - 1] = '\0';
	}

	wlr_log(WLR_INFO, "New window: %s (%s)", title ? title : "untitled", app_id ? app_id : "unknown");

	rule_consequence_t *rule = find_matching_rule(app_id, title, toplevel->tag);

	if (rule && rule->has & RULE_TYPE_MANAGE && !(rule->flags & RULE_TYPE_MANAGE)) {
		wlr_log(WLR_INFO, "Window %s ignored by rule (manage=off)", app_id ? app_id : "?");
		free_node(n);
		return;
	}

	bool should_focus = true;
	if (rule) {
		if (rule->has & RULE_TYPE_FOCUS && !(rule->flags & RULE_TYPE_FOCUS))
			should_focus = false;
	}

	// find target monitor
	output_t *target_output = m;
	if (rule && rule->has & RULE_TYPE_MONITOR) {
		wlr_log(WLR_DEBUG, "  Rule specifies monitor: %s", rule->monitor);
		struct output_t *new_monitor = find_output_by_name(rule->monitor);
		if (new_monitor) {
			target_output = new_monitor;
			wlr_log(WLR_DEBUG, "  Target desktop changed to: %s", target_output->name);
		} else {
			wlr_log(WLR_ERROR, "  Monitor %s not found", rule->monitor);
		}
	}

	if (!target_output)
		target_output = m;

	// If target_output is disabled, fall back to the focused output
	if (!target_output || !target_output->wlr_output) {
		wlr_log(WLR_INFO, "Target output is not available, using focused output");
		target_output = output_get_valid();
		if (!target_output) {
			wlr_log(WLR_ERROR, "No valid output available for toplevel");
			free_node(n);
			return;
		}
	}

	n->output = target_output;

	// find target desktop
	desktop_t *target_desktop = d;
	wlr_log(WLR_DEBUG, "Window %s: current desktop=%s, has_rule=%d", app_id ? app_id : "?", d->name,
		rule != NULL);
	if (rule && rule->has & RULE_TYPE_DESKTOP) {
		wlr_log(WLR_DEBUG, "  Rule specifies desktop: %s", rule->desktop);
		desktop_t *new_desk = find_desktop_by_name_in_monitor(target_output, rule->desktop);
		if (new_desk) {
			target_desktop = new_desk;
			wlr_log(WLR_DEBUG, "  Target desktop changed to: %s", target_desktop->name);
		} else {
			wlr_log(WLR_ERROR, "  Desktop %s not found", rule->desktop);
		}
	}

	bool desktop_changed = (target_desktop != d);
	wlr_log(WLR_DEBUG, "  Final target desktop: %s (changed=%d)", target_desktop->name,
		desktop_changed);

	rule_apply_consequence(n, n->client, rule);

	if (rule) {
		if (rule->has & RULE_TYPE_BLUR)
			toplevel_set_effect(toplevel, EFFECT_BLUR, rule->flags & RULE_TYPE_BLUR);
		if (rule->has & RULE_TYPE_MICA)
			toplevel_set_effect(toplevel, EFFECT_MICA, rule->flags & RULE_TYPE_MICA);
		if (rule->has & RULE_TYPE_ACRYLIC)
			toplevel_set_effect(toplevel, EFFECT_ACRYLIC, rule->flags & RULE_TYPE_ACRYLIC);
		if (rule->has & RULE_TYPE_BORDER_RADIUS)
			toplevel_set_border_radius(toplevel, rule->border_radius);
		if (rule->has & RULE_TYPE_SHADOW)
			toplevel_set_shadow(toplevel, rule->flags & RULE_TYPE_SHADOW);
		if (rule->has & RULE_TYPE_OPACITY)
			surface_set_opacity(&toplevel->scene_tree->node, rule->opacity);
	}

	// create foreign toplevel handles
	struct wlr_ext_foreign_toplevel_handle_v1_state ext_state = {
		.app_id = app_id,
		.title = title,
	};
	toplevel->ext_foreign_toplevel =
		wlr_ext_foreign_toplevel_handle_v1_create(server.foreign_toplevel_list, &ext_state);
	toplevel->ext_foreign_toplevel->data = toplevel;
	toplevel->foreign_identifier = toplevel->ext_foreign_toplevel->identifier;

	toplevel->foreign_toplevel =
		wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);

	toplevel->foreign_activate_request.notify = handle_foreign_activate_request;
	wl_signal_add(&toplevel->foreign_toplevel->events.request_activate,
		&toplevel->foreign_activate_request);

	toplevel->foreign_fullscreen_request.notify = handle_foreign_fullscreen_request;
	wl_signal_add(&toplevel->foreign_toplevel->events.request_fullscreen,
		&toplevel->foreign_fullscreen_request);

	toplevel->foreign_close_request.notify = handle_foreign_close_request;
	wl_signal_add(&toplevel->foreign_toplevel->events.request_close, &toplevel->foreign_close_request);

	toplevel->foreign_destroy.notify = handle_foreign_destroy;
	wl_signal_add(&toplevel->foreign_toplevel->events.destroy, &toplevel->foreign_destroy);

	// set app_id on foreign toplevel handle
	if (app_id)
		wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->foreign_toplevel, app_id);

	// center to output if floating, also ensure it does not tile
	if (rule && rule->state == STATE_FLOATING) {
		toplevel_set_floating(toplevel, n, target_output);
	} else if (rule && rule->state == STATE_PSEUDO_TILED) {
		struct wlr_box base_rect = n->client->toplevel->xdg_toplevel->base->geometry;
		n->client->floating_rectangle = (struct wlr_box){
			.x = 0,
			.y = 0,
			.width = base_rect.width,
			.height = base_rect.height
		};
		n->client->state = STATE_PSEUDO_TILED;
	} else if (auto_float_dialogs && toplevel->is_dialog) {
		toplevel_set_floating(toplevel, n, target_output);
	}

	// notify wlr_foreign_toplevel clients about the output association
	if (toplevel->foreign_toplevel && target_output && target_output->wlr_output)
		wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign_toplevel,
			target_output->wlr_output);

	// create borders if applicable
	create_borders(toplevel->scene_tree, &toplevel->border_tree, toplevel->border_rects);

	// mark dirty to update borders
	if (n->client->state == STATE_FLOATING)
		node_set_dirty(n);

	// insert node into tree
	node_t *focus = target_desktop->focus;
	insert_node(target_desktop, n, focus);

	// in scroller layout, also track the window in the scroller state.
	if (target_desktop->layout == LAYOUT_SCROLLER && target_desktop->scroller_state &&
		IS_TILED(n->client))
		scroller_add_tile(target_desktop->scroller_state, n->client, should_focus);

	// notify client of scale
	if (target_output && target_output->wlr_output) {
		float scale = target_output->wlr_output->scale;
		wlr_fractional_scale_v1_notify_scale(toplevel->xdg_toplevel->base->surface, scale);
		wlr_surface_set_preferred_buffer_scale(toplevel->xdg_toplevel->base->surface, ceil(scale));
	}

	// hide window if target desktop is not the focused desktop
	bool target_desktop_is_focused = (target_desktop == (target_output ? target_output->desk : NULL));
	if (desktop_changed && !target_desktop_is_focused) {
		n->client->flags.shown = false;
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
	}

	if (should_focus && target_output)
		activate_node(target_output, target_desktop, n);

	if (rule && rule->state == STATE_FULLSCREEN) {
		enter_fullscreen(target_output, target_desktop, n);
	} else if ((!rule || !(rule->has & RULE_TYPE_STATE)) &&
			toplevel->xdg_toplevel->requested.fullscreen) {
		// client requested fullscreen before map
		enter_fullscreen(target_output, target_desktop, n);
	}

	toplevel->wants_fade = true;
	arrange(target_output, target_desktop, true);

	ipc_put_status(SUB_MASK_NODE_ADD, "node_add[%s,%s,%u]\n",
		n->client && n->client->app_id[0] ? n->client->app_id : "?",
		n->client && n->client->title[0] ? n->client->title : "?", n->id);

	// apply the configured xdg-decoration policy
	toplevel_apply_decoration_mode(toplevel);

	if (!n->client->flags.block_out_from_screenshare) {
		toplevel->image_capture_surface = wlr_scene_surface_create(&toplevel->image_capture->tree,
			toplevel->xdg_toplevel->base->surface);
	}

	update_foreign_toplevel_state(toplevel);

	render_unfocused_client_update(n->client);

	wlr_log(WLR_INFO, "Window mapped and tiled: %s",
		n->client->title[0] ? n->client->title : "untitled");
}

void toplevel_freeze_sibling_buffers(desktop_t *d, node_t *n) {
	if (!d || !d->root)
		return;
	if (!enable_animations)
		return;
	if (n->client && n->client->flags.anim_disabled)
		return;

	node_t *root = d->root;
	node_t *leaf = first_extrema(root);
	while (leaf) {
		if (leaf != n && leaf->client && leaf->client->flags.shown && leaf->client->toplevel &&
				!leaf->client->toplevel->saved_surface_tree) {
			toplevel_save_buffer(leaf->client->toplevel);
			wlr_log(WLR_DEBUG, "Froze buffer for sibling node %u", leaf->id);
		}
		leaf = next_leaf(leaf, root);
	}
}

void toplevel_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, unmap);

	wlr_log(WLR_INFO, "Toplevel unmapped");

	toplevel->mapped = false;
	toplevel->configured = false;

	toplevel->image_capture_surface = NULL;
	animation_cancel_toplevel(toplevel);

	if (toplevel->ext_foreign_toplevel) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->ext_foreign_toplevel);
		toplevel->ext_foreign_toplevel = NULL;
	}

	if (toplevel->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_toplevel);
		toplevel->foreign_toplevel = NULL;
	}

	if (toplevel->node == NULL)
		return;

	if (!animation_fade_out(toplevel))
		animation_cancel_node(toplevel->node);

	if (enable_animations && toplevel->node->client && toplevel->node->client->flags.shown)
		toplevel_save_buffer(toplevel);

	node_t *n = toplevel->node;
	output_t *m = mon;
	desktop_t *d = NULL;

	// find the actual desktop this node belongs to by walking up to root
	if (m && n) {
		node_t *root = n;
		while (root->parent != NULL)
			root = root->parent;

		// find which desktop has this root
		desktop_t *desk = m->desk_head;
		while (desk != NULL) {
			if (desk->root == root) {
				d = desk;
				wlr_log(WLR_DEBUG, "Found node %u belongs to desktop %s", n->id, d->name);
				break;
			}
			desk = desk->next;
		}

		if (d == NULL) {
			// for floating/orphaned nodes, use the node's desktop field directly
			if (n && n->desktop != NULL) {
				d = n->desktop;
				m = d->output;
				wlr_log(WLR_DEBUG, "Found node %u belongs to desktop %s (via n->desktop)", n->id, d->name);
			} else {
				wlr_log(WLR_ERROR, "Could not find desktop for node %u root %p, using current desktop %s", n->id,
					(void *)root, m->desk->name);
				d = m->desk;
			}
		}
	}

	// freeze sibling buffers before modifying layout tree so current visual
	// state is captured
	toplevel_freeze_sibling_buffers(d, n);

	if (m && d) {
		if (n) {
			node_set_dirty(n);
			ipc_put_status(SUB_MASK_NODE_REMOVE, "node_remove[%s,%s,%u]\n",
				n->client && n->client->app_id[0] ? n->client->app_id : "?",
				n->client && n->client->title[0] ? n->client->title : "?", n->id);
		}
		// in scroller layout, remove from scroller state first
		if (d->layout == LAYOUT_SCROLLER && d->scroller_state && n->client) {
			scroller_remove_tile(d->scroller_state, n->client, m);
			scroller_apply_active_focus(d, m);
		}

		remove_node(d, n);

		if (n)
			n->destroying = true;
		if (n && n->client)
			n->client->toplevel = NULL;
		arrange(m, d, true);

		toplevel->node = NULL;

		// focus handling after removing node
		if (d->layout == LAYOUT_SCROLLER) {
			// fall back to tree focus only if croller is empty
			if (d->focus == NULL && d->root != NULL) {
				d->focus = first_extrema(d->root);
				if (d->focus != NULL)
					focus_node(d->output ? d->output : m, d, d->focus);
			}
		} else if (d->focus != NULL && d->focus->client != NULL) {
			focus_node(d->output ? d->output : m, d, d->focus);
		} else if (d->root != NULL) {
			d->focus = first_extrema(d->root);
			if (d->focus != NULL)
				focus_node(d->output ? d->output : m, d, d->focus);
		}
	}

	transaction_notify_view_unmapped(toplevel->node);
}

void toplevel_commit(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, commit);
	struct wlr_xdg_surface *xdg_surface = toplevel->xdg_toplevel->base;

	if (xdg_surface->initial_commit) {
		// initial commit can happen before the xdg_surface is marked initialized
		if (xdg_surface->initialized)
			wlr_xdg_surface_schedule_configure(xdg_surface);

		wlr_xdg_toplevel_set_wm_capabilities(toplevel->xdg_toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		return;
	}

	if (toplevel->mapped && toplevel->xdg_toplevel->base->surface->mapped) {
		// update constraints
		if (toplevel->node) {
			struct wlr_xdg_toplevel_state *state = &toplevel->xdg_toplevel->current;
			bool constraints_changed = false;
			if (state->min_width > 0 &&
					(uint16_t)state->min_width != toplevel->node->constraints.min_width) {
				toplevel->node->constraints.min_width = state->min_width;
				constraints_changed = true;
			}
			if (state->min_height > 0 &&
					(uint16_t)state->min_height != toplevel->node->constraints.min_height) {
				toplevel->node->constraints.min_height = state->min_height;
				constraints_changed = true;
			}
			if (constraints_changed && toplevel->node->output && toplevel->node->desktop) {
				wlr_log(WLR_DEBUG, "toplevel_commit: node %u constraints updated to %dx%d", toplevel->node->id,
					toplevel->node->constraints.min_width, toplevel->node->constraints.min_height);
				arrange(toplevel->node->output, toplevel->node->desktop, true);
			}
		}

		struct wlr_box *new_geo = &xdg_surface->geometry;
		bool new_size = new_geo->width != toplevel->geometry.width ||
			new_geo->height != toplevel->geometry.height || new_geo->x != toplevel->geometry.x ||
			new_geo->y != toplevel->geometry.y;

		if (new_size) {
			// update stored geometry
			memcpy(&toplevel->geometry, new_geo, sizeof(struct wlr_box));

			if (toplevel->node && toplevel->node->client) {
				client_t *c = toplevel->node->client;

				if (c->state == STATE_FLOATING) {
					if (c->floating_rectangle.width > 0) {
						c->floating_rectangle.width = toplevel->geometry.width;
						c->floating_rectangle.height = toplevel->geometry.height;
						wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->geometry.width,
							toplevel->geometry.height);
						transaction_commit_dirty_client();
					}
				} else if (IS_TILED(c)) {
					if (c->tiled_rectangle.width > 0 && c->tiled_rectangle.height > 0 &&
							(toplevel->geometry.width != c->tiled_rectangle.width ||
							toplevel->geometry.height != c->tiled_rectangle.height)) {
						if (!(toplevel->last_requested.width > 0 &&
								toplevel->geometry.width == toplevel->last_requested.width &&
								toplevel->geometry.height == toplevel->last_requested.height)) {
							wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, c->tiled_rectangle.width,
								c->tiled_rectangle.height);
							node_set_dirty(toplevel->node);
							transaction_commit_dirty_client();
						}
					}
				}
			}
		}

		uint32_t serial = toplevel->xdg_toplevel->base->current.configure_serial;
		bool successful = transaction_notify_view_ready_by_serial(toplevel, serial);

		if (successful)
			wlr_log(WLR_DEBUG, "Transaction completed for serial=%u", serial);

		// ack as configured if serial is non-zero
		if (!toplevel->configured && serial != 0) {
			toplevel->configured = true;
			wlr_log(WLR_DEBUG, "Toplevel marked configured via serial=%u (transaction match=%d)", serial,
				successful);
		}

		if (toplevel->saved_surface_tree && !successful)
			toplevel_send_frame_done(toplevel);

		if (!animation_is_resizing(toplevel->node))
			toplevel_center_and_clip_surface(toplevel);
	}

	// check ext_background_effect_v1 state
	const struct wlr_ext_background_effect_surface_v1_state *fx =
		wlr_ext_background_effect_v1_get_surface_state(xdg_surface->surface);
	bool wants_blur = fx && !pixman_region32_empty(&fx->blur_region);
	bool has_blur = blur_count(toplevel->blur) > 0;

	// only update blur from protocol if it wasn't set by a rule
	if (toplevel->node && toplevel->node->client && !toplevel->node->client->flags.blur_from_rule) {
		if (wants_blur != has_blur)
			toplevel_set_effect(toplevel, EFFECT_BLUR, wants_blur);
		if (toplevel->blur && fx) {
			if (!pixman_region32_equal(&toplevel->blur->blur_region, &fx->blur_region)) {
				pixman_region32_copy(&toplevel->blur->blur_region, &fx->blur_region);
				toplevel->blur->blur_region_dirty = true;
			}
		}
	}

	// update opacity
	if (toplevel->node && toplevel->node->client && !animation_is_opacity_fading(toplevel))
		surface_set_opacity(&toplevel->scene_tree->node, toplevel->node->client->opacity);

	if (toplevel->node && toplevel->node->output)
		output_schedule_frame(toplevel->node->output);
}

void toplevel_set_effect(toplevel_t *tl, surface_effect_t effect, bool enabled) {
	surface_set_effect(tl->scene_tree, tl->node, &tl->blur, effect, enabled);
}

void toplevel_set_border_radius(toplevel_t *tl, float radius) {
	surface_set_border_radius(tl->scene_tree, tl->content_tree, tl->border_tree, tl->node, &tl->rounded,
		&tl->shadow, radius);
}

void toplevel_set_shadow(toplevel_t *tl, bool enabled) {
	surface_set_shadow(tl->scene_tree, tl->node, &tl->shadow, enabled);
}

void toplevel_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, destroy);

	wlr_log(WLR_INFO, "Toplevel destroyed");

	animation_cancel_toplevel(toplevel);

	if (toplevel->ext_foreign_toplevel) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->ext_foreign_toplevel);
		toplevel->ext_foreign_toplevel = NULL;
	}

	if (toplevel->foreign_toplevel) {
		wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_toplevel);
		toplevel->foreign_toplevel = NULL;
	}

	if (toplevel->node && toplevel->node->client) {
		animation_cancel_node(toplevel->node);
		toplevel->node->client->toplevel = NULL;
		toplevel->node = NULL;
	}

	if (toplevel->capture_renderer) {
		capture_renderer_destroy(toplevel->capture_renderer);
		toplevel->capture_renderer = NULL;
	}

	if (toplevel->image_capture != NULL) {
		wlr_scene_node_destroy(&toplevel->image_capture->tree.node);
		toplevel->image_capture = NULL;
		toplevel->image_capture_source = NULL;
	}

	if (toplevel->blur) {
		blur_destroy_nodes(toplevel->blur);

		if (toplevel->blur->blur_buf)
			effects_destroy_buffer(&toplevel->blur->blur_buf, toplevel->blur->blur_native);

		if (toplevel->blur->mica_node) {
			wlr_scene_node_destroy(&toplevel->blur->mica_node->node);
			toplevel->blur->mica_node = NULL;
		}

		if (toplevel->blur->acrylic_node) {
			wlr_scene_node_destroy(&toplevel->blur->acrylic_node->node);
			toplevel->blur->acrylic_node = NULL;
		}

		if (toplevel->blur->acrylic_buf)
			effects_destroy_buffer(&toplevel->blur->acrylic_buf, toplevel->blur->acrylic_native);

		pixman_region32_fini(&toplevel->blur->blur_region);
		free(toplevel->blur);
		toplevel->blur = NULL;
	}

	if (toplevel->rounded) {
		// border_shader_node lives inside border_tree which destroy_borders will free,
		// but we still need to release the backing buffer
		if (toplevel->rounded->border_shader_buf) {
			effects_destroy_buffer(&toplevel->rounded->border_shader_buf,
				toplevel->rounded->border_shader_native);
			toplevel->rounded->border_shader_buf_w = 0;
			toplevel->rounded->border_shader_buf_h = 0;
		}
		toplevel->rounded->border_shader_node = NULL;

		if (toplevel->rounded->corner_mask_node) {
			wlr_scene_node_destroy(&toplevel->rounded->corner_mask_node->node);
			toplevel->rounded->corner_mask_node = NULL;
		}

		if (toplevel->rounded->corner_mask_buf) {
			effects_destroy_buffer(&toplevel->rounded->corner_mask_buf,
				toplevel->rounded->corner_mask_native);
		}

		free(toplevel->rounded);
		toplevel->rounded = NULL;
	}

	if (toplevel->shadow) {
		if (toplevel->shadow->shadow_node) {
			wlr_scene_node_destroy(&toplevel->shadow->shadow_node->node);
			toplevel->shadow->shadow_node = NULL;
		}
		if (toplevel->shadow->shadow_buf) {
			effects_destroy_buffer(&toplevel->shadow->shadow_buf, toplevel->shadow->shadow_native);
		}
		free(toplevel->shadow);
		toplevel->shadow = NULL;
	}

	if (toplevel->node && toplevel->node->client)
		render_unfocused_client_remove(toplevel->node->client);

	destroy_borders(&toplevel->border_tree, toplevel->border_rects);

	toplevel_remove_saved_buffer(toplevel);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->new_xdg_popup.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->request_minimize.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->set_app_id.link);
	wl_list_remove(&toplevel->outputs_update.link);

	if (toplevel->output_handler) {
		wlr_scene_node_destroy(&toplevel->output_handler->node);
		toplevel->output_handler = NULL;
	}

	wl_list_remove(&toplevel->link);

	free(toplevel->tag);
	free(toplevel);
}

void toplevel_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, request_move);
	wlr_log(WLR_DEBUG, "Toplevel requested move");

	if (toplevel->node && toplevel->node->client && toplevel->node->client->state == STATE_FLOATING)
		begin_interactive(toplevel, CURSOR_MOVE, 0);
	else if (toplevel->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, request_resize);
	wlr_log(WLR_DEBUG, "Toplevel requested resize");

	if (!event || !toplevel->node || !toplevel->node->client)
		return;

	if (toplevel->node->client->state == STATE_FLOATING)
		begin_interactive(toplevel, CURSOR_RESIZE, event->edges);
	else if (toplevel->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, request_maximize);

	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	if (toplevel->node == NULL || toplevel->node->client == NULL)
		return;
	if (toplevel->node->client->state == STATE_FULLSCREEN)
		return;

	bool requested_maximized = toplevel->xdg_toplevel->requested.maximized;
	if (requested_maximized == toplevel->client_maximized)
		return;

	toplevel->client_maximized = requested_maximized;
	toggle_monocle();
}

void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, request_fullscreen);

	if (!toplevel->xdg_toplevel->base->initialized)
		return;
	if (toplevel->node == NULL || toplevel->node->client == NULL)
		return;

	bool requested_fullscreen = toplevel->xdg_toplevel->requested.fullscreen;
	if (requested_fullscreen == (toplevel->node->client->state == STATE_FULLSCREEN))
		return;

	output_t *m = toplevel->node->output;
	desktop_t *d = m ? m->desk : NULL;

	if (requested_fullscreen) {
		set_state(m, d, toplevel->node, STATE_FULLSCREEN);
		wlr_scene_node_reparent(&toplevel->scene_tree->node, server.full_tree);
	} else {
		client_state_t last = toplevel->node->client->last_state;

		if (last == STATE_FLOATING) {
			set_state(m, d, toplevel->node, STATE_FLOATING);
			wlr_scene_node_reparent(&toplevel->scene_tree->node, server.float_tree);
		} else {
			set_state(m, d, toplevel->node, STATE_TILED);
			wlr_scene_node_reparent(&toplevel->scene_tree->node, server.tile_tree);
		}
	}

	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, requested_fullscreen);
	update_foreign_toplevel_state(toplevel);
}

void toplevel_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, request_minimize);

	if (!toplevel_is_ready(toplevel))
		return;

	if (minimize_to_scratchpad)
		scratchpad_add(toplevel->node);
}

void toplevel_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, set_title);

	if (toplevel->node && toplevel->node->client) {
		const char *title = toplevel->xdg_toplevel->title;
		if (title) {
			strncpy(toplevel->node->client->title, title, MAXLEN - 1);
			toplevel->node->client->title[MAXLEN - 1] = '\0';
			wlr_log(WLR_DEBUG, "Toplevel title changed: %s", title);
		}

		if (toplevel->foreign_toplevel && title)
			wlr_foreign_toplevel_handle_v1_set_title(toplevel->foreign_toplevel, title);

		if (toplevel->ext_foreign_toplevel)
			update_ext_foreign_toplevel(toplevel);

		tabs_update_label_for_leaf(toplevel->node);

		ipc_put_status(SUB_MASK_NODE_CHANGE, "node_change[%s,%s,%u,title]\n",
			toplevel->node->client->app_id[0] ? toplevel->node->client->app_id : "?", title ? title : "?",
			toplevel->node->id);
	}
}

void toplevel_set_app_id(struct wl_listener *listener, void *data) {
	(void)data;
	toplevel_t *toplevel = wl_container_of(listener, toplevel, set_app_id);

	if (toplevel->node && toplevel->node->client) {
		const char *app_id = toplevel->xdg_toplevel->app_id;
		if (app_id) {
			strncpy(toplevel->node->client->app_id, app_id, MAXLEN - 1);
			toplevel->node->client->app_id[MAXLEN - 1] = '\0';
			wlr_log(WLR_DEBUG, "Toplevel app_id changed: %s", app_id);
		}

		if (toplevel->foreign_toplevel && app_id)
			wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->foreign_toplevel, app_id);

		if (toplevel->ext_foreign_toplevel)
			update_ext_foreign_toplevel(toplevel);

		tabs_update_label_for_leaf(toplevel->node);

		ipc_put_status(SUB_MASK_NODE_CHANGE, "node_change[%s,%s,%u,app_id]\n", app_id ? app_id : "?",
			toplevel->node->client->title[0] ? toplevel->node->client->title : "?", toplevel->node->id);
	}
}

void focus_toplevel(struct toplevel_t *toplevel) {
	if (toplevel == NULL || toplevel->xdg_toplevel == NULL)
		return;

	// never steal keyboard focus to a window on a non-visible desktop
	if (toplevel->node && toplevel->node->desktop && toplevel->node->output &&
			toplevel->node->desktop != toplevel->node->output->desk) {
		toplevel->node->desktop->focus = toplevel->node;
		return;
	}

	struct wlr_seat *seat = server.seat;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (surface == NULL)
		return;

	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface)
		return;

	if (prev_surface != NULL) {
		struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);

		if (prev_toplevel != NULL)
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
	}

	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

	if (toplevel->foreign_toplevel)
		wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_toplevel, true);

	if (seat->keyboard_state.keyboard != NULL)
		wlr_seat_keyboard_notify_enter(seat, surface, seat->keyboard_state.keyboard->keycodes,
			seat->keyboard_state.keyboard->num_keycodes, &seat->keyboard_state.keyboard->modifiers);

	// update input method focus
	seat_t *s = seat_default();
	if (s && s->input_method_relay)
		input_method_relay_set_focus(s->input_method_relay, surface);

	// update tablet pad focus
	if (s)
		tablet_pads_set_focus(s, surface);

	if (toplevel->node && toplevel->node->output)
		server.focused_output = toplevel->node->output;

	// update borders
	if (toplevel->node) {
		output_t *m = toplevel->node->output;
		desktop_t *d = m ? m->desk : NULL;
		if (d && d->root != NULL) {
			FOR_EACH_LEAF(node, d->root) {
				if (node->client == NULL)
					continue;

				update_border_colors(node->client);
			}
		}
	}
}

bool toplevel_is_ready(struct toplevel_t *toplevel) {
	return toplevel && toplevel->mapped && toplevel->xdg_toplevel && toplevel->xdg_toplevel->base &&
		toplevel->xdg_toplevel->base->surface && toplevel->xdg_toplevel->base->surface->mapped;
}

static int buffer_copy_count = 0;

static void save_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct wlr_scene_tree *tree = data;

	buffer_copy_count++;
	wlr_log(WLR_DEBUG, "save_buffer_iterator called: buffer=%p, sx=%d, sy=%d", (void *)buffer, sx, sy);

	// ignore buffers with no content
	if (!buffer->buffer) {
		wlr_log(WLR_DEBUG, "Skipping buffer with no content");
		return;
	}

	struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(tree, NULL);
	if (!sbuf) {
		wlr_log(WLR_ERROR, "Could not allocate a scene buffer when saving a surface");
		return;
	}

	wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
	wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
	struct wlr_fbox src_box = clamp_scene_buffer_source_box(buffer, buffer->src_box);
	wlr_scene_buffer_set_source_box(sbuf, &src_box);
	wlr_scene_node_set_position(&sbuf->node, sx, sy);
	wlr_scene_buffer_set_transform(sbuf, buffer->transform);
	wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);

	wlr_log(WLR_DEBUG, "Successfully copied buffer %dx%d at (%d,%d)", buffer->dst_width,
		buffer->dst_height, sx, sy);
}

void toplevel_save_buffer(toplevel_t *toplevel) {
	if (!toplevel || !toplevel->scene_tree || !toplevel->content_tree)
		return;

	// removed saved buffer
	if (toplevel->saved_surface_tree) {
		wlr_log(WLR_DEBUG, "Removing existing saved buffer before saving new one");
		toplevel_remove_saved_buffer(toplevel);
	}

	toplevel->saved_surface_tree = wlr_scene_tree_create(toplevel->scene_tree);
	if (!toplevel->saved_surface_tree) {
		wlr_log(WLR_ERROR, "Could not allocate a scene tree node when saving a surface");
		return;
	}

	wlr_scene_node_set_enabled(&toplevel->saved_surface_tree->node, false);

	// copy scene buffers
	buffer_copy_count = 0;
	wlr_log(WLR_DEBUG, "Starting buffer iteration for content_tree=%p",
		(void *)toplevel->content_tree);

	wlr_scene_node_for_each_buffer(&toplevel->content_tree->node, save_buffer_iterator,
		toplevel->saved_surface_tree);

	wlr_log(WLR_DEBUG, "Buffer iteration complete, copied %d buffers", buffer_copy_count);

	bool has_children = !wl_list_empty(&toplevel->saved_surface_tree->children);
	wlr_log(WLR_DEBUG, "After iteration: saved_surface_tree has_children=%d", has_children);

	if (!has_children) {
		// cleanup
		wlr_scene_node_destroy(&toplevel->saved_surface_tree->node);
		toplevel->saved_surface_tree = NULL;
		wlr_log(WLR_DEBUG, "No buffers to save for toplevel - destroyed saved tree");
	} else {
		wlr_scene_node_set_enabled(&toplevel->content_tree->node, false);
		wlr_scene_node_set_enabled(&toplevel->saved_surface_tree->node, true);
		wlr_log(WLR_DEBUG, "Saved buffer for toplevel - swapped content_tree for saved_surface_tree");
	}
}

void toplevel_remove_saved_buffer(struct toplevel_t *toplevel) {
	if (!toplevel || !toplevel->saved_surface_tree)
		return;
	wlr_log(WLR_DEBUG, "Removing saved buffer for toplevel");

	wlr_scene_node_destroy(&toplevel->saved_surface_tree->node);
	toplevel->saved_surface_tree = NULL;

	if (toplevel->content_tree)
		wlr_scene_node_set_enabled(&toplevel->content_tree->node, true);
}

void toplevel_send_frame_done_iterator(struct wlr_scene_buffer *scene_buffer, int x, int y,
		void *data) {
	(void)x;
	(void)y;
	struct timespec *when = data;
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface == NULL)
		return;

	wlr_surface_send_frame_done(scene_surface->surface, when);
}

void toplevel_send_frame_done(struct toplevel_t *toplevel) {
	if (!toplevel || !toplevel->content_tree)
		return;

	struct timespec when;
	clock_gettime(CLOCK_MONOTONIC, &when);

	struct wlr_scene_node *node;
	wl_list_for_each(node, &toplevel->content_tree->children, link)
		wlr_scene_node_for_each_buffer(node, toplevel_send_frame_done_iterator, &when);
}

bool toplevel_can_tear(struct toplevel_t *toplevel) {
	// per-window rule override takes precedence
	if (toplevel->node && toplevel->node->client &&
		toplevel->node->client->flags.allow_tearing_from_rule)
		return toplevel->node->client->flags.allow_tearing;

	// explicit tearing hint from client
	if (toplevel->tearing_hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)
		return true;

	// global allow_tearing toggle
	if (allow_tearing)
		return true;

	return false;
}

toplevel_t *toplevel_create(struct wlr_xdg_toplevel *xdg_toplevel) {
	toplevel_t *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		wlr_log(WLR_ERROR, "allocation failed");
		return NULL;
	}

	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->mapped = false;
	toplevel->configured = false;
	toplevel->client_maximized = false;

	// create parent scene tree container
	toplevel->scene_tree = wlr_scene_tree_create(server.tile_tree);
	if (!toplevel->scene_tree) {
		wlr_log(WLR_ERROR, "Failed to create scene tree for toplevel");
		free(toplevel);
		return NULL;
	}

	// keep scene invisible until arrange_node_geometry positions and enables it
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

	// create content tree as child and add surface
	toplevel->content_tree = wlr_scene_tree_create(toplevel->scene_tree);
	if (!toplevel->content_tree) {
		wlr_log(WLR_ERROR, "Failed to create content tree for toplevel");
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		free(toplevel);
		return NULL;
	}

	// initialize border fields
	toplevel->border_tree = NULL;
	for (int i = 0; i < 4; i++)
		toplevel->border_rects[i] = NULL;

	toplevel->unmap.notify = toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);

	// create surface scene within the content tree
	struct wlr_scene_tree *xdg_tree = wlr_scene_xdg_surface_create(toplevel->content_tree,
		xdg_toplevel->base);
	if (!xdg_tree) {
		wlr_log(WLR_ERROR, "Failed to create XDG surface scene for toplevel");
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		free(toplevel);
		return NULL;
	}

	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel;

	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

	// create image capture
	toplevel->image_capture = wlr_scene_create();
	toplevel->image_capture_tree = wlr_scene_tree_create(&toplevel->image_capture->tree);

	toplevel->output_handler = wlr_scene_buffer_create(toplevel->scene_tree, NULL);
	if (!toplevel->output_handler) {
		wlr_log(WLR_ERROR, "Failed to create output handler for toplevel");
	} else {
		toplevel->output_handler->point_accepts_input = toplevel_output_handler_point_accepts_input;
	}

	// register event listeners
	toplevel->map.notify = toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);

	toplevel->commit.notify = toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->new_xdg_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&xdg_toplevel->base->events.new_popup, &toplevel->new_xdg_popup);

	toplevel->destroy.notify = toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);

	toplevel->request_resize.notify = toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);

	toplevel->request_maximize.notify = toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);

	toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

	toplevel->request_minimize.notify = toplevel_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);

	toplevel->set_title.notify = toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);

	toplevel->set_app_id.notify = toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

	if (toplevel->output_handler) {
		toplevel->outputs_update.notify = handle_outputs_update;
		wl_signal_add(&toplevel->output_handler->events.outputs_update, &toplevel->outputs_update);
	}

	return toplevel;
}
