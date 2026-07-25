#include "animation.h"
#include "bezier.h"
#include "config.h"
#include "copy_capture.h"
#include "cursor.h"
#include "effects.h"
#include "idle.h"
#include "idle_power.h"
#include "input.h"
#include "ipc.h"
#include "keyboard.h"
#include "layer.h"
#include "lock.h"
#include "output.h"
#include "output_config.h"
#include "render_unfocused.h"
#include "rule.h"
#include "scratchpad.h"
#include "screencopy.h"
#include "seat.h"
#include "server.h"
#include "spring.h"
#include "toplevel.h"
#include "transaction.h"
#include "workspace.h"
#include "xwayland.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_warp_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

void handle_new_input(struct wl_listener *listener, void *data);
void handle_keyboard_shortcuts_inhibit_new_inhibitor(struct wl_listener *listener, void *data);
void handle_output_power_set_mode(struct wl_listener *listener, void *data);
void handle_output_manager_apply(struct wl_listener *listener, void *data);
void handle_output_manager_test(struct wl_listener *listener, void *data);

static void handle_drm_lease_request(struct wl_listener *listener, void *data);
static void handle_ring_system_bell(struct wl_listener *listener, void *data);
void xdg_toplevel_tag_manager_v1_handle_set_tag(struct wl_listener *listener, void *data);
void xdg_dialog_handle_new(struct wl_listener *listener, void *data);

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

