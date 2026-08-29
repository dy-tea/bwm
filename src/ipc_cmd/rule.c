#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "rule.h"
#include <stdlib.h>
#include <string.h>

void ipc_cmd_rule(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "rule: missing arguments\n");
		return;
	}

	char *subcmd = *args;

	if (streq("-a", subcmd) || streq("--add", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "rule -a: missing app_id\n");
			return;
		}

		args++;
		num--;

		rule_t *r = make_rule();
		if (!r) {
			send_failure(client_fd, "rule: failed to create rule\n");
			return;
		}

		char *app_id = NULL;
		char *title = NULL;
		r->consequence.has = RULE_TYPE_FOLLOW | RULE_TYPE_FOCUS | RULE_TYPE_MANAGE;
		r->consequence.flags = RULE_TYPE_FOLLOW | RULE_TYPE_FOCUS | RULE_TYPE_MANAGE;

		while (num > 0) {
			char *arg = *args;

			if (strncmp(arg, "title=", 6) == 0) {
				title = arg + 6;
				strncpy(r->match.title, title, MAXLEN - 1);
				r->match.title[MAXLEN - 1] = '\0';
			} else if (strncmp(arg, "tag=", 4) == 0) {
				strncpy(r->match.tag, arg + 4, MAXLEN - 1);
				r->match.tag[MAXLEN - 1] = '\0';
			} else if (strncmp(arg, "app_id=", 7) == 0) {
				app_id = arg + 7;
				strncpy(r->match.app_id, app_id, MAXLEN - 1);
				r->match.app_id[MAXLEN - 1] = '\0';
			} else if (arg[0] != '-' && app_id == NULL && strchr(arg, '=') == NULL) {
				app_id = arg;
				strncpy(r->match.app_id, app_id, MAXLEN - 1);
				r->match.app_id[MAXLEN - 1] = '\0';
			} else if (streq("state=tiled", arg)) {
				r->consequence.state = STATE_TILED;
				r->consequence.has |= RULE_TYPE_STATE;
			} else if (streq("state=floating", arg)) {
				r->consequence.state = STATE_FLOATING;
				r->consequence.has |= RULE_TYPE_STATE;
			} else if (streq("state=fullscreen", arg)) {
				r->consequence.state = STATE_FULLSCREEN;
				r->consequence.has |= RULE_TYPE_STATE;
			} else if (streq("state=pseudo_tiled", arg)) {
				r->consequence.state = STATE_PSEUDO_TILED;
				r->consequence.has |= RULE_TYPE_STATE;
			} else if (streq("desktop=^", arg) || (strlen(arg) > 8 && strncmp(arg, "desktop=", 8) == 0)) {
				char *desk = arg + 8;
				strncpy(r->consequence.desktop, desk, SMALEN - 1);
				r->consequence.desktop[SMALEN - 1] = '\0';
				r->consequence.has |= RULE_TYPE_DESKTOP;
			} else if (streq("follow=on", arg)) {
				r->consequence.flags |= RULE_TYPE_FOLLOW;
				r->consequence.has |= RULE_TYPE_FOLLOW;
			} else if (streq("follow=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_FOLLOW;
				r->consequence.has |= RULE_TYPE_FOLLOW;
			} else if (streq("focus=on", arg)) {
				r->consequence.flags |= RULE_TYPE_FOCUS;
				r->consequence.has |= RULE_TYPE_FOCUS;
			} else if (streq("focus=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_FOCUS;
				r->consequence.has |= RULE_TYPE_FOCUS;
			} else if (streq("manage=on", arg)) {
				r->consequence.flags |= RULE_TYPE_MANAGE;
				r->consequence.has |= RULE_TYPE_MANAGE;
			} else if (streq("manage=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_MANAGE;
				r->consequence.has |= RULE_TYPE_MANAGE;
			} else if (streq("locked=on", arg)) {
				r->consequence.flags |= RULE_TYPE_LOCKED;
				r->consequence.has |= RULE_TYPE_LOCKED;
			} else if (streq("locked=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_LOCKED;
				r->consequence.has |= RULE_TYPE_LOCKED;
			} else if (streq("hidden=on", arg)) {
				r->consequence.flags |= RULE_TYPE_HIDDEN;
				r->consequence.has |= RULE_TYPE_HIDDEN;
			} else if (streq("hidden=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_HIDDEN;
				r->consequence.has |= RULE_TYPE_HIDDEN;
			} else if (streq("sticky=on", arg)) {
				r->consequence.flags |= RULE_TYPE_STICKY;
				r->consequence.has |= RULE_TYPE_STICKY;
			} else if (streq("sticky=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_STICKY;
				r->consequence.has |= RULE_TYPE_STICKY;
			} else if (streq("one_shot", arg)) {
				r->match.one_shot = true;
			} else if (strncmp(arg, "scroller_proportion=", 20) == 0) {
				float val = atof(arg + 20);
				if (val > 0.0f && val <= 1.0f) {
					r->consequence.scroller_proportion = val;
					r->consequence.has |= RULE_TYPE_SCROLLER_PROPORTION;
				} else {
					send_failure(client_fd, "scroller_proportion must be between 0.0 and 1.0");
					return;
				}
			} else if (strncmp(arg, "scroller_proportion_single=", 27) == 0) {
				float val = atof(arg + 27);
				if (val > 0.0f && val <= 1.0f) {
					r->consequence.scroller_proportion_single = val;
					r->consequence.has |= RULE_TYPE_SCROLLER_PROPORTION_SINGLE;
				} else {
					send_failure(client_fd, "scroller_proportion_single must be between 0.0 and 1.0");
					return;
				}
			} else if (streq("blur=on", arg)) {
				r->consequence.flags |= RULE_TYPE_BLUR;
				r->consequence.has |= RULE_TYPE_BLUR;
			} else if (streq("blur=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_BLUR;
				r->consequence.has |= RULE_TYPE_BLUR;
			} else if (streq("mica=on", arg)) {
				r->consequence.flags |= RULE_TYPE_MICA;
				r->consequence.has |= RULE_TYPE_MICA;
			} else if (streq("mica=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_MICA;
				r->consequence.has |= RULE_TYPE_MICA;
			} else if (streq("acrylic=on", arg)) {
				r->consequence.flags |= RULE_TYPE_ACRYLIC;
				r->consequence.has |= RULE_TYPE_ACRYLIC;
			} else if (streq("acrylic=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_ACRYLIC;
				r->consequence.has |= RULE_TYPE_ACRYLIC;
			} else if (strncmp("border_radius=", arg, 14) == 0) {
				r->consequence.border_radius = atof(arg + 14);
				r->consequence.has |= RULE_TYPE_BORDER_RADIUS;
			} else if (streq("shadow=on", arg)) {
				r->consequence.flags |= RULE_TYPE_SHADOW;
				r->consequence.has |= RULE_TYPE_SHADOW;
			} else if (streq("shadow=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_SHADOW;
				r->consequence.has |= RULE_TYPE_SHADOW;
			} else if (streq("block_out_from_screenshare=on", arg)) {
				r->consequence.flags |= RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE;
				r->consequence.has |= RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE;
			} else if (streq("block_out_from_screenshare=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE;
				r->consequence.has |= RULE_TYPE_BLOCK_OUT_FROM_SCREENSHARE;
			} else if (streq("allow_tearing=on", arg)) {
				r->consequence.flags |= RULE_TYPE_ALLOW_TEARING;
				r->consequence.has |= RULE_TYPE_ALLOW_TEARING;
			} else if (streq("allow_tearing=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_ALLOW_TEARING;
				r->consequence.has |= RULE_TYPE_ALLOW_TEARING;
			} else if (streq("shortcuts_inhibitor=on", arg)) {
				r->consequence.flags |= RULE_TYPE_SHORTCUTS_INHIBITOR;
				r->consequence.has |= RULE_TYPE_SHORTCUTS_INHIBITOR;
			} else if (streq("shortcuts_inhibitor=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_SHORTCUTS_INHIBITOR;
				r->consequence.has |= RULE_TYPE_SHORTCUTS_INHIBITOR;
			} else if (streq("animations_disable=on", arg)) {
				r->consequence.flags |= RULE_TYPE_ANIM_DISABLE;
				r->consequence.has |= RULE_TYPE_ANIM_DISABLE;
			} else if (streq("animations_disable=off", arg)) {
				r->consequence.flags &= ~RULE_TYPE_ANIM_DISABLE;
				r->consequence.has |= RULE_TYPE_ANIM_DISABLE;
			} else if (strncmp("render_unfocused_fps=", arg, 21) == 0) {
				int val = atoi(arg + 21);
				if (val >= 0 && val <= 1000) {
					r->consequence.render_unfocused_fps = val;
					r->consequence.has |= RULE_TYPE_RENDER_UNFOCUSED_FPS;
				} else {
					send_failure(client_fd, "render_unfocused_fps must be between 0 and 1000");
					return;
				}
			} else if (strncmp("opacity=", arg, 8) == 0) {
				float val = atof(arg + 8);
				if (val >= 0.0f && val <= 1.0f) {
					r->consequence.has |= RULE_TYPE_OPACITY;
					r->consequence.opacity = val;
				} else {
					send_failure(client_fd, "opacity must be between 0.0 and 1.0 inclusive");
					return;
				}
			}

			args++;
			num--;
		}

		if (!app_id && !title && r->match.tag[0] == '\0') {
			free(r);
			send_failure(client_fd, "rule -a: must specify app_id, title, or tag\n");
			return;
		}

		add_rule(r);
		send_success(client_fd, "rule added\n");
	} else if (streq("-r", subcmd) || streq("--remove", subcmd)) {
		if (num < 2) {
			send_failure(client_fd, "rule -r: missing index\n");
			return;
		}
		args++;
		int idx = atoi(*args);
		if (remove_rule_by_index(idx))
			send_success(client_fd, "rule removed\n");
		else
			send_failure(client_fd, "rule -r: invalid index\n");
	} else if (streq("-l", subcmd) || streq("--list", subcmd)) {
		char buf[DOORS_BUFSIZ];
		list_rules(buf, sizeof(buf));
		send_success(client_fd, buf);
	} else {
		send_failure(client_fd, "rule: unknown subcommand (use -a, -r, or -l)\n");
	}
}
