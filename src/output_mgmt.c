#include "once.h"
#include "output.h"
#include "output_config.h"
#include "output_mgmt.h"
#include "server.h"
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

static void handle_output_power_set_mode(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_output_power_v1_set_mode_event *event = data;
	output_set_power(event->output, event->mode);
}

static void build_output_state_from_head(struct wlr_output_configuration_head_v1 *head,
		struct wlr_output_state *state) {
	wlr_output_state_init(state);
	wlr_output_state_set_enabled(state, head->state.enabled);
	if (head->state.mode) {
		wlr_output_state_set_mode(state, head->state.mode);
	} else if (head->state.custom_mode.width > 0) {
		wlr_output_state_set_custom_mode(state, head->state.custom_mode.width,
			head->state.custom_mode.height, head->state.custom_mode.refresh);
	}
	wlr_output_state_set_scale(state, head->state.scale);
	wlr_output_state_set_transform(state, head->state.transform);
	wlr_output_state_set_adaptive_sync_enabled(state, head->state.adaptive_sync_enabled);
}

static void apply_output_head_config(struct wlr_output_configuration_head_v1 *config_head) {
	struct wlr_output *output = config_head->state.output;
	if (!output)
		return;

	output_t *out = output_from_wlr_output(output);

	struct wlr_output_state state;
	build_output_state_from_head(config_head, &state);

	wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);

	if (out)
		output_update_scale(out, config_head->state.scale);

	if (config_head->state.x >= 0 && config_head->state.y >= 0)
		wlr_output_layout_add(server.output_layout, output, config_head->state.x, config_head->state.y);
}

static void handle_output_manager_apply(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_output_configuration_v1 *config = data;

	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		apply_output_head_config(head);
	}

	wlr_output_configuration_v1_send_succeeded(config);
	output_update_manager_config();
}

static void handle_output_manager_test(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_output_configuration_v1 *config = data;

	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *output = head->state.output;
		if (!output)
			continue;

		struct wlr_output_state state;
		build_output_state_from_head(head, &state);

		if (!wlr_output_test_state(output, &state)) {
			wlr_output_configuration_v1_send_failed(config);
			wlr_output_state_finish(&state);
			return;
		}

		wlr_output_state_finish(&state);
	}

	wlr_output_configuration_v1_send_succeeded(config);
}

void output_mgmt_init(void) {
	ONCE();
	server.output_layout = wlr_output_layout_create(server.wl_display);
	if (!server.output_layout) {
		wlr_log(WLR_ERROR, "Failed to create output layout");
		exit(EXIT_FAILURE);
	}

	server.xdg_output_manager = wlr_xdg_output_manager_v1_create(server.wl_display,
		server.output_layout);
	if (!server.xdg_output_manager) {
		wlr_log(WLR_ERROR, "Failed to create xdg output manager");
		exit(EXIT_FAILURE);
	}

	server.output_power_manager = wlr_output_power_manager_v1_create(server.wl_display);
	if (!server.output_power_manager) {
		wlr_log(WLR_ERROR, "Failed to create output power manager");
		exit(EXIT_FAILURE);
	}
	server.output_power_set_mode.notify = handle_output_power_set_mode;
	wl_signal_add(&server.output_power_manager->events.set_mode, &server.output_power_set_mode);

	server.output_manager = wlr_output_manager_v1_create(server.wl_display);
	if (!server.output_manager) {
		wlr_log(WLR_ERROR, "Failed to create output manager");
		exit(EXIT_FAILURE);
	}
	server.output_manager_apply.notify = handle_output_manager_apply;
	wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
	server.output_manager_test.notify = handle_output_manager_test;
	wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);
}

void output_mgmt_fini(void) {
	ONCE();
	wl_list_remove(&server.output_power_set_mode.link);
	wl_list_remove(&server.output_manager_apply.link);
	wl_list_remove(&server.output_manager_test.link);
}