void server_init(void) {
	server = (struct server_t){0};

	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display),
		&server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "Failed to create backend");
		exit(EXIT_FAILURE);
	}

	// headless backend for virtual outputs
	server.headless_backend =
		wlr_headless_backend_create(wl_display_get_event_loop(server.wl_display));
	if (server.headless_backend) {
		wlr_log(WLR_INFO, "Created headless backend for virtual outputs");
		if (wlr_backend_is_multi(server.backend)) {
			wlr_multi_backend_add(server.backend, server.headless_backend);
			server.headless_output_counter = 0;
		}
	}

	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.new_input.notify = handle_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);

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

	// output management
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

	// scene graph
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

	// xdg shell
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 5);
	if (!server.xdg_shell) {
		wlr_log(WLR_ERROR, "Failed to create xdg shell");
		exit(EXIT_FAILURE);
	}

	wl_list_init(&server.toplevels);

	server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

	// xdg toplevel tag
	struct wlr_xdg_toplevel_tag_manager_v1 *xdg_toplevel_tag_manager_v1 =
		wlr_xdg_toplevel_tag_manager_v1_create(server.wl_display, 1);
	if (!xdg_toplevel_tag_manager_v1) {
		wlr_log(WLR_ERROR, "Failed to create xdg toplevel tag manager");
		exit(EXIT_FAILURE);
	}
	server.xdg_toplevel_tag_manager_v1_set_tag.notify = xdg_toplevel_tag_manager_v1_handle_set_tag;
	wl_signal_add(&xdg_toplevel_tag_manager_v1->events.set_tag,
		&server.xdg_toplevel_tag_manager_v1_set_tag);

	// xdg dialog
	struct wlr_xdg_wm_dialog_v1 *xdg_wm_dialog = wlr_xdg_wm_dialog_v1_create(server.wl_display, 1);
	if (!xdg_wm_dialog) {
		wlr_log(WLR_ERROR, "Failed to create xdg wm dialog");
		exit(EXIT_FAILURE);
	}
	server.xdg_dialog_new_dialog.notify = xdg_dialog_handle_new;
	wl_signal_add(&xdg_wm_dialog->events.new_dialog, &server.xdg_dialog_new_dialog);

	// xdg decoration
	server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display, 2);
	if (!server.xdg_decoration_manager) {
		wlr_log(WLR_ERROR, "Failed to create xdg decoration manager");
		exit(EXIT_FAILURE);
	}
	server.new_xdg_decoration.notify = handle_new_xdg_decoration;
	wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration,
		&server.new_xdg_decoration);

	// xdg activation
	wl_list_init(&server.pending_launcher_ctxs);
	server.xdg_activation_v1 = wlr_xdg_activation_v1_create(server.wl_display);
	if (!server.xdg_activation_v1) {
		wlr_log(WLR_ERROR, "Failed to create xdg activation");
		exit(EXIT_FAILURE);
	}
	server.xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
	wl_signal_add(&server.xdg_activation_v1->events.request_activate,
		&server.xdg_activation_request_activate);
	server.xdg_activation_new_token.notify = handle_xdg_activation_new_token;
	wl_signal_add(&server.xdg_activation_v1->events.new_token, &server.xdg_activation_new_token);

	// layer shell
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 5);
	if (!server.layer_shell) {
		wlr_log(WLR_ERROR, "Failed to create layer shell");
		exit(EXIT_FAILURE);
	}

	server.new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

	// cursor
	server.cursor = wlr_cursor_create();
	if (!server.cursor) {
		wlr_log(WLR_ERROR, "Failed to create cursor");
		exit(EXIT_FAILURE);
	}
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	if (!server.cursor_mgr) {
		wlr_log(WLR_ERROR, "Failed to create cursor manager");
		exit(EXIT_FAILURE);
	}
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.last_focused_xwayland_view = NULL;

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);

	server.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);

	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);

	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.cursor_tablet_tool_axis.notify = handle_tablet_tool_axis;
	wl_signal_add(&server.cursor->events.tablet_tool_axis, &server.cursor_tablet_tool_axis);

	server.cursor_tablet_tool_proximity.notify = handle_tablet_tool_proximity;
	wl_signal_add(&server.cursor->events.tablet_tool_proximity, &server.cursor_tablet_tool_proximity);

	server.cursor_tablet_tool_tip.notify = handle_tablet_tool_tip;
	wl_signal_add(&server.cursor->events.tablet_tool_tip, &server.cursor_tablet_tool_tip);

	server.cursor_tablet_tool_button.notify = handle_tablet_tool_button;
	wl_signal_add(&server.cursor->events.tablet_tool_button, &server.cursor_tablet_tool_button);

	cursor_init_gestures();

	// relative pointer
	server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.wl_display);
	if (!server.relative_pointer_manager) {
		wlr_log(WLR_ERROR, "Failed to create relative pointer manager");
		exit(EXIT_FAILURE);
	}

	// pointer constraints
	server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
	if (!server.pointer_constraints) {
		wlr_log(WLR_ERROR, "Failed to create pointer constraints");
		exit(EXIT_FAILURE);
	}

	server.cursor_requires_warp = false;
	wl_list_init(&server.pointer_constraint_commit.link);

	server.new_pointer_constraint.notify = handle_pointer_constraint;
	wl_signal_add(&server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint);

	// pointer warp
	server.pointer_warp_manager = wlr_pointer_warp_v1_create(server.wl_display, 1);
	if (!server.pointer_warp_manager) {
		wlr_log(WLR_ERROR, "Failed to create pointer warp manager");
		exit(EXIT_FAILURE);
	}
	server.pointer_warp.notify = handle_pointer_warp;
	wl_signal_add(&server.pointer_warp_manager->events.warp, &server.pointer_warp);

	// cursor shape
	server.cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
	if (!server.cursor_shape_manager) {
		wlr_log(WLR_ERROR, "Failed to create cursor shape manager");
		exit(EXIT_FAILURE);
	}

	server.cursor_request_set_shape.notify = handle_cursor_request_set_shape;
	wl_signal_add(&server.cursor_shape_manager->events.request_set_shape,
		&server.cursor_request_set_shape);

	// pointer gestures
	server.pointer_gestures = wlr_pointer_gestures_v1_create(server.wl_display);

	// virtual pointer
	server.virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server.wl_display);
	if (!server.virtual_pointer_manager) {
		wlr_log(WLR_ERROR, "Failed to create virtual pointer manager");
		exit(EXIT_FAILURE);
	}

	server.new_virtual_pointer.notify = handle_new_virtual_pointer;
	wl_signal_add(&server.virtual_pointer_manager->events.new_virtual_pointer,
		&server.new_virtual_pointer);

	// virtual keyboard
	server.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	if (!server.virtual_keyboard_manager) {
		wlr_log(WLR_ERROR, "Failed to create virtual keyboard manager");
		exit(EXIT_FAILURE);
	}

	server.new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&server.virtual_keyboard_manager->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);

	// xwayland support
	wl_list_init(&server.xwayland.views);
	server.xwayland.wlr_xwayland = wlr_xwayland_create(server.wl_display, server.compositor, true);
	if (server.xwayland.wlr_xwayland) {
		server.xwayland.xcursor_manager = server.cursor_mgr;

		server.xwayland_surface.notify = handle_xwayland_surface;
		wl_signal_add(&server.xwayland.wlr_xwayland->events.new_surface, &server.xwayland_surface);

		server.xwayland_ready.notify = handle_xwayland_ready;
		wl_signal_add(&server.xwayland.wlr_xwayland->events.ready, &server.xwayland_ready);

		setenv("DISPLAY", server.xwayland.wlr_xwayland->display_name, true);
	}

	// idle notifier
	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);
	if (!server.idle_notifier) {
		wlr_log(WLR_ERROR, "Failed to create idle notifier");
		exit(EXIT_FAILURE);
	}

	// seats
	wl_list_init(&server.seats);
	wl_list_init(&server.keyboards);
	wl_list_init(&server.pointers);
	wl_list_init(&server.touches);
	wl_list_init(&server.keyboard_groups);
	wl_list_init(&server.physical_keyboards);

	// session lock
	server.session_lock_manager = wlr_session_lock_manager_v1_create(server.wl_display);
	if (!server.session_lock_manager) {
		wlr_log(WLR_ERROR, "Failed to create session lock manager");
		exit(EXIT_FAILURE);
	}

	server.new_session_lock.notify = handle_new_session_lock;
	wl_signal_add(&server.session_lock_manager->events.new_lock, &server.new_session_lock);

	server.locked = false;
	server.current_session_lock = NULL;
	const float lockcolor[] = {
		0.1f,
		0.1f,
		0.1f,
		1.0f
	};
	struct wlr_box full_geo = {0};
	wlr_output_layout_get_box(server.output_layout, NULL, &full_geo);
	server.lock_background = wlr_scene_rect_create(server.lock_tree, full_geo.width, full_geo.height,
		lockcolor);
	wlr_scene_node_set_enabled(&server.lock_background->node, false);

	// xdg system bell
	server.xdg_system_bell = wlr_xdg_system_bell_v1_create(server.wl_display, 1);
	if (!server.xdg_system_bell) {
		wlr_log(WLR_ERROR, "Failed to create xdg system bell");
		exit(EXIT_FAILURE);
	}
	server.ring_system_bell.notify = handle_ring_system_bell;
	wl_signal_add(&server.xdg_system_bell->events.ring, &server.ring_system_bell);

	// idle inhibitor
	server.idle_inhibit_manager = wlr_idle_inhibit_v1_create(server.wl_display);
	if (!server.idle_inhibit_manager) {
		wlr_log(WLR_ERROR, "Failed to create idle inhibit manager");
		exit(EXIT_FAILURE);
	}
	server.new_idle_inhibitor.notify = handle_new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit_manager->events.new_inhibitor, &server.new_idle_inhibitor);

	// content type manager
	server.content_type_manager = wlr_content_type_manager_v1_create(server.wl_display, 1);
	if (!server.content_type_manager) {
		wlr_log(WLR_ERROR, "Failed to create content type manager");
		exit(EXIT_FAILURE);
	}

	// keyboard shortcuts inhibitor
	server.keyboard_shortcuts_inhibit_manager =
		wlr_keyboard_shortcuts_inhibit_v1_create(server.wl_display);
	if (!server.keyboard_shortcuts_inhibit_manager) {
		wlr_log(WLR_ERROR, "Failed to create keyboard shortcuts inhibit manager");
		exit(EXIT_FAILURE);
	}
	server.keyboard_shortcuts_inhibit_new_inhibitor.notify =
		handle_keyboard_shortcuts_inhibit_new_inhibitor;
	wl_signal_add(&server.keyboard_shortcuts_inhibit_manager->events.new_inhibitor,
		&server.keyboard_shortcuts_inhibit_new_inhibitor);

	// drm lease
