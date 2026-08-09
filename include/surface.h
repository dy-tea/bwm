#pragma once

#include "effects_backend.h"
#include "types.h"

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>

struct wlr_scene_tree;

// a blur region is rendered as one scene buffer per region rectangle. The pool
// is grown as needed and nodes beyond the current count are disabled.
typedef struct blur_node_pool_t {
	struct wlr_scene_buffer **nodes;
	size_t count;
	size_t cap;
} blur_node_pool_t;

static inline size_t blur_pool_count(const blur_node_pool_t *p) {
	return p ? p->count : 0;
}

static inline struct wlr_scene_buffer *blur_pool_get(const blur_node_pool_t *p, size_t i) {
	return (p && i < p->count) ? p->nodes[i] : NULL;
}

void blur_pool_set_count(blur_node_pool_t *p, struct wlr_scene_tree *parent, size_t count);
void blur_pool_set_buffer(blur_node_pool_t *p, struct wlr_buffer *buf);
void blur_pool_set_buffer_null(blur_node_pool_t *p);
void blur_pool_destroy(blur_node_pool_t *p);

typedef struct surface_blur_t {
	blur_node_pool_t blur_pool;
	struct wlr_scene_buffer *mica_node;
	struct wlr_scene_buffer *acrylic_node;
	bool blur_scene_hidden;
	struct wlr_buffer *blur_buf;
	uint64_t blur_native[2];
	struct wlr_buffer *acrylic_buf;
	uint64_t acrylic_native[2];
	pixman_region32_t blur_region;
	bool blur_region_dirty, blur_mask_valid;
	struct be_corner_mask_params blur_mask_params;
} surface_blur_t;

static inline size_t blur_count(const surface_blur_t *b) {
	return b ? blur_pool_count(&b->blur_pool) : 0;
}

static inline struct wlr_scene_buffer *blur_get(const surface_blur_t *b, size_t i) {
	return (b && i < b->blur_pool.count) ? b->blur_pool.nodes[i] : NULL;
}

static inline void blur_set_node_count(surface_blur_t *b, struct wlr_scene_tree *parent,
		size_t count) {
	if (b)
		blur_pool_set_count(&b->blur_pool, parent, count);
}

static inline void blur_set_buffer(surface_blur_t *b, struct wlr_buffer *buf) {
	if (b)
		blur_pool_set_buffer(&b->blur_pool, buf);
}

static inline void blur_set_buffer_null(surface_blur_t *b) {
	if (b)
		blur_pool_set_buffer_null(&b->blur_pool);
}

static inline void blur_destroy_nodes(surface_blur_t *b) {
	if (b)
		blur_pool_destroy(&b->blur_pool);
}

typedef struct surface_rounded_t {
	struct wlr_scene_buffer *border_shader_node;
	struct wlr_buffer *border_shader_buf;
	uint64_t border_shader_native[2];
	int border_shader_buf_w;
	int border_shader_buf_h;
	bool border_dirty;
	bool corner_mask_dirty;
	float border_color[4];

	float gradient_colors[BORDER_GRADIENT_MAX_STOPS * 4];
	int gradient_count;
	float gradient_angle;
	float gradient2_colors[BORDER_GRADIENT_MAX_STOPS * 4];
	int gradient2_count;
	float gradient2_angle;
	float gradient_lerp;

	struct wlr_scene_buffer *corner_mask_node;
	struct wlr_buffer *corner_mask_buf;
	uint64_t corner_mask_native[2];
	float cached_radius;

	struct be_border_params cached_border_params;
	struct wlr_buffer *cached_border_buf;
	bool border_cache_valid;
} surface_rounded_t;

typedef struct surface_shadow_t {
	struct wlr_scene_buffer *shadow_node;
	struct wlr_buffer *shadow_buf;
	uint64_t shadow_native[2];
	int shadow_buf_w;
	int shadow_buf_h;
	bool shadow_dirty;
	bool shadow_geometry_dirty;
} surface_shadow_t;

typedef enum {
	EFFECT_BLUR,
	EFFECT_MICA,
	EFFECT_ACRYLIC,
} surface_effect_t;

struct wlr_scene_node;

void surface_set_effect(struct wlr_scene_tree *scene_tree, struct node_t *node,
	struct surface_blur_t **blur, surface_effect_t effect, bool enabled);
void surface_set_border_radius(struct wlr_scene_tree *scene_tree,
	struct wlr_scene_tree *content_tree, struct wlr_scene_tree *border_tree, struct node_t *node,
	struct surface_rounded_t **rounded, struct surface_shadow_t **shadow, float radius);
void surface_set_shadow(struct wlr_scene_tree *scene_tree, struct node_t *node,
	struct surface_shadow_t **shadow, bool enabled);

void surface_update_rounded(struct surface_rounded_t **rounded, float color[4], border_theme_t *bt);

void surface_client_set_effect(struct client_t *client, surface_effect_t effect, bool enabled);
void surface_client_set_border_radius(struct client_t *client, float radius);
void surface_client_set_shadow(struct client_t *client, bool enabled);

void surface_set_opacity(struct wlr_scene_node *node, float opacity);
void rounded_mark_border_size(struct surface_rounded_t *rounded, int content_w, int content_h,
	int border_w, float scale);
