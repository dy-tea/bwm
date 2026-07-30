#include "global_shortcuts.h"
#include "server.h"

#ifdef HAVE_SYSTEMD
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <systemd/sd-bus.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

extern struct server_t server;
static gs_state_t gs;

static int gs_method_create_session(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int gs_method_bind_shortcuts(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int gs_method_list_shortcuts(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int gs_method_configure_shortcuts(sd_bus_message *m, void *userdata,
	sd_bus_error *ret_error);

static int gs_session_method_close(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int gs_request_method_close(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

static int gs_property_get_version(sd_bus *bus, const char *path, const char *interface,
	const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);

static const sd_bus_vtable gs_main_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", gs_method_create_session,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("BindShortcuts", "ooa(sa{sv})sa{sv}", "ua{sv}", gs_method_bind_shortcuts,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ListShortcuts", "oo", "ua{sv}", gs_method_list_shortcuts,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ConfigureShortcuts", "osa{sv}", "", gs_method_configure_shortcuts,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_WRITABLE_PROPERTY("version", "u", gs_property_get_version, NULL, 0, 0),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable gs_session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", gs_session_method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable gs_request_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", gs_request_method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

static uint32_t parse_single_modifier(const char *name, size_t len) {
	char buf[32];
	if (len >= sizeof(buf))
		return 0;
	memcpy(buf, name, len);
	buf[len] = '\0';

	if (strcasecmp(buf, "Control") == 0 || strcasecmp(buf, "Ctrl") == 0)
		return WLR_MODIFIER_CTRL;
	if (strcasecmp(buf, "Shift") == 0)
		return WLR_MODIFIER_SHIFT;
	if (strcasecmp(buf, "Super") == 0 || strcasecmp(buf, "Win") == 0 || strcasecmp(buf, "Mod4") == 0)
		return WLR_MODIFIER_LOGO;
	if (strcasecmp(buf, "Alt") == 0 || strcasecmp(buf, "Mod1") == 0)
		return WLR_MODIFIER_ALT;
	if (strcasecmp(buf, "Hyper") == 0 || strcasecmp(buf, "Mod3") == 0)
		return WLR_MODIFIER_MOD3;
	return 0;
}

static bool parse_trigger_string(const char *trigger, uint32_t *out_mods, xkb_keysym_t *out_keysym,
		char *out_desc, size_t desc_size) {
	*out_mods = 0;
	*out_keysym = XKB_KEY_NoSymbol;
	out_desc[0] = '\0';

	if (!trigger || !*trigger)
		return false;

	const char *p = trigger;
	uint32_t mods = 0;

	while (*p == '<') {
		const char *end = strchr(p, '>');
		if (!end)
			return false;
		size_t len = (size_t)(end - p - 1);
		uint32_t m = parse_single_modifier(p + 1, len);
		if (m)
			mods |= m;
		p = end + 1;
	}

	if (!*p)
		return false;

	xkb_keysym_t keysym = xkb_keysym_from_name(p, XKB_KEYSYM_CASE_INSENSITIVE);
	if (keysym == XKB_KEY_NoSymbol)
		return false;

	*out_mods = mods;
	*out_keysym = keysym;

	char buf[256];
	int pos = 0;
	if (mods & WLR_MODIFIER_CTRL)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "Ctrl+");
	if (mods & WLR_MODIFIER_ALT)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "Alt+");
	if (mods & WLR_MODIFIER_SHIFT)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "Shift+");
	if (mods & WLR_MODIFIER_LOGO)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "Super+");

	xkb_keysym_get_name(keysym, buf + pos, sizeof(buf) - pos);
	if (buf[pos] >= 'a' && buf[pos] <= 'z')
		buf[pos] -= 'a' - 'A';

	snprintf(out_desc, desc_size, "%s", buf);
	return true;
}

static void shortcut_destroy(gs_shortcut_t *sc) {
	wl_list_remove(&sc->link);
	free(sc->id);
	free(sc->description);
	free(sc->trigger_description);
	free(sc);
}

static void session_destroy(gs_session_t *sess) {
	if (sess->destroyed)
		return;
	sess->destroyed = true;

	gs_shortcut_t *sc, *sc_tmp;
	wl_list_for_each_safe(sc, sc_tmp, &sess->shortcuts, link) {
		shortcut_destroy(sc);
	}

	wl_list_remove(&sess->link);

	if (sess->session_slot) {
		sd_bus_slot_unref(sess->session_slot);
		sess->session_slot = NULL;
	}
	if (sess->request_slot) {
		sd_bus_slot_unref(sess->request_slot);
		sess->request_slot = NULL;
	}

	free(sess->app_id);
	free(sess->session_path);
	free(sess->request_path);
	free(sess);
}

static gs_session_t *session_find_by_path(const char *path) {
	gs_session_t *sess;
	wl_list_for_each(sess, &gs.sessions, link) {
		if (!sess->destroyed && strcmp(sess->session_path, path) == 0)
			return sess;
	}
	return NULL;
}

static int append_shortcut_to_array(sd_bus_message *reply, gs_shortcut_t *sc) {
	int r = sd_bus_message_open_container(reply, 'r', "sa{sv}");
	if (r < 0)
		return r;

	r = sd_bus_message_append(reply, "s", sc->id);
	if (r < 0)
		goto fail;

	r = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (r < 0)
		goto fail;

	// description
	r = sd_bus_message_open_container(reply, 'e', "sv");
	if (r < 0)
		goto fail;
	r = sd_bus_message_append(reply, "s", "description");
	if (r < 0)
		goto fail;
	r = sd_bus_message_open_container(reply, 'v', "s");
	if (r < 0)
		goto fail;
	r = sd_bus_message_append(reply, "s", sc->description ? sc->description : "");
	if (r < 0)
		goto fail;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		goto fail;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		goto fail;

	// trigger_description
	r = sd_bus_message_open_container(reply, 'e', "sv");
	if (r < 0)
		goto fail;
	r = sd_bus_message_append(reply, "s", "trigger_description");
	if (r < 0)
		goto fail;
	r = sd_bus_message_open_container(reply, 'v', "s");
	if (r < 0)
		goto fail;
	r = sd_bus_message_append(reply, "s", sc->trigger_description ? sc->trigger_description : "");
	if (r < 0)
		goto fail;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		goto fail;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		goto fail;

	r = sd_bus_message_close_container(reply); // a{sv}
	if (r < 0)
		goto fail;
	r = sd_bus_message_close_container(reply); // (sa{sv})
	if (r < 0)
		goto fail;
	return r;

fail:
	sd_bus_message_close_container(reply);
	return r;
}

static int open_shortcuts_array(sd_bus_message *reply) {
	int r;
	r = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (r < 0)
		return r;
	r = sd_bus_message_open_container(reply, 'e', "sv");
	if (r < 0)
		goto fail;
	r = sd_bus_message_append(reply, "s", "shortcuts");
	if (r < 0)
		goto fail;
	r = sd_bus_message_open_container(reply, 'v', "a(sa{sv})");
	if (r < 0)
		goto fail;
	r = sd_bus_message_open_container(reply, 'a', "(sa{sv})");
	if (r < 0)
		goto fail;
	return 0;
fail:
	sd_bus_message_close_container(reply);
	return r;
}

static int close_shortcuts_array(sd_bus_message *reply) {
	int r;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		return r;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		return r;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		return r;
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		return r;
	return 0;
}

static void emit_activated(gs_session_t *sess, gs_shortcut_t *sc, uint64_t timestamp) {
	if (!gs.bus)
		return;
	sd_bus_message *sig = NULL;
	int r = sd_bus_message_new_signal(gs.bus, &sig, GS_OBJECT_PATH, GS_IMPL_IFACE, "Activated");
	if (r < 0 || !sig)
		return;
	sd_bus_message_append(sig, "ost", sess->session_path, sc->id, timestamp);
	sd_bus_message_open_container(sig, 'a', "{sv}");
	sd_bus_message_close_container(sig);
	sd_bus_send(gs.bus, sig, NULL);
	sd_bus_message_unref(sig);
}

static void emit_deactivated(gs_session_t *sess, gs_shortcut_t *sc, uint64_t timestamp) {
	if (!gs.bus)
		return;
	sd_bus_message *sig = NULL;
	int r = sd_bus_message_new_signal(gs.bus, &sig, GS_OBJECT_PATH, GS_IMPL_IFACE, "Deactivated");
	if (r < 0 || !sig)
		return;
	sd_bus_message_append(sig, "ost", sess->session_path, sc->id, timestamp);
	sd_bus_message_open_container(sig, 'a', "{sv}");
	sd_bus_message_close_container(sig);
	sd_bus_send(gs.bus, sig, NULL);
	sd_bus_message_unref(sig);
}

static void emit_shortcuts_changed(gs_session_t *sess) {
	if (!gs.bus)
		return;
	sd_bus_message *sig = NULL;
	int r = sd_bus_message_new_signal(gs.bus, &sig, GS_OBJECT_PATH, GS_IMPL_IFACE, "ShortcutsChanged");
	if (r < 0 || !sig)
		return;
	sd_bus_message_append(sig, "o", sess->session_path);
	sd_bus_message_open_container(sig, 'a', "(sa{sv})");
	gs_shortcut_t *sc;
	wl_list_for_each(sc, &sess->shortcuts, link) {
		append_shortcut_to_array(sig, sc);
	}
	sd_bus_message_close_container(sig);
	sd_bus_send(gs.bus, sig, NULL);
	sd_bus_message_unref(sig);
}

static int gs_method_create_session(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)userdata;
	(void)ret_error;

	const char *handle_path, *session_path, *app_id;
	int r = sd_bus_message_read(m, "oos", &handle_path, &session_path, &app_id);
	if (r < 0)
		return r;

	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r >= 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			sd_bus_message_skip(m, "v");
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}

	wlr_log(WLR_INFO, "CreateSession from %s (session=%s)", app_id, session_path);

	gs_session_t *sess = calloc(1, sizeof(*sess));
	if (!sess)
		return -ENOMEM;

	sess->app_id = strdup(app_id);
	sess->session_path = strdup(session_path);
	sess->request_path = strdup(handle_path);
	wl_list_init(&sess->shortcuts);
	wl_list_insert(&gs.sessions, &sess->link);

	r = sd_bus_add_object_vtable(gs.bus, &sess->session_slot, sess->session_path, GS_SESSION_IFACE,
		gs_session_vtable, sess);
	if (r < 0) {
		wlr_log(WLR_ERROR, "failed to register session: %s", strerror(-r));
		session_destroy(sess);
		return r;
	}

	r = sd_bus_add_object_vtable(gs.bus, &sess->request_slot, sess->request_path, GS_REQUEST_IFACE,
		gs_request_vtable, sess);
	if (r < 0) {
		wlr_log(WLR_ERROR, "failed to register request: %s", strerror(-r));
		session_destroy(sess);
		return r;
	}

	sd_bus_emit_object_added(gs.bus, sess->session_path);
	sd_bus_emit_object_added(gs.bus, sess->request_path);

	return sd_bus_reply_method_return(m, "ua{sv}", GS_RESPONSE_SUCCESS, 0);
}

