#pragma once

#include <stdbool.h>

typedef struct node_t node_t;

void scratchpad_init(void);
void scratchpad_fini(void);

void scratchpad_add(node_t *n);
void scratchpad_remove(node_t *n);
bool scratchpad_has(node_t *n);

void scratchpad_show(node_t *n);
void scratchpad_hide(node_t *n);
void scratchpad_toggle(node_t *n);

void scratchpad_toggle_auto(void);

node_t *scratchpad_find_by_app_id(const char *app_id);
node_t *scratchpad_find_by_title(const char *title);
