#include "animation.h"
#include "backend.h"
#include "bell.h"
#include "bezier.h"
#include "config.h"
#include "copy_capture.h"
#include "cursor.h"
#include "cursor_shape.h"
#include "dialog.h"
#include "drm_lease.h"
#include "effects.h"
#include "foreign_capture.h"
#include "global_shortcuts.h"
#include "idle.h"
#include "idle_power.h"
#include "input.h"
#include "input_method.h"
#include "ipc.h"
#include "launcher.h"
#include "layer.h"
#include "lock.h"
#include "output_config.h"
#include "output_mgmt.h"
#include "pointer_constraint.h"
#include "pointer_warp.h"
#include "render_unfocused.h"
#include "rule.h"
#include "scratchpad.h"
#include "screencopy.h"
#include "seat.h"
#include "security_ctx.h"
#include "server.h"
#include "shortcuts_inhibit.h"
#include "spring.h"
#include "tearing.h"
#include "toplevel.h"
#include "toplevel_tag.h"
#include "transaction.h"
#include "virtual_keyboard.h"
#include "virtual_pointer.h"
#include "workspace.h"
#include "xdg_decoration.h"
#include "xdg_shell.h"
#include "xwayland.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

void server_init(void) {
	server = (struct server_t){0};

	server.wl_display = wl_display_create();

	backend_init();

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		exit(EXIT_FAILURE);
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "Failed to create allocator");
		exit(EXIT_FAILURE);
	}

	effects_init();

	server.bg_effect_manager = wlr_ext_background_effect_manager_v1_create(server.wl_display, 1,
		EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);

	server.compositor = wlr_compositor_create(server.wl_display, 6, server.renderer);
	if (!server.compositor) {
		wlr_log(WLR_ERROR, "Failed to create compositor");
		exit(EXIT_FAILURE);
	}
	wlr_subcompositor_create(server.wl_display);

	// dmabuf support
	if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(server.wl_display, server.renderer);
		server.linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 4,
			server.renderer);
		server.export_dmabuf_manager = wlr_export_dmabuf_manager_v1_create(server.wl_display);
	}

	// drm syncobj
	if (wlr_renderer_get_drm_fd(server.renderer) >= 0 && server.renderer->features.timeline &&
			server.backend->features.timeline) {
		wlr_linux_drm_syncobj_manager_v1_create(server.wl_display, 1,
			wlr_renderer_get_drm_fd(server.renderer));
	}

	// data device
	wlr_data_device_manager_create(server.wl_display);

	// primary selection
	const char *disable_primary_selection = getenv("DOORS_DISABLE_PRIMARY_SELECTION");
	if (disable_primary_selection == NULL)
		wlr_primary_selection_v1_device_manager_create(server.wl_display);

	// input devices
	wl_list_init(&server.seats);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.pointers);
	wl_list_init(&server.touches);
	wl_list_init(&server.keyboard_groups);
	wl_list_init(&server.physical_keyboards);

	output_mgmt_init();

	// scene
	server.scene = wlr_scene_create();
	if (!server.scene) {
		wlr_log(WLR_ERROR, "Failed to create scene");
		exit(EXIT_FAILURE);
	}
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	if (server.linux_dmabuf)
		wlr_scene_set_linux_dmabuf_v1(server.scene, server.linux_dmabuf);

	// scene trees for layering
	server.bg_tree = wlr_scene_tree_create(&server.scene->tree);
	server.bot_tree = wlr_scene_tree_create(&server.scene->tree);
	server.tile_tree = wlr_scene_tree_create(&server.scene->tree);
	server.float_tree = wlr_scene_tree_create(&server.scene->tree);
	server.top_tree = wlr_scene_tree_create(&server.scene->tree);
	server.full_tree = wlr_scene_tree_create(&server.scene->tree);
	server.over_tree = wlr_scene_tree_create(&server.scene->tree);
	server.shader_tree = wlr_scene_tree_create(&server.scene->tree);
	server.drag_tree = wlr_scene_tree_create(&server.scene->tree);
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);

	xdg_shell_init();
	xdg_decoration_init();
	dialog_init();
	toplevel_tag_init();
	launcher_init();
	layer_init();
	cursor_init();
	pointer_constraint_init();
	pointer_warp_init();
	cursor_shape_init();
	virtual_keyboard_init();
	virtual_pointer_init();
	shortcuts_inhibit_init();
	bell_init();
	idle_init();
	session_lock_init();
	xwayland_init();

	server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.wl_display);
	if (!server.relative_pointer_manager) {
		wlr_log(WLR_ERROR, "Failed to create relative pointer manager");
		exit(EXIT_FAILURE);
	}

	server.pointer_gestures = wlr_pointer_gestures_v1_create(server.wl_display);
	if (!server.pointer_gestures) {
		wlr_log(WLR_ERROR, "Failed to create pointer gestures");
		exit(EXIT_FAILURE);
	}

	// idle notifier
	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);
	if (!server.idle_notifier) {
		wlr_log(WLR_ERROR, "Failed to create idle notifier");
		exit(EXIT_FAILURE);
	}

	// content type manager
	server.content_type_manager = wlr_content_type_manager_v1_create(server.wl_display, 1);
	if (!server.content_type_manager) {
		wlr_log(WLR_ERROR, "Failed to create content type manager");
		exit(EXIT_FAILURE);
	}

	// drm lease
