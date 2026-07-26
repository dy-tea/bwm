#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wlr/util/box.h>

typedef struct output_t output_t;
typedef struct node_t node_t;
typedef struct toplevel_t toplevel_t;
typedef struct layer_surface_t layer_surface_t;
struct wlr_scene_tree;

void animation_init(void);
void animation_fini(void);

void animation_set_bezier(const char *name);
const char *animation_get_bezier(void);
void animation_set_duration(uint32_t ms);
uint32_t animation_get_duration(void);

int animation_type_from_name(const char *name);
bool animation_set_type_config(const char *type_name, const char *bezier_name,
	uint32_t duration_ms);
const char *animation_type_get_bezier(const char *type_name);
uint32_t animation_type_get_duration(const char *type_name);
bool animation_set_type_spring(const char *type_name, const char *spring_name);
const char *animation_type_get_spring(const char *type_name);

bool animation_type_set_enabled(const char *type_name, bool enabled);
bool animation_type_get_enabled(const char *type_name);

bool animation_apply_geometry(node_t *node, struct wlr_scene_tree *scene_tree, struct wlr_box target,
	bool animate);

bool animation_apply_geometry_from(node_t *node, struct wlr_scene_tree *scene_tree,
	struct wlr_box from, struct wlr_box target, bool animate);

bool animation_start_workspace_slide(output_t *output, node_t *node,
	struct wlr_scene_tree *scene_tree, struct wlr_box from, struct wlr_box to, bool slide_out);

bool animation_start_resize(toplevel_t *toplevel, struct wlr_box from, struct wlr_box to);

bool animation_fade_in(toplevel_t *toplevel);
bool animation_fade_in_layer(layer_surface_t *layer);
bool animation_fade_out(toplevel_t *toplevel);
bool animation_fade_out_layer(layer_surface_t *layer);
bool animation_is_opacity_fading(toplevel_t *toplevel);

bool animation_workspace_switch_active(output_t *output);
bool animation_node_workspace_slide_out(node_t *node);
void animation_cancel_node(node_t *node);
void animation_cancel_toplevel(toplevel_t *toplevel);
bool animation_has_fade_out(struct wlr_scene_tree *scene_tree);
void animation_cancel_scene_tree(struct wlr_scene_tree *scene_tree);

bool animation_update_output(output_t *output, struct timespec now);
void animation_update_slide_blur(output_t *output);

// Get the current animation progress for a toplevel resize (returns true if animating)
// If progress is not NULL, it will be set to the eased progress value (0.0 to 1.0)
// The anim_from and anim_to boxes will be set to the animation bounds if the toplevel is animating
bool animation_get_toplevel_resize_progress(toplevel_t *toplevel, double *progress,
	struct wlr_box *anim_from, struct wlr_box *anim_to);

// Check if a node currently has an active resize animation
bool animation_is_resizing(node_t *node);

// Get the interpolated client rectangle for any geometry animation (resize or position).
// Returns true if the toplevel has an active geometry animation, and fills *out with
// the interpolated rectangle.
bool animation_get_geometry_progress(toplevel_t *toplevel, struct wlr_box *out);
