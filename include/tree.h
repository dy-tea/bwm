#pragma once

#include "types.h"
#include <wlr/types/wlr_scene.h>

#define MIN_WIDTH 1
#define MIN_HEIGHT 1

// node creation and destruction
node_t *make_node(uint32_t id);
client_t *make_client(void);
void free_node(node_t *n);

// Tree layout
void arrange(monitor_t *m, desktop_t *d, bool use_transaction);
void apply_layout(monitor_t *m, desktop_t *d, node_t *n, struct wlr_box rect,
                  struct wlr_box root_rect);

// node insertion and removal
node_t *find_public(desktop_t *d);
node_t *insert_node(monitor_t *m, desktop_t *d, node_t *n, node_t *f);
void remove_node(monitor_t *m, desktop_t *d, node_t *n);
void kill_node(monitor_t *m, desktop_t *d, node_t *n);

// node queries
bool is_leaf(node_t *n);
bool is_tiled(client_t *c);
bool is_floating(client_t *c);
bool is_first_child(node_t *n);
bool is_second_child(node_t *n);
unsigned int clients_count_in(node_t *n);
int tiled_count(node_t *n, bool include_receptacles);
node_t *brother_tree(node_t *n);
node_t *first_extrema(node_t *n);
node_t *second_extrema(node_t *n);
node_t *next_leaf(node_t *n, node_t *r);
node_t *prev_leaf(node_t *n, node_t *r);

// focus management
bool focus_node(monitor_t *m, desktop_t *d, node_t *n);
bool activate_node(monitor_t *m, desktop_t *d, node_t *n);
node_t *find_fence(node_t *n, direction_t dir);
bool is_adjacent(node_t *a, node_t *b, direction_t dir);

// node manipulation
void swap_nodes(monitor_t *m1, desktop_t *d1, node_t *n1, monitor_t *m2,
                desktop_t *d2, node_t *n2);
bool set_state(monitor_t *m, desktop_t *d, node_t *n, client_state_t s);
void set_floating(monitor_t *m, desktop_t *d, node_t *n, bool value);
void close_node(node_t *n);

// preselection
presel_t *make_presel(void);
void presel_dir(monitor_t *m, desktop_t *d, node_t *n, direction_t dir);
void presel_cancel(monitor_t *m, desktop_t *d, node_t *n);

// tree transformations
void rotate_tree(node_t *n, int deg);
void flip_tree(node_t *n, flip_t flp);
void equalize_tree(node_t *n);
void balance_tree(node_t *n);

// geometry
struct wlr_box get_rectangle(monitor_t *m, desktop_t *d, node_t *n);
unsigned int node_area(desktop_t *d, node_t *n);

// Transaction helpers
void node_set_dirty(node_t *n);
void node_set_pending_size(node_t *n, int width, int height);
void node_set_pending_position(node_t *n, int x, int y);
void node_set_pending_rectangle(node_t *n, struct wlr_box rect);
void node_set_pending_hidden(node_t *n, bool hidden);

// Debug helpers
void print_tree(node_t *n, int depth);
void validate_tree(const char *context, desktop_t *d);

struct wlr_scene_tree *client_get_scene_tree(client_t *client);

// Border helpers
void create_borders(struct wlr_scene_tree *parent, struct wlr_scene_tree **border_tree,
                    struct wlr_scene_rect *rects[4]);
void destroy_borders(struct wlr_scene_tree **border_tree, struct wlr_scene_rect *rects[4]);
void update_borders(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
                    struct wlr_box geo, unsigned int bw);
void update_border_colors(struct wlr_scene_tree *border_tree, struct wlr_scene_rect *rects[4],
                          client_t *client);

// macros for state checking
#define IS_TILED(c) (is_tiled(c))
#define IS_FLOATING(c) (is_floating(c))
#define IS_RECEPTACLE(n) ((n) != NULL && (n)->client == NULL && is_leaf(n))
