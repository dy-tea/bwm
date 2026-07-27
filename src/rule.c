#include "rule.h"
#include "scroller.h"
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
		r->match.tag[0] = '\0';
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
	if (r == NULL)
		return;

	rule_t *prev = NULL;
	rule_t *cur = rule_head;
	while (cur && cur != r) {
		prev = cur;
		cur = cur->next;
	}
	if (cur == NULL)
		return;

	if (prev)
		prev->next = r->next;
	else
		rule_head = r->next;

	if (r == rule_tail)
		rule_tail = prev;

	free(r);
}

bool remove_rule_by_index(int idx) {
	rule_t *prev = NULL;
	rule_t *cur = rule_head;
	for (int i = 0; cur != NULL && i < idx; prev = cur, cur = cur->next, ++i)
		;

	if (cur == NULL)
		return false;

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
		if (r->match.tag[0] != '\0')
			offset += snprintf(buf + offset, buf_size - offset, "tag=%s ", r->match.tag);

		if (r->match.one_shot)
			offset += snprintf(buf + offset, buf_size - offset, "one_shot ");

		offset += snprintf(buf + offset, buf_size - offset, "-> ");

		if (r->consequence.has & RULE_TYPE_DESKTOP)
			offset += snprintf(buf + offset, buf_size - offset, "desktop=%s ", r->consequence.desktop);
		if (r->consequence.has & RULE_TYPE_MONITOR)
			offset += snprintf(buf + offset, buf_size - offset, "monitor=%s ", r->consequence.monitor);
		if (r->consequence.has & RULE_TYPE_STATE) {
			const char *state_str = "unknown";
			switch (r->consequence.state) {
			case STATE_TILED:
				state_str = "tiled";
				break;
			case STATE_FLOATING:
				state_str = "floating";
				break;
			case STATE_FULLSCREEN:
				state_str = "fullscreen";
				break;
			case STATE_PSEUDO_TILED:
				state_str = "pseudo_tiled";
				break;
			}
			offset += snprintf(buf + offset, buf_size - offset, "state=%s ", state_str);
		}
		if (r->consequence.has & RULE_TYPE_FOLLOW)
			offset += snprintf(buf + offset, buf_size - offset, "follow=%s ",
				r->consequence.flags & RULE_TYPE_FOLLOW ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_FOCUS)
			offset += snprintf(buf + offset, buf_size - offset, "focus=%s ",
				r->consequence.flags & RULE_TYPE_FOCUS ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_MANAGE)
			offset += snprintf(buf + offset, buf_size - offset, "manage=%s ",
				r->consequence.flags & RULE_TYPE_MANAGE ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_LOCKED)
			offset += snprintf(buf + offset, buf_size - offset, "locked=%s ",
				r->consequence.flags & RULE_TYPE_LOCKED ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_HIDDEN)
			offset += snprintf(buf + offset, buf_size - offset, "hidden=%s ",
				r->consequence.flags & RULE_TYPE_HIDDEN ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_STICKY)
			offset += snprintf(buf + offset, buf_size - offset, "sticky=%s ",
				r->consequence.flags & RULE_TYPE_STICKY ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_SCROLLER_PROPORTION)
			offset += snprintf(buf + offset, buf_size - offset, "scroller_proportion=%.2f ",
				r->consequence.scroller_proportion);
		if (r->consequence.has & RULE_TYPE_SCROLLER_PROPORTION_SINGLE)
			offset += snprintf(buf + offset, buf_size - offset, "scroller_proportion_single=%.2f ",
				r->consequence.scroller_proportion_single);
		if (r->consequence.has & RULE_TYPE_BLUR)
			offset += snprintf(buf + offset, buf_size - offset, "blur=%s ",
				r->consequence.flags & RULE_TYPE_BLUR ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_MICA)
			offset += snprintf(buf + offset, buf_size - offset, "mica=%s ",
				r->consequence.flags & RULE_TYPE_MICA ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_ACRYLIC)
			offset += snprintf(buf + offset, buf_size - offset, "acrylic=%s ",
				r->consequence.flags & RULE_TYPE_ACRYLIC ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_BORDER_RADIUS)
			offset += snprintf(buf + offset, buf_size - offset, "border_radius=%.1f ",
				r->consequence.border_radius);
		if (r->consequence.has & RULE_TYPE_SHADOW)
			offset += snprintf(buf + offset, buf_size - offset, "shadow=%s ",
				r->consequence.flags & RULE_TYPE_SHADOW ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_ANIM_DISABLE)
			offset += snprintf(buf + offset, buf_size - offset, "animations_disable=%s ",
				r->consequence.flags & RULE_TYPE_ANIM_DISABLE ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE)
			offset += snprintf(buf + offset, buf_size - offset, "block_out_from_screenshare=%s ",
				r->consequence.has & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_ALLOW_TEARING)
			offset += snprintf(buf + offset, buf_size - offset, "allow_tearing=%s ",
				r->consequence.flags & RULE_TYPE_ALLOW_TEARING ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_SHORTCUTS_INHIBITOR)
			offset += snprintf(buf + offset, buf_size - offset, "shortcuts_inhibitor=%s ",
				r->consequence.flags & RULE_TYPE_SHORTCUTS_INHIBITOR ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_RENDER_UNFOCUSED)
			offset += snprintf(buf + offset, buf_size - offset, "render_unfocused=%s ",
				r->consequence.flags & RULE_TYPE_RENDER_UNFOCUSED ? "on" : "off");
		if (r->consequence.has & RULE_TYPE_OPACITY)
			offset += snprintf(buf + offset, buf_size - offset, "opacity=%.1f ", r->consequence.opacity);

		offset += snprintf(buf + offset, buf_size - offset, "\n");

		r = r->next;
		idx++;
	}

	if (idx == 0) {
		snprintf(buf, buf_size, "No rules defined\n");
	}
}