#if WLR_HAS_DRM_BACKEND
	drm_lease_init();
#endif

	// color manager
	if (server.renderer->features.input_color_transform) {
		const enum wp_color_manager_v1_render_intent render_intents[] = {
			WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
		};
		size_t transfer_functions_len = 0, primaries_len = 0;
		enum wp_color_manager_v1_transfer_function *transfer_functions =
			wlr_color_manager_v1_transfer_function_list_from_renderer(server.renderer,
			&transfer_functions_len);
		enum wp_color_manager_v1_primaries *primaries =
			wlr_color_manager_v1_primaries_list_from_renderer(server.renderer, &primaries_len);
		struct wlr_color_manager_v1 *cm = wlr_color_manager_v1_create(server.wl_display, 2,
				&(struct wlr_color_manager_v1_options){
			.features = {
				.parametric = true,
				.set_mastering_display_primaries = true,
			},
			.render_intents = render_intents,
			.render_intents_len = sizeof(render_intents) / sizeof(render_intents[0]),
			.transfer_functions = transfer_functions,
			.transfer_functions_len = transfer_functions_len,
			.primaries = primaries,
			.primaries_len = primaries_len,
		});
		free(transfer_functions);
		free(primaries);
		wlr_scene_set_color_manager_v1(server.scene, cm);
	}

	// color representation
	enum wp_color_representation_surface_v1_alpha_mode color_representation_alpha_modes[] =
		{WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT};
	const struct wlr_color_representation_v1_coeffs_and_range color_representation_coeffs_and_range[] =
			{
		{WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
			WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL}
	};
	const struct wlr_color_representation_v1_options color_representation_options = {
		color_representation_alpha_modes,
		sizeof(color_representation_alpha_modes) / sizeof(color_representation_alpha_modes[0]),
		color_representation_coeffs_and_range,
		sizeof(color_representation_coeffs_and_range) / sizeof(color_representation_coeffs_and_range[0]),
	};
	wlr_color_representation_manager_v1_create(server.wl_display, 1, &color_representation_options);

	server.foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server.wl_display, 1);
	if (!server.foreign_toplevel_list) {
		wlr_log(WLR_ERROR, "Failed to create foreign toplevel list");
		exit(EXIT_FAILURE);
	}

	server.foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server.wl_display);
	if (!server.foreign_toplevel_manager) {
		wlr_log(WLR_ERROR, "Failed to create foreign toplevel manager");
		exit(EXIT_FAILURE);
	}

	server.export_dmabuf_manager = wlr_export_dmabuf_manager_v1_create(server.wl_display);
	if (!server.export_dmabuf_manager) {
		wlr_log(WLR_ERROR, "Failed to create export dmabuf manager");
		exit(EXIT_FAILURE);
	}

	server.data_control_manager = wlr_data_control_manager_v1_create(server.wl_display);
	if (!server.data_control_manager) {
		wlr_log(WLR_ERROR, "Failed to create data control manager");
		exit(EXIT_FAILURE);
	}

	server.ext_data_control_manager = wlr_ext_data_control_manager_v1_create(server.wl_display, 1);
	if (!server.ext_data_control_manager) {
		wlr_log(WLR_ERROR, "Failed to create ext data control manager");
		exit(EXIT_FAILURE);
	}

	server.gamma_control_manager = wlr_gamma_control_manager_v1_create(server.wl_display);
	if (!server.gamma_control_manager) {
		wlr_log(WLR_ERROR, "Failed to create gamma control manager");
		exit(EXIT_FAILURE);
	}
	wlr_scene_set_gamma_control_manager_v1(server.scene, server.gamma_control_manager);

	server.tablet_v2 = wlr_tablet_v2_create(server.wl_display);
	if (!server.tablet_v2) {
		wlr_log(WLR_ERROR, "Failed to create tablet v2");
		exit(EXIT_FAILURE);
	}

	server.input_method_manager = wlr_input_method_manager_v2_create(server.wl_display);
	if (!server.input_method_manager) {
		wlr_log(WLR_ERROR, "Failed to create input method manager");
		exit(EXIT_FAILURE);
	}
	server.text_input_manager = wlr_text_input_manager_v3_create(server.wl_display);
	if (!server.text_input_manager) {
		wlr_log(WLR_ERROR, "Failed to create text input manager");
		exit(EXIT_FAILURE);
	}

	wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
	wlr_viewporter_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_presentation_create(server.wl_display, server.backend, 2);
	wlr_alpha_modifier_v1_create(server.wl_display);
	wlr_fixes_create(server.wl_display, 1);

	struct wlr_xdg_foreign_registry *xdg_foreign_registry =
		wlr_xdg_foreign_registry_create(server.wl_display);
	wlr_xdg_foreign_v1_create(server.wl_display, xdg_foreign_registry);
	wlr_xdg_foreign_v2_create(server.wl_display, xdg_foreign_registry);

	// default seat
	seat_t *default_seat = seat_create("seat0");
	server.seat = default_seat ? default_seat->wlr_seat : NULL;

	foreign_capture_init();
	tearing_init();
	screencopy_init();
	image_copy_capture_init();
	transaction_init();
	animation_init();
	bezier_init();
	spring_init();
	scratchpad_init();
	workspace_init();
	render_unfocused_init();
	idle_power_init();
	ipc_init();
	rule_init();
	output_config_init();
	input_init();
	global_shortcuts_init();
	security_ctx_init();
}

