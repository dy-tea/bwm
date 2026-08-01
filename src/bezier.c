#include "bezier.h"
#include "once.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wl_list bezier_curves;

void bezier_init(void) {
	ONCE();
	wl_list_init(&bezier_curves);

	bezier_add("default", 1.0 / 3.0, 1.0, 2.0 / 3.0, 1.0);
	bezier_add("linear", 0.0, 0.0, 1.0, 1.0);
	bezier_add("ease", 0.25, 0.1, 0.25, 1.0);
	bezier_add("ease_in", 0.42, 0.0, 1.0, 1.0);
	bezier_add("ease_out", 0.0, 0.0, 0.58, 1.0);
	bezier_add("ease_in_out", 0.42, 0.0, 0.58, 1.0);
}

void bezier_fini(void) {
	ONCE();
	bezier_curve_t *curve, *tmp;
	wl_list_for_each_safe(curve, tmp, &bezier_curves, link) {
		wl_list_remove(&curve->link);
		free(curve);
	}
}

static void bake_curve(bezier_curve_t *curve) {
	double p0x = 0.0, p0y = 0.0;
	double p1x = curve->p1x, p1y = curve->p1y;
	double p2x = curve->p2x, p2y = curve->p2y;
	double p3x = 1.0, p3y = 1.0;

	for (int i = 0; i < BEZIER_BAKED_POINTS; i++) {
		double t = (double)i / (BEZIER_BAKED_POINTS - 1);
		double one_minus_t = 1.0 - t;

		double b0 = one_minus_t * one_minus_t * one_minus_t;
		double b1 = 3.0 * one_minus_t * one_minus_t * t;
		double b2 = 3.0 * one_minus_t * t * t;
		double b3 = t * t * t;

		curve->baked[i].x = (float)(b0 * p0x + b1 * p1x + b2 * p2x + b3 * p3x);
		curve->baked[i].y = (float)(b0 * p0y + b1 * p1y + b2 * p2y + b3 * p3y);
	}
}

bezier_curve_t *bezier_add(const char *name, double p1x, double p1y, double p2x, double p2y) {
	if (!name || name[0] == '\0')
		return NULL;

	// update existing curve
	bezier_curve_t *existing = bezier_find(name);
	if (existing) {
		existing->p1x = p1x;
		existing->p1y = p1y;
		existing->p2x = p2x;
		existing->p2y = p2y;
		bake_curve(existing);
		wlr_log(WLR_DEBUG, "bezier: updated '%s' with P1=(%.3f,%.3f) P2=(%.3f,%.3f)", name, p1x, p1y, p2x,
			p2y);
		return existing;
	}

	bezier_curve_t *curve = calloc(1, sizeof(*curve));
	if (!curve)
		return NULL;

	snprintf(curve->name, sizeof(curve->name), "%s", name);
	curve->p1x = p1x;
	curve->p1y = p1y;
	curve->p2x = p2x;
	curve->p2y = p2y;
	bake_curve(curve);

	wl_list_insert(&bezier_curves, &curve->link);
	wlr_log(WLR_DEBUG, "bezier: added '%s' with P1=(%.3f,%.3f) P2=(%.3f,%.3f)", name, p1x, p1y, p2x,
		p2y);
	return curve;
}

bool bezier_exists(const char *name) {
	return bezier_find(name) != NULL;
}

bezier_curve_t *bezier_find(const char *name) {
	if (!name || name[0] == '\0')
		return NULL;

	bezier_curve_t *curve;
	wl_list_for_each(curve, &bezier_curves, link)
		if (strcmp(curve->name, name) == 0)
			return curve;

	return NULL;
}

double bezier_evaluate(const char *name, double x) {
	bezier_curve_t *curve = bezier_find(name);
	if (!curve)
		return x;

	return bezier_evaluate_curve(curve, x);
}

double bezier_evaluate_curve(const bezier_curve_t *curve, double x) {
	if (!curve)
		return x;
	if (x <= 0.0)
		return 0.0;
	if (x >= 1.0)
		return 1.0;

	// binary search through baked points
	int lo = 0, hi = BEZIER_BAKED_POINTS - 1;
	while (lo < hi - 1) {
		int mid = (lo + hi) / 2;
		if (curve->baked[mid].x < x)
			lo = mid;
		else
			hi = mid;
	}

	float x0 = curve->baked[lo].x;
	float x1 = curve->baked[hi].x;
	float y0 = curve->baked[lo].y;
	float y1 = curve->baked[hi].y;

	if (x1 - x0 < 0.000001f)
		return y0;

	float t = (float)((x - x0) / (x1 - x0));
	return y0 + t * (y1 - y0);
}
