#include "once.h"
#include "server.h"
#include <wlr/config.h>
#include <wlr/types/wlr_drm_lease_v1.h>

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

void drm_lease_init(void) {
	ONCE();
	server.drm_lease_manager = wlr_drm_lease_v1_manager_create(server.wl_display, server.backend);
	if (server.drm_lease_manager) {
		server.drm_lease_request.notify = handle_drm_lease_request;
		wl_signal_add(&server.drm_lease_manager->events.request, &server.drm_lease_request);
	} else {
		wlr_log(WLR_ERROR, "failed to create drm lease manager");
	}
}

void drm_lease_fini(void) {
	ONCE();
	if (server.drm_lease_manager)
		wl_list_remove(&server.drm_lease_request.link);
}

#endif