#if WLR_HAS_DRM_BACKEND
	server.drm_lease_manager = wlr_drm_lease_v1_manager_create(server.wl_display, server.backend);
	if (server.drm_lease_manager) {
		server.drm_lease_request.notify = handle_drm_lease_request;
		wl_signal_add(&server.drm_lease_manager->events.request, &server.drm_lease_request);
	} else {
		wlr_log(WLR_ERROR, "failed to create drm lease manager");
	}
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

	// foreign toplevel list
	server.foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server.wl_display, 1);
	if (!server.foreign_toplevel_list) {
		wlr_log(WLR_ERROR, "Failed to create foreign toplevel list");
		exit(EXIT_FAILURE);
	}

	// foreign toplevel manager
	server.foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server.wl_display);
	if (!server.foreign_toplevel_manager) {
		wlr_log(WLR_ERROR, "Failed to create foreign toplevel manager");
		exit(EXIT_FAILURE);
	}

	// foreign toplevel image capture source
	server.foreign_toplevel_image_capture_source_manager =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server.wl_display, 1);
	if (!server.foreign_toplevel_image_capture_source_manager) {
		wlr_log(WLR_ERROR, "Failed to create foreign toplevel image capture source manager");
		exit(EXIT_FAILURE);
	}

	server.new_toplevel_capture_request.notify = handle_new_toplevel_capture_request;
	wl_signal_add(&server.foreign_toplevel_image_capture_source_manager->events.capture_request,
		&server.new_toplevel_capture_request);

	// xdg foreign
	struct wlr_xdg_foreign_registry *xdg_foreign_registry =
		wlr_xdg_foreign_registry_create(server.wl_display);
	wlr_xdg_foreign_v1_create(server.wl_display, xdg_foreign_registry);
	wlr_xdg_foreign_v2_create(server.wl_display, xdg_foreign_registry);

	// single pixel buffer
	wlr_single_pixel_buffer_manager_v1_create(server.wl_display);

	// screencopy
	screencopy_init();

	// viewporter
	wlr_viewporter_create(server.wl_display);

	// fractional scale
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);

	// presentation
	wlr_presentation_create(server.wl_display, server.backend, 2);

	// export dmabuf
	server.export_dmabuf_manager = wlr_export_dmabuf_manager_v1_create(server.wl_display);
	if (!server.export_dmabuf_manager) {
		wlr_log(WLR_ERROR, "Failed to create export dmabuf manager");
		exit(EXIT_FAILURE);
	}

	// wlr data control
	server.data_control_manager = wlr_data_control_manager_v1_create(server.wl_display);
	if (!server.data_control_manager) {
		wlr_log(WLR_ERROR, "Failed to create data control manager");
		exit(EXIT_FAILURE);
	}

	// ext data control
	server.ext_data_control_manager = wlr_ext_data_control_manager_v1_create(server.wl_display, 1);
	if (!server.ext_data_control_manager) {
		wlr_log(WLR_ERROR, "Failed to create ext data control manager");
		exit(EXIT_FAILURE);
	}

	// gamma control
	server.gamma_control_manager = wlr_gamma_control_manager_v1_create(server.wl_display);
	if (!server.gamma_control_manager) {
		wlr_log(WLR_ERROR, "Failed to create gamma control manager");
		exit(EXIT_FAILURE);
	}
	wlr_scene_set_gamma_control_manager_v1(server.scene, server.gamma_control_manager);

	// image copy capture
	image_copy_capture_init();

	// alpha modifier
	wlr_alpha_modifier_v1_create(server.wl_display);

	// fixes
	wlr_fixes_create(server.wl_display, 1);

	// tearing control
	server.tearing_control_v1 = wlr_tearing_control_manager_v1_create(server.wl_display, 1);
	if (!server.tearing_control_v1) {
		wlr_log(WLR_ERROR, "Failed to create tearing control manager");
		exit(EXIT_FAILURE);
	}
	server.tearing_control_new_object.notify = handle_new_tearing_hint;
	wl_list_init(&server.tearing_controllers);
	wl_signal_add(&server.tearing_control_v1->events.new_object, &server.tearing_control_new_object);

	// tablet support
	server.tablet_v2 = wlr_tablet_v2_create(server.wl_display);
	if (!server.tablet_v2) {
		wlr_log(WLR_ERROR, "Failed to create tablet v2");
		exit(EXIT_FAILURE);
	}

	// input method support
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

	// default seat
	seat_t *default_seat = seat_create("seat0");
	server.seat = default_seat ? default_seat->wlr_seat : NULL;

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

	// security context manager
	server.security_context_manager_v1 = wlr_security_context_manager_v1_create(server.wl_display);
	if (!server.security_context_manager_v1) {
		wlr_log(WLR_ERROR, "Failed to create security context manager");
	} else {
		wl_display_set_global_filter(server.wl_display, filter_global, NULL);
	}
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

