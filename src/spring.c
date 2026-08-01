#include "once.h"
#include "spring.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wl_list springs;
static const double TWO_PI = 6.283185307179586;

spring_curve_t *spring_add(const char *name, double stiffness, double damping, double mass,
		double value_epsilon, double velocity_epsilon) {
	if (!name || name[0] == '\0')
		return NULL;

	spring_curve_t *existing = spring_find(name);
	if (existing) {
		existing->stiffness = stiffness;
		existing->damping = damping;
		existing->mass = mass;
		existing->value_epsilon = value_epsilon;
		existing->velocity_epsilon = velocity_epsilon;
		wlr_log(WLR_DEBUG, "spring: updated '%s' (k=%.1f, d=%.1f, m=%.1f)", name, stiffness, damping,
			mass);
		return existing;
	}

	spring_curve_t *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	snprintf(c->name, sizeof(c->name), "%s", name);
	c->stiffness = stiffness;
	c->damping = damping;
	c->mass = mass;
	c->value_epsilon = value_epsilon;
	c->velocity_epsilon = velocity_epsilon;
	wl_list_insert(&springs, &c->link);
	wlr_log(WLR_DEBUG, "spring: added '%s' (k=%.1f, d=%.1f, m=%.1f)", name, stiffness, damping, mass);
	return c;
}

bool spring_exists(const char *name) {
	return spring_find(name) != NULL;
}

spring_curve_t *spring_find(const char *name) {
	if (!name || name[0] == '\0')
		return NULL;

	spring_curve_t *c;
	wl_list_for_each(c, &springs, link)
		if (strcmp(c->name, name) == 0)
			return c;

	return NULL;
}

double spring_evaluate(const spring_curve_t *curve, double dt, double *position, double *velocity,
		bool *done) {
	if (!curve || !position || !velocity)
		return *position;

	double p = *position;
	double v = *velocity;
	double dT = p - 1.0;

	// frequency (rad/s) from stiffness and mass
	double o = TWO_PI * sqrt(curve->stiffness / curve->mass) / TWO_PI;
	double zeta = curve->damping / (2.0 * sqrt(curve->stiffness * curve->mass));

	if (zeta < 1.0) {
		// underdamped
		double od = o * sqrt(1.0 - zeta * zeta);
		double sinTerm = sin(od * dt);
		double cosTerm = cos(od * dt);
		double expTerm = exp(-zeta * o * dt);

		double c1 = dT;
		double c2 = (v + zeta * o * dT) / od;

		p = 1.0 + expTerm * (c1 * cosTerm + c2 * sinTerm);
		v = expTerm * ((-c1 * zeta * o + c2 * od) * cosTerm + (-c2 * zeta * o - c1 * od) * sinTerm);
	} else if (zeta == 1.0) {
		// critically damped
		double c1 = dT;
		double c2 = v + o * dT;

		double expTerm = exp(-o * dt);

		p = 1.0 + expTerm * (c1 + c2 * dt);
		v = expTerm * (v - c2 * o * dt);
	} else {
		// overdamped
		double sqrtTerm = sqrt(zeta * zeta - 1.0);
		double r1 = -o * (zeta - sqrtTerm);
		double r2 = -o * (zeta + sqrtTerm);

		double c2 = (v - r1 * dT) / (r2 - r1);
		double c1 = dT - c2;

		double expTerm1 = exp(r1 * dt);
		double expTerm2 = exp(r2 * dt);

		p = 1.0 + c1 * expTerm1 + c2 * expTerm2;
		v = c1 * r1 * expTerm1 + c2 * r2 * expTerm2;
	}

	*position = p;
	*velocity = v;

	bool settled = fabs(p - 1.0) < curve->value_epsilon && fabs(v) < curve->velocity_epsilon;

	if (done)
		*done = settled;

	return p;
}

void spring_init(void) {
	ONCE();
	wl_list_init(&springs);

	spring_add("default", 300.0, 20.0, 1.0, SPRING_EPSILON_DEFAULT, SPRING_EPSILON_DEFAULT);
	spring_add("bouncy", 400.0, 10.0, 1.0, SPRING_EPSILON_DEFAULT, SPRING_EPSILON_DEFAULT);
	spring_add("gentle", 100.0, 15.0, 1.0, SPRING_EPSILON_DEFAULT, SPRING_EPSILON_DEFAULT);
	spring_add("slow", 50.0, 10.0, 2.0, SPRING_EPSILON_DEFAULT, SPRING_EPSILON_DEFAULT);
}

void spring_fini(void) {
	ONCE();
	spring_curve_t *c, *tmp;
	wl_list_for_each_safe(c, tmp, &springs, link) {
		wl_list_remove(&c->link);
		free(c);
	}
}
