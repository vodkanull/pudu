#include "pudu.h"
#include <signal.h>

#include "build/default_config.inc"

char *trim(char *str) {
	while (isspace(*str)) str++;
	if (*str == 0) return str;
	char *end = str + strlen(str) - 1;
	while (end > str && isspace(*end)) end--;
	*(end + 1) = 0;
	return str;
}

xkb_keysym_t keysym_from_name(const char *name) {
	if (strcmp(name, "Return") == 0) return XKB_KEY_Return;
	if (strcmp(name, "Escape") == 0) return XKB_KEY_Escape;
	if (strcmp(name, "Tab") == 0) return XKB_KEY_Tab;
	if (strcmp(name, "Space") == 0) return XKB_KEY_space;
	if (strcmp(name, "Left") == 0) return XKB_KEY_Left;
	if (strcmp(name, "Right") == 0) return XKB_KEY_Right;
	if (strcmp(name, "Up") == 0) return XKB_KEY_Up;
	if (strcmp(name, "Down") == 0) return XKB_KEY_Down;
	if (strlen(name) == 1) {
		return xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	}
	return xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
}

uint32_t modifier_from_name(const char *name) {
	if (strcasecmp(name, "Shift") == 0 ||
	    strcasecmp(name, "Shift_L") == 0 ||
	    strcasecmp(name, "Shift_R") == 0 ||
	    strcasecmp(name, "S") == 0) {
		return WLR_MODIFIER_SHIFT;
	}
	if (strcasecmp(name, "Control") == 0 ||
	    strcasecmp(name, "Control_L") == 0 ||
	    strcasecmp(name, "Control_R") == 0 ||
	    strcasecmp(name, "Ctrl") == 0 ||
	    strcasecmp(name, "Ctrl_L") == 0 ||
	    strcasecmp(name, "Ctrl_R") == 0 ||
	    strcasecmp(name, "C") == 0) {
		return WLR_MODIFIER_CTRL;
	}
	if (strcasecmp(name, "Alt") == 0 ||
	    strcasecmp(name, "Alt_L") == 0 ||
	    strcasecmp(name, "Alt_R") == 0 ||
	    strcasecmp(name, "Mod1") == 0 ||
	    strcasecmp(name, "A") == 0) {
		return WLR_MODIFIER_ALT;
	}
	if (strcasecmp(name, "Super") == 0 ||
	    strcasecmp(name, "Super_L") == 0 ||
	    strcasecmp(name, "Super_R") == 0 ||
	    strcasecmp(name, "Logo") == 0 ||
	    strcasecmp(name, "Mod4") == 0 ||
	    strcasecmp(name, "Win") == 0 ||
	    strcasecmp(name, "Win_L") == 0 ||
	    strcasecmp(name, "Win_R") == 0 ||
	    strcasecmp(name, "Command") == 0 ||
	    strcasecmp(name, "Cmd") == 0 ||
	    strcasecmp(name, "W") == 0) {
		return WLR_MODIFIER_LOGO;
	}
	if (strcasecmp(name, "Mod2") == 0) {
		return WLR_MODIFIER_MOD2;
	}
	if (strcasecmp(name, "Mod3") == 0) {
		return WLR_MODIFIER_MOD3;
	}
	if (strcasecmp(name, "Mod5") == 0) {
		return WLR_MODIFIER_MOD5;
	}
	if (strcasecmp(name, "Caps_Lock") == 0 ||
	    strcasecmp(name, "Caps") == 0) {
		return WLR_MODIFIER_CAPS;
	}
	return 0;
}

enum config_section {
	SEC_NONE,
	SEC_AUTOSTART,
	SEC_BINDS,
	SEC_UI,
	SEC_INPUT,
};

static void config_add_autostart(struct pudu_server *server, const char *cmd) {
	struct pudu_autostart *as = calloc(1, sizeof(*as));
	as->command = strdup(cmd);
	wl_list_insert(&server->autostarts, &as->link);
}

