#include "copy_capture.h"
#include "screencopy.h"
#include "server.h"
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

static bool is_privileged(const struct wl_global *global) {
	return global == server.output_manager->global || global == server.output_power_manager->global ||
		global == server.input_method_manager->global || global == server.foreign_toplevel_list->global ||
		global == server.foreign_toplevel_manager->global ||
		global == server.data_control_manager->global ||
		global == server.ext_data_control_manager->global ||
		global == server.export_dmabuf_manager->global || global == server.gamma_control_manager->global ||
		global == server.security_context_manager_v1->global || global == server.layer_shell->global ||
		global == server.session_lock_manager->global ||
		global == server.keyboard_shortcuts_inhibit_manager->global ||
		global == server.virtual_keyboard_manager->global ||
		global == server.virtual_pointer_manager->global || global == server.xdg_output_manager->global ||
		global == server.workspace_manager->global || global == screencopy_get_global() ||
		global == image_copy_capture_get_global() || global == image_capture_source_get_global();
}

static bool filter_global(const struct wl_client *client, const struct wl_global *global,
		void *data) {
	(void)data;
	const struct wlr_security_context_v1_state *security_context =
		wlr_security_context_manager_v1_lookup_client(server.security_context_manager_v1,
		(struct wl_client *)client);

	if (is_privileged(global))
		return security_context == NULL;

	return true;
}

void security_ctx_init(void) {
	server.security_context_manager_v1 = wlr_security_context_manager_v1_create(server.wl_display);
	if (!server.security_context_manager_v1)
		wlr_log(WLR_ERROR, "Failed to create security context manager");
	else
		wl_display_set_global_filter(server.wl_display, filter_global, NULL);
}
