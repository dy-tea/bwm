#pragma once

#include "types.h"
#include <wlr/util/box.h>

// Configuration
extern float scroller_default_proportion;
extern float scroller_default_proportion_single;
extern int scroller_structs;
extern bool scroller_focus_center;
extern bool scroller_prefer_center;
extern bool scroller_prefer_overspread;
extern bool scroller_ignore_proportion_single;
extern bool edge_scroller_pointer_focus;

// Proportion presets
extern float *scroller_proportion_preset;
extern int scroller_proportion_preset_count;

void scroller_arrange(monitor_t *m, desktop_t *d, struct wlr_box available);

void scroller_stack_push(client_t *head, client_t *new_client);
void scroller_stack_remove(client_t *client);
client_t *scroller_get_stack_head(client_t *client);

void scroller_resize_width(client_t *client, float delta);
void scroller_resize_stack(client_t *client, float delta);

void scroller_set_proportion(client_t *client, float proportion);
void scroller_cycle_proportion_preset(client_t *client);

void scroller_center_window(desktop_t *d, client_t *client);

bool scroller_is_tiled(client_t *c);
int scroller_count_tiled_clients(desktop_t *d);
void scroller_init_client(client_t *c);

bool scroller_focus_next(desktop_t *d);
bool scroller_focus_prev(desktop_t *d);
bool scroller_focus_down(desktop_t *d);
bool scroller_focus_up(desktop_t *d);

void scroller_apply_client_rules(client_t *c, float rule_proportion, float rule_proportion_single);