void handle_keyboard_shortcuts_inhibit_new_inhibitor(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
	const char *app_id = NULL;
	const char *title = NULL;
	const char *tag = NULL;

	struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(inhibitor->surface);
	if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		app_id = xdg_surface->toplevel->app_id;
		title = xdg_surface->toplevel->title;
		toplevel_t *tl = xdg_surface->toplevel->base->data;
		if (tl)
			tag = tl->tag;
	} else {
		struct wlr_xwayland_surface *xwayland_surface =
			wlr_xwayland_surface_try_from_wlr_surface(inhibitor->surface);
		if (xwayland_surface) {
			app_id = xwayland_surface->class;
			title = xwayland_surface->title;
		}
	}

	bool allow = true;
	if (app_id || title || tag) {
		rule_consequence_t *rule = find_matching_rule(app_id, title, tag);
		if (rule && rule->has & RULE_TYPE_SHORTCUTS_INHIBITOR)
			allow = rule->flags & RULE_TYPE_SHORTCUTS_INHIBITOR;
	}

	if (allow)
		wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
}

void handle_output_power_set_mode(struct wl_listener *listener, void *data) {
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

void handle_output_manager_apply(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_output_configuration_v1 *config = data;

	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		apply_output_head_config(head);
	}

	wlr_output_configuration_v1_send_succeeded(config);
	output_update_manager_config();
}

