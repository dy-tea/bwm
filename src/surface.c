#include "effects_backend.h"
#include "output.h"
#include "server.h"
#include "surface.h"
#include "toplevel.h"
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

static bool corner_mask_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

typedef struct {
	struct wlr_scene_buffer **node;
	struct wlr_buffer **buf;
	uint64_t *native;
} effect_fields_t;

static effect_fields_t get_effect_fields(surface_blur_t *b, surface_effect_t effect) {
	effect_fields_t f = {0};
	switch (effect) {
	case EFFECT_BLUR:
		f.node = &b->blur_node;
		f.buf = &b->blur_buf;
		f.native = b->blur_native;
		break;
	case EFFECT_MICA:
		f.node = &b->mica_node;
		break;
	case EFFECT_ACRYLIC:
		f.node = &b->acrylic_node;
		f.buf = &b->acrylic_buf;
		f.native = b->acrylic_native;
		break;
	}
	return f;
}

void surface_set_effect(struct wlr_scene_tree *scene_tree, node_t *node, surface_blur_t **blur,
		surface_effect_t effect, bool enabled) {
	if (!scene_tree)
		return;

	if (enabled) {
		if (!*blur) {
			*blur = calloc(1, sizeof(**blur));
			if (!*blur)
				return;
			pixman_region32_init(&(*blur)->blur_region);
		}

		effect_fields_t f = get_effect_fields(*blur, effect);
		if (!*f.node) {
			*f.node = wlr_scene_buffer_create(scene_tree, NULL);
			if (*f.node) {
				wlr_scene_node_lower_to_bottom(&(*f.node)->node);
				if (node && node->output) {
					struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene,
						node->output->wlr_output);
					if (so) {
						pixman_region32_union_rect(&so->damage_ring.current, &so->damage_ring.current, 0, 0,
							(unsigned int)node->output->width, (unsigned int)node->output->height);
						output_schedule_frame(node->output);
					}
				}
			}
		}
	} else if (*blur) {
		effect_fields_t f = get_effect_fields(*blur, effect);
		if (*f.node) {
			wlr_scene_node_destroy(&(*f.node)->node);
			*f.node = NULL;
		}
		if (f.buf && *f.buf)
			effects_destroy_buffer(f.buf, f.native);
		// clear blur_region when blur effect is disabled
		if (effect == EFFECT_BLUR)
			pixman_region32_clear(&(*blur)->blur_region);
	}
}

void surface_set_border_radius(struct wlr_scene_tree *scene_tree,
		struct wlr_scene_tree *content_tree, struct wlr_scene_tree *border_tree, node_t *node,
		surface_rounded_t **rounded, surface_shadow_t **shadow, float radius) {
	if (!scene_tree || !content_tree)
		return;

	if (node && node->client)
		node->client->border_radius = radius;

	if (*shadow)
		(*shadow)->shadow_dirty = true;

	if (radius > 0.0f) {
		if (!*rounded) {
			*rounded = calloc(1, sizeof(**rounded));
			if (!*rounded)
				return;
		}

		if (!(*rounded)->corner_mask_node) {
			(*rounded)->corner_mask_node = wlr_scene_buffer_create(scene_tree, NULL);
			if ((*rounded)->corner_mask_node) {
				wlr_scene_node_place_above(&(*rounded)->corner_mask_node->node, &content_tree->node);

				if (border_tree)
					wlr_scene_node_place_below(&(*rounded)->corner_mask_node->node, &border_tree->node);

				(*rounded)->corner_mask_node->point_accepts_input = corner_mask_no_input;
			}
		}
		if ((*rounded)->cached_radius != radius) {
			(*rounded)->cached_radius = radius;
			(*rounded)->border_dirty = true;
			(*rounded)->corner_mask_dirty = true;
		}
	} else if (*rounded) {
		if ((*rounded)->corner_mask_node) {
			wlr_scene_node_destroy(&(*rounded)->corner_mask_node->node);
			(*rounded)->corner_mask_node = NULL;

			if ((*rounded)->corner_mask_buf) {
				effects_destroy_buffer(&(*rounded)->corner_mask_buf, (*rounded)->corner_mask_native);
			}
		}

		bool has_gradient = ((*rounded)->gradient_count >= 2);
		if (!has_gradient) {
			if ((*rounded)->border_shader_node) {
				wlr_scene_node_destroy(&(*rounded)->border_shader_node->node);
				(*rounded)->border_shader_node = NULL;

				if ((*rounded)->border_shader_buf) {
					effects_destroy_buffer(&(*rounded)->border_shader_buf, (*rounded)->border_shader_native);
					(*rounded)->border_shader_buf_w = 0;
					(*rounded)->border_shader_buf_h = 0;
				} (*rounded)->border_cache_valid = false;
			} (*rounded)->border_dirty = false;
			(*rounded)->corner_mask_dirty = false;
		} else {
			(*rounded)->border_dirty = true;
			(*rounded)->corner_mask_dirty = true;
		}
	}
}

