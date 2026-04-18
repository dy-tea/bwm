#pragma once

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

struct bwm_text_node {
  int width;
  int max_width;
  int height;
  int baseline;
  bool pango_markup;
  float color[4];
  float background[4];

  struct wlr_scene_node *node;
};

struct bwm_text_node *bwm_text_node_create(struct wlr_scene_tree *parent,
	const char *text, float color[4], bool pango_markup);

void bwm_text_node_set_color(struct bwm_text_node *node, float color[4]);
void bwm_text_node_set_background(struct bwm_text_node *node, float background[4]);
void bwm_text_node_set_text(struct bwm_text_node *node, const char *text);
void bwm_text_node_set_max_width(struct bwm_text_node *node, int max_width);

int bwm_text_node_default_height(void);
