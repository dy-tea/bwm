#include "animation.h"
#include "effects.h"
#include "input_method.h"
#include "layer.h"
#include "output.h"
#include "popup.h"
#include "seat.h"
#include "server.h"
#include "tablet.h"
#include "tree.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_ext_background_effect_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

extern struct server_t server;

static void layer_surface_destroy(struct wl_listener *listener, void *data);
static void layer_surface_map(struct wl_listener *listener, void *data);
static void layer_surface_unmap(struct wl_listener *listener, void *data);
static void layer_surface_commit(struct wl_listener *listener, void *data);

struct wlr_scene_tree *output_shell_layer(output_t *output, enum zwlr_layer_shell_v1_layer layer);

void layer_surface_set_blur(layer_surface_t *ls, bool enabled) {
	if (!ls || !ls->scene_tree)
		return;

	if (enabled && !ls->blur_node) {
		ls->blur_node = wlr_scene_buffer_create(ls->scene_tree, NULL);
		if (ls->blur_node) {
			ls->blur_region_dirty = true;
			wlr_scene_node_lower_to_bottom(&ls->blur_node->node);
			// ensure next frame recaptures the background for this new blur node
			struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene, ls->output->wlr_output);
			if (so) {
				pixman_region32_union_rect(&so->damage_ring.current, &so->damage_ring.current, 0, 0,
					(unsigned int)ls->output->width, (unsigned int)ls->output->height);
				output_schedule_frame(ls->output);
			}
		}
	} else if (!enabled && ls->blur_node) {
		wlr_scene_node_destroy(&ls->blur_node->node);
		ls->blur_node = NULL;
		pixman_region32_clear(&ls->blur_region);
		ls->blur_region_dirty = true;
	}
}

static void layer_surface_save_buffer_iterator(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	struct wlr_scene_tree *tree = data;
	if (!buffer || !buffer->buffer)
		return;

	struct wlr_scene_buffer *sbuf = wlr_scene_buffer_create(tree, NULL);
	if (!sbuf)
		return;

	wlr_scene_buffer_set_dest_size(sbuf, buffer->dst_width, buffer->dst_height);
	wlr_scene_buffer_set_opaque_region(sbuf, &buffer->opaque_region);
	wlr_scene_buffer_set_source_box(sbuf, &buffer->src_box);
	wlr_scene_node_set_position(&sbuf->node, sx, sy);
	wlr_scene_buffer_set_transform(sbuf, buffer->transform);
	wlr_scene_buffer_set_buffer(sbuf, buffer->buffer);
}

static void layer_surface_save_buffer(layer_surface_t *layer) {
	if (!layer || !layer->saved_tree)
		return;

	wlr_scene_node_for_each_buffer(&layer->scene_tree->node, layer_surface_save_buffer_iterator,
		layer->saved_tree);
	wlr_log(WLR_DEBUG, "layer: save_buffer: saved_tree children=%d",
		wl_list_length(&layer->saved_tree->children));
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, destroy);

	animation_cancel_scene_tree(layer->scene_tree);

	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->new_popup.link);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->surface_commit.link);
	wl_list_remove(&layer->link);

	pixman_region32_fini(&layer->blur_region);

	arrange_layers(layer->output);
	free(layer);
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, map);

	layer->mapped = true;
	arrange_layers(layer->output);

	if (layer->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		focus_layer_surface(layer);

	effects_invalidate_mica(layer->output->effects);
	animation_fade_in_layer(layer);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, unmap);
	layer->mapped = false;

	if (!enable_animations) {
		animation_cancel_scene_tree(layer->scene_tree);
		wlr_scene_node_set_enabled(&layer->scene_tree->node, false);
	} else {
		struct wlr_scene_node *child;
		wl_list_for_each(child, &layer->scene_tree->children, link)
			wlr_scene_node_set_enabled(child, true);

		layer->saved_tree = wlr_scene_tree_create(&server.scene->tree);
		if (!layer->saved_tree || !animation_fade_out_layer(layer)) {
			if (layer->saved_tree)
				wlr_scene_node_destroy(&layer->saved_tree->node);
			layer->saved_tree = NULL;
			animation_cancel_scene_tree(layer->scene_tree);
			wlr_scene_node_set_enabled(&layer->scene_tree->node, false);
		} else {
			layer_surface_save_buffer(layer);
		}
	}

	arrange_layers(layer->output);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	layer_surface_t *layer = wl_container_of(listener, layer, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;

	if (!layer_surface->initialized)
		return;

	bool needs_arrange = false;
	if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		struct wlr_scene_tree *new_tree = output_shell_layer(layer->output, layer_surface->current.layer);
		wlr_scene_node_reparent(&layer->scene_layer->tree->node, new_tree);
		needs_arrange = true;
	}

	// rearrange if needed
	if (needs_arrange || layer_surface->initial_commit) {
		layer->mapped = layer_surface->surface->mapped;
		arrange_layers(layer->output);
	}

	// check ext_background_effect_v1 state
	const struct wlr_ext_background_effect_surface_v1_state *fx =
		wlr_ext_background_effect_v1_get_surface_state(layer_surface->surface);
	bool wants_blur = fx && !pixman_region32_empty(&fx->blur_region);
	bool had_blur = layer->blur_node != NULL;
	if (wants_blur != had_blur)
		layer_surface_set_blur(layer, wants_blur);

	// update blur region if blur is enabled
	if (fx && layer->blur_node) {
		if (!pixman_region32_equal(&layer->blur_region, &fx->blur_region)) {
			pixman_region32_copy(&layer->blur_region, &fx->blur_region);
			layer->blur_region_dirty = true;
		}
	}

	output_schedule_frame(layer->output);
}

