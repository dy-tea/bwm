#pragma once

#include "types.h"

#include <stdbool.h>
#include <wlr/util/box.h>

typedef struct output_t output_t;
typedef struct desktop_t desktop_t;
typedef struct client_t client_t;
typedef struct node_t node_t;

typedef struct layout_impl_t {
	const char *name;

	void (*arrange)(output_t *m, desktop_t *d, struct wlr_box available);

	void (*on_focus)(output_t *m, desktop_t *d, node_t *n);

	bool (*focus)(desktop_t *d, direction_t dir);

	bool (*swap)(output_t *m, desktop_t *d, direction_t dir);

	void (*init_client)(client_t *c);

	int (*collect)(desktop_t *d, node_t ***out_nodes);

	bool single_visible;

	bool has_directional_nav;
} layout_impl_t;

const layout_impl_t *layout_get_impl(layout_t layout);

void layout_set(desktop_t *d, layout_t new_layout);
void layout_toggle(desktop_t *d, layout_t target);
void layout_cycle(output_t *m, desktop_t *d, int direction);
