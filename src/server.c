#include "server.h"
#include "cursor.h"
#include "output.h"
#include "toplevel.h"
#include "types.h"
#include "transaction.h"
#include "workspace.h"
#include "ipc.h"
#include "layer.h"
#include "config.h"
#include "lock.h"
#include "output_config.h"
#include "input.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>

void handle_request_start_drag(struct wl_listener *listener, void *data);
void handle_start_drag(struct wl_listener *listener, void *data);
void handle_drag_icon_destroy(struct wl_listener *listener, void *data);
void request_set_selection(struct wl_listener *listener, void *data);
void handle_new_input(struct wl_listener *listener, void *data);
void handle_output_power_set_mode(struct wl_listener *listener, void *data);
void handle_output_manager_apply(struct wl_listener *listener, void *data);
void handle_output_manager_test(struct wl_listener *listener, void *data);

void server_init(void) {
  server = (struct bwm_server){0};

  server.wl_display = wl_display_create();
  server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), &server.session);
  if (server.backend == NULL) {
    wlr_log(WLR_ERROR, "Failed to create backend");
    exit(EXIT_FAILURE);
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

  wlr_compositor_create(server.wl_display, 5, server.renderer);
  wlr_subcompositor_create(server.wl_display);

  // dmabuf support
  if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF)) {
  	wlr_drm_create(server.wl_display, server.renderer);
    server.linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 4, server.renderer);
    wlr_export_dmabuf_manager_v1_create(server.wl_display);
  }

  wlr_data_device_manager_create(server.wl_display);
  wlr_primary_selection_v1_device_manager_create(server.wl_display);

  server.output_layout = wlr_output_layout_create(server.wl_display);
  wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

  server.output_power_manager = wlr_output_power_manager_v1_create(server.wl_display);
  server.output_power_set_mode.notify = handle_output_power_set_mode;
  wl_signal_add(&server.output_power_manager->events.set_mode, &server.output_power_set_mode);

  server.output_manager = wlr_output_manager_v1_create(server.wl_display);
  server.output_manager_apply.notify = handle_output_manager_apply;
  wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
  server.output_manager_test.notify = handle_output_manager_test;
  wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);

  wl_list_init(&server.outputs);

  // scene graph
  server.scene = wlr_scene_create();
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
	server.drag_tree = wlr_scene_tree_create(&server.scene->tree);
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);

  // xdg shell
  server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 5);
  wl_list_init(&server.toplevels);

  server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
  wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

  // layer shell
  server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 5);

  server.new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

  // cursor
  server.cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

  server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
  wlr_xcursor_manager_load(server.cursor_mgr, 1);

  server.cursor_mode = CURSOR_PASSTHROUGH;

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

  // relative pointer
  server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.wl_display);

  // pointer constraints
  server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);

  server.cursor_requires_warp = false;
  wl_list_init(&server.pointer_constraint_commit.link);

  server.new_pointer_constraint.notify = handle_pointer_constraint;
  wl_signal_add(&server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint);

  // cursor shape
  server.cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);

  server.cursor_request_set_shape.notify = handle_cursor_request_set_shape;
  wl_signal_add(&server.cursor_shape_manager->events.request_set_shape, &server.cursor_request_set_shape);

  // idle notifier
  server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);

  // seat
  server.seat = wlr_seat_create(server.wl_display, "seat0");
  wl_list_init(&server.keyboards);

  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  wlr_seat_set_capabilities(server.seat, caps);

  server.request_cursor.notify = request_cursor;
  wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

  server.pointer_focus_change.notify = seat_pointer_focus_change;
  wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.pointer_focus_change);

  server.request_set_selection.notify = request_set_selection;
  wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

  server.request_start_drag.notify = handle_request_start_drag;
  wl_signal_add(&server.seat->events.request_start_drag, &server.request_start_drag);

  server.start_drag.notify = handle_start_drag;
  wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

  // session lock
  server.session_lock_manager = wlr_session_lock_manager_v1_create(server.wl_display);

  server.new_session_lock.notify = handle_new_session_lock;
  wl_signal_add(&server.session_lock_manager->events.new_lock, &server.new_session_lock);

  server.locked = false;
  server.current_session_lock = NULL;
  const float lockcolor[] = {0.1f, 0.1f, 0.1f, 1.0f};
  struct wlr_box full_geo = {0};
  wlr_output_layout_get_box(server.output_layout, NULL, &full_geo);
  server.lock_background = wlr_scene_rect_create(server.lock_tree, full_geo.width, full_geo.height, lockcolor);
  wlr_scene_node_set_enabled(&server.lock_background->node, false);

  // color manager
  if (server.renderer->features.input_color_transform) {
		const enum wp_color_manager_v1_render_intent render_intents[] = {
			WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
		};
		const enum wp_color_manager_v1_transfer_function transfer_functions[] = {
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886,
		};
		const enum wp_color_manager_v1_primaries primaries[] = {
			WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
			WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
		};
		struct wlr_color_manager_v1 *cm = wlr_color_manager_v1_create(
				server.wl_display, 1, &(struct wlr_color_manager_v1_options){
			.features = {
				.parametric = true,
				.set_mastering_display_primaries = true,
			},
			.render_intents = render_intents,
			.render_intents_len = sizeof(render_intents) / sizeof(render_intents[0]),
			.transfer_functions = transfer_functions,
			.transfer_functions_len = sizeof(transfer_functions) / sizeof(transfer_functions[0]),
			.primaries = primaries,
			.primaries_len = sizeof(primaries) / sizeof(primaries[0]),
		});
		wlr_scene_set_color_manager_v1(server.scene, cm);
	}

  // color representation
  enum wp_color_representation_surface_v1_alpha_mode
    color_representation_alpha_modes[] = {WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT};
  const struct wlr_color_representation_v1_coeffs_and_range
    color_representation_coeffs_and_range[] = {
    {
      WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
      WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL}
    };
  const struct wlr_color_representation_v1_options color_representation_options = {
    color_representation_alpha_modes,
    sizeof(color_representation_alpha_modes) /
        sizeof(color_representation_alpha_modes[0]),
    color_representation_coeffs_and_range,
    sizeof(color_representation_coeffs_and_range) /
        sizeof(color_representation_coeffs_and_range[0]),
  };
  wlr_color_representation_manager_v1_create(server.wl_display, 1, &color_representation_options);

  // foreign toplevel list
  server.foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server.wl_display, 1);

  // foreign toplevel manager
  server.foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server.wl_display);

  // xdg foreign
  struct wlr_xdg_foreign_registry *xdg_foreign_registry = wlr_xdg_foreign_registry_create(server.wl_display);
  wlr_xdg_foreign_v1_create(server.wl_display, xdg_foreign_registry);
  wlr_xdg_foreign_v2_create(server.wl_display, xdg_foreign_registry);

  // single pixel buffer
  wlr_single_pixel_buffer_manager_v1_create(server.wl_display);

  // screencopy
  wlr_screencopy_manager_v1_create(server.wl_display);

  // viewporter
  wlr_viewporter_create(server.wl_display);

  // presentation
  wlr_presentation_create(server.wl_display, server.backend, 2);

  // export dmabuf
  wlr_export_dmabuf_manager_v1_create(server.wl_display);

  // ext data control
  wlr_ext_data_control_manager_v1_create(server.wl_display, 1);

  // gamma control
  wlr_scene_set_gamma_control_manager_v1(server.scene, wlr_gamma_control_manager_v1_create(server.wl_display));

  // image copy capture
  wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);
  wlr_ext_output_image_capture_source_manager_v1_create(server.wl_display, 1);

  // alpha modifier
  wlr_alpha_modifier_v1_create(server.wl_display);

  // fixes
  wlr_fixes_create(server.wl_display, 1);

  // init desktop structure
  monitor_t *m = (monitor_t *)calloc(1, sizeof(monitor_t));
  m->id = next_monitor_id++;
  strncpy(m->name, "default", SMALEN - 1);
  m->wired = true;
  m->window_gap = window_gap;
  m->border_width = border_width;
  m->padding = (padding_t){0};
  m->rectangle = (struct wlr_box){0, 0, 1920, 1080};

  // create default desktop
  desktop_t *d = (desktop_t *)calloc(1, sizeof(desktop_t));
  d->id = next_desktop_id++;
  strncpy(d->name, "1", SMALEN - 1);
  d->layout = LAYOUT_TILED;
  d->user_layout = LAYOUT_TILED;
  d->window_gap = window_gap;
  d->border_width = border_width;
  d->padding = (padding_t){0};
  d->root = NULL;
  d->focus = NULL;

  m->desk = d;
  m->desk_head = d;
  m->desk_tail = d;
  d->monitor = m;

  mon = m;
  mon_head = m;
  mon_tail = m;

  server.monitors = m;
  server.focused_monitor = m;

  transaction_init();
  workspace_init();
  ipc_init();
  output_config_init();
  config_init();
  input_init();
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

