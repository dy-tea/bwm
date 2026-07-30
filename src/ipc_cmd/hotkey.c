#include "config.h"
#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

void ipc_cmd_hotkey(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "hotkey: usage: list | <modifiers+key> <command> [args...]\n");
		return;
	}

	if (streq("list", args[0])) {
		char buf[DOORS_BUFSIZ];
		int offset = 0;
		for (size_t i = 0; i < num_keybinds; i++) {
			keybind_t *kb = &keybinds[i];

			char mods[64] = {0};
			if (kb->modifiers & WLR_MODIFIER_LOGO)
				strcat(mods, "super+");
			if (kb->modifiers & WLR_MODIFIER_ALT)
				strcat(mods, "alt+");
			if (kb->modifiers & WLR_MODIFIER_CTRL)
				strcat(mods, "ctrl+");
			if (kb->modifiers & WLR_MODIFIER_SHIFT)
				strcat(mods, "shift+");
			if (mods[0])
				mods[strlen(mods) - 1] = '\0';

			char key_name[64] = {0};
			if (kb->use_keycode)
				snprintf(key_name, sizeof(key_name), "keycode_%u", kb->keycode);
			else
				xkb_keysym_get_name(kb->keysym, key_name, sizeof(key_name));

			const char *action_label;
			char action_buf[MAXLEN * 2 + 32];
			if (kb->action == BIND_EXTERNAL)
				action_label = kb->external_cmd;
			else
				action_label = bind_action_name(kb->action);

			if (kb->submap_name[0])
				snprintf(action_buf, sizeof(action_buf), "%s [submap: %s]", action_label, kb->submap_name);
			else
				snprintf(action_buf, sizeof(action_buf), "%s", action_label);

			offset += snprintf(buf + offset, sizeof(buf) - offset, "%zu\t%s\t%s\t%s\n", i,
				mods[0] ? mods : "(none)", key_name, action_buf);
		}
		offset += snprintf(buf + offset, sizeof(buf) - offset, "total: %zu keybinds\n", num_keybinds);
		send_success(client_fd, buf);
		return;
	}

	if (num < 2) {
		send_failure(client_fd, "hotkey: usage: list | <modifiers+key> <command> [args...]\n");
		return;
	}

	char hotkey_str[MAXLEN];
	snprintf(hotkey_str, sizeof(hotkey_str), "%s", args[0]);

	uint32_t modifiers = 0;
	xkb_keysym_t keysym = XKB_KEY_NoSymbol;
	uint32_t keycode = 0;
	bool use_keycode = false;

	char *plus = strrchr(hotkey_str, '+');
	if (plus) {
		*plus = '\0';
		modifiers = parse_modifiers(hotkey_str);
		char *key_part = plus + 1;
		while (*key_part == ' ')
			key_part++;
		keysym = parse_keysym(key_part);
		keycode = parse_keycode(key_part);
		if (keycode > 0)
			use_keycode = true;
	} else {
		keysym = parse_keysym(args[0]);
		keycode = parse_keycode(args[0]);
		if (keycode > 0)
			use_keycode = true;
	}

	if (keysym == XKB_KEY_NoSymbol && keycode == 0) {
		send_failure(client_fd, "hotkey: unknown keysym\n");
		return;
	}

	char cmd[MAXLEN] = {0};
	int offset = 0;
	for (int i = 1; i < num; i++) {
		if (i > 1)
			offset += snprintf(cmd + offset, sizeof(cmd) - offset, " ");
		offset += snprintf(cmd + offset, sizeof(cmd) - offset, "%s", args[i]);
	}

	int desktop_index = 0;
	char submap_name[MAXLEN];
	submap_name[0] = '\0';
	bind_action_t action = parse_action(cmd, &desktop_index, submap_name);

	add_keybind(modifiers, keysym, keycode, use_keycode, action, desktop_index,
		(action == BIND_EXTERNAL || action == BIND_GLOBAL_SHORTCUT) ? cmd : NULL,
		submap_name[0] ? submap_name : NULL);

	send_success(client_fd, "hotkey added\n");
}
