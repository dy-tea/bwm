#include "once.h"
#include "render_unfocused.h"
#include "server.h"
#include "tree.h"
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_scene.h>

int render_unfocused_fps = 5;

static struct wl_list render_unfocused_clients;
static struct wl_event_source *render_unfocused_timer = NULL;

struct render_unfocused_link {
	struct wl_list link;
	client_t *client;
};

static void send_frame_done_to_client(client_t *client) {
	struct timespec when;
	clock_gettime(CLOCK_MONOTONIC, &when);

	struct wlr_scene_tree *tree = client_get_content_tree(client);
	if (!tree)
		return;

	struct wlr_scene_node *node;
	wl_list_for_each(node, &tree->children, link)
		wlr_scene_node_for_each_buffer(node, toplevel_send_frame_done_interator, &when);
}

static int handle_render_unfocused_timer(void *data) {
	(void)data;
	if (wl_list_empty(&render_unfocused_clients))
		return 0;

	struct render_unfocused_link *rfl, *tmp;
	wl_list_for_each_safe(rfl, tmp, &render_unfocused_clients, link) {
		if (!rfl->client) {
			wl_list_remove(&rfl->link);
			free(rfl);
			continue;
		}
		struct wlr_scene_tree *st = client_get_scene_tree(rfl->client);
		if (st && st->node.enabled)
			continue;
		send_frame_done_to_client(rfl->client);
	}

	if (!wl_list_empty(&render_unfocused_clients) && render_unfocused_fps > 0)
		wl_event_source_timer_update(render_unfocused_timer, 1000 / render_unfocused_fps);

	return 0;
}

void render_unfocused_client_update(client_t *client) {
	if (!client)
		return;

	bool enabled = client->flags.render_unfocused;

	struct render_unfocused_link *rfl, *tmp;
	wl_list_for_each_safe(rfl, tmp, &render_unfocused_clients, link) {
		if (rfl->client == client) {
			if (!enabled) {
				wl_list_remove(&rfl->link);
				free(rfl);
			}
			return;
		}
	}

	if (!enabled)
		return;

	rfl = calloc(1, sizeof(*rfl));
	if (!rfl)
		return;
	rfl->client = client;
	wl_list_insert(&render_unfocused_clients, &rfl->link);

	if (!render_unfocused_timer && render_unfocused_fps > 0) {
		render_unfocused_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display),
			handle_render_unfocused_timer, NULL);
		if (render_unfocused_timer)
			wl_event_source_timer_update(render_unfocused_timer, 1000 / render_unfocused_fps);
	}
}

void render_unfocused_client_remove(client_t *client) {
	if (!client)
		return;

	struct render_unfocused_link *rfl, *tmp;
	wl_list_for_each_safe(rfl, tmp, &render_unfocused_clients, link) {
		if (rfl->client == client) {
			wl_list_remove(&rfl->link);
			free(rfl);
			return;
		}
	}
}

void render_unfocused_init(void) {
	ONCE();
	wl_list_init(&render_unfocused_clients);
}

void render_unfocused_fini(void) {
	ONCE();
	if (render_unfocused_timer) {
		wl_event_source_remove(render_unfocused_timer);
		render_unfocused_timer = NULL;
	}
	struct render_unfocused_link *rfl, *tmp;
	wl_list_for_each_safe(rfl, tmp, &render_unfocused_clients, link) {
		wl_list_remove(&rfl->link);
		free(rfl);
	}
}