static int gs_method_bind_shortcuts(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)userdata;
	(void)ret_error;

	const char *handle_path, *session_path, *parent_window;
	int r = sd_bus_message_read(m, "oos", &handle_path, &session_path, &parent_window);
	if (r < 0)
		return r;

	gs_session_t *sess = session_find_by_path(session_path);
	if (!sess) {
		sd_bus_message_skip(m, "a(sa{sv})sa{sv}");
		return sd_bus_reply_method_errorf(m, "org.freedesktop.DBus.Error.Failed", "Unknown session");
	}

	gs_shortcut_t *sc_tmp2, *sc_tmp3;
	wl_list_for_each_safe(sc_tmp2, sc_tmp3, &sess->shortcuts, link)
		shortcut_destroy(sc_tmp2);

	r = sd_bus_message_enter_container(m, 'a', "(sa{sv})");
	if (r < 0)
		return r;

	while ((r = sd_bus_message_enter_container(m, 'r', "sa{sv}")) > 0) {
		const char *id;
		r = sd_bus_message_read(m, "s", &id);
		if (r < 0) {
			sd_bus_message_exit_container(m);
			break;
		}

		char *description = NULL;
		char *preferred_trigger = NULL;

		r = sd_bus_message_enter_container(m, 'a', "{sv}");
		if (r >= 0) {
			while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
				const char *key;
				r = sd_bus_message_read(m, "s", &key);
				if (r < 0)
					break;
				if (strcmp(key, "description") == 0) {
					const char *val;
					if (sd_bus_message_read(m, "v", "s", &val) >= 0)
						description = strdup(val);
					else
						sd_bus_message_skip(m, "v");
				} else if (strcmp(key, "preferred_trigger") == 0) {
					const char *val;
					if (sd_bus_message_read(m, "v", "s", &val) >= 0)
						preferred_trigger = strdup(val);
					else
						sd_bus_message_skip(m, "v");
				} else {
					sd_bus_message_skip(m, "v");
				}
				sd_bus_message_exit_container(m);
			}
			sd_bus_message_exit_container(m);
		}

		sd_bus_message_exit_container(m);

		gs_shortcut_t *sc = calloc(1, sizeof(*sc));
		if (sc) {
			sc->id = strdup(id);
			sc->description = description;
			sc->session = sess;
			char desc_buf[256];
			sc->has_trigger = parse_trigger_string(preferred_trigger, &sc->modifiers, &sc->keysym, desc_buf,
				sizeof(desc_buf));
			if (sc->has_trigger)
				sc->trigger_description = strdup(desc_buf);
			wl_list_insert(&sess->shortcuts, &sc->link);
		} else {
			free(description);
		}
		free(preferred_trigger);
	}

	sd_bus_message_exit_container(m);

	sd_bus_message_enter_container(m, 'a', "{sv}");
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	sd_bus_message *reply = NULL;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0 || !reply)
		return -ENOMEM;

	r = sd_bus_message_append(reply, "u", GS_RESPONSE_SUCCESS);
	if (r < 0)
		goto reply_fail;
	r = open_shortcuts_array(reply);
	if (r < 0)
		goto reply_fail;
	gs_shortcut_t *sc;
	wl_list_for_each(sc, &sess->shortcuts, link) {
		append_shortcut_to_array(reply, sc);
	}
	r = close_shortcuts_array(reply);
	if (r < 0)
		goto reply_fail;
	sd_bus_send(gs.bus, reply, NULL);
	sd_bus_message_unref(reply);
	emit_shortcuts_changed(sess);
	return 1;

