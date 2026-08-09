#pragma once

#include <stdbool.h>
#include <wlr/util/box.h>

typedef struct node_t node_t;
typedef struct desktop_t desktop_t;
typedef struct output_t output_t;

typedef enum {
	MASTER_LEFT,
	MASTER_TOP,
	MASTER_RIGHT,
	MASTER_BOTTOM,
	MASTER_CENTER,
} master_area_orientation_t;

typedef enum {
	STACK_VERTICAL,
	STACK_HORIZONTAL,
} stack_layout_t;

extern float master_stack_ratio;
extern master_area_orientation_t master_stack_orientation;
extern stack_layout_t master_stack_layout;

void master_stack_arrange(struct output_t *m, desktop_t *d, struct wlr_box available);

int master_stack_collect(desktop_t *d, node_t ***out_nodes);

bool master_stack_focus_south(desktop_t *d);
bool master_stack_focus_north(desktop_t *d);
bool master_stack_focus_east(desktop_t *d);
bool master_stack_focus_west(desktop_t *d);

bool master_stack_swap_south(output_t *m, desktop_t *d);
bool master_stack_swap_north(output_t *m, desktop_t *d);
bool master_stack_swap_east(output_t *m, desktop_t *d);
bool master_stack_swap_west(output_t *m, desktop_t *d);

bool master_stack_increment(desktop_t *d);
bool master_stack_decrement(desktop_t *d);
bool master_stack_promote(desktop_t *d);
bool master_stack_demote(desktop_t *d);
void master_stack_set_count(desktop_t *d, int count);
bool master_stack_adjust_ratio(float delta);
void master_stack_set_orientation(master_area_orientation_t orientation);
void master_stack_flip_orientation(void);
void master_stack_cycle_orientation(void);
void master_stack_cycle_stack_layout(void);