static int ipc_socket_handler(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)data;
	if (mask & WL_EVENT_READABLE) {
		int client_fd = accept(ipc_get_socket_fd(), NULL, NULL);
		if (client_fd >= 0)
			ipc_handle_incoming(client_fd);
	}
	return 0;
}

int server_run(void) {
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "doors", true);

	// add IPC socket to event loop
	struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
	int ipc_fd = ipc_get_socket_fd();
	if (ipc_fd >= 0)
		server.ipc_event_source = wl_event_loop_add_fd(event_loop, ipc_fd, WL_EVENT_READABLE,
			ipc_socket_handler, NULL);

	// setup hotkey event listener
	setup_hotkey_event_listener(event_loop);

	// setup config watcher
	wl_event_loop_add_idle(event_loop, run_config_idle, NULL);
	wl_event_loop_add_idle(event_loop, load_hotkeys_idle, event_loop);

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.wl_display);
	return 0;
}

// this will probably not work
void server_restart(void) {
	ipc_fini();

	wl_display_terminate(server.wl_display);

	if (fork() == 0)
		execl("/bin/sh", "/bin/sh", "doors", (char *)NULL);
}

void server_fini(void) {
	xwayland_fini();
	screencopy_fini();
	image_copy_capture_fini();
	render_unfocused_fini();
	idle_power_fini();
	animation_fini();
	transaction_fini();
	scratchpad_fini();
	workspace_fini();
	ipc_fini();
	rule_fini();
	output_config_fini();
	input_fini();
	global_shortcuts_fini();
	config_fini();

	wl_display_destroy_clients(server.wl_display);
	backend_fini();

	xdg_shell_fini();
	xdg_decoration_fini();
	layer_fini();
	pointer_constraint_fini();
	pointer_warp_fini();
	virtual_pointer_fini();
	virtual_keyboard_fini();
	tearing_fini();
	session_lock_fini();
	launcher_fini();
	output_mgmt_fini();
	toplevel_tag_fini();
	shortcuts_inhibit_fini();
	dialog_fini();
	idle_fini();
	bell_fini();
	cursor_shape_fini();
	foreign_capture_fini();

#ifdef WLR_HAS_DRM_BACKEND
	drm_lease_fini();
#endif

	wlr_backend_destroy(server.backend);

	wl_event_loop_dispatch_idle(wl_display_get_event_loop(server.wl_display));

	seat_t *seat, *tmp_seat;
	wl_list_for_each_safe(seat, tmp_seat, &server.seats, link)
		seat_destroy(seat);

	effects_fini();

	wlr_scene_node_destroy(&server.scene->tree.node);

	cursor_fini();
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);

	wl_display_destroy(server.wl_display);
}
