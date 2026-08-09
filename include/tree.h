#pragma once

#include "output.h"
#include "types.h"

#include <wlr/types/wlr_scene.h>

typedef struct surface_rounded_t surface_rounded_t;

#define MIN_WIDTH 1
#define MIN_HEIGHT 1

// node creation and destruction
node_t *make_node(uint32_t id);
client_t *make_client(void);
void free_node(node_t *n);

// Tree layout
void arrange(struct output_t *m, desktop_t *d, bool use_transaction);
void apply_layout(struct output_t *m, desktop_t *d, node_t *n, struct wlr_box rect,
	struct wlr_box root_rect);

// node insertion and removal
node_t *find_public(desktop_t *d);
node_t *insert_node(desktop_t *d, node_t *n, node_t *f);
void remove_node(desktop_t *d, node_t *n);
void kill_node(desktop_t *d, node_t *n);

// node queries
bool is_leaf(node_t *n);
bool is_tiled(client_t *c);
bool is_floating(client_t *c);
bool is_first_child(node_t *n);
bool is_second_child(node_t *n);
unsigned int clients_count_in(node_t *n);
int tiled_count(node_t *n, bool include_receptacles);
int visible_tiled_count(desktop_t *d);
node_t *brother_tree(node_t *n);
node_t *first_extrema(node_t *n);
node_t *second_extrema(node_t *n);
node_t *next_leaf(node_t *n, node_t *r);
node_t *prev_leaf(node_t *n, node_t *r);

// Iterate over every leaf of the tree rooted at r. `it` is declared by the
// macro and must be a fresh identifier (some callers break out of the loop).
#define FOR_EACH_LEAF(it, r)                                                            \
	for (node_t *it = first_extrema(r); it != NULL; it = next_leaf(it, r))
#define FOR_EACH_LEAF_EXCLUDE_ROOT(it, r)                                               \
	for (node_t *it = first_extrema(r); it != NULL && it != (r); it = next_leaf(it, r))

// focus management
bool focus_node(struct output_t *m, desktop_t *d, node_t *n);
bool activate_node(struct output_t *m, desktop_t *d, node_t *n);
node_t *find_fence(node_t *n, direction_t dir);
bool is_adjacent(node_t *a, node_t *b, direction_t dir);

// node manipulation
void swap_nodes(struct output_t *m1, desktop_t *d1, node_t *n1, struct output_t *m2, desktop_t *d2,
	node_t *n2);
int collect_tiled_leaves(desktop_t *d, node_t ***out_nodes);
bool set_state(struct output_t *m, desktop_t *d, node_t *n, client_state_t s);
void set_floating(struct output_t *m, desktop_t *d, node_t *n, bool value);
void enter_fullscreen(struct output_t *m, desktop_t *d, node_t *n);
void close_node(node_t *n);

// preselection
presel_t *make_presel(void);
void presel_dir(node_t *n, direction_t dir);
void presel_cancel(node_t *n);

// tree transformations
void rotate_tree(node_t *n, int deg);
void flip_tree(node_t *n, flip_t flp);
void equalize_tree(node_t *n);
void balance_tree(node_t *n);

// geometry
unsigned int node_area(node_t *n);

// Transaction helpers
void node_set_dirty(node_t *n);
void node_set_pending_size(node_t *n, int width, int height);
void node_set_pending_position(node_t *n, int x, int y);
void node_set_pending_rectangle(node_t *n, struct wlr_box rect);
void node_set_pending_hidden(node_t *n, bool hidden);

// Color helpers
void parse_color(const char *hex, float *color);

// Debug helpers
void validate_tree(const char *context, desktop_t *d);

struct wlr_scene_tree *client_get_scene_tree(client_t *client);
struct wlr_scene_tree *client_get_content_tree(client_t *client);
node_t *client_get_node(client_t *client);
output_t *client_get_output(client_t *client);
surface_rounded_t *client_get_rounded(client_t *client);

typedef struct {
	output_t *output;
	desktop_t *desk;
	bool is_focused;
	bool is_active;
} border_state_t;

border_state_t get_border_state(client_t *client);

// Border helpers
void create_borders(struct wlr_scene_tree *parent, struct wlr_scene_tree **border_tree,
	struct wlr_scene_rect *rects[4]);
void destroy_borders(struct wlr_scene_tree **border_tree, struct wlr_scene_rect *rects[4]);
void update_borders(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
	struct wlr_box geo, unsigned int bw);
void update_border_colors(client_t *client);
void refresh_border_colors(void);
void refresh_border_color_cache(void);

struct wlr_scene_tree *client_border_tree(client_t *client);
struct wlr_scene_rect **client_border_rects(client_t *client);

// Misc helpers
desktop_t *find_desktop_by_name_in_monitor(output_t *mon, const char *name);
output_t *find_output_by_name(const char *name);

// macros for state checking
#define IS_TILED(c) (is_tiled(c))
#define IS_FLOATING(c) (is_floating(c))
#define IS_RECEPTACLE(n) ((n) != NULL && (n)->client == NULL && is_leaf(n))

// Client dispatch: unify toplevel/xwayland_view access
#define CLIENT_DISPATCH(client, field) \
	((client)->toplevel ? (client)->toplevel->field : \
	 (client)->xwayland_view ? (client)->xwayland_view->field : NULL)

static inline int effective_border_width(desktop_t *d) {
	if (smart_borders && d && visible_tiled_count(d) <= 1)
		return 0;
	return border_width;
}

static inline int compute_window_gap(desktop_t *d) {
	if (smart_gaps && visible_tiled_count(d) <= 1)
		return 0;
	return d->window_gap;
}

static inline struct wlr_box apply_bleed(struct wlr_box r, int bw, int wg) {
	int bleed = wg + 2 * bw;
	r.x += bw;
	r.y += bw;
	r.width = (bleed < r.width ? r.width - bleed : 0);
	r.height = (bleed < r.height ? r.height - bleed : 0);
	return r;
}