static void config_add_old_binding(struct pudu_server *server,
		const char *spec, const char *action_str) {
	char *copy = strdup(spec);
	char *last_plus = strrchr(copy, '+');
	char *key_name;
	uint32_t mods = 0;
	if (last_plus) {
		*last_plus = '\0';
		char *mod_token = strtok(copy, "+");
		while (mod_token) {
			char *t = trim(mod_token);
			if (strcasecmp(t, "Super") == 0 || strcasecmp(t, "Mod") == 0) {
				mods |= server->mod_modifier;
			} else {
				mods |= modifier_from_name(t);
			}
			mod_token = strtok(NULL, "+");
		}
		key_name = trim(last_plus + 1);
	} else {
		key_name = trim(copy);
	}
	xkb_keysym_t sym = keysym_from_name(key_name);
	free(copy);
	if (sym == XKB_KEY_NoSymbol) return;

	struct pudu_binding *b = calloc(1, sizeof(*b));
	b->mods = mods;
	b->sym = sym;

	if (strcmp(action_str, "PUDU_CLOSE") == 0) b->action = PUDU_ACTION_CLOSE;
	else if (strcmp(action_str, "PUDU_EXIT") == 0) b->action = PUDU_ACTION_EXIT;
	else if (strcmp(action_str, "PUDU_CYCLE_TOPLEVELS") == 0) b->action = PUDU_ACTION_CYCLE_TOPLEVELS;
	else if (strcmp(action_str, "PUDU_SWAP_MASTER") == 0) b->action = PUDU_ACTION_SWAP_MASTER;
	else if (strcmp(action_str, "PUDU_WORKSPACE_NEXT") == 0) b->action = PUDU_ACTION_WORKSPACE_NEXT;
	else if (strcmp(action_str, "PUDU_WORKSPACE_PREV") == 0) b->action = PUDU_ACTION_WORKSPACE_PREV;
	else if (strcmp(action_str, "PUDU_WORKSPACE_MOVE_NEXT") == 0) b->action = PUDU_ACTION_MOVE_WORKSPACE_NEXT;
	else if (strcmp(action_str, "PUDU_WORKSPACE_MOVE_PREV") == 0) b->action = PUDU_ACTION_MOVE_WORKSPACE_PREV;
	else if (strcmp(action_str, "PUDU_RELOAD") == 0) b->action = PUDU_ACTION_RELOAD;
	else {
		b->action = PUDU_ACTION_EXEC;
		b->command = strdup(action_str);
	}

	wl_list_insert(&server->bindings, &b->link);
}

