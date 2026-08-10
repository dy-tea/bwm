#include "animation.h"
#include "bezier.h"
#include "config.h"
#include "effects.h"
#include "idle_power.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include "master_stack.h"
#include "output.h"
#include "realtime.h"
#include "scroller.h"
#include "server.h"
#include "spring.h"
#include "tabs.h"
#include "text.h"
#include "toplevel.h"
#include "transaction.h"
#include "tree.h"
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

static void tabs_rebuild_all(void) {
	for (output_t *m = mon_head; m; m = m->next)
		for (desktop_t *d = m->desk; d; d = d->next)
			if (d->root)
				tabs_rebuild(d->root);
}

void ipc_cmd_config(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "config: Missing arguments\n");
		return;
	}

	if (streq("border_width", *args)) {
		if (num >= 2) {
			int val = atoi(args[1]);
			border_width = val;
			transaction_commit_dirty();
			send_success(client_fd, "border_width set\n");
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%d\n", border_width);
			send_success(client_fd, buf);
		}
	} else if (streq("window_gap", *args)) {
		if (num >= 2) {
			int val = atoi(args[1]);
			window_gap = val;
			for (output_t *m = mon_head; m != NULL; m = m->next)
				for (desktop_t *d = m->desk_head; d != NULL; d = d->next)
					d->window_gap = window_gap;
			transaction_commit_dirty();
			send_success(client_fd, "window_gap set\n");
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%d\n", window_gap);
			send_success(client_fd, buf);
		}
	} else if (streq("single_monocle", *args)) {
		ipc_handle_bool(args, num, client_fd, &single_monocle, IPC_FLAG_COMMIT);
	} else if (streq("borderless_monocle", *args)) {
		ipc_handle_bool(args, num, client_fd, &borderless_monocle, IPC_FLAG_COMMIT);
	} else if (streq("borderless_singleton", *args)) {
		ipc_handle_bool(args, num, client_fd, &borderless_singleton, IPC_FLAG_COMMIT);
	} else if (streq("smart_gaps", *args)) {
		ipc_handle_bool(args, num, client_fd, &smart_gaps, IPC_FLAG_COMMIT);
	} else if (streq("smart_borders", *args)) {
		ipc_handle_bool(args, num, client_fd, &smart_borders, IPC_FLAG_COMMIT);
	} else if (streq("respect_tiled_min_size", *args)) {
		ipc_handle_bool(args, num, client_fd, &respect_tiled_min_size, IPC_FLAG_COMMIT);
	} else if (streq("focus_wrapping", *args)) {
		ipc_handle_bool(args, num, client_fd, &focus_wrapping, IPC_FLAG_COMMIT);
	} else if (streq("focus_on_activate", *args)) {
		if (num >= 2) {
			if (strcmp(args[1], "focus") == 0)
				focus_on_activate = FOCUS_ON_ACTIVATE_FOCUS;
			else if (strcmp(args[1], "none") == 0)
				focus_on_activate = FOCUS_ON_ACTIVATE_NONE;
			else if (strcmp(args[1], "smart") == 0)
				focus_on_activate = FOCUS_ON_ACTIVATE_SMART;
			else if (strcmp(args[1], "urgent") == 0)
				focus_on_activate = FOCUS_ON_ACTIVATE_URGENT;
			else {
				send_failure(client_fd,
					"config focus_on_activate: expected \"focus\", \"none\", \"smart\", or \"urgent\"\n");
				return;
			}
			transaction_commit_dirty();
			send_success(client_fd, "focus_on_activate set\n");
		} else {
			const char *mode = "focus";
			if (focus_on_activate == FOCUS_ON_ACTIVATE_NONE)
				mode = "none";
			else if (focus_on_activate == FOCUS_ON_ACTIVATE_SMART)
				mode = "smart";
			else if (focus_on_activate == FOCUS_ON_ACTIVATE_URGENT)
				mode = "urgent";
			char buf[64];
			snprintf(buf, sizeof(buf), "%s\n", mode);
			send_success(client_fd, buf);
		}
	} else if (streq("hide_lone_tab", *args)) {
		ipc_handle_bool(args, num, client_fd, &hide_lone_tab, IPC_FLAG_COMMIT);
	} else if (streq("gapless_monocle", *args)) {
		ipc_handle_bool(args, num, client_fd, &gapless_monocle, IPC_FLAG_COMMIT);
	} else if (streq("decoration_mode", *args)) {
		if (num >= 2) {
			if (strcmp(args[1], "none") == 0)
				decoration_mode = DECORATION_NONE;
			else if (strcmp(args[1], "tabs") == 0)
				decoration_mode = DECORATION_TABS;
			else if (strcmp(args[1], "always") == 0)
				decoration_mode = DECORATION_ALWAYS;
			else if (strcmp(args[1], "csd") == 0)
				decoration_mode = DECORATION_CSD;
			else {
				send_failure(client_fd,
					"config decoration_mode: expected \"none\", \"tabs\", \"always\", or \"csd\"\n");
				return;
			}
			tabs_rebuild_all();

			// refresh decor
			toplevel_t *tl;
			wl_list_for_each(tl, &server.toplevels, link)
				toplevel_apply_decoration_mode(tl);

			transaction_commit_dirty();
			send_success(client_fd, "decoration_mode set\n");
		} else {
			const char *mode_str = "";
			switch (decoration_mode) {
			case DECORATION_NONE:
				mode_str = "none\n";
				break;
			case DECORATION_TABS:
				mode_str = "tabs\n";
				break;
			case DECORATION_ALWAYS:
				mode_str = "always\n";
				break;
			case DECORATION_CSD:
				mode_str = "csd\n";
				break;
			}
			send_success(client_fd, mode_str);
		}
	} else if (streq("enable_animations", *args)) {
		ipc_handle_bool(args, num, client_fd, &enable_animations, IPC_FLAG_NONE);
	} else if (streq("workspace_anim_direction", *args)) {
		if (num >= 2) {
			if (streq(args[1], "vertical")) {
				workspace_anim_direction = WORKSPACE_ANIM_VERTICAL;
				send_success(client_fd, "workspace_anim_direction set to vertical\n");
			} else if (streq(args[1], "horizontal")) {
				workspace_anim_direction = WORKSPACE_ANIM_HORIZONTAL;
				send_success(client_fd, "workspace_anim_direction set to horizontal\n");
			} else {
				send_failure(client_fd, "workspace_anim_direction: must be 'vertical' or 'horizontal'\n");
			}
		} else {
			send_success(client_fd,
				workspace_anim_direction == WORKSPACE_ANIM_VERTICAL ? "vertical\n" : "horizontal\n");
		}
	} else if (streq("workspace_anim_slide_up", *args)) {
		ipc_handle_bool(args, num, client_fd, &workspace_anim_slide_up, IPC_FLAG_NONE);
	} else if (streq("edge_scroller_pointer_focus", *args)) {
		ipc_handle_bool(args, num, client_fd, &edge_scroller_pointer_focus, IPC_FLAG_NONE);
	} else if (args[0][0] == 't' && strncmp(*args, "tab_color_", 10) == 0) {
		typedef struct {
			const char *name;
			float *color;
		} tab_cfg_t;
		static const tab_cfg_t tab_colors[] = {
			{"bar_bg", color_bar_bg},
			{"bg", color_tab_bg},
			{"bg_active", color_tab_bg_active},
			{"text", color_tab_text},
			{"text_active", color_tab_text_active},
			{"sep", color_tab_sep},
		};
		const char *suffix = *args + 10;
		for (size_t i = 0; i < sizeof(tab_colors) / sizeof(tab_colors[0]); i++) {
			if (streq(suffix, tab_colors[i].name)) {
				if (num >= 2) {
					float rgba[4];
					if (ipc_parse_color_float(args[1], rgba)) {
						memcpy(tab_colors[i].color, rgba, sizeof(rgba));
						tabs_rebuild_all();
						char msg[128];
						snprintf(msg, sizeof(msg), "%s set\n", *args);
						send_success(client_fd, msg);
					} else {
						char msg[128];
						snprintf(msg, sizeof(msg), "config %s: expected \"R G B [A]\"\n", *args);
						send_failure(client_fd, msg);
					}
				} else {
					char buf[128];
					ipc_format_color_float(buf, sizeof(buf), tab_colors[i].color);
					send_success(client_fd, buf);
				}
				return;
			}
		}
		send_failure(client_fd, "config: unknown tab_color setting\n");
	} else if (streq("text_font", *args)) {
		if (num >= 2) {
			snprintf(text_font, sizeof(text_font), "%s", args[1]);
			tabs_rebuild_all();
			send_success(client_fd, "text_font set\n");
		} else {
			char buf[256];
			snprintf(buf, sizeof(buf), "%s\n", text_font);
			send_success(client_fd, buf);
		}
	} else if (streq("text_height", *args)) {
		if (num >= 2) {
			int val = atoi(args[1]);
			if (val > 0) {
				text_height = val;
				tabs_rebuild_all();
				send_success(client_fd, "text_height set\n");
			} else {
				send_failure(client_fd, "config text_height: value must be > 0\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%d\n", text_height);
			send_success(client_fd, buf);
		}
	} else if (streq("scroller_default_proportion", *args)) {
		ipc_handle_float(args, num, client_fd, &scroller_default_proportion, IPC_FLAG_NONE, 0.1f, 1.0f,
			"%.2f\n", NULL);
	} else if (streq("scroller_proportion_preset", *args)) {
		if (num >= 2) {
			char *value = args[1];
			int count = 1;
			for (char *p = value; *p; p++)
				if (*p == ',')
					count++;

			if (scroller_proportion_preset)
				free(scroller_proportion_preset);

			scroller_proportion_preset = malloc(count * sizeof(float));
			if (!scroller_proportion_preset) {
				send_failure(client_fd, "memory allocation failed\n");
				return;
			}

			char *token = strtok(value, ",");
			int i = 0;
			while (token && i < count) {
				float val = atof(token);
				if (val < 0.1f)
					val = 0.1f;
				if (val > 1.0f)
					val = 1.0f;
				scroller_proportion_preset[i++] = val;
				token = strtok(NULL, ",");
			}
			scroller_proportion_preset_count = i;

			send_success(client_fd, "scroller_proportion_preset set\n");
		} else {
			char buf[512];
			int offset = 0;
			for (int i = 0; i < scroller_proportion_preset_count && offset < 500; i++) {
				offset += snprintf(buf + offset, sizeof(buf) - offset, "%.2f%s", scroller_proportion_preset[i],
					i < scroller_proportion_preset_count - 1 ? "," : "\n");
			}
			send_success(client_fd, buf);
		}
	} else if (streq("scroller_default_proportion_single", *args)) {
		ipc_handle_float(args, num, client_fd, &scroller_default_proportion_single, IPC_FLAG_NONE, 0.1f,
			1.0f, "%.2f\n", NULL);
	} else if (streq("scroller_focus_center", *args)) {
		ipc_handle_bool(args, num, client_fd, &scroller_focus_center, IPC_FLAG_NONE);
	} else if (streq("scroller_prefer_center", *args)) {
		ipc_handle_bool(args, num, client_fd, &scroller_prefer_center, IPC_FLAG_NONE);
	} else if (streq("scroller_prefer_overspread", *args)) {
		ipc_handle_bool(args, num, client_fd, &scroller_prefer_overspread, IPC_FLAG_NONE);
	} else if (streq("scroller_ignore_proportion_single", *args)) {
		ipc_handle_bool(args, num, client_fd, &scroller_ignore_proportion_single, IPC_FLAG_NONE);
	} else if (streq("scroller_structs", *args)) {
		ipc_handle_int(args, num, client_fd, &scroller_structs, IPC_FLAG_NONE, 0, 1000000, NULL);
	} else if (streq("focus_follows_pointer", *args) || streq("focus_follows_mouse", *args)) {
		if (num >= 2) {
			if (strcmp(args[1], "no") == 0 || strcmp(args[1], "false") == 0)
				focus_follows_mouse = FOLLOWS_NO;
			else if (strcmp(args[1], "yes") == 0 || strcmp(args[1], "true") == 0)
				focus_follows_mouse = FOLLOWS_YES;
			else if (strcmp(args[1], "always") == 0)
				focus_follows_mouse = FOLLOWS_ALWAYS;
			else {
				send_failure(client_fd,
					"config focus_follows_pointer: expected \"no\", \"yes\", or \"always\"\n");
				return;
			}
			transaction_commit_dirty();
			send_success(client_fd, "focus_follows_pointer set\n");
		} else {
			const char *mode = "no";
			if (focus_follows_mouse == FOLLOWS_YES)
				mode = "yes";
			else if (focus_follows_mouse == FOLLOWS_ALWAYS)
				mode = "always";
			char buf[64];
			snprintf(buf, sizeof(buf), "%s\n", mode);
			send_success(client_fd, buf);
		}
	} else if (streq("pointer_follows_focus", *args)) {
		ipc_handle_bool(args, num, client_fd, &pointer_follows_focus, IPC_FLAG_COMMIT);
	} else if (streq("split_ratio", *args)) {
		if (num >= 2) {
			double val = atof(args[1]);
			if (val > 0 && val < 1) {
				split_ratio = val;
				transaction_commit_dirty();
				send_success(client_fd, "split_ratio set\n");
			} else {
				send_failure(client_fd, "config split_ratio: invalid value\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%f\n", split_ratio);
			send_success(client_fd, buf);
		}
	} else if (streq("top_padding", *args)) {
		ipc_handle_int(args, num, client_fd, &padding.top, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
	} else if (streq("right_padding", *args)) {
		ipc_handle_int(args, num, client_fd, &padding.right, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
	} else if (streq("bottom_padding", *args)) {
		ipc_handle_int(args, num, client_fd, &padding.bottom, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
	} else if (streq("left_padding", *args)) {
		ipc_handle_int(args, num, client_fd, &padding.left, IPC_FLAG_COMMIT, INT_MIN, INT_MAX, NULL);
	} else if (streq("normal_border_color", *args)) {
		if (num >= 2) {
			strncpy(normal_border_color, args[1], sizeof(normal_border_color) - 1);
			normal_border_color[sizeof(normal_border_color) - 1] = '\0';
			refresh_border_colors();
			transaction_commit_dirty();
			send_success(client_fd, "normal_border_color set\n");
		} else {
			send_success(client_fd, normal_border_color);
			send_success(client_fd, "\n");
		}
	} else if (streq("active_border_color", *args)) {
		if (num >= 2) {
			strncpy(active_border_color, args[1], sizeof(active_border_color) - 1);
			active_border_color[sizeof(active_border_color) - 1] = '\0';
			refresh_border_colors();
			transaction_commit_dirty();
			send_success(client_fd, "active_border_color set\n");
		} else {
			send_success(client_fd, active_border_color);
			send_success(client_fd, "\n");
		}
	} else if (streq("focused_border_color", *args)) {
		if (num >= 2) {
			strncpy(focused_border_color, args[1], sizeof(focused_border_color) - 1);
			focused_border_color[sizeof(focused_border_color) - 1] = '\0';
			refresh_border_colors();
			transaction_commit_dirty();
			send_success(client_fd, "focused_border_color set\n");
		} else {
			send_success(client_fd, focused_border_color);
			send_success(client_fd, "\n");
		}
	} else if (streq("presel_feedback_color", *args)) {
		if (num >= 2) {
			strncpy(presel_feedback_color, args[1], sizeof(presel_feedback_color) - 1);
			presel_feedback_color[sizeof(presel_feedback_color) - 1] = '\0';
			transaction_commit_dirty();
			send_success(client_fd, "presel_feedback_color set\n");
		} else {
			send_success(client_fd, presel_feedback_color);
			send_success(client_fd, "\n");
		}
	} else if (streq("tiling_drag_indicator_color", *args)) {
		if (num >= 2) {
			strncpy(tiling_drag_indicator_color, args[1], sizeof(tiling_drag_indicator_color) - 1);
			tiling_drag_indicator_color[sizeof(tiling_drag_indicator_color) - 1] = '\0';
			send_success(client_fd, "tiling_drag_indicator_color set\n");
		} else {
			send_success(client_fd, tiling_drag_indicator_color);
			send_success(client_fd, "\n");
		}
	} else if (streq("normal_border_gradient", *args) || streq("active_border_gradient",
			*args) || streq("focused_border_gradient", *args) || streq("normal_border_gradient2",
			*args) || streq("active_border_gradient2", *args) || streq("focused_border_gradient2",
			*args) || streq("normal_border_gradient_lerp", *args) || streq("active_border_gradient_lerp",
			*args) || streq("focused_border_gradient_lerp", *args)) {
		border_theme_t *bt;
		if (args[0][0] == 'n')
			bt = &normal_border_theme;
		else if (args[0][0] == 'a')
			bt = &active_border_theme;
		else
			bt = &focused_border_theme;

		bool is_gradient2 = (strstr(*args, "gradient2") != NULL);
		bool is_lerp = (strstr(*args, "lerp") != NULL);

		if (is_lerp) {
			if (num >= 2) {
				bt->gradient_lerp = (float)atof(args[1]);
				if (bt->gradient_lerp < 0.0f)
					bt->gradient_lerp = 0.0f;
				if (bt->gradient_lerp > 1.0f)
					bt->gradient_lerp = 1.0f;
				refresh_border_colors();
				transaction_commit_dirty();
				send_success(client_fd, "border gradient lerp set\n");
			} else {
				char buf[32];
				snprintf(buf, sizeof(buf), "%f\n", bt->gradient_lerp);
				send_success(client_fd, buf);
			}
		} else {
			float *grad = is_gradient2 ? bt->gradient2 : bt->gradient;
			int *gcount = is_gradient2 ? &bt->gradient2_count : &bt->gradient_count;
			float *gangle = is_gradient2 ? &bt->gradient2_angle : &bt->gradient_angle;

			if (num >= 2) {
				char joined[512] = "";
				for (int i = 1; i < num; i++) {
					if (i > 1)
						strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
					strncat(joined, args[i], sizeof(joined) - strlen(joined) - 1);
				}

				if (streq(joined, "clear")) {
					*gcount = 0;
					*gangle = 0.0f;
				} else {
					ipc_parse_gradient(joined, grad, gcount, gangle);
				}

				refresh_border_colors();
				transaction_commit_dirty();
				send_success(client_fd, is_gradient2 ? "border gradient2 set\n" : "border gradient set\n");
			} else {
				char buf[512];
				ipc_format_gradient(buf, sizeof(buf), grad, *gcount, *gangle);
				send_success(client_fd, buf);
				send_success(client_fd, "\n");
			}
		}
	} else if (streq("automatic_scheme", *args)) {
		if (num >= 2) {
			if (streq("longest_side", args[1]) || streq("longest-side", args[1])) {
				automatic_scheme = SCHEME_LONGEST_SIDE;
			} else if (streq("alternate", args[1])) {
				automatic_scheme = SCHEME_ALTERNATE;
			} else if (streq("spiral", args[1])) {
				automatic_scheme = SCHEME_SPIRAL;
			} else {
				send_failure(client_fd, "config automatic_scheme: invalid value\n");
				return;
			}
			transaction_commit_dirty();
			send_success(client_fd, "automatic_scheme set\n");
		} else {
			char scheme_buf[64];
			const char *scheme_str = "spiral";
			if (automatic_scheme == SCHEME_LONGEST_SIDE)
				scheme_str = "longest_side";
			else if (automatic_scheme == SCHEME_ALTERNATE)
				scheme_str = "alternate";
			snprintf(scheme_buf, sizeof(scheme_buf), "%s\n", scheme_str);
			send_success(client_fd, scheme_buf);
		}
	} else if (streq("initial_polarity", *args)) {
		if (num >= 2) {
			if (streq("first_child", args[1]) || streq("first-child", args[1])) {
				initial_polarity = FIRST_CHILD;
			} else if (streq("second_child", args[1]) || streq("second-child", args[1])) {
				initial_polarity = SECOND_CHILD;
			} else {
				send_failure(client_fd, "config initial_polarity: invalid value\n");
				return;
			}
			send_success(client_fd, "initial_polarity set\n");
		} else {
			send_success(client_fd, initial_polarity == FIRST_CHILD ? "first_child\n" : "second_child\n");
		}
	} else if (streq("directional_focus_tightness", *args)) {
		ipc_handle_int(args, num, client_fd, &directional_focus_tightness, IPC_FLAG_NONE, 0, 100,
			"invalid value");
	} else if (streq("mapping_events_count", *args)) {
		ipc_handle_int(args, num, client_fd, &mapping_events_count, IPC_FLAG_NONE, 0, 1000000,
			"invalid value");
	} else if (streq("minimize_to_scratchpad", *args)) {
		ipc_handle_bool(args, num, client_fd, &minimize_to_scratchpad, IPC_FLAG_NONE);
	} else if (streq("ignore_ewmh_fullscreen", *args)) {
		ipc_handle_int(args, num, client_fd, &ignore_ewmh_fullscreen, IPC_FLAG_NONE, 0, 2,
			"invalid value (0-2)");
	} else if (streq("click_to_focus", *args)) {
		ipc_handle_bool(args, num, client_fd, &click_to_focus, IPC_FLAG_NONE);
	} else if (streq("record_history", *args)) {
		ipc_handle_bool(args, num, client_fd, &record_history, IPC_FLAG_NONE);
	} else if (streq("allow_tearing", *args)) {
		ipc_handle_bool(args, num, client_fd, &allow_tearing, IPC_FLAG_COMMIT);
	} else if (streq("auto_float_dialogs", *args)) {
		ipc_handle_bool(args, num, client_fd, &auto_float_dialogs, IPC_FLAG_NONE);
	} else if (streq("blur_enabled", *args)) {
		ipc_handle_bool(args, num, client_fd, &blur_enabled, IPC_FLAG_NONE);
	} else if (streq("blur_algorithm", *args)) {
		if (num >= 2) {
			enum blur_algorithm algo = blur_algorithm_from_str(args[1]);
			if (algo == BLUR_ALGORITHM_NONE && strcmp(args[1], "none") != 0) {
				send_failure(client_fd, "config blur_algorithm: unknown algorithm\n");
			} else {
				blur_algorithm = algo;
				send_success(client_fd, "blur_algorithm set\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%s\n", effects_algorithm_to_str(blur_algorithm));
			send_success(client_fd, buf);
		}
	} else if (streq("blur_passes", *args)) {
		ipc_handle_int(args, num, client_fd, &blur_passes, IPC_FLAG_NONE, 1, 10, "value must be 1-10");
	} else if (streq("blur_radius", *args)) {
		if (num >= 2) {
			float val = atof(args[1]);
			if (val > 0.0f) {
				blur_radius = val;
				send_success(client_fd, "blur_radius set\n");
			} else {
				send_failure(client_fd, "config blur_radius: value must be > 0\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%.2f\n", blur_radius);
			send_success(client_fd, buf);
		}
	} else if (streq("blur_full_res", *args)) {
		ipc_handle_bool(args, num, client_fd, &blur_full_res, IPC_FLAG_NONE);
	} else if (streq("blur_offset", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_offset, IPC_FLAG_NONE, 0.0f, 8.0f, "%.3f\n",
			"value must be 0.0-8.0");
	} else if (streq("blur_saturation", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_saturation, IPC_FLAG_NONE, 0.0f, 3.0f, "%.3f\n",
			"value must be 0.0-3.0");
	} else if (streq("refraction_strength", *args)) {
		ipc_handle_float(args, num, client_fd, &refraction_strength, IPC_FLAG_NONE, 0.0f, 30.0f, "%.3f\n",
			"value must be 0.0-30.0");
	} else if (streq("refraction_edge_size_px", *args)) {
		ipc_handle_float(args, num, client_fd, &refraction_edge_size_px, IPC_FLAG_NONE, 0.0f, 400.0f,
			"%.3f\n", "value must be 0.0-400.0");
	} else if (streq("refraction_corner_radius_px", *args)) {
		ipc_handle_float(args, num, client_fd, &refraction_corner_radius_px, IPC_FLAG_NONE, 0.0f, 400.0f,
			"%.3f\n", "value must be 0.0-400.0");
	} else if (streq("refraction_normal_pow", *args)) {
		ipc_handle_float(args, num, client_fd, &refraction_normal_pow, IPC_FLAG_NONE, 0.0f, 8.0f, "%.3f\n",
			"value must be 0.0-8.0");
	} else if (streq("refraction_rgb_fringing", *args)) {
		ipc_handle_float(args, num, client_fd, &refraction_rgb_fringing, IPC_FLAG_NONE, 0.0f, 1.0f,
			"%.6f\n", "value must be 0.0-1.0");
	} else if (streq("refraction_texture_repeat_mode", *args)) {
		if (num >= 2) {
			int val = atoi(args[1]);
			if (val == 0 || val == 1) {
				refraction_texture_repeat_mode = val;
				send_success(client_fd, "refraction_texture_repeat_mode set\n");
			} else {
				send_failure(client_fd,
					"config refraction_texture_repeat_mode: value must be 0 (clamp) or 1 (mirror)\n");
			}
		} else {
			char buf[32];
			snprintf(buf, sizeof(buf), "%d\n", refraction_texture_repeat_mode);
			send_success(client_fd, buf);
		}
	} else if (streq("refraction_offset", *args)) {
		ipc_handle_float(args, num, client_fd, &refraction_offset, IPC_FLAG_NONE, 0.0f, 8.0f, "%.3f\n",
			"value must be 0.0-8.0");
	} else if (streq("blur_downsample", *args)) {
		if (num >= 2) {
			int val = atoi(args[1]);
			if (val >= 1 && val <= 8) {
				blur_downsample = val;
				for (output_t *m = mon_head; m; m = m->next) {
					if (m && m->effects)
						effects_output_resize(m->effects, m->width, m->height, m);
				}
				send_success(client_fd, "blur_downsample set\n");
			} else {
				send_failure(client_fd, "config blur_downsample: value must be 1-8\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%d\n", blur_downsample);
			send_success(client_fd, buf);
		}
	} else if (streq("blur_vibrancy", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_vibrancy, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n",
			"value must be 0.0-1.0");
	} else if (streq("blur_vibrancy_darkness", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_vibrancy_darkness, IPC_FLAG_NONE, 0.0f, 1.0f,
			"%.3f\n", "value must be 0.0-1.0");
	} else if (streq("blur_noise_strength", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_noise_strength, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n",
			"value must be 0.0-1.0");
	} else if (streq("blur_brightness", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_brightness, IPC_FLAG_NONE, 0.5f, 2.0f, "%.3f\n",
			"value must be 0.5-2.0");
	} else if (streq("blur_contrast", *args)) {
		ipc_handle_float(args, num, client_fd, &blur_contrast, IPC_FLAG_NONE, 0.5f, 2.0f, "%.3f\n",
			"value must be 0.5-2.0");
	} else if (streq("mica_enabled", *args)) {
		if (num >= 2) {
			mica_enabled = (strcmp(args[1], "true") == 0);
			for (output_t *output = mon_head; output; output = output->next)
				effects_invalidate_mica(output->effects);
			send_success(client_fd, "mica_enabled set\n");
		} else {
			send_success(client_fd, mica_enabled ? "true\n" : "false\n");
		}
	} else if (streq("mica_tint_strength", *args)) {
		if (num >= 2) {
			float val = (float)atof(args[1]);
			if (val >= 0.0f && val <= 1.0f) {
				mica_tint_strength = val;
				for (output_t *output = mon_head; output; output = output->next)
					effects_invalidate_mica(output->effects);
				send_success(client_fd, "mica_tint_strength set\n");
			} else {
				send_failure(client_fd, "config mica_tint_strength: value must be 0.0-1.0\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%.3f\n", mica_tint_strength);
			send_success(client_fd, buf);
		}
	} else if (streq("mica_tint", *args)) {
		if (num >= 2) {
			float rgba[4];
			if (ipc_parse_color_float(args[1], rgba)) {
				memcpy(mica_tint, rgba, sizeof(rgba));
				for (output_t *output = mon_head; output; output = output->next)
					effects_invalidate_mica(output->effects);
				send_success(client_fd, "mica_tint set\n");
			} else {
				send_failure(client_fd, "config mica_tint: expected \"R G B [A]\"\n");
			}
		} else {
			char buf[128];
			ipc_format_color_float(buf, sizeof(buf), mica_tint);
			send_success(client_fd, buf);
		}
	} else if (streq("acrylic_tint", *args)) {
		if (num >= 2) {
			float rgba[4];
			if (ipc_parse_color_float(args[1], rgba)) {
				memcpy(acrylic_tint, rgba, sizeof(rgba));
				send_success(client_fd, "acrylic_tint set\n");
			} else {
				send_failure(client_fd, "config acrylic_tint: expected \"R G B [A]\"\n");
			}
		} else {
			char buf[128];
			ipc_format_color_float(buf, sizeof(buf), acrylic_tint);
			send_success(client_fd, buf);
		}
	} else if (streq("acrylic_tint_strength", *args)) {
		ipc_handle_float(args, num, client_fd, &acrylic_tint_strength, IPC_FLAG_NONE, 0.0f, 1.0f, "%.3f\n",
			"value must be 0.0-1.0");
	} else if (streq("acrylic_noise_strength", *args)) {
		ipc_handle_float(args, num, client_fd, &acrylic_noise_strength, IPC_FLAG_NONE, 0.0f, 1.0f,
			"%.3f\n", "value must be 0.0-1.0");
	} else if (streq("acrylic_light_anchor", *args)) {
		if (num >= 2) {
			float a, b;
			int n = sscanf(args[1], "%f %f", &a, &b);
			if (n == 2 && a >= -1.0f && a <= 1.0f && b >= -1.0f && b <= 1.0f) {
				acrylic_light_anchor[0] = a;
				acrylic_light_anchor[1] = b;
				send_success(client_fd, "acrylic_light_anchor set\n");
			} else {
				send_failure(client_fd, "config acrylic_light_anchor: values must be -1.0 to 1.0\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%.3f\n", acrylic_light_anchor[0]);
			send_success(client_fd, buf);
		}
	} else if (streq("acrylic_blur_passes", *args)) {
		ipc_handle_int(args, num, client_fd, &acrylic_blur_passes, IPC_FLAG_NONE, 0, 10,
			"value must be 0-10");
	} else if (streq("screen_shader", *args)) {
		if (num >= 2) {
			if (!screen_shader_set(args[1])) {
				send_failure(client_fd,
					"config screen_shader: unknown shader (builtin: none grayscale invert sepia nightlight)\n");
			} else {
				send_success(client_fd, "screen_shader set\n");
			}
		} else {
			char buf[256];
			snprintf(buf, sizeof(buf), "%s\n", screen_shader_get_name());
			send_success(client_fd, buf);
		}
	} else if (streq("screen_shader_file", *args)) {
		if (num >= 2) {
			if (!screen_shader_load_file(args[1])) {
				send_failure(client_fd, "config screen_shader_file: failed to load shader\n");
			} else {
				send_success(client_fd, "screen_shader_file loaded\n");
			}
		} else {
			send_failure(client_fd, "config screen_shader_file: missing path argument\n");
		}
	} else if (streq("screen_shader_enabled", *args)) {
		bool set = ipc_handle_bool(args, num, client_fd, &screen_shader_enabled, IPC_FLAG_NONE);
		if (set && !screen_shader_enabled)
			screen_shader_hide_nodes();
	} else if (streq("animation_bezier", *args)) {
		if (num >= 2) {
			if (bezier_exists(args[1])) {
				animation_set_bezier(args[1]);
				send_success(client_fd, "animation_bezier set\n");
			} else {
				send_failure(client_fd, "config animation_bezier: no such bezier curve\n");
			}
		} else {
			char buf[96];
			snprintf(buf, sizeof(buf), "%s\n", animation_get_bezier());
			send_success(client_fd, buf);
		}
	} else if (streq("animation_duration", *args)) {
		if (num >= 2) {
			int val = atoi(args[1]);
			if (val > 0) {
				animation_set_duration((uint32_t)val);
				send_success(client_fd, "animation_duration set\n");
			} else {
				send_failure(client_fd, "config animation_duration: must be > 0\n");
			}
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "%u\n", animation_get_duration());
			send_success(client_fd, buf);
		}
	} else if (streq("animation", *args)) {
		if (num < 2) {
			send_failure(client_fd,
				"config animation: expected <type> [bezier|duration|spring|enabled] [value]\n");
		} else if (num >= 3 && streq("spring", args[2])) {
			const char *sname = num >= 4 ? args[3] : "";
			if (sname[0] != '\0' && !spring_exists(sname)) {
				send_failure(client_fd, "config animation: no such spring curve\n");
			} else if (animation_set_type_spring(args[1], sname)) {
				send_success(client_fd, "animation type spring set\n");
			} else {
				send_failure(client_fd, "config animation: unknown type\n");
			}
		} else if (num >= 3 && streq("bezier", args[2])) {
			const char *bname = num >= 4 ? args[3] : "";
			if (bname[0] != '\0' && !bezier_exists(bname)) {
				send_failure(client_fd, "config animation: no such bezier curve\n");
			} else if (animation_set_type_config(args[1], bname, 0)) {
				send_success(client_fd, "animation type bezier set\n");
			} else {
				send_failure(client_fd, "config animation: unknown type\n");
			}
		} else if (num >= 3 && streq("duration", args[2])) {
			if (num < 4) {
				send_failure(client_fd, "config animation <type> duration: expected value\n");
			} else {
				int val = atoi(args[3]);
				if (val <= 0) {
					send_failure(client_fd, "config animation <type> duration: must be > 0\n");
				} else if (animation_set_type_config(args[1], NULL, (uint32_t)val)) {
					send_success(client_fd, "animation type duration set\n");
				} else {
					send_failure(client_fd, "config animation: unknown type\n");
				}
			}
		} else if (num >= 3 && streq("enabled", args[2])) {
			if (num < 4) {
				bool enabled = animation_type_get_enabled(args[1]);
				send_success(client_fd, enabled ? "enabled\n" : "disabled\n");
			} else {
				bool val = streq(args[3], "true") || streq(args[3], "1");
				if (animation_type_set_enabled(args[1], val))
					send_success(client_fd, val ? "enabled\n" : "disabled\n");
				else
					send_failure(client_fd, "config animation: unknown type\n");
			}
		} else {
			int idx = animation_type_from_name(args[1]);
			if (idx < 0) {
				send_failure(client_fd, "config animation: unknown type\n");
			} else {
				const char *bname = animation_type_get_bezier(args[1]);
				uint32_t dur = animation_type_get_duration(args[1]);
				const char *sname = animation_type_get_spring(args[1]);
				bool enabled = animation_type_get_enabled(args[1]);
				char buf[256];
				int off = 0;
				if (sname) {
					off = snprintf(buf, sizeof(buf), "spring: %s\n", sname);
				} else {
					off = snprintf(buf, sizeof(buf), "bezier: %s\n", bname ? bname : "(global default)");
				}
				off += snprintf(buf + off, sizeof(buf) - off, "duration: %u\n",
					dur > 0 ? dur : animation_get_duration());
				snprintf(buf + off, sizeof(buf) - off, "enabled: %s\n", enabled ? "true" : "false");
				send_success(client_fd, buf);
			}
		}
	} else if (streq("shadow_size", *args)) {
		ipc_handle_float(args, num, client_fd, &shadow_size, IPC_FLAG_NONE, 0.0f, 100.0f, "%.1f\n",
			"value must be 0-100");
	} else if (streq("shadow_offset_x", *args)) {
		ipc_handle_float(args, num, client_fd, &shadow_offset_x, IPC_FLAG_NONE, -100.0f, 100.0f, "%.1f\n",
			"value must be -100-100");
	} else if (streq("shadow_offset_y", *args)) {
		ipc_handle_float(args, num, client_fd, &shadow_offset_y, IPC_FLAG_NONE, -100.0f, 100.0f, "%.1f\n",
			"value must be -100-100");
	} else if (streq("shadow_color", *args)) {
		if (num >= 2) {
			float rgba[4];
			if (ipc_parse_color_float(args[1], rgba)) {
				memcpy(shadow_color, rgba, sizeof(rgba));
				send_success(client_fd, "shadow_color set\n");
			} else {
				send_failure(client_fd, "config shadow_color: expected \"R G B [A]\"\n");
			}
		} else {
			char buf[128];
			ipc_format_color_float(buf, sizeof(buf), shadow_color);
			send_success(client_fd, buf);
		}
	} else if (streq("render_unfocused_fps", *args)) {
		ipc_handle_int(args, num, client_fd, &render_unfocused_fps, IPC_FLAG_NONE, 1, INT_MAX,
			"value must be >=1");
	} else if (streq("idle_timeout", *args)) {
		if (ipc_handle_int(args, num, client_fd, &idle_timeout, IPC_FLAG_NONE, 0, 86400,
			"value must be 0-86400"))
			idle_power_reset_timer();
	} else if (streq("idle_dpms", *args)) {
		if (ipc_handle_bool(args, num, client_fd, &idle_dpms, IPC_FLAG_NONE))
			idle_power_reset_timer();
	} else if (streq("realtime_scheduling", *args)) {
		ipc_handle_bool(args, num, client_fd, &realtime_scheduling, IPC_FLAG_NONE);
		if (realtime_scheduling)
			set_rr_scheduling();
	} else {
		send_failure(client_fd, "config: unknown setting\n");
	}
}
