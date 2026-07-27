#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

#define MAXLEN 256
#define SMALEN 64

// forward declarations
struct node_t;
struct desktop_t;
struct client_t;
struct output_t;
struct toplevel_t;
struct xwayland_toplevel_t;
struct tab_bar_t;
struct scroller_state_t;

struct output_t *output_at(double x, double y);

// enums
typedef enum {
	TYPE_HORIZONTAL,
	TYPE_VERTICAL,
	TYPE_TABBED
} split_type_t;

typedef enum {
	SCHEME_LONGEST_SIDE,
	SCHEME_ALTERNATE,
	SCHEME_SPIRAL
} automatic_scheme_t;

typedef enum {
	STATE_TILED,
	STATE_PSEUDO_TILED,
	STATE_FLOATING,
	STATE_FULLSCREEN
} client_state_t;

typedef enum {
	DECORATION_NONE,
	DECORATION_TABS,
	DECORATION_ALWAYS,
	DECORATION_CSD
} decoration_mode_t;

typedef enum {
	FOCUS_ON_ACTIVATE_FOCUS,
	FOCUS_ON_ACTIVATE_NONE,
	FOCUS_ON_ACTIVATE_SMART,
	FOCUS_ON_ACTIVATE_URGENT
} focus_on_activate_mode_t;

typedef enum {
	FOLLOWS_NO,
	FOLLOWS_YES,
	FOLLOWS_ALWAYS
} focus_follows_mouse_mode_t;

typedef enum {
	LAYER_BELOW,
	LAYER_NORMAL,
	LAYER_ABOVE
} stack_layer_t;

typedef enum {
	LAYOUT_TILED,
	LAYOUT_MONOCLE,
	LAYOUT_SCROLLER,
	LAYOUT_MASTER_STACK
} layout_t;

static inline char layout_to_char(layout_t l) {
	switch (l) {
	case LAYOUT_TILED:
		return 'T';
	case LAYOUT_MONOCLE:
		return 'M';
	case LAYOUT_SCROLLER:
		return 'S';
	case LAYOUT_MASTER_STACK:
		return 'K';
	}
	return 0;
}

typedef enum {
	DIR_NORTH,
	DIR_WEST,
	DIR_SOUTH,
	DIR_EAST
} direction_t;

typedef enum {
	FIRST_CHILD,
	SECOND_CHILD
} child_polarity_t;

typedef enum {
	FLIP_HORIZONTAL,
	FLIP_VERTICAL
} flip_t;

typedef enum {
	WORKSPACE_ANIM_VERTICAL,
	WORKSPACE_ANIM_HORIZONTAL
} workspace_anim_direction_t;

// structures
typedef struct {
	int top, right, bottom, left;
} padding_t;

typedef struct {
	uint16_t min_width, min_height;
} constraints_t;

typedef struct {
	double split_ratio;
	direction_t split_dir;
	bool freed;
} presel_t;

typedef struct {
	uint32_t urgent : 1;
	uint32_t shown : 1;
	uint32_t freed : 1;
	uint32_t master_stack_master : 1;
	uint32_t cursor_in_left_half : 1;
	uint32_t cursor_in_upper_half : 1;
	uint32_t blur : 1;
	uint32_t blur_from_rule : 1;
	uint32_t mica : 1;
	uint32_t acrylic : 1;
	uint32_t shadow : 1;
	uint32_t block_out_from_screenshare : 1;
	uint32_t allow_tearing : 1;
	uint32_t allow_tearing_from_rule : 1;
	uint32_t render_unfocused : 1;
	uint32_t render_unfocused_from_rule : 1;
	uint32_t anim_disabled : 1;
} client_flags_t;

typedef struct client_t {
	char app_id[MAXLEN];
	char title[MAXLEN];
	client_state_t state, last_state;
	stack_layer_t layer, last_layer;
	struct wlr_box floating_rectangle, tiled_rectangle, committed_tiled_rectangle, arranged_rectangle;
	struct toplevel_t *toplevel;
	struct xwayland_toplevel_t *xwayland_view;

	// Master-stack layout properties
	uint64_t master_stack_order;

	// Visual effects
	float border_radius;
	float opacity;
	float shadow_size, shadow_offset_x, shadow_offset_y;
	float shadow_color[4];

	client_flags_t flags;
} client_t;