reply_fail:
	sd_bus_message_unref(reply);
	return r;
}

static int gs_method_list_shortcuts(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)userdata;
	(void)ret_error;

	const char *session_path, *handle_path;
	int r = sd_bus_message_read(m, "oo", &session_path, &handle_path);
	if (r < 0)
		return r;

	gs_session_t *sess = session_find_by_path(session_path);
	if (!sess)
		return sd_bus_reply_method_errorf(m, "org.freedesktop.DBus.Error.Failed", "Unknown session");

	sd_bus_message *reply = NULL;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0 || !reply)
		return -ENOMEM;

	r = sd_bus_message_append(reply, "u", GS_RESPONSE_SUCCESS);
	if (r < 0)
		goto reply_fail;
	r = open_shortcuts_array(reply);
	if (r < 0)
		goto reply_fail;
	gs_shortcut_t *sc;
	wl_list_for_each(sc, &sess->shortcuts, link) {
		append_shortcut_to_array(reply, sc);
	}
	r = close_shortcuts_array(reply);
	if (r < 0)
		goto reply_fail;
	sd_bus_send(gs.bus, reply, NULL);
	sd_bus_message_unref(reply);
	return 1;

reply_fail:
	sd_bus_message_unref(reply);
	return r;
}

static int gs_method_configure_shortcuts(sd_bus_message *m, void *userdata,
		sd_bus_error *ret_error) {
	(void)userdata;
	(void)ret_error;

	const char *session_path, *parent_window;
	int r = sd_bus_message_read(m, "os", &session_path, &parent_window);
	if (r < 0)
		return r;

	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r >= 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			sd_bus_message_skip(m, "v");
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}

	return 1;
}

