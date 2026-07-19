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

struct pudu_config {
	int active_border_size;
	float active_border_color[4];
	float inactive_border_color[4];
	int border_transition_ms;
	int border_radius;
	int inner_gap;
	int outer_gap;
	float master_ratio;
	int arrange_anim_ms;
	bool arrange_anim;
	bool natural_scroll;
	bool new_is_master;
	uint32_t mod_modifier;
	int workspace_count;
	char keyboard_layout[64];
	struct wl_list bindings;
	struct wl_list autostarts;
	bool ok;
	char error_msg[512];
};

static bool config_add_autostart(struct pudu_config *cfg, const char *cmd) {
	struct pudu_autostart *as = calloc(1, sizeof(*as));
	if (!as) return false;
	as->command = strdup(cmd);
	if (!as->command) { free(as); return false; }
	wl_list_insert(&cfg->autostarts, &as->link);
	return true;
}

static bool config_add_old_binding(struct pudu_config *cfg,
		const char *spec, const char *action_str, int line_num) {
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
				mods |= cfg->mod_modifier;
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
	if (sym == XKB_KEY_NoSymbol) {
		if (cfg->ok) {
			cfg->ok = false;
			snprintf(cfg->error_msg, sizeof(cfg->error_msg),
				"Line %d: unknown key '%s' in binding", line_num, key_name);
		}
		return false;
	}

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

	wl_list_insert(&cfg->bindings, &b->link);
	return true;
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

static void clear_list_bindings(struct wl_list *list) {
	struct pudu_binding *b, *tmp;
	wl_list_for_each_safe(b, tmp, list, link) {
		wl_list_remove(&b->link);
		free(b->command);
		free(b);
	}
}

static void clear_list_autostarts(struct wl_list *list) {
	struct pudu_autostart *as, *tmp;
	wl_list_for_each_safe(as, tmp, list, link) {
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

static bool is_numeric(const char *val) {
	char *end;
	strtol(val, &end, 10);
	return end != val && *end == '\0';
}

static bool is_float_val(const char *val) {
	char *end;
	strtod(val, &end);
	return end != val && *end == '\0';
}

static bool is_valid_bool(const char *val) {
	return strcasecmp(val, "true") == 0 ||
	       strcasecmp(val, "false") == 0 ||
	       strcasecmp(val, "yes") == 0 ||
	       strcasecmp(val, "no") == 0 ||
	       strcmp(val, "1") == 0 ||
	       strcmp(val, "0") == 0;
}

static bool config_parse_bool(const char *val) {
	return (strcasecmp(val, "true") == 0 ||
	        strcasecmp(val, "yes") == 0 ||
	        strcmp(val, "1") == 0);
}

static bool config_parse_kv(struct pudu_config *cfg,
		const char *key, const char *value, int line_num) {
	if (strcmp(key, "inner_gap") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		cfg->inner_gap = atoi(value);
	} else if (strcmp(key, "outer_gap") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		cfg->outer_gap = atoi(value);
	} else if (strcmp(key, "active_border_size") == 0 || strcmp(key, "border_size") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		int n = atoi(value);
		if (n >= 0) cfg->active_border_size = n;
	} else if (strcmp(key, "active_border_color") == 0 || strcmp(key, "border_active") == 0) {
		if (!parse_color(value, cfg->active_border_color)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid color '%s' (expected #rrggbb)", line_num, value);
			}
			return false;
		}
	} else if (strcmp(key, "inactive_border_color") == 0 || strcmp(key, "border_inactive") == 0) {
		if (!parse_color(value, cfg->inactive_border_color)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid color '%s' (expected #rrggbb)", line_num, value);
			}
			return false;
		}
	} else if (strcmp(key, "border_transition_ms") == 0 || strcmp(key, "border_transition") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		cfg->border_transition_ms = atoi(value);
	} else if (strcmp(key, "border_radius") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		int n = atoi(value);
		if (n >= 0) cfg->border_radius = n;
	} else if (strcmp(key, "arrange_anim_ms") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		int n = atoi(value);
		if (n >= 0) cfg->arrange_anim_ms = n;
	} else if (strcmp(key, "new_is_master") == 0) {
		if (!is_valid_bool(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid boolean '%s' (expected true/false/yes/no/1/0)", line_num, value);
			}
			return false;
		}
		cfg->new_is_master = config_parse_bool(value);
	} else if (strcmp(key, "arrange_anim") == 0) {
		if (!is_valid_bool(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid boolean '%s' (expected true/false/yes/no/1/0)", line_num, value);
			}
			return false;
		}
		cfg->arrange_anim = config_parse_bool(value);
	} else if (strcmp(key, "natural_scroll") == 0) {
		if (!is_valid_bool(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid boolean '%s' (expected true/false/yes/no/1/0)", line_num, value);
			}
			return false;
		}
		cfg->natural_scroll = config_parse_bool(value);
	} else if (strcmp(key, "workspace_count") == 0) {
		if (!is_numeric(value)) {
			if (cfg->ok) {
				cfg->ok = false;
				snprintf(cfg->error_msg, sizeof(cfg->error_msg),
					"Line %d: invalid number '%s' for '%s'", line_num, value, key);
			}
			return false;
		}
		int n = atoi(value);
		if (n >= 1) cfg->workspace_count = n;
	} else if (strcmp(key, "keyboard_layout") == 0) {
		strncpy(cfg->keyboard_layout, value, sizeof(cfg->keyboard_layout) - 1);
	} else {
		if (cfg->ok) {
			cfg->ok = false;
			snprintf(cfg->error_msg, sizeof(cfg->error_msg),
				"Line %d: unknown option '%s'", line_num, key);
		}
		return false;
	}
	return true;
}

/* Config error overlay rendering */

struct config_error_buf_data {
	struct wlr_buffer base;
	unsigned char *data;
	int w, h;
};

static void config_error_buf_destroy(struct wlr_buffer *wb) {
	struct config_error_buf_data *buf = wl_container_of(wb, buf, base);
	free(buf->data);
	free(buf);
}

static bool config_error_buf_begin_data_ptr_access(
		struct wlr_buffer *wb, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	struct config_error_buf_data *buf = wl_container_of(wb, buf, base);
	*data = buf->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = buf->w * 4;
	return true;
}

static void config_error_buf_end_data_ptr_access(struct wlr_buffer *wb) {}

static const struct wlr_buffer_impl config_error_buf_impl = {
	.destroy = config_error_buf_destroy,
	.begin_data_ptr_access = config_error_buf_begin_data_ptr_access,
	.end_data_ptr_access = config_error_buf_end_data_ptr_access,
};

static struct wlr_buffer *create_error_buffer(int w, int h, const char *msg) {
	struct config_error_buf_data *buf = calloc(1, sizeof(*buf));
	if (!buf) return NULL;
	wlr_buffer_init(&buf->base, &config_error_buf_impl, w, h);
	buf->w = w;
	buf->h = h;
	buf->data = calloc(1, w * h * 4);
	if (!buf->data) { free(buf); return NULL; }

	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		buf->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_t *cr = cairo_create(surface);

	/* Background - dark bar */
	cairo_set_source_rgba(cr, 0.08, 0.08, 0.08, 0.95);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

	/* Left accent bar - full height red stripe */
	cairo_set_source_rgba(cr, 0.8, 0.2, 0.2, 0.9);
	cairo_rectangle(cr, 0, 0, 4, h);
	cairo_fill(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* Message text */
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);

	int ty = (h - 16) / 2 + 5;
	cairo_move_to(cr, 18, ty);
	cairo_show_text(cr, "Config Error: ");

	cairo_text_extents_t te;
	cairo_text_extents(cr, "Config Error: ", &te);
	int label_w = te.x_advance;

	cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
	int max_chars = (w - 36 - label_w) / 7;
	if (max_chars < 10) max_chars = 10;
	int line_start = 0;
	int len = strlen(msg);
	char *copy = strdup(msg);
	if (!copy) { cairo_destroy(cr); cairo_surface_destroy(surface); return NULL; }
	int first = 1;
	for (int i = 0; i <= len; i++) {
		if (i == len || copy[i] == '\n' || (i - line_start >= max_chars && copy[i] == ' ')) {
			int end = (i == len || copy[i] == '\n') ? i : i;
			char saved = copy[end];
			copy[end] = '\0';
			if (first) {
				cairo_move_to(cr, 18 + label_w, ty);
				first = 0;
			} else {
				cairo_move_to(cr, 18, ty);
			}
			cairo_show_text(cr, copy + line_start);
			copy[end] = saved;
			ty += 20;
			line_start = (copy[i] == ' ') ? i + 1 : (copy[i] == '\n') ? i + 1 : end;
			if (ty > h - 10) break;
		}
	}
	free(copy);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return &buf->base;
}

void render_config_error(struct pudu_server *server) {
	if (!server->config_error_msg[0]) return;

	if (!server->config_error_tree) {
		server->config_error_tree = wlr_scene_tree_create(server->overlay_tree);
		server->config_error_buf = wlr_scene_buffer_create(
			server->config_error_tree, NULL);
	}

	int panel_w = 1920, panel_h = 36;
	int output_w = 1920;

	struct pudu_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		output_w = output->wlr_output->width;
		break;
	}

	if (output_w > 0) {
		panel_w = output_w;
	}

	struct wlr_buffer *buf = create_error_buffer(panel_w, panel_h, server->config_error_msg);
	if (!buf) return;

	wlr_scene_node_set_position(&server->config_error_tree->node, 0, 0);
	wlr_scene_buffer_set_buffer(server->config_error_buf, buf);
	wlr_buffer_drop(buf);
	wlr_scene_node_set_enabled(&server->config_error_tree->node, true);
}

void clear_config_error(struct pudu_server *server) {
	server->config_error_msg[0] = '\0';
	if (server->config_error_tree) {
		wlr_scene_node_set_enabled(&server->config_error_tree->node, false);
	}
}

static bool config_parse_line(struct pudu_config *cfg,
		char *line, int line_num) {
	line = strip_comment(line);
	line = trim(line);
	if (line[0] == '\0') return true;

	char *eq = strchr(line, '=');
	if (eq) {
		*eq = '\0';
		char *key = trim(line);
		char *value = trim(eq + 1);
		return config_parse_kv(cfg, key, value, line_num);
	}

	char *space = strchr(line, ' ');
	if (!space) space = strchr(line, '\t');
	if (space) {
		*space = '\0';
		char *key = trim(line);
		char *value = trim(space + 1);
		return config_parse_kv(cfg, key, value, line_num);
	}

	return true;
}

bool load_config(struct pudu_server *server) {
	const char *home = getenv("HOME");
	if (!home) return false;

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
		/* First pass: parse into temporary config to validate */
		struct pudu_config cfg = {
			.arrange_anim = server->arrange_anim,
			.arrange_anim_ms = server->arrange_anim_ms,
			.master_ratio = server->master_ratio,
			.natural_scroll = server->natural_scroll,
			.new_is_master = server->new_is_master,
			.mod_modifier = server->mod_modifier,
			.workspace_count = server->workspace_count,
		};
		memcpy(cfg.keyboard_layout, server->keyboard_layout, sizeof(cfg.keyboard_layout));
		wl_list_init(&cfg.bindings);
		wl_list_init(&cfg.autostarts);
		cfg.ok = true;

		int line_num = 0;
		char line[1024];

		while (cfg.ok && fgets(line, sizeof(line), f)) {
			line_num++;
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
				/* No need to track section in cfg, just validate name */
				if (strcmp(name, "autostart") == 0 ||
				    strcmp(name, "binds") == 0 ||
				    strcmp(name, "ui") == 0 ||
				    strcmp(name, "input") == 0) {
					/* valid */
				} else if (cfg.ok) {
					cfg.ok = false;
					snprintf(cfg.error_msg, sizeof(cfg.error_msg),
						"Line %d: unknown section '%s'", line_num, name);
				}
				continue;
			}
			if (strchr(trimmed, '}')) {
				continue;
			}

			/* We need section context, do a second parse later */
		}

		/* Second pass: actually collect data if still valid */
		if (cfg.ok) {
			rewind(f);
			line_num = 0;
			int section = SEC_NONE;

			while (cfg.ok && fgets(line, sizeof(line), f)) {
				line_num++;
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
					if (!config_add_autostart(&cfg, trimmed)) {
						if (cfg.ok) {
							cfg.ok = false;
							snprintf(cfg.error_msg, sizeof(cfg.error_msg),
								"Line %d: out of memory", line_num);
						}
					}
					break;
				case SEC_BINDS: {
					char *space = strchr(trimmed, ' ');
					if (!space) space = strchr(trimmed, '\t');
					if (space) {
						*space = '\0';
						char *spec = trim(trimmed);
						char *action = trim(space + 1);
						config_add_old_binding(&cfg, spec, action, line_num);
					} else if (cfg.ok) {
						cfg.ok = false;
						snprintf(cfg.error_msg, sizeof(cfg.error_msg),
							"Line %d: missing action in binding", line_num);
					}
					break;
				}
				case SEC_UI:
				case SEC_INPUT:
					if (!config_parse_line(&cfg, trimmed, line_num)) {
						/* error already set by config_parse_kv */
					}
					break;
				default:
					if (strncmp(trimmed, "PUDU_MOD", 8) == 0 &&
							(trimmed[8] == ' ' || trimmed[8] == '\t')) {
						char *val = trim(trimmed + 8);
						uint32_t mod = modifier_from_name(val);
						if (mod) cfg.mod_modifier = mod;
					}
					break;
				}
			}
		}

		fclose(f);

		if (!cfg.ok) {
			/* Validation failed: discard temp config, show error */
			clear_list_bindings(&cfg.bindings);
			clear_list_autostarts(&cfg.autostarts);
			wlr_log(WLR_ERROR, "config: %s", cfg.error_msg);
			strncpy(server->config_error_msg, cfg.error_msg,
				sizeof(server->config_error_msg) - 1);
			render_config_error(server);
			return false;
		}

		/* Validation passed: apply config to server */
		clear_config_error(server);
		clear_bindings(server);
		clear_autostarts(server);

		/* Move bindings and autostarts from cfg to server */
		struct pudu_binding *b, *btmp;
		wl_list_for_each_safe(b, btmp, &cfg.bindings, link) {
			wl_list_remove(&b->link);
			wl_list_insert(&server->bindings, &b->link);
		}
		struct pudu_autostart *as, *astmp;
		wl_list_for_each_safe(as, astmp, &cfg.autostarts, link) {
			wl_list_remove(&as->link);
			wl_list_insert(&server->autostarts, &as->link);
		}

		/* Apply scalar values */
		server->inner_gap = cfg.inner_gap;
		server->outer_gap = cfg.outer_gap;
		server->active_border_size = cfg.active_border_size;
		memcpy(server->active_border_color, cfg.active_border_color, sizeof(cfg.active_border_color));
		memcpy(server->inactive_border_color, cfg.inactive_border_color, sizeof(cfg.inactive_border_color));
		server->border_transition_ms = cfg.border_transition_ms;
		server->border_radius = cfg.border_radius;
		server->arrange_anim_ms = cfg.arrange_anim_ms;
		server->new_is_master = cfg.new_is_master;
		server->arrange_anim = cfg.arrange_anim;
		server->natural_scroll = cfg.natural_scroll;
		server->mod_modifier = cfg.mod_modifier;
		server->workspace_count = cfg.workspace_count;
		strncpy(server->keyboard_layout, cfg.keyboard_layout, sizeof(server->keyboard_layout) - 1);
		server_apply_keyboard_layout(server);
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
	return true;
}
