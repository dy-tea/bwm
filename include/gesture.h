#pragma once

#include <stdbool.h>
#include <stdint.h>

enum gesture_type {
	GESTURE_TYPE_NONE = 0,
	GESTURE_TYPE_HOLD,
	GESTURE_TYPE_PINCH,
	GESTURE_TYPE_SWIPE,
};

enum gesture_direction {
	GESTURE_DIRECTION_NONE = 0,
	GESTURE_DIRECTION_UP = 1 << 0,
	GESTURE_DIRECTION_DOWN = 1 << 1,
	GESTURE_DIRECTION_LEFT = 1 << 2,
	GESTURE_DIRECTION_RIGHT = 1 << 3,
	GESTURE_DIRECTION_INWARD = 1 << 4,
	GESTURE_DIRECTION_OUTWARD = 1 << 5,
	GESTURE_DIRECTION_CLOCKWISE = 1 << 6,
	GESTURE_DIRECTION_COUNTERCLOCKWISE = 1 << 7,
};

const char *gesture_type_string(enum gesture_type type);
const char *gesture_direction_string(enum gesture_direction direction);

#define GESTURE_FINGERS_ANY 0

struct gesture {
	enum gesture_type type;
	uint8_t fingers;
	uint32_t directions;
};

char *gesture_parse(const char *input, struct gesture *output);
char *gesture_to_string(struct gesture *gesture);
bool gesture_check(struct gesture *target, enum gesture_type type, uint8_t fingers);
bool gesture_match(struct gesture *target, struct gesture *to_match, bool exact);
bool gesture_equal(struct gesture *a, struct gesture *b);
int8_t gesture_compare(struct gesture *a, struct gesture *b);

struct gesture_tracker {
	enum gesture_type type;
	uint8_t fingers;
	double dx, dy;
	double scale;
	double rotation;
};

void gesture_tracker_begin(struct gesture_tracker *tracker,
		enum gesture_type type, uint8_t fingers);
bool gesture_tracker_check(struct gesture_tracker *tracker,
		enum gesture_type type);
void gesture_tracker_update(struct gesture_tracker *tracker, double dx,
		double dy, double scale, double rotation);
void gesture_tracker_cancel(struct gesture_tracker *tracker);
void gesture_tracker_end(struct gesture_tracker *tracker);
