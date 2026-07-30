#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

struct sd_bus;
struct sd_bus_slot;

// D-Bus interface / bus names
#define GS_IMPL_IFACE "org.freedesktop.impl.portal.GlobalShortcuts"
#define GS_SESSION_IFACE "org.freedesktop.portal.Session"
#define GS_REQUEST_IFACE "org.freedesktop.portal.Request"
#define GS_BUS_NAME "org.freedesktop.impl.portal.desktop.doors"
#define GS_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define GS_VERSION 2

// Portal response codes
#define GS_RESPONSE_SUCCESS 0
#define GS_RESPONSE_CANCELLED 1
#define GS_RESPONSE_ERROR 2

typedef struct gs_shortcut {
	char *id;
	char *description;
	char *trigger_description; // human-readable, e.g. "Super+K"
	uint32_t modifiers; // XKB modifier mask (WLR_MODIFIER_*)
	xkb_keysym_t keysym;
	bool has_trigger;

	struct gs_session *session;
	struct wl_list link; // gs_session.shortcuts
} gs_shortcut_t;

typedef struct gs_session {
	char *app_id;
	char *session_path;
	char *request_path;
	bool destroyed;
	struct wl_list shortcuts; // gs_shortcut.link
	struct wl_list link; // gs_state.sessions
	struct sd_bus_slot *session_slot;
	struct sd_bus_slot *request_slot;
} gs_session_t;

typedef struct gs_state {
	struct sd_bus *bus;
	struct sd_bus_slot *main_slot;
	struct sd_bus_slot *session_fallback_slot;
	struct sd_bus_slot *request_fallback_slot;
	struct wl_event_source *bus_event_source;
	struct wl_list sessions; // gs_session.link
	bool initialized;
} gs_state_t;

void global_shortcuts_init(void);
void global_shortcuts_fini(void);
bool global_shortcuts_handle_key(uint32_t modifiers, uint32_t keycode, const xkb_keysym_t *syms,
	int nsyms, uint32_t state, uint32_t time_msec);