void handle_output_power_set_mode(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_output_power_v1_set_mode_event *event = data;
  output_set_power(event->output, event->mode);
}

static void apply_output_head_config(struct wlr_output_configuration_head_v1 *config_head) {
  struct wlr_output *output = config_head->state.output;
  if (!output)
    return;

  struct wlr_output_state state;
  wlr_output_state_init(&state);

  wlr_output_state_set_enabled(&state, config_head->state.enabled);

  if (config_head->state.mode) {
    wlr_output_state_set_mode(&state, config_head->state.mode);
  } else if (config_head->state.custom_mode.width > 0) {
    wlr_output_state_set_custom_mode(&state,
        config_head->state.custom_mode.width,
        config_head->state.custom_mode.height,
        config_head->state.custom_mode.refresh);
  }

  wlr_output_state_set_scale(&state, config_head->state.scale);
  wlr_output_state_set_transform(&state, config_head->state.transform);
  wlr_output_state_set_adaptive_sync_enabled(&state, config_head->state.adaptive_sync_enabled);

  wlr_output_commit_state(output, &state);
  wlr_output_state_finish(&state);

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
    wlr_output_state_init(&state);

    wlr_output_state_set_enabled(&state, head->state.enabled);

    if (head->state.mode) {
      wlr_output_state_set_mode(&state, head->state.mode);
    } else if (head->state.custom_mode.width > 0) {
      wlr_output_state_set_custom_mode(&state,
        head->state.custom_mode.width,
        head->state.custom_mode.height,
        head->state.custom_mode.refresh);
    }

    wlr_output_state_set_scale(&state, head->state.scale);
    wlr_output_state_set_transform(&state, head->state.transform);
    wlr_output_state_set_adaptive_sync_enabled(&state, head->state.adaptive_sync_enabled);

    if (!wlr_output_test_state(output, &state)) {
      wlr_output_configuration_v1_send_failed(config);
      wlr_output_state_finish(&state);
      return;
    }

    wlr_output_state_finish(&state);
  }

  wlr_output_configuration_v1_send_succeeded(config);
}

