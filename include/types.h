#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

#define MAXLEN 256
#define SMALEN 64

// forward declarations
struct node_t;
struct desktop_t;
struct monitor_t;
struct client_t;
struct bwm_toplevel;

// enums
typedef enum { TYPE_HORIZONTAL, TYPE_VERTICAL } split_type_t;

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

typedef enum { LAYER_BELOW, LAYER_NORMAL, LAYER_ABOVE } stack_layer_t;

typedef enum { LAYOUT_TILED, LAYOUT_MONOCLE } layout_t;

typedef enum { DIR_NORTH, DIR_WEST, DIR_SOUTH, DIR_EAST } direction_t;

typedef enum { FIRST_CHILD, SECOND_CHILD } child_polarity_t;

typedef enum { FLIP_HORIZONTAL, FLIP_VERTICAL } flip_t;

// structures
typedef struct {
  int top;
  int right;
  int bottom;
  int left;
} padding_t;

typedef struct {
  uint16_t min_width;
  uint16_t min_height;
} constraints_t;

typedef struct {
  double split_ratio;
  direction_t split_dir;
} presel_t;

typedef struct client_t {
  char app_id[MAXLEN];
  char title[MAXLEN];
  unsigned int border_width;
  bool urgent;
  bool shown;
  client_state_t state;
  client_state_t last_state;
  stack_layer_t layer;
  stack_layer_t last_layer;
  struct wlr_box floating_rectangle;
  struct wlr_box tiled_rectangle;
  struct bwm_toplevel *toplevel;
} client_t;

typedef struct node_t {
  uint32_t id;
  split_type_t split_type;
  double split_ratio;
  presel_t *presel;
  struct wlr_box rectangle;
  constraints_t constraints;
  bool vacant;
  bool hidden;
  bool sticky;
  bool private_node;
  bool locked;
  bool marked;
  struct node_t *first_child;
  struct node_t *second_child;
  struct node_t *parent;
  client_t *client;

  // transaction support
  struct bwm_transaction_inst *instruction;
  size_t ntxnrefs;
  bool dirty;
  bool destroying;

  // current state
  struct {
    struct wlr_box rectangle;
    double split_ratio;
    split_type_t split_type;
    bool hidden;
  } current;

  // pending state
  struct {
    struct wlr_box rectangle;
    double split_ratio;
    split_type_t split_type;
    bool hidden;
  } pending;
} node_t;

typedef struct desktop_t {
  char name[SMALEN];
  uint32_t id;
  layout_t layout;
  layout_t user_layout;
  node_t *root;
  node_t *focus;
  struct desktop_t *prev;
  struct desktop_t *next;
  padding_t padding;
  int window_gap;
  unsigned int border_width;
} desktop_t;

typedef struct monitor_t {
  char name[SMALEN];
  uint32_t id;
  struct bwm_output *output;
  bool wired;
  padding_t padding;
  unsigned int sticky_count;
  int window_gap;
  unsigned int border_width;
  struct wlr_box rectangle;
  desktop_t *desk;
  desktop_t *desk_head;
  desktop_t *desk_tail;
  struct monitor_t *prev;
  struct monitor_t *next;
} monitor_t;

typedef struct {
  monitor_t *monitor;
  desktop_t *desktop;
  node_t *node;
} coordinates_t;

// global settings
extern automatic_scheme_t automatic_scheme;
extern child_polarity_t initial_polarity;
extern bool single_monocle;
extern bool borderless_monocle;
extern bool gapless_monocle;
extern padding_t monocle_padding;
extern int border_width;
extern int window_gap;

// global state
extern monitor_t *mon;
extern monitor_t *mon_head;
extern monitor_t *mon_tail;
extern uint32_t next_node_id;
extern uint32_t next_desktop_id;
extern uint32_t next_monitor_id;
