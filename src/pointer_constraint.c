#include "once.h"
#include "pointer_constraint.h"
#include "server.h"
#include "toplevel.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>

static void cursor_warp_to_constraint_hint(void) {
	struct wlr_pointer_constraint_v1 *active = server.active_pointer_constraint;
	if (active == NULL)
		return;

	if (active->current.cursor_hint.enabled) {
		double sx = active->current.cursor_hint.x;
		double sy = active->current.cursor_hint.y;

		toplevel_t *toplevel = active->surface->data;
		if (!toplevel)
			return;

		double lx = sx - toplevel->node->rectangle.x;
		double ly = sy - toplevel->node->rectangle.y;

		wlr_cursor_warp(server.cursor, NULL, lx, ly);
		wlr_seat_pointer_warp(active->seat, sx, sy);
	}
}

static void cursor_check_constraint_region(void) {
	struct wlr_pointer_constraint_v1 *constraint = server.active_pointer_constraint;
	pixman_region32_t *region = &constraint->region;
	toplevel_t *toplevel = constraint->surface->data;
	if (server.cursor_requires_warp && toplevel) {
		server.cursor_requires_warp = false;

		double sx = server.cursor->x + toplevel->node->rectangle.x;
		double sy = server.cursor->y + toplevel->node->rectangle.y;

		if (!pixman_region32_contains_point(region, floor(sx), floor(sy), NULL)) {
			int count;
			pixman_box32_t *boxes = pixman_region32_rectangles(region, &count);
			if (count > 0) {
				sx = (boxes[0].x1 + boxes[0].x2) / 2.0;
				sy = (boxes[0].y1 + boxes[0].y2) / 2.0;

				wlr_cursor_warp_closest(server.cursor, NULL, sx + toplevel->node->rectangle.x,
					sy + toplevel->node->rectangle.y);
			}
		}
	}

	// empty region if locked
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
		pixman_region32_copy(&server.pointer_confine, region);
	else
		pixman_region32_clear(&server.pointer_confine);
}

static void handle_cursor_contraint_commit(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	cursor_check_constraint_region();
}

void pointer_constrain(struct wlr_pointer_constraint_v1 *constraint) {
	if (server.active_pointer_constraint == constraint)
		return;

	wl_list_remove(&server.pointer_constraint_commit.link);
	if (server.active_pointer_constraint) {
		if (!constraint)
			cursor_warp_to_constraint_hint();

		// deactivate current constraint
		wlr_pointer_constraint_v1_send_deactivated(server.active_pointer_constraint);
	}

	// set the new constraint
	server.active_pointer_constraint = constraint;

	if (!constraint) {
		wl_list_init(&server.pointer_constraint_commit.link);
		return;
	}

	server.cursor_requires_warp = true;

	if (pixman_region32_not_empty(&constraint->current.region))
		pixman_region32_intersect(&constraint->region, &constraint->surface->input_region,
			&constraint->current.region);
	else
		pixman_region32_copy(&constraint->region, &constraint->surface->input_region);

	cursor_check_constraint_region();

	wlr_pointer_constraint_v1_send_activated(constraint);

	server.pointer_constraint_commit.notify = handle_cursor_contraint_commit;
	wl_signal_add(&constraint->surface->events.commit, &server.pointer_constraint_commit);
}

static void handle_constraint_set_region(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
	server.cursor_requires_warp = true;
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	pointer_constraint_t *constraint = wl_container_of(listener, constraint, destroy);
	struct wlr_pointer_constraint_v1 *wlr_constraint = data;

	wl_list_remove(&constraint->set_region.link);
	wl_list_remove(&constraint->destroy.link);

	if (server.active_pointer_constraint == wlr_constraint) {
		cursor_warp_to_constraint_hint();

		if (server.pointer_constraint_commit.link.next != NULL)
			wl_list_remove(&server.pointer_constraint_commit.link);

		wl_list_init(&server.pointer_constraint_commit.link);
		server.active_pointer_constraint = NULL;
	}

	free(constraint);
}

static void handle_pointer_constraint(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_pointer_constraint_v1 *constraint = data;

	pointer_constraint_t *pointer_constraint = calloc(1, sizeof(*pointer_constraint));
	if (!pointer_constraint) {
		wlr_log(WLR_ERROR, "allocation failed");
		return;
	}
	pointer_constraint->constraint = constraint;
	pointer_constraint->set_region.notify = handle_constraint_set_region;
	wl_signal_add(&constraint->events.set_region, &pointer_constraint->set_region);
	pointer_constraint->destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &pointer_constraint->destroy);

	if (constraint->surface == server.seat->pointer_state.focused_surface)
		server.active_pointer_constraint = constraint;
}

void pointer_constraint_init(void) {
	ONCE();
	server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
	if (!server.pointer_constraints) {
		wlr_log(WLR_ERROR, "Failed to create pointer constraints");
		exit(EXIT_FAILURE);
	}

	server.cursor_requires_warp = false;
	wl_list_init(&server.pointer_constraint_commit.link);

	server.new_pointer_constraint.notify = handle_pointer_constraint;
	wl_signal_add(&server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint);
}

void pointer_constraint_fini(void) {
	ONCE();
	wl_list_remove(&server.new_pointer_constraint.link);
}
