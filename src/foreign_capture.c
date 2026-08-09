#include "once.h"
#include "server.h"
#include "toplevel.h"
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_scene.h>

static void handle_new_toplevel_capture_request(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_event *request = data;
	void *handle_data = request->toplevel_handle->data;

	struct wlr_ext_image_capture_source_v1 **image_capture_source_ptr = NULL;
	struct wlr_scene *image_capture = NULL;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl == handle_data) {
			image_capture_source_ptr = &tl->image_capture_source;
			image_capture = tl->image_capture;
			break;
		}
	}

	if (image_capture_source_ptr == NULL) {
		xwayland_toplevel_t *xwayland_view = handle_data;
		image_capture_source_ptr = &xwayland_view->image_capture_source;
		image_capture = xwayland_view->image_capture;
	}

	if (image_capture_source_ptr == NULL || image_capture == NULL) {
		wlr_log(WLR_ERROR, "Failed to determine toplevel type for image capture");
		return;
	}

	if (*image_capture_source_ptr == NULL) {
		*image_capture_source_ptr =
			wlr_ext_image_capture_source_v1_create_with_scene_node(&image_capture->tree.node,
			wl_display_get_event_loop(server.wl_display), server.allocator, server.renderer);

		if (*image_capture_source_ptr == NULL)
			return;
	}

	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request,
		*image_capture_source_ptr);
}

void foreign_capture_init(void) {
	ONCE();
	server.foreign_toplevel_image_capture_source_manager =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server.wl_display, 1);
	if (!server.foreign_toplevel_image_capture_source_manager) {
		wlr_log(WLR_ERROR, "Failed to create foreign toplevel image capture source manager");
		exit(EXIT_FAILURE);
	}
	server.new_toplevel_capture_request.notify = handle_new_toplevel_capture_request;
	wl_signal_add(&server.foreign_toplevel_image_capture_source_manager->events.capture_request,
		&server.new_toplevel_capture_request);
}

void foreign_capture_fini(void) {
	ONCE();
	wl_list_remove(&server.new_toplevel_capture_request.link);
}