bool parse_color(const char *str, float color[4]) {
	if (str[0] != '#') return false;
	unsigned int r, g, b;
	if (strlen(str) == 7 && sscanf(str + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
		color[0] = r / 255.0f;
		color[1] = g / 255.0f;
		color[2] = b / 255.0f;
		color[3] = 1.0f;
		return true;
	}
	return false;
}

void clear_bindings(struct pudu_server *server) {
	struct pudu_binding *b, *tmp;
	wl_list_for_each_safe(b, tmp, &server->bindings, link) {
		wl_list_remove(&b->link);
		free(b->command);
		free(b);
	}
}

void clear_autostarts(struct pudu_server *server) {
	struct pudu_autostart *as, *tmp;
	wl_list_for_each_safe(as, tmp, &server->autostarts, link) {
		if (as->pid > 0) {
			kill(as->pid, SIGTERM);
		}
		wl_list_remove(&as->link);
		free(as->command);
		free(as);
	}
}

static char *strip_comment(char *line) {
	for (char *p = line; *p; p++) {
		if (*p == '#') {
			if (p > line && (*(p - 1) == ' ' || *(p - 1) == '\t')) {
				int len = 0;
				for (char *q = p + 1; *q && isxdigit(*q); q++) len++;
				if (len == 6) continue;
			}
			*p = '\0';
			break;
		}
	}
	return line;
}

static bool config_parse_bool(const char *val) {
	return (strcasecmp(val, "true") == 0 ||
	        strcasecmp(val, "yes") == 0 ||
	        strcmp(val, "1") == 0);
}

static void config_parse_kv(struct pudu_server *server, const char *key, const char *value) {
	if (strcmp(key, "inner_gap") == 0) {
		server->inner_gap = atoi(value);
	} else if (strcmp(key, "outer_gap") == 0) {
		server->outer_gap = atoi(value);
	} else if (strcmp(key, "active_border_size") == 0 || strcmp(key, "border_size") == 0) {
		int n = atoi(value);
		if (n >= 0) server->active_border_size = n;
	} else if (strcmp(key, "active_border_color") == 0 || strcmp(key, "border_active") == 0) {
		parse_color(value, server->active_border_color);
	} else if (strcmp(key, "inactive_border_color") == 0 || strcmp(key, "border_inactive") == 0) {
		parse_color(value, server->inactive_border_color);
	} else if (strcmp(key, "border_transition_ms") == 0 || strcmp(key, "border_transition") == 0) {
		server->border_transition_ms = atoi(value);
	} else if (strcmp(key, "border_radius") == 0) {
		int n = atoi(value);
		if (n >= 0) server->border_radius = n;
	} else if (strcmp(key, "arrange_anim_ms") == 0) {
		int n = atoi(value);
		if (n >= 0) server->arrange_anim_ms = n;
	} else if (strcmp(key, "new_is_master") == 0) {
		server->new_is_master = config_parse_bool(value);
	} else if (strcmp(key, "arrange_anim") == 0) {
		server->arrange_anim = config_parse_bool(value);
	} else if (strcmp(key, "natural_scroll") == 0) {
		server->natural_scroll = config_parse_bool(value);
	} else if (strcmp(key, "blur") == 0) {
		server->blur = config_parse_bool(value);
	} else if (strcmp(key, "blur_strength") == 0) {
		int n = atoi(value);
		if (n >= 1 && n <= 10) server->blur_strength = n;
	} else if (strcmp(key, "blur_opacity") == 0) {
		float v = atof(value);
		if (v >= 0.0f && v <= 1.0f) server->blur_opacity = v;
	} else if (strcmp(key, "workspace_count") == 0) {
		int n = atoi(value);
		if (n >= 1) server->workspace_count = n;
	}
}

static void config_parse_line(struct pudu_server *server, char *line) {
	line = strip_comment(line);
	line = trim(line);
	if (line[0] == '\0') return;

	char *eq = strchr(line, '=');
	if (eq) {
		*eq = '\0';
		char *key = trim(line);
		char *value = trim(eq + 1);
		config_parse_kv(server, key, value);
		return;
	}

	char *space = strchr(line, ' ');
	if (!space) space = strchr(line, '\t');
	if (space) {
		*space = '\0';
		char *key = trim(line);
		char *value = trim(space + 1);
		config_parse_kv(server, key, value);
	}
}

void load_config(struct pudu_server *server) {
	clear_bindings(server);
	clear_autostarts(server);

	const char *home = getenv("HOME");
	if (!home) return;

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/.config/pudu/config", home);
	FILE *f = fopen(config_path, "r");
	if (!f) {
		char config_parent[512];
		snprintf(config_parent, sizeof(config_parent), "%s/.config", home);
		mkdir(config_parent, 0755);

		char config_dir[512];
		snprintf(config_dir, sizeof(config_dir), "%s/.config/pudu", home);
		mkdir(config_dir, 0755);

		f = fopen(config_path, "w");
		if (f) {
			fputs(default_config, f);
			fclose(f);
			f = fopen(config_path, "r");
		}
	}

	if (f) {
		char line[1024];
		int section = SEC_NONE;
		while (fgets(line, sizeof(line), f)) {
			size_t len = strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
				line[--len] = '\0';
			}

			char *trimmed = trim(line);
			if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

			if (strchr(trimmed, '{')) {
				char *brace = strchr(trimmed, '{');
				*brace = '\0';
				char *name = trim(trimmed);
				if (strcmp(name, "autostart") == 0) section = SEC_AUTOSTART;
				else if (strcmp(name, "binds") == 0) section = SEC_BINDS;
				else if (strcmp(name, "ui") == 0) section = SEC_UI;
				else if (strcmp(name, "input") == 0) section = SEC_INPUT;
				else section = SEC_NONE;
				continue;
			}
			if (strchr(trimmed, '}')) {
				section = SEC_NONE;
				continue;
			}

			switch (section) {
			case SEC_AUTOSTART:
				config_add_autostart(server, trimmed);
				break;
			case SEC_BINDS: {
				char *space = strchr(trimmed, ' ');
				if (!space) space = strchr(trimmed, '\t');
				if (space) {
					*space = '\0';
					char *spec = trim(trimmed);
					char *action = trim(space + 1);
					config_add_old_binding(server, spec, action);
				}
				break;
			}
			case SEC_UI:
			case SEC_INPUT:
				config_parse_line(server, trimmed);
				break;
			default:
				if (strncmp(trimmed, "PUDU_MOD", 8) == 0 &&
						(trimmed[8] == ' ' || trimmed[8] == '\t')) {
					char *val = trim(trimmed + 8);
					uint32_t mod = modifier_from_name(val);
					if (mod) server->mod_modifier = mod;
				}
				break;
			}
		}
		fclose(f);
	}

	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->mapped) continue;
		t->border_w = 0;
		t->border_r = -1;
		const float *target = (server->focused_toplevel_ptr == t) ?
			server->active_border_color : server->inactive_border_color;
		set_border_target(t, target);
	}

	sync_dynamic_workspaces(server);
}