void handle_output_manager_test(struct wl_listener *listener, void *data) {
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
	ipc_cleanup();

	wl_display_terminate(server.wl_display);

	if (fork() == 0)
		execl("/bin/sh", "/bin/sh", "doors", (char *)NULL);
}

void server_fini(void) {
	if (server.xwayland.wlr_xwayland) {
		wl_list_remove(&server.xwayland_ready.link);
		wl_list_remove(&server.xwayland_surface.link);
		wlr_xwayland_destroy(server.xwayland.wlr_xwayland);
		server.xwayland.wlr_xwayland = NULL;
	}

	screencopy_fini();
	image_copy_capture_fini();
	render_unfocused_fini();
	idle_power_fini();
	animation_fini();
	transaction_fini();
	scratchpad_fini();
	workspace_fini();
	if (server.ipc_event_source) {
		wl_event_source_remove(server.ipc_event_source);
		server.ipc_event_source = NULL;
	}
	ipc_cleanup();
	rule_fini();
	output_config_fini();
	input_fini();
	config_fini();
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_output.link);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_decoration.link);

	wl_list_remove(&server.new_layer_surface.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.cursor_request_set_shape.link);

	wl_list_remove(&server.cursor_tablet_tool_axis.link);
	wl_list_remove(&server.cursor_tablet_tool_proximity.link);
	wl_list_remove(&server.cursor_tablet_tool_tip.link);
	wl_list_remove(&server.cursor_tablet_tool_button.link);

	wl_list_remove(&server.new_pointer_constraint.link);
	wl_list_remove(&server.pointer_warp.link);

	wl_list_remove(&server.swipe_begin.link);
	wl_list_remove(&server.swipe_update.link);
	wl_list_remove(&server.swipe_end.link);
	wl_list_remove(&server.pinch_begin.link);
	wl_list_remove(&server.pinch_update.link);
	wl_list_remove(&server.pinch_end.link);
	wl_list_remove(&server.hold_begin.link);
	wl_list_remove(&server.hold_end.link);

	wl_list_remove(&server.tearing_control_new_object.link);

	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.new_virtual_keyboard.link);

	wl_list_remove(&server.new_session_lock.link);

	wl_list_remove(&server.xdg_activation_request_activate.link);
	wl_list_remove(&server.xdg_activation_new_token.link);

	launcher_ctx_t *ctx, *tmp;
	wl_list_for_each_safe(ctx, tmp, &server.pending_launcher_ctxs, link)
		launcher_ctx_destroy(ctx);

	wl_list_remove(&server.output_power_set_mode.link);

	wl_list_remove(&server.new_idle_inhibitor.link);

	wl_list_remove(&server.keyboard_shortcuts_inhibit_new_inhibitor.link);

	wl_list_remove(&server.output_manager_apply.link);
	wl_list_remove(&server.output_manager_test.link);

	wl_list_remove(&server.new_toplevel_capture_request.link);

	wl_list_remove(&server.ring_system_bell.link);

	wl_list_remove(&server.xdg_toplevel_tag_manager_v1_set_tag.link);
	wl_list_remove(&server.xdg_dialog_new_dialog.link);

#ifdef WLR_HAS_DRM_BACKEND
	if (server.drm_lease_manager)
		wl_list_remove(&server.drm_lease_request.link);
#endif

	wlr_backend_destroy(server.backend);

	wl_event_loop_dispatch_idle(wl_display_get_event_loop(server.wl_display));

	seat_t *seat, *tmp_seat;
	wl_list_for_each_safe(seat, tmp_seat, &server.seats, link)
		seat_destroy(seat);

	effects_fini();

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_cursor_destroy(server.cursor);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);

	wl_display_destroy(server.wl_display);
}

static int handle_system_bell_timer(void *data) {
	(void)data;
	server.system_bell_timer = NULL;
	return 0;
}

static void handle_ring_system_bell(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;

	if (server.system_bell_timer != NULL)
		return;

	server.system_bell_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server.wl_display),
		handle_system_bell_timer, NULL);
	wl_event_source_timer_update(server.system_bell_timer, 100);

	execute_bell_bind();
}


#if WLR_HAS_DRM_BACKEND
static void handle_drm_lease_request(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_drm_lease_request_v1 *req = data;
	struct wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(req);
	if (!lease) {
		wlr_log(WLR_ERROR, "Failed to grant lease request");
		wlr_drm_lease_request_v1_reject(req);
	}
}


#endif