void surface_set_shadow(struct wlr_scene_tree *scene_tree, node_t *node, surface_shadow_t **shadow,
		bool enabled) {
	if (!scene_tree)
		return;

	if (node && node->client)
		node->client->flags.shadow = enabled;

	if (enabled) {
		if (!*shadow) {
			*shadow = calloc(1, sizeof(**shadow));
			if (!*shadow)
				return;
		} (*shadow)->shadow_dirty = true;
	} else if (*shadow) {
		if ((*shadow)->shadow_node) {
			wlr_scene_node_destroy(&(*shadow)->shadow_node->node);
			(*shadow)->shadow_node = NULL;
		}
		if ((*shadow)->shadow_buf) {
			effects_destroy_buffer(&(*shadow)->shadow_buf, (*shadow)->shadow_native);
		} (*shadow)->shadow_dirty = false;
	}
}

void surface_update_rounded(surface_rounded_t **rounded, float color[4], border_theme_t *bt) {
	if (!*rounded) {
		*rounded = calloc(1, sizeof(**rounded));
		if (!*rounded)
			return;
	}

	surface_rounded_t *r = *rounded;

	bool changed = false;
	if (memcmp(r->border_color, color, sizeof(r->border_color)) != 0)
		changed = true;
	if (r->gradient_count != bt->gradient_count)
		changed = true;
	if (r->gradient_angle != bt->gradient_angle)
		changed = true;
	if (r->gradient2_count != bt->gradient2_count)
		changed = true;
	if (r->gradient2_angle != bt->gradient2_angle)
		changed = true;
	if (r->gradient_lerp != bt->gradient_lerp)
		changed = true;
	if (bt->gradient_count > 0 && memcmp(r->gradient_colors, bt->gradient,
		bt->gradient_count * 4 * sizeof(float)) != 0)
		changed = true;
	if (bt->gradient2_count > 0 && memcmp(r->gradient2_colors, bt->gradient2,
		bt->gradient2_count * 4 * sizeof(float)) != 0)
		changed = true;

	memcpy(r->border_color, color, sizeof(r->border_color));
	memcpy(r->gradient_colors, bt->gradient, bt->gradient_count * 4 * sizeof(float));
	r->gradient_count = bt->gradient_count;
	r->gradient_angle = bt->gradient_angle;
	memcpy(r->gradient2_colors, bt->gradient2, bt->gradient2_count * 4 * sizeof(float));
	r->gradient2_count = bt->gradient2_count;
	r->gradient2_angle = bt->gradient2_angle;
	r->gradient_lerp = bt->gradient_lerp;

	if (changed) {
		r->border_dirty = true;
		r->corner_mask_dirty = true;
	}
}

void surface_client_set_effect(client_t *client, surface_effect_t effect, bool enabled) {
	if (client->toplevel)
		toplevel_set_effect(client->toplevel, effect, enabled);
	else if (client->xwayland_view)
		xwayland_set_effect(client->xwayland_view, effect, enabled);
}

void surface_client_set_border_radius(client_t *client, float radius) {
	if (client->toplevel)
		toplevel_set_border_radius(client->toplevel, radius);
	else if (client->xwayland_view)
		xwayland_set_border_radius(client->xwayland_view, radius);
	else
		client->border_radius = radius;
}

void surface_client_set_shadow(client_t *client, bool enabled) {
	if (client->toplevel)
		toplevel_set_shadow(client->toplevel, enabled);
	else if (client->xwayland_view)
		xwayland_set_shadow(client->xwayland_view, enabled);
}

static void surface_set_opacity_for_each_buffer(struct wlr_scene_buffer *buffer, int x, int y,
		void *data) {
	(void)x;
	(void)y;
	float *opacity = data;
	if (buffer)
		wlr_scene_buffer_set_opacity(buffer, *opacity);
}

void surface_set_opacity(struct wlr_scene_node *node, float opacity) {
	if (node)
		wlr_scene_node_for_each_buffer(node, surface_set_opacity_for_each_buffer, &opacity);
}
