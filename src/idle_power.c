#include "idle.h"
#include "idle_power.h"
#include "once.h"
#include "output.h"
#include "server.h"
#include "types.h"
#include <wayland-server-core.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

static struct wl_event_source *idle_timer = NULL;
static struct wl_event_source *dpms_poll_timer = NULL;
static bool displays_off = false;

static bool is_inhibited(void) {
	struct wlr_idle_inhibitor_v1 *idle;
	wl_list_for_each(idle, &server.idle_inhibit_manager->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(idle->surface);
		struct wlr_scene_tree *tree = surface->data;
		int x, y;
		if (tree != NULL && wlr_scene_node_coords(&tree->node, &x, &y))
			return true;
	}
	return false;
}

static void turn_displays_on(void) {
	if (!displays_off)
		return;
	displays_off = false;
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		if (!m->wlr_output)
			continue;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_commit_state(m->wlr_output, &state);
		wlr_output_state_finish(&state);
	}
	wlr_log(WLR_DEBUG, "idle_power: displays turned on");
}

static void turn_displays_off(void) {
	if (displays_off)
		return;
	displays_off = true;
	output_t *m;
	wl_list_for_each(m, &mon_list, link) {
		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, false);
		wlr_output_commit_state(m->wlr_output, &state);
		wlr_output_state_finish(&state);
	}
	wlr_log(WLR_DEBUG, "idle_power: displays turned off");
}

static int handle_dpms_poll_timer(void *data) {
	(void)data;
	if (!displays_off)
		return 0;

	update_idle_inhibitors(NULL);
	if (is_inhibited()) {
		turn_displays_on();
		return 0;
	}

	wl_event_source_timer_update(dpms_poll_timer, 1000);
	return 0;
}

static int handle_idle_timer(void *data) {
	(void)data;
	if (!settings.idle_dpms || settings.idle_timeout <= 0)
		return 0;

	update_idle_inhibitors(NULL);
	if (is_inhibited())
		return 0;

	turn_displays_off();

	if (dpms_poll_timer)
		wl_event_source_timer_update(dpms_poll_timer, 1000);

	return 0;
}

static void reset_idle_timer(void) {
	if (!idle_timer)
		return;

	if (displays_off)
		turn_displays_on();

	if (settings.idle_dpms && settings.idle_timeout > 0)
		wl_event_source_timer_update(idle_timer, settings.idle_timeout * 1000);
	else
		wl_event_source_timer_update(idle_timer, 0);
}

static struct wl_event_loop *event_loop(void) {
	return wl_display_get_event_loop(server.wl_display);
}

void idle_power_init(void) {
	ONCE();
	idle_timer = wl_event_loop_add_timer(event_loop(), handle_idle_timer, NULL);
	dpms_poll_timer = wl_event_loop_add_timer(event_loop(), handle_dpms_poll_timer, NULL);
	displays_off = false;

	if (settings.idle_dpms && settings.idle_timeout > 0 && idle_timer)
		wl_event_source_timer_update(idle_timer, settings.idle_timeout * 1000);

	wlr_log(WLR_DEBUG, "idle_power: initialized (timeout=%ds, dpms=%s)", settings.idle_timeout,
		settings.idle_dpms ? "on" : "off");
}

void idle_power_fini(void) {
	ONCE();
	turn_displays_on();
	if (idle_timer) {
		wl_event_source_remove(idle_timer);
		idle_timer = NULL;
	}
	if (dpms_poll_timer) {
		wl_event_source_remove(dpms_poll_timer);
		dpms_poll_timer = NULL;
	}
}

void idle_power_notify_activity(void) {
	reset_idle_timer();
}

void idle_power_check_inhibitors(void) {
	if (displays_off && is_inhibited())
		turn_displays_on();
}

void idle_power_reset_timer(void) {
	if (!idle_timer)
		return;

	if (settings.idle_dpms && settings.idle_timeout > 0)
		wl_event_source_timer_update(idle_timer, settings.idle_timeout * 1000);
	else
		wl_event_source_timer_update(idle_timer, 0);
}