static bool match_string(const char *pattern, const char *value) {
	if (pattern == NULL || pattern[0] == '\0')
		return true;
	if (value == NULL || value[0] == '\0')
		return false;
	return strcmp(pattern, value) == 0;
}

rule_consequence_t *find_matching_rule(const char *app_id, const char *title, const char *tag) {
	static rule_consequence_t merged;
	memset(&merged, 0, sizeof(merged));

	rule_t *r = rule_head;
	while (r != NULL) {
		rule_t *next = r->next;

		bool app_id_matches = match_string(r->match.app_id, app_id);
		bool title_matches = match_string(r->match.title, title);
		bool tag_matches = match_string(r->match.tag, tag);

		if (app_id_matches && title_matches && tag_matches) {
			rule_type_t bits = r->consequence.has;
			merged.has |= bits;
			merged.flags = (merged.flags & ~bits) | (r->consequence.flags & bits);

			if (bits & RULE_TYPE_DESKTOP)
				strncpy(merged.desktop, r->consequence.desktop, SMALEN);
			if (bits & RULE_TYPE_MONITOR)
				strncpy(merged.monitor, r->consequence.monitor, SMALEN);
			if (bits & RULE_TYPE_STATE)
				merged.state = r->consequence.state;
			if (bits & RULE_TYPE_SCROLLER_PROPORTION)
				merged.scroller_proportion = r->consequence.scroller_proportion;
			if (bits & RULE_TYPE_SCROLLER_PROPORTION_SINGLE)
				merged.scroller_proportion_single = r->consequence.scroller_proportion_single;
			if (bits & RULE_TYPE_BORDER_RADIUS)
				merged.border_radius = r->consequence.border_radius;
			if (bits & RULE_TYPE_OPACITY)
				merged.opacity = r->consequence.opacity;

			if (r->match.one_shot)
				remove_rule(r);
		}

		r = next;
	}

	return merged.has ? &merged : NULL;
}

void rule_apply_consequence(node_t *node, client_t *client, const rule_consequence_t *rule) {
	if (!rule)
		return;

	if (rule->has & RULE_TYPE_STATE)
		client->state = rule->state;

	if (rule->has & RULE_TYPE_HIDDEN)
		node->hidden = rule->flags & RULE_TYPE_HIDDEN;
	if (rule->has & RULE_TYPE_STICKY)
		node->sticky = rule->flags & RULE_TYPE_STICKY;
	if (rule->has & RULE_TYPE_LOCKED)
		node->locked = rule->flags & RULE_TYPE_LOCKED;

	if (rule->has & RULE_TYPE_SCROLLER_PROPORTION || rule->has & RULE_TYPE_SCROLLER_PROPORTION_SINGLE)
		scroller_apply_client_rules(client,
			rule->has & RULE_TYPE_SCROLLER_PROPORTION ? rule->scroller_proportion : 0.0f,
			rule->has & RULE_TYPE_SCROLLER_PROPORTION_SINGLE ? rule->scroller_proportion_single : 0.0f);

	if (rule->has & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE)
		client->flags.block_out_from_screenshare = rule->flags & RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE;

	if (rule->has & RULE_TYPE_ALLOW_TEARING) {
		client->flags.allow_tearing = rule->flags & RULE_TYPE_ALLOW_TEARING;
		client->flags.allow_tearing_from_rule = true;
	}

	if (rule->has & RULE_TYPE_RENDER_UNFOCUSED) {
		client->flags.render_unfocused = rule->flags & RULE_TYPE_RENDER_UNFOCUSED;
		client->flags.render_unfocused_from_rule = true;
	}

	if (rule->has & RULE_TYPE_BLUR) {
		client->flags.blur = rule->flags & RULE_TYPE_BLUR;
		client->flags.blur_from_rule = true;
	}

	if (rule->has & RULE_TYPE_MICA)
		client->flags.mica = rule->flags & RULE_TYPE_MICA;

	if (rule->has & RULE_TYPE_ACRYLIC)
		client->flags.acrylic = rule->flags & RULE_TYPE_ACRYLIC;

	if (rule->has & RULE_TYPE_BORDER_RADIUS)
		client->border_radius = rule->border_radius;

	if (rule->has & RULE_TYPE_OPACITY)
		client->opacity = rule->opacity;

	if (rule->has & RULE_TYPE_ANIM_DISABLE)
		client->flags.anim_disabled = rule->flags & RULE_TYPE_ANIM_DISABLE;

	if (rule->has & RULE_TYPE_SHADOW) {
		client->flags.shadow = rule->flags & RULE_TYPE_SHADOW;
		client->shadow_size = shadow_size;
		client->shadow_offset_x = shadow_offset_x;
		client->shadow_offset_y = shadow_offset_y;
		memcpy(client->shadow_color, shadow_color, sizeof(shadow_color));
	}
}
