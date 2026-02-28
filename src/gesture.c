#include "gesture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <wlr/util/log.h>

const char *gesture_type_string(enum gesture_type type) {
	switch (type) {
	case GESTURE_TYPE_NONE:
		return "none";
	case GESTURE_TYPE_HOLD:
		return "hold";
	case GESTURE_TYPE_PINCH:
		return "pinch";
	case GESTURE_TYPE_SWIPE:
		return "swipe";
	}
	return "???";
}

const char *gesture_direction_string(enum gesture_direction direction) {
	switch (direction) {
	case GESTURE_DIRECTION_UP:
		return "up";
	case GESTURE_DIRECTION_DOWN:
		return "down";
	case GESTURE_DIRECTION_LEFT:
		return "left";
	case GESTURE_DIRECTION_RIGHT:
		return "right";
	case GESTURE_DIRECTION_INWARD:
		return "inward";
	case GESTURE_DIRECTION_OUTWARD:
		return "outward";
	case GESTURE_DIRECTION_CLOCKWISE:
		return "clockwise";
	case GESTURE_DIRECTION_COUNTERCLOCKWISE:
		return "counterclockwise";
	case GESTURE_DIRECTION_NONE:
		break;
	}
	return "none";
}

static enum gesture_type gesture_parse_type(const char *type_str) {
	if (strcasecmp(type_str, "hold") == 0)
		return GESTURE_TYPE_HOLD;
	else if (strcasecmp(type_str, "pinch") == 0)
		return GESTURE_TYPE_PINCH;
	else if (strcasecmp(type_str, "swipe") == 0)
		return GESTURE_TYPE_SWIPE;
	return GESTURE_TYPE_NONE;
}

static enum gesture_direction gesture_parse_direction(const char *dir_str) {
	if (strcasecmp(dir_str, "up") == 0) {
		return GESTURE_DIRECTION_UP;
	} else if (strcasecmp(dir_str, "down") == 0) {
		return GESTURE_DIRECTION_DOWN;
	} else if (strcasecmp(dir_str, "left") == 0) {
		return GESTURE_DIRECTION_LEFT;
	} else if (strcasecmp(dir_str, "right") == 0) {
		return GESTURE_DIRECTION_RIGHT;
	} else if (strcasecmp(dir_str, "inward") == 0) {
		return GESTURE_DIRECTION_INWARD;
	} else if (strcasecmp(dir_str, "outward") == 0) {
		return GESTURE_DIRECTION_OUTWARD;
	} else if (strcasecmp(dir_str, "clockwise") == 0) {
		return GESTURE_DIRECTION_CLOCKWISE;
	} else if (strcasecmp(dir_str, "counterclockwise") == 0) {
		return GESTURE_DIRECTION_COUNTERCLOCKWISE;
	}
	return GESTURE_DIRECTION_NONE;
}

char *gesture_parse(const char *input, struct gesture *output) {
	if (!input || !output) {
		return "invalid input";
	}

	memset(output, 0, sizeof(struct gesture));

	char *input_copy = strdup(input);
	if (!input_copy)
		return "memory allocation failed";

	char *saveptr = NULL;
	char *token = strtok_r(input_copy, ":", &saveptr);
	if (!token) {
		free(input_copy);
		return "missing gesture type";
	}

	output->type = gesture_parse_type(token);
	if (output->type == GESTURE_TYPE_NONE) {
		free(input_copy);
		return "unknown gesture type";
	}

	token = strtok_r(NULL, ":", &saveptr);
	if (token) {
		if (strcmp(token, "*") == 0) {
			output->fingers = GESTURE_FINGERS_ANY;
		} else {
			int fingers = atoi(token);
			if (fingers < 1 || fingers > 5) {
				free(input_copy);
				return "invalid finger count";
			}
			output->fingers = (uint8_t)fingers;
		}

		token = strtok_r(NULL, ":", &saveptr);
		if (token) {
			char *dirs = strdup(token);
			if (!dirs) {
				free(input_copy);
				return "memory allocation failed";
			}

			output->directions = GESTURE_DIRECTION_NONE;
			char *dir_saveptr = NULL;
			char *dir_token = strtok_r(dirs, "+", &dir_saveptr);
			while (dir_token) {
				enum gesture_direction dir = gesture_parse_direction(dir_token);
				if (dir != GESTURE_DIRECTION_NONE)
					output->directions |= dir;
				dir_token = strtok_r(NULL, "+", &dir_saveptr);
			}
			free(dirs);
		}
	}

	free(input_copy);
	return NULL;
}