static int hotkey_reload_handler(int fd, uint32_t mask, void *data) {
  (void)data;
  if (mask & WL_EVENT_READABLE) {
    char buf[64];
    read(fd, buf, sizeof(buf));
    reload_hotkeys();
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

  // add IPC socket to event loop
  struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
  int ipc_fd = ipc_get_socket_fd();
  if (ipc_fd >= 0)
    wl_event_loop_add_fd(event_loop, ipc_fd, WL_EVENT_READABLE, ipc_socket_handler, NULL);

  // add inotify for hotkey config file watching
  int hotkey_fd = get_hotkey_watch_fd();
  if (hotkey_fd >= 0)
    wl_event_loop_add_fd(event_loop, hotkey_fd, WL_EVENT_READABLE, hotkey_reload_handler, NULL);

  // run config after server is fully initialized
  wl_event_loop_add_idle(event_loop, run_config_idle, NULL);

  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
  wl_display_run(server.wl_display);
  return 0;
}

void server_fini(void) {
  transaction_fini();
  workspace_fini();
  ipc_cleanup();
  input_fini();
  config_fini();
  wl_display_destroy_clients(server.wl_display);

  wl_list_remove(&server.new_input.link);
  wl_list_remove(&server.new_output.link);
  wl_list_remove(&server.new_xdg_toplevel.link);
  wl_list_remove(&server.new_layer_surface.link);
  wl_list_remove(&server.cursor_motion.link);
  wl_list_remove(&server.cursor_motion_absolute.link);
  wl_list_remove(&server.cursor_button.link);
  wl_list_remove(&server.cursor_axis.link);
  wl_list_remove(&server.cursor_frame.link);
  wl_list_remove(&server.request_cursor.link);
  wl_list_remove(&server.pointer_focus_change.link);
  wl_list_remove(&server.request_set_selection.link);
  wl_list_remove(&server.request_start_drag.link);
  wl_list_remove(&server.start_drag.link);
  wl_list_remove(&server.new_session_lock.link);

  wlr_scene_node_destroy(&server.scene->tree.node);
  wlr_cursor_destroy(server.cursor);
  wlr_xcursor_manager_destroy(server.cursor_mgr);
  wlr_allocator_destroy(server.allocator);
  wlr_renderer_destroy(server.renderer);
  wlr_backend_destroy(server.backend);
  wl_display_destroy(server.wl_display);
}

void handle_request_start_drag(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_seat_request_start_drag_event *event = data;
  if (wlr_seat_validate_pointer_grab_serial(server.seat, event->origin, event->serial))
    wlr_seat_start_pointer_drag(server.seat, event->drag, event->serial);
  else
    wlr_data_source_destroy(event->drag->source);
}

void handle_start_drag(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_drag *drag = data;
  if (!drag->icon)
    return;

  struct wlr_scene_node *node = &wlr_scene_drag_icon_create(server.drag_tree, drag->icon)->node;
  drag->icon->data = node;

  struct wl_listener *listener_icon = calloc(1, sizeof(*listener_icon));
  listener_icon->notify = handle_drag_icon_destroy;
  wl_signal_add(&drag->icon->events.destroy, listener_icon);
}

void handle_drag_icon_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  wl_list_remove(&listener->link);
  free(listener);
}
