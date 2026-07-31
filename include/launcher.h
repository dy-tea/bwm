#pragma once

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

typedef struct {
	pid_t pid;
	char *desktop_name;
	struct wlr_xdg_activation_token_v1 *token;
	struct wl_listener token_destroy;
	struct wl_listener seat_destroy;
	bool activated;
	bool had_focused_surface;
	struct wl_list link;
} launcher_ctx_t;

launcher_ctx_t *launcher_ctx_find_pid(pid_t pid);
void launcher_ctx_consume(launcher_ctx_t *ctx);
void launcher_ctx_destroy(launcher_ctx_t *ctx);
launcher_ctx_t *launcher_ctx_create(struct wlr_xdg_activation_token_v1 *token,
	const char *desktop_name);
const char *launcher_ctx_get_token_name(launcher_ctx_t *ctx);

void launcher_init(void);
void launcher_fini(void);