typedef struct node_t {
	uint32_t id;
	split_type_t split_type;
	double split_ratio;
	presel_t *presel;
	struct wlr_box rectangle;
	constraints_t constraints;
	bool vacant, hidden, sticky, scratchpad, private_node, locked, marked;
	struct node_t *first_child, *second_child, *parent;
	client_t *client;
	struct output_t *output;
	struct desktop_t *desktop;

	// transaction support
	struct transaction_inst_t *instruction, *pending_inst;
	size_t ntxnrefs;
	bool dirty;
	bool destroying;
	bool freed;

	// current and pending state
	struct {
		struct wlr_box rectangle;
		double split_ratio;
		split_type_t split_type;
		bool hidden;
	} current, pending;

	struct tab_bar_t *tab_bar;
} node_t;

typedef struct desktop_t {
	char name[SMALEN];
	uint32_t id;
	layout_t layout, user_layout;
	node_t *root, *focus;
	struct scroller_state_t *scroller_state;
	struct desktop_t *prev, *next;
	padding_t padding;
	int window_gap;
	int master_stack_count;
	struct output_t *output;
	uint32_t fullscreen_recreate_pending_window_id;
} desktop_t;

typedef struct {
	struct output_t *output;
	desktop_t *desktop;
	node_t *node;
} coordinates_t;

// global settings
extern automatic_scheme_t automatic_scheme;
extern child_polarity_t initial_polarity;
extern bool single_monocle;
extern bool borderless_monocle;
extern bool borderless_singleton;
extern bool gapless_monocle;
extern bool removal_adjustment;
extern int focus_follows_mouse;
extern bool pointer_follows_focus;
extern bool record_history;
extern bool click_to_focus;
extern bool allow_tearing;
extern bool auto_float_dialogs;
extern decoration_mode_t decoration_mode;
extern bool enable_animations;
extern bool hide_lone_tab;
extern workspace_anim_direction_t workspace_anim_direction;
extern bool workspace_anim_slide_up;
extern int mapping_events_count;

// Transaction system settings
extern int txn_timeout_ms;
extern bool debug_txn_timings;
extern bool debug_noatomic;
extern bool debug_txn_wait;
extern int directional_focus_tightness;
extern int ignore_ewmh_fullscreen;

// Render-unfocused settings
extern int render_unfocused_fps;

// Idle power management (DPMS timeout)
extern int idle_timeout;
extern bool idle_dpms;

// Real-time scheduling
extern bool realtime_scheduling;

// Shadow settings
extern float shadow_size;
extern float shadow_offset_x;
extern float shadow_offset_y;
extern float shadow_color[4];

extern padding_t monocle_padding;
extern padding_t padding;
extern int border_width;
extern int window_gap;
extern bool smart_gaps;
extern bool smart_borders;
extern bool focus_wrapping;
extern int focus_on_activate; // focus_on_activate_mode_t
extern double split_ratio;
extern char normal_border_color[16];
extern char active_border_color[16];
extern char focused_border_color[16];
extern char presel_feedback_color[16];
extern char tiling_drag_indicator_color[16];

extern float normal_border_color_rgba[4];
extern float active_border_color_rgba[4];
extern float focused_border_color_rgba[4];
extern float presel_feedback_color_rgba[4];
extern float tiling_drag_indicator_color_rgba[4];

// border gradient theme
#define BORDER_GRADIENT_MAX_STOPS 10
typedef struct {
	float gradient[BORDER_GRADIENT_MAX_STOPS * 4];
	int gradient_count;
	float gradient_angle;
	float gradient2[BORDER_GRADIENT_MAX_STOPS * 4];
	int gradient2_count;
	float gradient2_angle;
	float gradient_lerp;
} border_theme_t;

extern border_theme_t normal_border_theme;
extern border_theme_t active_border_theme;
extern border_theme_t focused_border_theme;

// global state
extern struct output_t *mon;
extern struct output_t *mon_head;
extern struct output_t *mon_tail;
extern uint32_t next_node_id;
extern uint32_t next_desktop_id;
extern uint32_t next_monitor_id;
extern desktop_t *orphan_desk_head;
extern desktop_t *orphan_desk_tail;