void handle_new_layer_surface(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_layer_surface_v1 *layer_surface = data;
	layer_surface_t *layer;
	output_t *output;

	wlr_log(WLR_INFO, "New layer surface on layer %d", layer_surface->pending.layer);

	if (!layer_surface->output) {
		wlr_log(WLR_INFO, "Layer surface has no output, using focused_monitor");
		if (!server.focused_output) {
			wlr_log(WLR_ERROR, "No focused monitor, destroying layer surface");
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
		layer_surface->output = server.focused_output->wlr_output;
	}

	output = layer_surface->output->data;
	if (!output) {
		wlr_log(WLR_ERROR, "No output data, destroying layer surface");
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	layer = calloc(1, sizeof(*layer));
	if (!layer) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	layer->layer_surface = layer_surface;
	layer->output = output;
	layer_surface->data = layer;
	pixman_region32_init(&layer->blur_region);

	struct wlr_scene_tree *layer_tree;
	switch (layer_surface->pending.layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		layer_tree = output->layer_bg;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		layer_tree = output->layer_bottom;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		layer_tree = output->layer_top;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		layer_tree = output->layer_overlay;
		break;
	default:
		layer_tree = output->layer_top;
		break;
	}

	layer->scene_layer = wlr_scene_layer_surface_v1_create(layer_tree, layer_surface);
	layer->scene_tree = layer->scene_layer->tree;

	layer->scene_tree->node.data = layer;

	layer->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

	layer->new_popup.notify = handle_new_layer_popup;
	wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);

	layer->map.notify = layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &layer->map);

	layer->unmap.notify = layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);

	layer->surface_commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &layer->surface_commit);

	wl_list_insert(&output->layers[layer_surface->pending.layer], &layer->link);

	// fractional scale
	wlr_fractional_scale_v1_notify_scale(layer_surface->surface, layer->output->wlr_output->scale);
	wlr_surface_set_preferred_buffer_scale(layer_surface->surface,
		ceil(layer->output->wlr_output->scale));

	wlr_surface_send_enter(layer_surface->surface, layer_surface->output);

	arrange_layers(output);
}

void arrange_layers(output_t *output) {
	struct wlr_box usable_area = output->rectangle;
	struct wlr_box full_area = output->rectangle;
	layer_surface_t *layer;
	int i;

	if (!output->wlr_output->enabled)
		return;

	for (i = 3; i >= 0; i--) {
		wl_list_for_each_reverse(layer, &output->layers[i], link) {
			if (!layer->layer_surface->initialized)
				continue;
			if (layer->layer_surface->current.exclusive_zone > 0)
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &full_area, &usable_area);
		}
	}

	if (!wlr_box_equal(&usable_area, &output->usable_area)) {
		output->usable_area = usable_area;
		output_t *m = output;
		if (m && m->desk)
			arrange(m, m->desk, true);
	}

	for (i = 3; i >= 0; i--) {
		wl_list_for_each_reverse(layer, &output->layers[i], link) {
			if (!layer->layer_surface->initialized)
				continue;
			if (layer->layer_surface->current.exclusive_zone <= 0)
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &full_area, &usable_area);
		}
	}
}

void focus_layer_surface(layer_surface_t *layer_surface) {
	if (!layer_surface->layer_surface || !layer_surface->layer_surface->surface ||
		!layer_surface->layer_surface->surface->mapped)
		return;

	struct wlr_surface *surface = layer_surface->layer_surface->surface;

	if (layer_surface->layer_surface->current.keyboard_interactive ==
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
		return;
	if (layer_surface->scene_layer == NULL)
		return;

	// notify keyboard enter
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard)
		wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes,
			&keyboard->modifiers);

	seat_t *s = seat_default();
	if (s && s->input_method_relay)
		input_method_relay_set_focus(s->input_method_relay, surface);

	if (s)
		tablet_pads_set_focus(s, surface);
}

struct wlr_scene_tree *output_shell_layer(output_t *output, enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return output->layer_bg;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return output->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return output->layer_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return output->layer_overlay;
	default:
		return output->layer_top;
	}
}

void layer_init(void) {
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 5);
	if (!server.layer_shell) {
		wlr_log(WLR_ERROR, "Failed to create layer shell");
		exit(EXIT_FAILURE);
	}

	server.new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);
}

void layer_fini(void) {
	wl_list_remove(&server.new_layer_surface.link);
}
