#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "types.h"

#define MAX_RULES 128

typedef struct {
  char app_id[MAXLEN];
  char title[MAXLEN];
  bool one_shot;
} rule_match_t;

typedef struct {
  char desktop[SMALEN];
  client_state_t state;
  bool has_state;
  bool has_desktop;
  bool follow;
  bool has_follow;
  bool focus;
  bool has_focus;
  bool manage;
  bool has_manage;
  bool locked;
  bool has_locked;
  bool hidden;
  bool has_hidden;
  bool sticky;
  bool has_sticky;
} rule_consequence_t;

typedef struct rule_t {
  rule_match_t match;
  rule_consequence_t consequence;
  struct rule_t *next;
} rule_t;

extern rule_t *rule_head;
extern rule_t *rule_tail;

void rule_init(void);
void rule_fini(void);
rule_t *make_rule(void);
void add_rule(rule_t *r);
void remove_rule(rule_t *r);
bool remove_rule_by_index(int idx);
void list_rules(char *buf, size_t buf_size);
rule_consequence_t *find_matching_rule(const char *app_id, const char *title);