char *gesture_to_string(struct gesture *gesture) {
	if (!gesture)
		return strdup("");

	char buf[256] = {0};
	snprintf(buf, sizeof(buf), "%s", gesture_type_string(gesture->type));

	if (gesture->fingers != GESTURE_FINGERS_ANY) {
		char fingers[16];
		snprintf(fingers, sizeof(fingers), ":%d", gesture->fingers);
		strncat(buf, fingers, sizeof(buf) - strlen(buf) - 1);
	}

	if (gesture->directions != GESTURE_DIRECTION_NONE) {
		strncat(buf, ":", sizeof(buf) - strlen(buf) - 1);

		bool first = true;
		if (gesture->directions & GESTURE_DIRECTION_UP) {
			strncat(buf, first ? "up" : "+up", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_DOWN) {
			strncat(buf, first ? "down" : "+down", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_LEFT) {
			strncat(buf, first ? "left" : "+left", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_RIGHT) {
			strncat(buf, first ? "right" : "+right", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_INWARD) {
			strncat(buf, first ? "inward" : "+inward", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_OUTWARD) {
			strncat(buf, first ? "outward" : "+outward", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_CLOCKWISE) {
			strncat(buf, first ? "clockwise" : "+clockwise", sizeof(buf) - strlen(buf) - 1);
			first = false;
		}
		if (gesture->directions & GESTURE_DIRECTION_COUNTERCLOCKWISE)
			strncat(buf, first ? "counterclockwise" : "+counterclockwise", sizeof(buf) - strlen(buf) - 1);
	}

	return strdup(buf);
}

bool gesture_check(struct gesture *target, enum gesture_type type, uint8_t fingers) {
	if (!target || target->type != type)
		return false;

	if (target->fingers != GESTURE_FINGERS_ANY && target->fingers != fingers)
		return false;

	return true;
}

bool gesture_match(struct gesture *target, struct gesture *to_match, bool exact) {
	if (!target || !to_match)
		return false;

	if (target->type != to_match->type)
		return false;

	if (target->fingers != GESTURE_FINGERS_ANY && target->fingers != to_match->fingers)
		return false;

	if (exact)
		return target->directions == to_match->directions;

	return (target->directions & to_match->directions) != 0;
}

bool gesture_equal(struct gesture *a, struct gesture *b) {
	if (!a || !b)
		return false;

	return a->type == b->type &&
		a->fingers == b->fingers &&
		a->directions == b->directions;
}

int8_t gesture_compare(struct gesture *a, struct gesture *b) {
	if (!a || !b)
		return -1;

	if (a->type != b->type)
		return -1;

	if (a->fingers != b->fingers)
		return (a->fingers > b->fingers) ? 1 : -1;

	int a_bits = __builtin_popcount(a->directions);
	int b_bits = __builtin_popcount(b->directions);

	if (a_bits == b_bits)
		return 0;

	return (a_bits > b_bits) ? 1 : -1;
}

void gesture_tracker_begin(struct gesture_tracker *tracker,
		enum gesture_type type, uint8_t fingers) {
	if (!tracker)
		return;

	tracker->type = type;
	tracker->fingers = fingers;
	tracker->dx = 0;
	tracker->dy = 0;
	tracker->scale = 1.0;
	tracker->rotation = 0;
}

bool gesture_tracker_check(struct gesture_tracker *tracker,
		enum gesture_type type) {
	if (!tracker)
		return false;

	return tracker->type == type && tracker->fingers > 0;
}

void gesture_tracker_update(struct gesture_tracker *tracker, double dx,
		double dy, double scale, double rotation) {
	if (!tracker)
		return;

	tracker->dx += dx;
	tracker->dy += dy;

	if (!isnan(scale))
		tracker->scale = scale;

	if (!isnan(rotation))
		tracker->rotation = rotation;
}

void gesture_tracker_cancel(struct gesture_tracker *tracker) {
	if (!tracker)
		return;

	tracker->type = GESTURE_TYPE_NONE;
	tracker->fingers = 0;
	tracker->dx = 0;
	tracker->dy = 0;
	tracker->scale = 1.0;
	tracker->rotation = 0;
}

void gesture_tracker_end(struct gesture_tracker *tracker) {
	if (!tracker)
		return;

	tracker->type = GESTURE_TYPE_NONE;
	tracker->fingers = 0;
}