static int gs_session_method_close(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	gs_session_t *sess = userdata;
	wlr_log(WLR_INFO, "session closed by %s", sess->app_id);
	session_destroy(sess);
	return sd_bus_reply_method_return(m, "");
}

static int gs_request_method_close(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	(void)userdata;
	return sd_bus_reply_method_return(m, "");
}

static int gs_property_get_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
	(void)bus;
	(void)path;
	(void)interface;
	(void)property;
	(void)userdata;
	(void)ret_error;
	return sd_bus_message_append(reply, "u", GS_VERSION);
}

static int bus_dispatch(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	(void)data;
	if (!gs.bus)
		return 0;
	int r;
	do {
		if ((r = sd_bus_process(gs.bus, NULL)) < 0) {
			wlr_log(WLR_ERROR, "sd_bus_process failed: %s", strerror(-r));
			break;
		}
	} while (r != 0);
	return 0;
}

void global_shortcuts_init(void) {
	memset(&gs, 0, sizeof(gs));
	wl_list_init(&gs.sessions);

	int r = sd_bus_open_user(&gs.bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "failed to connect to session bus: %s", strerror(-r));
		return;
	}

	r = sd_bus_request_name(gs.bus, GS_BUS_NAME, 0);
	if (r < 0) {
		wlr_log(WLR_ERROR, "failed to request bus name '%s': %s", GS_BUS_NAME,
			strerror(-r));
		sd_bus_unref(gs.bus);
		gs.bus = NULL;
		return;
	}

	r = sd_bus_add_object_vtable(gs.bus, &gs.main_slot, GS_OBJECT_PATH, GS_IMPL_IFACE, gs_main_vtable,
		NULL);
	if (r < 0) {
		wlr_log(WLR_ERROR, "failed to register main vtable: %s", strerror(-r));
		sd_bus_unref(gs.bus);
		gs.bus = NULL;
		return;
	}

	int bus_fd = sd_bus_get_fd(gs.bus);
	if (bus_fd < 0) {
		wlr_log(WLR_ERROR, "failed to get bus fd: %s", strerror(-bus_fd));
		sd_bus_unref(gs.bus);
		gs.bus = NULL;
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	gs.bus_event_source = wl_event_loop_add_fd(loop, bus_fd, WL_EVENT_READABLE, bus_dispatch, NULL);
	if (!gs.bus_event_source) {
		wlr_log(WLR_ERROR, "failed to add bus fd to event loop");
		sd_bus_unref(gs.bus);
		gs.bus = NULL;
		return;
	}

	gs.initialized = true;
	wlr_log(WLR_INFO, "initialized on %s", GS_BUS_NAME);
}

