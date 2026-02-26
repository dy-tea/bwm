#include "rule.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

rule_t *rule_head = NULL;
rule_t *rule_tail = NULL;

void rule_init(void) {
  rule_head = NULL;
  rule_tail = NULL;
}

void rule_fini(void) {
  rule_t *r = rule_head;
  while (r != NULL) {
    rule_t *next = r->next;
    free(r);
    r = next;
  }
  rule_head = NULL;
  rule_tail = NULL;
}

rule_t *make_rule(void) {
  rule_t *r = calloc(1, sizeof(rule_t));
  if (r) {
    r->match.app_id[0] = '\0';
    r->match.title[0] = '\0';
    r->match.one_shot = false;
  }
  return r;
}

void add_rule(rule_t *r) {
  if (rule_tail == NULL) {
    rule_head = rule_tail = r;
  } else {
    rule_tail->next = r;
    rule_tail = r;
  }
}

void remove_rule(rule_t *r) {
  if (r == NULL) return;

  if (r == rule_head)
    rule_head = r->next;

  if (r == rule_tail)
    rule_tail = NULL;

  if (r->next)
    r->next = r->next;

  free(r);
}

bool remove_rule_by_index(int idx) {
  rule_t *r = rule_head;
  int i = 0;
  for (; r != NULL && i < idx; r = r->next, i++)
  	;
  if (r == NULL) return false;

  rule_t *prev = NULL;
  rule_t *cur = rule_head;
  for (i = 0; cur != NULL && i < idx; prev = cur, cur = cur->next, ++i)
    ;

  if (cur == NULL) return false;

  if (prev)
    prev->next = cur->next;
  else
    rule_head = cur->next;

  if (cur == rule_tail)
    rule_tail = prev;

  free(cur);
  return true;
}

void list_rules(char *buf, size_t buf_size) {
  size_t offset = 0;
  int idx = 0;

  rule_t *r = rule_head;
  while (r != NULL) {
    offset += snprintf(buf + offset, buf_size - offset, "%d: ", idx);

    if (r->match.app_id[0] != '\0')
      offset += snprintf(buf + offset, buf_size - offset, "app_id=%s ", r->match.app_id);
    if (r->match.title[0] != '\0')
      offset += snprintf(buf + offset, buf_size - offset, "title=%s ", r->match.title);

    if (r->match.one_shot)
      offset += snprintf(buf + offset, buf_size - offset, "one_shot ");

    offset += snprintf(buf + offset, buf_size - offset, "-> ");

    if (r->consequence.has_desktop)
      offset += snprintf(buf + offset, buf_size - offset, "desktop=%s ", r->consequence.desktop);
    if (r->consequence.has_state) {
      const char *state_str = "unknown";
      switch (r->consequence.state) {
        case STATE_TILED: state_str = "tiled"; break;
        case STATE_FLOATING: state_str = "floating"; break;
        case STATE_FULLSCREEN: state_str = "fullscreen"; break;
        case STATE_PSEUDO_TILED: state_str = "pseudo_tiled"; break;
      }
      offset += snprintf(buf + offset, buf_size - offset, "state=%s ", state_str);
    }
    if (r->consequence.has_follow)
      offset += snprintf(buf + offset, buf_size - offset, "follow=%s ", r->consequence.follow ? "on" : "off");
    if (r->consequence.has_focus)
      offset += snprintf(buf + offset, buf_size - offset, "focus=%s ", r->consequence.focus ? "on" : "off");
    if (r->consequence.has_manage)
      offset += snprintf(buf + offset, buf_size - offset, "manage=%s ", r->consequence.manage ? "on" : "off");
    if (r->consequence.has_locked)
      offset += snprintf(buf + offset, buf_size - offset, "locked=%s ", r->consequence.locked ? "on" : "off");
    if (r->consequence.has_hidden)
      offset += snprintf(buf + offset, buf_size - offset, "hidden=%s ", r->consequence.hidden ? "on" : "off");
    if (r->consequence.has_sticky)
      offset += snprintf(buf + offset, buf_size - offset, "sticky=%s ", r->consequence.sticky ? "on" : "off");

    offset += snprintf(buf + offset, buf_size - offset, "\n");

    r = r->next;
    idx++;
  }

  if (idx == 0) {
    snprintf(buf, buf_size, "No rules defined\n");
  }
}

static bool match_string(const char *pattern, const char *value) {
  if (pattern == NULL || pattern[0] == '\0') return true;
  if (value == NULL || value[0] == '\0') return false;
  return strcmp(pattern, value) == 0;
}

rule_consequence_t *find_matching_rule(const char *app_id, const char *title) {
  rule_t *r = rule_head;
  rule_t *matched = NULL;

  while (r != NULL) {
    bool app_id_matches = match_string(r->match.app_id, app_id);
    bool title_matches = match_string(r->match.title, title);

    if (app_id_matches && title_matches) {
      matched = r;
      break;
    }
    r = r->next;
  }

  if (matched == NULL) return NULL;

  if (matched->match.one_shot)
    remove_rule(matched);

  return &matched->consequence;
}
