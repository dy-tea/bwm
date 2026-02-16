#pragma once

#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <stdbool.h>

void workspace_init(void);
void workspace_fini(void);
void workspace_create_desktop(const char *name);
void workspace_switch_to_desktop(const char *name);
struct wlr_ext_workspace_handle_v1 *workspace_get_active(void);

struct desktop_t;
struct desktop_t *find_desktop_by_name(const char *name);