void global_shortcuts_fini(void) {
	if (!gs.initialized)
		return;

	gs_session_t *sess, *sess_tmp;
	wl_list_for_each_safe(sess, sess_tmp, &gs.sessions, link) {
		session_destroy(sess);
	}

	if (gs.bus_event_source) {
		wl_event_source_remove(gs.bus_event_source);
		gs.bus_event_source = NULL;
	}
	if (gs.main_slot) {
		sd_bus_slot_unref(gs.main_slot);
		gs.main_slot = NULL;
	}
	if (gs.bus) {
		sd_bus_flush(gs.bus);
		sd_bus_unref(gs.bus);
		gs.bus = NULL;
	}

	gs.initialized = false;
}

bool global_shortcuts_handle_key(uint32_t modifiers, uint32_t keycode, const xkb_keysym_t *syms,
		int nsyms, uint32_t state, uint32_t time_msec) {
	(void)keycode;

	if (!gs.initialized || !gs.bus)
		return false;

	gs_session_t *sess;
	wl_list_for_each_reverse(sess, &gs.sessions, link) {
		if (sess->destroyed)
			continue;
		gs_shortcut_t *sc;
		wl_list_for_each(sc, &sess->shortcuts, link) {
			if (!sc->has_trigger)
				continue;
			bool mods_match = (modifiers & sc->modifiers) == sc->modifiers;
			if (!mods_match)
				continue;
			bool keysym_matched = false;
			for (int i = 0; i < nsyms; i++) {
				if (syms[i] == sc->keysym) {
					keysym_matched = true;
					break;
				}
			}
			if (!keysym_matched)
				continue;
			uint64_t timestamp = (uint64_t)time_msec;
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
				emit_activated(sess, sc, timestamp);
				wlr_log(WLR_DEBUG, "activated '%s' from %s", sc->id, sess->app_id);
			} else {
				emit_deactivated(sess, sc, timestamp);
			}
			return true;
		}
	}

	return false;
}


#else // !HAVE_SYSTEMD

#include <stdbool.h>
#include <stdint.h>

void global_shortcuts_init(void) {
	// noop
}

void global_shortcuts_fini(void) {
	// noop
}

bool global_shortcuts_handle_key(uint32_t modifiers, uint32_t keycode, const xkb_keysym_t *syms,
		int nsyms, uint32_t state, uint32_t time_msec) {
	(void)modifiers;
	(void)keycode;
	(void)syms;
	(void)nsyms;
	(void)state;
	(void)time_msec;
	return false;
}


#endif // HAVE_SYSTEMD
