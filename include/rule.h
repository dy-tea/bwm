#pragma once

#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

#define MAX_RULES 128

typedef enum {
	RULE_TYPE_NONE = 0,
	RULE_TYPE_DESKTOP = 1 << 0,
	RULE_TYPE_MONITOR = 1 << 1,
	RULE_TYPE_STATE = 1 << 2,
	RULE_TYPE_FOLLOW = 1 << 3,
	RULE_TYPE_FOCUS = 1 << 4,
	RULE_TYPE_MANAGE = 1 << 5,
	RULE_TYPE_LOCKED = 1 << 6,
	RULE_TYPE_HIDDEN = 1 << 7,
	RULE_TYPE_STICKY = 1 << 8,
	RULE_TYPE_SCROLLER_PROPORTION = 1 << 9,
	RULE_TYPE_SCROLLER_PROPORTION_SINGLE = 1 << 10,
	RULE_TYPE_BLUR = 1 << 11,
	RULE_TYPE_MICA = 1 << 12,
	RULE_TYPE_ACRYLIC = 1 << 13,
	RULE_TYPE_BORDER_RADIUS = 1 << 14,
	RULE_TYPE_SHADOW = 1 << 15,
	RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE = 1 << 16,
	RULE_TYPE_ALLOW_TEARING = 1 << 17,
	RULE_TYPE_SHORTCUTS_INHIBITOR = 1 << 18,
	RULE_TYPE_RENDER_UNFOCUSED_FPS = 1 << 19,
	RULE_TYPE_OPACITY = 1 << 20,
	RULE_TYPE_ANIM_DISABLE = 1 << 21,
	RULE_TYPE_LAST = 1 << 22,
} rule_type_t;

typedef struct {
	char app_id[MAXLEN];
	char title[MAXLEN];
	char tag[MAXLEN];
	bool one_shot;
} rule_match_t;

typedef struct {
	uint32_t has; // what fields are set in this rule
	uint32_t flags; // value of boolean fields

	// non-boolean fields
	char desktop[SMALEN];
	char monitor[SMALEN];
	client_state_t state;
	float scroller_proportion;
	float scroller_proportion_single;
	float border_radius;
	float opacity;
	int render_unfocused_fps;
} rule_consequence_t;

typedef struct rule_t {
	rule_match_t match;
	rule_consequence_t consequence;
	struct wl_list link;
} rule_t;

extern struct wl_list rule_list;

void rule_init(void);
void rule_fini(void);
rule_t *make_rule(void);
void add_rule(rule_t *r);
void remove_rule(rule_t *r);
bool remove_rule_by_index(int idx);
void list_rules(char *buf, size_t buf_size);
rule_consequence_t *find_matching_rule(const char *app_id, const char *title, const char *tag);
void rule_apply_consequence(node_t *node, client_t *client, const rule_consequence_t *rule);
