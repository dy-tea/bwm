#pragma once

#include "types.h"

#include <stdbool.h>
#include <wlr/util/box.h>

struct output_t;

#define SCROLLER_MIN_WIDTH 1
#define SCROLLER_MIN_HEIGHT 1

typedef struct {
	enum {
		SCROLLER_WIDTH_PROPORTION,
		SCROLLER_WIDTH_FIXED
	} type;
	double value;
} scroller_column_width_t;

typedef struct {
	enum {
		SCROLLER_HEIGHT_AUTO,
		SCROLLER_HEIGHT_FIXED
	} type;
	double value;
} scroller_window_height_t;

typedef struct scroller_tile_t {
	client_t *client;
	scroller_window_height_t height;
	struct wlr_box rect;
} scroller_tile_t;

typedef struct scroller_column_t {
	scroller_tile_t *tiles;
	int tile_count, capacity, active_tile_idx;
	scroller_column_width_t width;
	double resolved_width;
} scroller_column_t;

typedef struct scroller_state_t {
	scroller_column_t *columns;
	int column_count, capacity, active_column_idx;
	double view_offset;
	struct wlr_box working_area;
	bool activate_prev_column_on_removal;
} scroller_state_t;

scroller_state_t *scroller_create(void);
void scroller_destroy(scroller_state_t *s);

void scroller_arrange(struct output_t *m, desktop_t *d, struct wlr_box available);

bool scroller_add_tile(scroller_state_t *s, client_t *client, bool activate);
bool scroller_add_tile_to_column(scroller_state_t *s, client_t *client, int col_idx, bool activate);
void scroller_remove_tile(scroller_state_t *s, client_t *client, struct output_t *m);

bool scroller_focus_next(desktop_t *d);
bool scroller_focus_prev(desktop_t *d);
bool scroller_focus_down(desktop_t *d);
bool scroller_focus_up(desktop_t *d);
void scroller_center_window(desktop_t *d, client_t *client);

bool scroller_consume_into_column(desktop_t *d);
bool scroller_expel_from_column(desktop_t *d);

bool scroller_is_tiled(const client_t *c);
void scroller_apply_client_rules(client_t *c, float rule_proportion, float rule_proportion_single);

bool scroller_resize_width(desktop_t *d, float delta);
bool scroller_resize_stack(desktop_t *d, float delta);
void scroller_set_proportion(client_t *client, float proportion);
void scroller_cycle_proportion_preset(client_t *client);

void scroller_apply_active_focus(desktop_t *d, struct output_t *m);

void scroller_view_offset_gesture_begin(desktop_t *d, bool is_touchpad);
void scroller_view_offset_gesture_update(desktop_t *d, double delta_x);
bool scroller_view_offset_gesture_end(desktop_t *d);

int scroller_collect(desktop_t *d, node_t ***out_nodes);

extern float scroller_default_proportion;
extern float scroller_default_proportion_single;
extern int scroller_structs;
extern bool scroller_focus_center;
extern bool scroller_prefer_center;
extern bool scroller_prefer_overspread;
extern bool scroller_ignore_proportion_single;
extern bool edge_scroller_pointer_focus;
extern float *scroller_proportion_preset;
extern int scroller_proportion_preset_count;
