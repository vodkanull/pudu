#include "pudu.h"
#include <signal.h>
#include <fcntl.h>

void handle_xdg_activation_request_activate(
		struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, xdg_activation_request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct wlr_xdg_surface *xdg_surface =
		wlr_xdg_surface_try_from_wlr_surface(event->surface);
	if (xdg_surface == NULL) {
		return;
	}
	struct wlr_scene_tree *xdg_tree = xdg_surface->data;
	if (xdg_tree == NULL || xdg_tree->node.parent == NULL) {
		return;
	}
	struct pudu_toplevel *toplevel = xdg_tree->node.parent->node.data;
	if (toplevel == NULL) {
		return;
	}
	focus_toplevel(toplevel);
}

static void close_all_fds(void) {
#if defined(__linux__) && defined(CLOSE_RANGE_CLOEXEC)
	close_range(3, ~0U, CLOSE_RANGE_CLOEXEC);
#else
	long maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd < 0) maxfd = 1024;
	for (int fd = 3; fd < maxfd; fd++) {
		close(fd);
	}
#endif
}

static void spawn(const char *cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		close_all_fds();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(1);
	}
}

extern unsigned char startup_sound[];
extern unsigned int startup_sound_len;

static void play_startup_sound(void) {
	if (startup_sound_len == 0) return;

	char template[] = "/tmp/pudu_startup_XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) return;

	ssize_t written = write(fd, startup_sound, startup_sound_len);
	close(fd);

	if (written != (ssize_t)startup_sound_len) {
		unlink(template);
		return;
	}

	const char *players[] = {
		"ffplay -nodisp -autoexit -loglevel quiet",
		"mpv --no-video --no-terminal --quiet",
		"mpg123 -q",
		"cvlc --play-and-exit --quiet",
		"mplayer -really-quiet",
		NULL
	};

	for (int i = 0; players[i]; i++) {
		char check[4096];
		const char *space = strchr(players[i], ' ');
		if (space) {
			size_t cmd_len = space - players[i];
			memcpy(check, players[i], cmd_len);
			check[cmd_len] = '\0';
		} else {
			strcpy(check, players[i]);
		}

		int found = 0;
		if (check[0] == '/') {
			if (access(check, X_OK) == 0) found = 1;
		} else {
			char *path_env = getenv("PATH");
			if (path_env) {
				char *path_copy = strdup(path_env);
				char *dir = strtok(path_copy, ":");
				while (dir) {
					char full[4096];
					snprintf(full, sizeof(full), "%s/%s", dir, check);
					if (access(full, X_OK) == 0) {
						found = 1;
						break;
					}
					dir = strtok(NULL, ":");
				}
				free(path_copy);
			}
		}

		if (found) {
			char cmd[4096];
			snprintf(cmd, sizeof(cmd), "%s '%s'; rm -f '%s'", players[i], template, template);
			spawn(cmd);
			return;
		}
	}

	unlink(template);
}

void clear_bindings(struct pudu_server *server);
void clear_autostarts(struct pudu_server *server);

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 1;
	}

	struct pudu_server server = {0};
	server.inner_gap = 5;
	server.outer_gap = 5;
	server.master_ratio = MASTER_RATIO;
	server.active_border_size = 3;
	server.active_border_color[0] = 1.0f;
	server.active_border_color[1] = 0.333f;
	server.active_border_color[2] = 0.333f;
	server.active_border_color[3] = 1.0f;
	server.inactive_border_color[0] = 0.2f;
	server.inactive_border_color[1] = 0.2f;
	server.inactive_border_color[2] = 0.2f;
	server.inactive_border_color[3] = 1.0f;
	server.mod_modifier = WLR_MODIFIER_LOGO;
	server.new_is_master = true;
	server.current_workspace = 1;
	server.workspace_count = 9;
	wl_list_init(&server.bindings);
	wl_list_init(&server.autostarts);

	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.wl_display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);

	server.background_tree = wlr_scene_tree_create(&server.scene->tree);
	server.bottom_tree = wlr_scene_tree_create(&server.scene->tree);
	server.toplevel_tree = wlr_scene_tree_create(&server.scene->tree);
	server.top_tree = wlr_scene_tree_create(&server.scene->tree);
	server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);
	wlr_scene_node_set_enabled(&server.lock_tree->node, false);

	/* Default background color #080808 */
	float bg_color[4] = {0.0314f, 0.0314f, 0.0314f, 1.0f};
	wlr_scene_rect_create(server.background_tree, 10000, 10000, bg_color);

	wl_list_init(&server.toplevels);
	wl_list_init(&server.layer_surfaces);
	wl_list_init(&server.popups);
	wl_list_init(&server.workspaces);
	wl_list_init(&server.manager_clients);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 6);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.decoration_mgr = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
	wl_signal_add(&server.decoration_mgr->events.new_toplevel_decoration,
		&server.new_toplevel_decoration);

	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface,
		&server.new_layer_surface);

	server.foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server.wl_display);

	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_export_dmabuf_manager_v1_create(server.wl_display);
	wlr_data_control_manager_v1_create(server.wl_display);
	struct wlr_gamma_control_manager_v1 *gamma =
		wlr_gamma_control_manager_v1_create(server.wl_display);
	wlr_scene_set_gamma_control_manager_v1(server.scene, gamma);
	server.session_lock_manager = wlr_session_lock_manager_v1_create(server.wl_display);
	server.new_session_lock.notify = server_new_session_lock;
	wl_signal_add(&server.session_lock_manager->events.new_lock, &server.new_session_lock);
	server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
	server.new_pointer_constraint.notify = server_new_pointer_constraint;
	wl_signal_add(&server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint);
	server.active_pointer_constraint = NULL;

	server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
	server.xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
	wl_signal_add(&server.xdg_activation->events.request_activate,
		&server.xdg_activation_request_activate);

	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);

	server.workspace_manager_global = wl_global_create(server.wl_display,
		&ext_workspace_manager_v1_interface, 1, &server, workspace_manager_bind);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);


	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
			&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_allocator_destroy(server.allocator);
		wlr_renderer_destroy(server.renderer);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_allocator_destroy(server.allocator);
		wlr_renderer_destroy(server.renderer);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "wlroots", true);
	setenv("XDG_SESSION_DESKTOP", "pudu", true);
	setenv("XDG_SESSION_TYPE", "wayland", true);

	spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE 2>/dev/null || true");
	spawn("systemctl --user import-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE 2>/dev/null || true");
	spawn("sleep 1 && /usr/lib/xdg-desktop-portal-wlr 2>/dev/null || xdg-desktop-portal-wlr");

	load_config(&server);
	setup_config_watcher(&server);

	struct pudu_autostart *as, *as_tmp;
	wl_list_for_each_safe(as, as_tmp, &server.autostarts, link) {
		spawn(as->command);
		free(as->command);
		wl_list_remove(&as->link);
		free(as);
	}

	sync_dynamic_workspaces(&server);
	update_workspace_ipc(&server);
	workspace_update_toplevel_visibility(&server);

	if (startup_cmd) {
		if (fork() == 0) {
			close_all_fds();
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
			_exit(1);
		}
	}

	wlr_log(WLR_INFO, "Running pudu on WAYLAND_DISPLAY=%s", socket);
	play_startup_sound();
	wl_display_run(server.wl_display);

	cleanup_config_watcher(&server);
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_toplevel_decoration.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.new_session_lock.link);
	wl_list_remove(&server.xdg_activation_request_activate.link);
	wl_list_remove(&server.new_pointer_constraint.link);

	clear_bindings(&server);
	clear_autostarts(&server);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct pudu_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (handle_keybinding(server, syms[i], modifiers)) {
				handled = true;
				break;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct pudu_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct pudu_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) return;
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct pudu_server *server,
		struct wlr_input_device *device) {
	if (wlr_input_device_is_libinput(device)) {
		struct libinput_device *libinput_dev = wlr_libinput_get_device_handle(device);
		if (libinput_device_config_tap_get_finger_count(libinput_dev) > 0) {
			libinput_device_config_tap_set_enabled(libinput_dev, LIBINPUT_CONFIG_TAP_ENABLED);
		}
	}
	wlr_cursor_attach_input_device(server->cursor, device);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(
			listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(
			listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct pudu_toplevel *desktop_toplevel_at(
		struct pudu_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

static struct pudu_toplevel *toplevel_at(
		struct pudu_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct pudu_toplevel *t = desktop_toplevel_at(server, lx, ly, surface, sx, sy);
	if (t) return t;

	struct pudu_toplevel *tmp;
	wl_list_for_each(tmp, &server->toplevels, link) {
		if (!tmp->mapped || tmp->fullscreen) continue;
		if (tmp->workspace != server->current_workspace) continue;

		double x = tmp->scene_tree->node.x;
		double y = tmp->scene_tree->node.y;
		double w = tmp->allocated.width;
		double h = tmp->allocated.height;
		int edge = 2;

		if (lx >= x - edge && lx < x + w + edge &&
				ly >= y - edge && ly < y + h + edge) {
			if (lx < x || lx >= x + w || ly < y || ly >= y + h) {
				*surface = NULL;
				return tmp;
			}
		}
	}
	return NULL;
}

struct pudu_pointer_constraint {
	struct wl_listener destroy;
	struct wlr_pointer_constraint_v1 *constraint;
	struct pudu_server *server;
};

void update_active_pointer_constraint(struct pudu_server *server, struct wlr_surface *surface) {
	struct wlr_pointer_constraint_v1 *constraint = NULL;
	if (surface) {
		constraint = wlr_pointer_constraints_v1_constraint_for_surface(
			server->pointer_constraints, surface, server->seat);
	}
	if (server->active_pointer_constraint == constraint) return;

	if (server->active_pointer_constraint) {
		wlr_pointer_constraint_v1_send_deactivated(server->active_pointer_constraint);
	}
	server->active_pointer_constraint = constraint;
	if (constraint) {
		wlr_pointer_constraint_v1_send_activated(constraint);
	}
}

static void pointer_constraint_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_pointer_constraint *pc = wl_container_of(listener, pc, destroy);
	struct pudu_server *server = pc->server;
	if (server->active_pointer_constraint == pc->constraint) {
		server->active_pointer_constraint = NULL;
	}
	wl_list_remove(&pc->destroy.link);
	free(pc);
}

void server_new_pointer_constraint(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, new_pointer_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct pudu_pointer_constraint *pc = calloc(1, sizeof(*pc));
	if (!pc) return;
	pc->server = server;
	pc->constraint = constraint;
	pc->destroy.notify = pointer_constraint_handle_destroy;
	wl_signal_add(&constraint->events.destroy, &pc->destroy);
	constraint->data = pc;
	update_active_pointer_constraint(server, server->seat->pointer_state.focused_surface);
}

static void process_cursor_motion(struct pudu_server *server, uint32_t time) {
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);

	if (server->cursor_mode == PUDU_CURSOR_MOVE) {
		struct pudu_toplevel *t = server->grabbed_toplevel;
		if (t) {
			double new_x = server->cursor->x - server->grab_x;
			double new_y = server->cursor->y - server->grab_y;
			wlr_scene_node_set_position(&t->scene_tree->node, new_x, new_y);
		}
		return;
	}

	if (server->cursor_mode == PUDU_CURSOR_RESIZE_H) {
		struct wlr_box area;
		get_output_area_under_cursor(server, &area);
		if (area.width > 0) {
			int ig = server->inner_gap;
			int og = server->outer_gap;
			int frames_w = area.width - 2 * og - ig;
			if (frames_w < 1) frames_w = 1;
			int rel_x = server->cursor->x - (area.x + og);
			float ratio = (float)rel_x / frames_w;
			if (ratio < 0.1f) ratio = 0.1f;
			if (ratio > 0.9f) ratio = 0.9f;
			server->master_ratio = ratio;
			arrange_workspace(server, server->current_workspace);
		}
		return;
	}

	if (server->active_pointer_constraint) {
		if (server->active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
			wlr_cursor_warp_closest(server->cursor, NULL,
				server->cursor_prev_x, server->cursor_prev_y);
		} else if (server->active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
			double sx, sy;
			if (wlr_region_confine(&server->active_pointer_constraint->region,
					server->cursor_prev_x, server->cursor_prev_y,
					server->cursor->x, server->cursor->y,
					&sx, &sy)) {
				wlr_cursor_warp_closest(server->cursor, NULL, sx, sy);
			}
		}
	}

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct pudu_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	update_active_pointer_constraint(server, surface);

	if (toplevel && toplevel != server->focused_toplevel_ptr &&
			toplevel->workspace == server->current_workspace) {
		focus_toplevel(toplevel);
	}

	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	server->cursor_prev_x = server->cursor->x;
	server->cursor_prev_y = server->cursor->y;
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	server->cursor_prev_x = server->cursor->x;
	server->cursor_prev_y = server->cursor->y;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void finish_move(struct pudu_server *server) {
	struct pudu_toplevel *t = server->grabbed_toplevel;
	server->cursor_mode = PUDU_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;

	if (!t) return;

	t->floating = false;

	struct wlr_box area;
	get_output_area_under_cursor(server, &area);
	if (area.width <= 0) return;
	int ig = server->inner_gap;
	int og = server->outer_gap;

	int ws = t->workspace;
	wl_list_remove(&t->link);
	wl_list_init(&t->link);

	int other = 0;
	struct pudu_toplevel *tmp;
	wl_list_for_each(tmp, &server->toplevels, link) {
		if (tmp->workspace == ws && tmp->mapped && !tmp->fullscreen && !tmp->floating) {
			other++;
		}
	}

	int target = 0;
	int frames_w = area.width - 2 * og - ig;
	if (frames_w < 1) frames_w = 1;
	int master_fw = (int)(frames_w * server->master_ratio);
	int mid_x = area.x + og + master_fw + ig / 2;
	if (server->cursor->x > mid_x && other > 0) {
		int idx = (int)((server->cursor->y - area.y) * other / area.height);
		if (idx < 0) idx = 0;
		if (idx >= other) idx = other - 1;
		target = 1 + idx;
	}
	if (target > other) target = other;

	if (other == 0) {
		wl_list_insert(&server->toplevels, &t->link);
	} else {
		int j = 0;
		struct pudu_toplevel *ins = NULL;
		wl_list_for_each(tmp, &server->toplevels, link) {
			if (tmp->workspace == ws && tmp->mapped && !tmp->fullscreen && !tmp->floating) {
				if (j == target) { ins = tmp; break; }
				j++;
			}
		}
		if (ins)
			wl_list_insert(ins->link.prev, &t->link);
		else
			wl_list_insert(server->toplevels.prev, &t->link);
	}

	arrange_workspace(server, ws);
}

void server_cursor_button(struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode == PUDU_CURSOR_MOVE) {
			finish_move(server);
			return;
		}
		if (server->cursor_mode == PUDU_CURSOR_RESIZE_H) {
			server->cursor_mode = PUDU_CURSOR_PASSTHROUGH;
			return;
		}
		wlr_seat_pointer_notify_button(server->seat,
				event->time_msec, event->button, event->state);
		return;
	}

	/* Press event */
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct pudu_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

	/* Horizontal resize: click on the border between master and stack */
	if (event->button == BTN_LEFT) {
		int ws = server->current_workspace;
		int count = 0;
		struct pudu_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->workspace == ws && t->mapped && !t->fullscreen && !t->floating) {
				count++;
			}
		}
		if (count >= 2) {
			struct wlr_box area;
			get_output_area_under_cursor(server, &area);
			if (area.width > 0) {
				int ig = server->inner_gap;
				int og = server->outer_gap;
				int frames_w = area.width - 2 * og - ig;
				if (frames_w < 1) frames_w = 1;
				int master_fw = (int)(frames_w * server->master_ratio);
				int border_x = area.x + og + master_fw;
				if (server->cursor->x >= border_x - 8 && server->cursor->x <= border_x + ig + 8) {
					server->cursor_mode = PUDU_CURSOR_RESIZE_H;
					server->grab_x = server->cursor->x;
					server->grab_y = server->cursor->y;
					return;
				}
			}
		}
	}

	if ((mods & server->mod_modifier) && event->button == BTN_LEFT) {
		if (!toplevel) {
			toplevel = desktop_toplevel_at(server,
				server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		}
		if (toplevel) {
			focus_toplevel(toplevel);
			toplevel->floating = true;
			wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, WLR_EDGE_NONE);
			server->cursor_mode = PUDU_CURSOR_MOVE;
			server->grabbed_toplevel = toplevel;
			server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
			server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
		}
		return;
	}

	if (toplevel == NULL) {
		toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	}

	if (toplevel) {
		focus_toplevel(toplevel);
	}

	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	double delta = event->delta;
	int32_t delta_discrete = event->delta_discrete;
	if (server->natural_scroll) {
		delta = -delta;
		delta_discrete = -delta_discrete;
	}
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, delta,
			delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct pudu_output *output = wl_container_of(listener, output, frame);
	struct pudu_server *server = output->server;
	struct wlr_scene *scene = server->scene;

	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(scene, output->wlr_output);

	/* Advance workspace animation synchronized with vsync */
	if (server->animating) {
		int keep_going = workspace_animation_tick(server);
		if (keep_going) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}

	if (!wlr_scene_output_commit(scene_output, NULL)) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_resource_destroyed(struct wl_listener *listener, void *data) {
	struct pudu_output_resource *ores = wl_container_of(listener, ores, destroy);
	wl_list_remove(&ores->link);
	free(ores);
}

static void output_bind(struct wl_listener *listener, void *data) {
	struct pudu_output *output = wl_container_of(listener, output, bind);
	struct wlr_output_event_bind *event = data;

	struct pudu_output_resource *ores = calloc(1, sizeof(*ores));
	if (!ores) return;
	ores->resource = event->resource;
	ores->destroy.notify = output_resource_destroyed;
	wl_resource_add_destroy_listener(event->resource, &ores->destroy);
	wl_list_insert(&output->output_resources, &ores->link);

	struct pudu_server *server = output->server;
	struct pudu_manager_client *mc;
	int found = 0;
	wl_list_for_each(mc, &server->manager_clients, link) {
		if (wl_resource_get_client(mc->manager_resource) == wl_resource_get_client(event->resource)) {
			if (mc->group_resource) {
				ext_workspace_group_handle_v1_send_output_enter(
					mc->group_resource, event->resource);
			}
			ext_workspace_manager_v1_send_done(mc->manager_resource);
			wl_client_flush(wl_resource_get_client(event->resource));
			found = 1;
			break;
		}
	}
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct pudu_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->bind.link);
	wl_list_remove(&output->link);
	free(output);
}

void server_new_output(struct wl_listener *listener, void *data) {
	struct pudu_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct pudu_output *output = calloc(1, sizeof(*output));
	if (!output) return;
	output->wlr_output = wlr_output;
	output->server = server;
	wl_list_init(&output->output_resources);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->bind.notify = output_bind;
	wl_signal_add(&wlr_output->events.bind, &output->bind);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	/* Initialise usable_area even if no layer surfaces exist yet */
	arrange_layers(output);
}

static void toplevel_destroy_border(struct pudu_toplevel *toplevel) {
	for (int i = 0; i < 4; i++) {
		if (toplevel->border_rects[i]) {
			wlr_scene_node_destroy(&toplevel->border_rects[i]->node);
			toplevel->border_rects[i] = NULL;
		}
	}
}

static void toplevel_update_border(struct pudu_toplevel *toplevel) {
	struct pudu_server *server = toplevel->server;
	int b = server->active_border_size;
	int w = toplevel->xdg_toplevel->base->geometry.width;
	int h = toplevel->xdg_toplevel->base->geometry.height;

	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	if (toplevel->border_w == w && toplevel->border_h == h &&
			toplevel->border_b == b) {
		for (int i = 0; i < 4; i++) {
			if (toplevel->border_rects[i]) {
				wlr_scene_rect_set_color(toplevel->border_rects[i], toplevel->border_color);
			}
		}
		return;
	}

	if (!toplevel->border_rects[0]) {
		for (int i = 0; i < 4; i++) {
			toplevel->border_rects[i] = wlr_scene_rect_create(
				toplevel->border_tree, 1, 1, toplevel->border_color);
		}
	}
	wlr_scene_rect_set_size(toplevel->border_rects[0], w + 2 * b, b);
	wlr_scene_node_set_position(&toplevel->border_rects[0]->node, -b, -b);
	wlr_scene_rect_set_size(toplevel->border_rects[1], w + 2 * b, b);
	wlr_scene_node_set_position(&toplevel->border_rects[1]->node, -b, h);
	wlr_scene_rect_set_size(toplevel->border_rects[2], b, h);
	wlr_scene_node_set_position(&toplevel->border_rects[2]->node, -b, 0);
	wlr_scene_rect_set_size(toplevel->border_rects[3], b, h);
	wlr_scene_node_set_position(&toplevel->border_rects[3]->node, w, 0);
	wlr_scene_node_lower_to_bottom(&toplevel->border_tree->node);
	for (int i = 0; i < 4; i++) {
		wlr_scene_rect_set_color(toplevel->border_rects[i], toplevel->border_color);
	}

	toplevel->border_w = w;
	toplevel->border_h = h;
	toplevel->border_b = b;
}

static int border_anim_cb(void *data) {
	struct pudu_toplevel *toplevel = data;
	if (!toplevel->border_animating) return 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	uint32_t elapsed = now - toplevel->border_anim_start;
	int duration = 100;

	double t = (double)elapsed / duration;
	if (t > 1.0) t = 1.0;

	for (int i = 0; i < 4; i++) {
		float start = toplevel->border_start[i];
		float target = toplevel->border_target[i];
			toplevel->border_color[i] = start + (target - start) * t;
	}
	toplevel_update_border(toplevel);

	if (t >= 1.0) {
		toplevel->border_animating = false;
		return 0;
	}

	wl_event_source_timer_update(toplevel->border_anim_timer, 16);
	return 0;
}

static void start_border_animation(struct pudu_toplevel *toplevel, const float target[4]) {
	memcpy(toplevel->border_start, toplevel->border_color, sizeof(toplevel->border_start));
	memcpy(toplevel->border_target, target, sizeof(toplevel->border_target));

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	toplevel->border_anim_start = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	toplevel->border_animating = true;

	if (!toplevel->border_anim_timer) {
		struct wl_event_loop *loop = wl_display_get_event_loop(toplevel->server->wl_display);
		toplevel->border_anim_timer = wl_event_loop_add_timer(loop, border_anim_cb, toplevel);
	}
	wl_event_source_timer_update(toplevel->border_anim_timer, 16);
}

static void set_border_target(struct pudu_toplevel *toplevel, const float target[4]) {
	start_border_animation(toplevel, target);
}

void focus_toplevel(struct pudu_toplevel *toplevel) {
	if (toplevel == NULL || !toplevel->xdg_toplevel->base->initialized) {
		return;
	}
	struct pudu_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

	struct pudu_toplevel *prev = server->focused_toplevel_ptr;
	if (prev && prev != toplevel) {
		if (prev->foreign_handle) {
			wlr_foreign_toplevel_handle_v1_set_activated(prev->foreign_handle, false);
		}
		struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
		if (prev_surface) {
			struct wlr_xdg_toplevel *prev_toplevel =
				wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
			if (prev_toplevel != NULL) {
				wlr_xdg_toplevel_set_activated(prev_toplevel, false);
			}
		}
		set_border_target(prev, server->inactive_border_color);
	}

	server->focused_toplevel_ptr = toplevel;
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	set_border_target(toplevel, server->active_border_color);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (toplevel->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_handle, true);
	}

	if (seat->keyboard_state.focused_surface != surface) {
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
		if (keyboard != NULL) {
			wlr_seat_keyboard_notify_enter(seat, surface,
				keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
		}
	}
}

struct pudu_toplevel *focused_toplevel(struct pudu_server *server) {
	return server->focused_toplevel_ptr;
}

#define POS_ANIM_DURATION_MS 280

static double ease_out_back(double t) {
	const double c1 = 1.70158;
	const double c3 = c1 + 1;
	return 1.0 + c3 * pow(t - 1.0, 3.0) + c1 * pow(t - 1.0, 2.0);
}

static int toplevel_pos_anim_cb(void *data) {
	struct pudu_toplevel *t = data;
	if (!t->pos_animating) return 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	uint32_t elapsed = now - t->anim_start_time;

	double progress = (double)elapsed / POS_ANIM_DURATION_MS;
	if (progress > 1.0) progress = 1.0;
	double eased = ease_out_back(progress);

	double x = t->anim_start_x + (t->anim_target_x - t->anim_start_x) * eased;
	double y = t->anim_start_y + (t->anim_target_y - t->anim_start_y) * eased;
	wlr_scene_node_set_position(&t->scene_tree->node, x, y);

	if (progress >= 1.0) {
		t->pos_animating = false;
		return 0;
	}

	wl_event_source_timer_update(t->pos_anim_timer, 16);
	return 0;
}

static void toplevel_animate_to(struct pudu_toplevel *t, double target_x, double target_y) {
	double current_x = t->scene_tree->node.x;
	double current_y = t->scene_tree->node.y;

	if (fabs(current_x - target_x) < 0.5 && fabs(current_y - target_y) < 0.5) {
		wlr_scene_node_set_position(&t->scene_tree->node, target_x, target_y);
		t->pos_animating = false;
		return;
	}

	t->anim_start_x = current_x;
	t->anim_start_y = current_y;
	t->anim_target_x = target_x;
	t->anim_target_y = target_y;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t->anim_start_time = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	t->pos_animating = true;

	if (!t->pos_anim_timer) {
		struct wl_event_loop *loop = wl_display_get_event_loop(t->server->wl_display);
		t->pos_anim_timer = wl_event_loop_add_timer(loop, toplevel_pos_anim_cb, t);
	}
	wl_event_source_timer_update(t->pos_anim_timer, 16);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wlr_log(WLR_DEBUG, "map: title='%s' app_id='%s'",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
		toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");

	if (toplevel->server->new_is_master) {
		wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	} else {
		wl_list_insert(toplevel->server->toplevels.prev, &toplevel->link);
	}
	toplevel->mapped = true;
	arrange_workspace(toplevel->server, toplevel->workspace);
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);

	/* Center modal/dialog windows on the parent’s output */
	if (toplevel->floating && toplevel->parent_toplevel) {
		struct pudu_toplevel *parent = toplevel->parent_toplevel;
		struct pudu_server *server = toplevel->server;
		struct wlr_output *wlr_output = wlr_output_layout_output_at(
			server->output_layout,
			parent->scene_tree->node.x + parent->allocated.width / 2.0,
			parent->scene_tree->node.y + parent->allocated.height / 2.0);
		if (!wlr_output) {
			wlr_output = wlr_output_layout_get_center_output(server->output_layout);
		}
		if (wlr_output) {
			struct wlr_box area;
			wlr_output_layout_get_box(server->output_layout, wlr_output, &area);
			int w = toplevel->xdg_toplevel->base->geometry.width;
			int h = toplevel->xdg_toplevel->base->geometry.height;
			if (w < 1) w = 1;
			if (h < 1) h = 1;
			int x = area.x + (area.width - w) / 2;
			int y = area.y + (area.height - h) / 2;
			wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y + 30);
			wlr_scene_node_set_position(&toplevel->border_tree->node, 0, 0);
			wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
			toplevel_animate_to(toplevel, x, y);
		}
	}

	if (toplevel->workspace != toplevel->server->current_workspace) {
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
		struct pudu_toplevel *cur = NULL, *tmp;
		wl_list_for_each(tmp, &toplevel->server->toplevels, link) {
			if (tmp->workspace == toplevel->server->current_workspace
					&& tmp->mapped && tmp != toplevel) {
				cur = tmp;
				break;
			}
		}
		if (cur) focus_toplevel(cur);
		else wlr_seat_keyboard_notify_clear_focus(toplevel->server->seat);
	} else {
		focus_toplevel(toplevel);
	}
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	int ws = toplevel->workspace;
	if (!wl_list_empty(&toplevel->link)) {
		wl_list_remove(&toplevel->link);
		wl_list_init(&toplevel->link);
	}
	toplevel->mapped = false;
	if (toplevel->fullscreen) {
		server_update_layer_visibility(toplevel->server);
	}
	workspace_update_toplevel_visibility(toplevel->server);
	if (ws == toplevel->server->current_workspace) {
		arrange_workspace(toplevel->server, ws);
	}
}



static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (xdg->base->initial_commit && xdg->base->initialized) {
		wlr_log(WLR_DEBUG, "first commit: title='%s' app_id='%s'",
			xdg->title ? xdg->title : "", xdg->app_id ? xdg->app_id : "");
		if (xdg->requested.fullscreen) {
			apply_fullscreen_state(toplevel, true, xdg->requested.fullscreen_output);
		}
		if (toplevel->xdg_decoration) {
			wlr_log(WLR_DEBUG, "first commit: decoration present, setting SERVER_SIDE (no-op)");
			wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->xdg_decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
		if (toplevel->foreign_handle) {
			wlr_foreign_toplevel_handle_v1_set_title(
				toplevel->foreign_handle, xdg->title ? xdg->title : "");
			wlr_foreign_toplevel_handle_v1_set_app_id(
				toplevel->foreign_handle, xdg->app_id ? xdg->app_id : "");
		}
		wlr_xdg_surface_schedule_configure(xdg->base);
	}

	struct wlr_box geo = xdg->base->geometry;
	int w = geo.width;
	int h = geo.height;
	if (toplevel->border_w != w || toplevel->border_h != h ||
			toplevel->border_b != toplevel->server->active_border_size) {
		toplevel_update_border(toplevel);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	struct pudu_server *server = toplevel->server;
	wlr_log(WLR_INFO, "DESTROY xdg=%p toplevel=%p server=%p link={%p,%p}",
		(void*)toplevel->xdg_toplevel, (void*)toplevel, (void*)server,
		(void*)toplevel->link.prev, (void*)toplevel->link.next);
	if (toplevel->border_anim_timer) {
		wl_event_source_remove(toplevel->border_anim_timer);
		toplevel->border_anim_timer = NULL;
	}
	if (toplevel->pos_anim_timer) {
		wl_event_source_remove(toplevel->pos_anim_timer);
		toplevel->pos_anim_timer = NULL;
	}
	toplevel_destroy_border(toplevel);
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->set_app_id.link);
	/* toplevel->link is removed in unmap; if destroy fires without unmap
	 * (e.g. client crash) we must remove it here to avoid use-after-free */
	if (!wl_list_empty(&toplevel->link)) {
		wl_list_remove(&toplevel->link);
		wl_list_init(&toplevel->link);
	}
	if (toplevel->foreign_handle) {
		wl_list_remove(&toplevel->ft_handle_request_maximize.link);
		wl_list_remove(&toplevel->ft_handle_request_fullscreen.link);
		wl_list_remove(&toplevel->ft_handle_request_activate.link);
		wl_list_remove(&toplevel->ft_handle_request_close.link);
		wl_list_remove(&toplevel->ft_handle_destroy.link);
		wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_handle);
	}
	/* If a workspace animation is in progress, clear our pointer from the
	 * animation lists so workspace_animation_finish() doesn't use-after-free. */
	if (server->animating) {
		for (int i = 0; i < server->anim_old_count; i++) {
			if (server->anim_old_list[i] == toplevel) {
				server->anim_old_list[i] = NULL;
			}
		}
		for (int i = 0; i < server->anim_new_count; i++) {
			if (server->anim_new_list[i] == toplevel) {
				server->anim_new_list[i] = NULL;
			}
		}
	}

	toplevel->scene_tree->node.data = NULL;
	if (toplevel->xdg_toplevel && toplevel->xdg_toplevel->base) {
		toplevel->xdg_toplevel->base->data = NULL;
	}
	if (server->focused_toplevel_ptr == toplevel) {
		server->focused_toplevel_ptr = NULL;
	}
	if (toplevel->fullscreen) {
		server_update_layer_visibility(server);
	}
	/* Clear parent_toplevel pointers on any child modals */
	struct pudu_toplevel *child;
	wl_list_for_each(child, &server->toplevels, link) {
		if (child->parent_toplevel == toplevel) {
			child->parent_toplevel = NULL;
		}
	}
	wlr_scene_node_destroy(&toplevel->scene_tree->node);
	free(toplevel);
}

void toplevel_set_fullscreen(struct pudu_toplevel *toplevel, bool fullscreen) {
	toplevel->fullscreen = fullscreen;
}

void server_update_layer_visibility(struct pudu_server *server) {
	struct pudu_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		bool fullscreen_here = false;
		struct pudu_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->fullscreen && t->mapped && t->workspace == server->current_workspace) {
				struct wlr_box *geo = &t->xdg_toplevel->base->geometry;
				double cx = t->scene_tree->node.x + geo->width / 2.0;
				double cy = t->scene_tree->node.y + geo->height / 2.0;
				struct wlr_output *wlr_out = wlr_output_layout_output_at(server->output_layout, cx, cy);
				if (wlr_out == output->wlr_output) {
					fullscreen_here = true;
					break;
				}
			}
		}
		output_set_layer_shell_visible(output, !fullscreen_here);
		if (!fullscreen_here) {
			arrange_layers(output);
		}
	}
}

void apply_fullscreen_state(struct pudu_toplevel *toplevel, bool fullscreen, struct wlr_output *wlr_output) {
	struct pudu_server *server = toplevel->server;
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;

	wlr_xdg_toplevel_set_fullscreen(xdg, fullscreen);
	toplevel_set_fullscreen(toplevel, fullscreen);

	if (fullscreen) {
		wlr_xdg_toplevel_set_tiled(xdg, WLR_EDGE_NONE);
		wlr_scene_node_set_enabled(&toplevel->border_tree->node, false);
		struct wlr_scene_tree *xdg_tree = xdg->base->data;
		if (xdg_tree) {
			wlr_scene_node_set_position(&xdg_tree->node, 0, 0);
		}
		wlr_scene_node_set_position(&toplevel->border_tree->node, 0, 0);
		if (!wlr_output) {
			wlr_output = wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
		}
		if (wlr_output) {
			struct wlr_box full_area;
			wlr_output_layout_get_box(server->output_layout, wlr_output, &full_area);
			wlr_xdg_toplevel_set_size(xdg, full_area.width, full_area.height);
			wlr_scene_node_set_position(&toplevel->scene_tree->node, full_area.x, full_area.y);
		}
	} else {
		wlr_xdg_toplevel_set_size(xdg, 0, 0);
		wlr_scene_node_set_enabled(&toplevel->border_tree->node, true);
		arrange_workspace(server, toplevel->workspace);
	}

	wlr_xdg_surface_schedule_configure(xdg->base);
	server_update_layer_visibility(server);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (xdg->base->initialized) {
		wlr_xdg_toplevel_set_maximized(xdg, xdg->requested.maximized);
		wlr_xdg_surface_schedule_configure(xdg->base);
		if (!xdg->requested.maximized) {
			arrange_workspace(toplevel->server, toplevel->workspace);
		}
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (xdg->base->initialized) {
		apply_fullscreen_state(toplevel, xdg->requested.fullscreen, xdg->requested.fullscreen_output);
	}
}

static void ft_handle_request_maximize(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, event->maximized);
	}
}

static void ft_handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (xdg->base->initialized) {
		apply_fullscreen_state(toplevel, event->fullscreen, NULL);
	}
}

static void ft_handle_request_activate(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_activate);
	focus_toplevel(toplevel);
}

static void ft_handle_request_close(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_close);
	wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void ft_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_destroy);
	wl_list_remove(&toplevel->ft_handle_request_maximize.link);
	wl_list_remove(&toplevel->ft_handle_request_fullscreen.link);
	wl_list_remove(&toplevel->ft_handle_request_activate.link);
	wl_list_remove(&toplevel->ft_handle_request_close.link);
	wl_list_remove(&toplevel->ft_handle_destroy.link);
	toplevel->foreign_handle = NULL;
}
struct pudu_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct pudu_toplevel *toplevel;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static void decoration_handle_request_mode(struct wl_listener *listener, void *data) {
	struct pudu_decoration *dec = wl_container_of(listener, dec, request_mode);
	struct wlr_xdg_toplevel_decoration_v1 *deco = dec->decoration;

	if (dec->toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (toplevel->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_set_title(
			toplevel->foreign_handle, xdg->title ? xdg->title : "");
	}
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, set_app_id);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (toplevel->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_set_app_id(
			toplevel->foreign_handle, xdg->app_id ? xdg->app_id : "");
	}
}

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_decoration *dec = wl_container_of(listener, dec, destroy);
	if (dec->toplevel) {
		dec->toplevel->xdg_decoration = NULL;
	}
	wl_list_remove(&dec->request_mode.link);
	wl_list_remove(&dec->destroy.link);
	free(dec);
}

void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	struct pudu_toplevel *toplevel = NULL;
	struct wlr_scene_tree *xdg_tree = decoration->toplevel->base->data;
	if (xdg_tree && xdg_tree->node.parent) {
		toplevel = xdg_tree->node.parent->node.data;
	}
	if (!toplevel) {
		return;
	}

	toplevel->xdg_decoration = decoration;

	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}

	struct pudu_decoration *dec = calloc(1, sizeof(*dec));
	if (!dec) return;
	dec->decoration = decoration;
	dec->toplevel = toplevel;

	dec->request_mode.notify = decoration_handle_request_mode;
	wl_signal_add(&decoration->events.request_mode, &dec->request_mode);
	dec->destroy.notify = decoration_handle_destroy;
	wl_signal_add(&decoration->events.destroy, &dec->destroy);
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct pudu_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) return;
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->workspace = server->current_workspace;
	wl_list_init(&toplevel->link);

	/* If this toplevel has a parent (e.g., dialog/modal), make it floating */
	if (xdg_toplevel->parent) {
		toplevel->floating = true;
		struct wlr_scene_tree *parent_tree = xdg_toplevel->parent->base->data;
		if (parent_tree && parent_tree->node.parent) {
			toplevel->parent_toplevel = parent_tree->node.parent->node.data;
		}
	}

	/* Parent tree: positioned/cascade by us, used for hit-testing */
	toplevel->scene_tree = wlr_scene_tree_create(server->toplevel_tree);
	if (!toplevel->scene_tree) {
		free(toplevel);
		return;
	}
	toplevel->scene_tree->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

	/* XDG surface: renders on top */
	struct wlr_scene_tree *xdg_tree = wlr_scene_xdg_surface_create(
		toplevel->scene_tree, xdg_toplevel->base);
	if (xdg_tree == NULL) {
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		free(toplevel);
		return;
	}
	xdg_toplevel->base->data = xdg_tree;

	/* Border tree: holds border rects */
	toplevel->border_tree = wlr_scene_tree_create(toplevel->scene_tree);
	for (int i = 0; i < 4; i++) {
		toplevel->border_rects[i] = NULL;
	}
	memcpy(toplevel->border_color, server->inactive_border_color, sizeof(float) * 4);
	toplevel->border_w = 0;
	toplevel->border_h = 0;
	toplevel->border_b = 0;
	toplevel->border_animating = false;
	toplevel->border_anim_timer = NULL;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

	/* Foreign toplevel handle for bars/taskbars (e.g. waybar) */
	struct wlr_foreign_toplevel_handle_v1 *ft =
		wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
	if (ft) {
		toplevel->foreign_handle = ft;
		ft->data = toplevel;

		toplevel->ft_handle_request_maximize.notify = ft_handle_request_maximize;
		wl_signal_add(&ft->events.request_maximize, &toplevel->ft_handle_request_maximize);
		toplevel->ft_handle_request_fullscreen.notify = ft_handle_request_fullscreen;
		wl_signal_add(&ft->events.request_fullscreen, &toplevel->ft_handle_request_fullscreen);
		toplevel->ft_handle_request_activate.notify = ft_handle_request_activate;
		wl_signal_add(&ft->events.request_activate, &toplevel->ft_handle_request_activate);
		toplevel->ft_handle_request_close.notify = ft_handle_request_close;
		wl_signal_add(&ft->events.request_close, &toplevel->ft_handle_request_close);
		toplevel->ft_handle_destroy.notify = ft_handle_destroy;
		wl_signal_add(&ft->events.destroy, &toplevel->ft_handle_destroy);
	}

	/* Update foreign handle when title or app_id changes */
	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
	toplevel->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);
}

static void popup_position(struct pudu_popup *popup) {
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	struct wlr_scene_tree *tree = xdg_popup->base->data;
	if (!tree) return;

	struct wlr_output *output = wlr_output_layout_output_at(
		popup->server->output_layout,
		popup->server->cursor->x,
		popup->server->cursor->y);
	if (!output) {
		output = wlr_output_layout_get_center_output(popup->server->output_layout);
	}
	struct wlr_box box = {0};
	if (output) {
		wlr_output_layout_get_box(popup->server->output_layout, output, &box);
	}
	wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
	wlr_scene_node_set_position(&tree->node, xdg_popup->current.geometry.x, xdg_popup->current.geometry.y);
}

static void popup_handle_map(struct wl_listener *listener, void *data) {
	struct pudu_popup *popup = wl_container_of(listener, popup, map);
	popup_position(popup);
}

static void popup_handle_reposition(struct wl_listener *listener, void *data) {
	struct pudu_popup *popup = wl_container_of(listener, popup, reposition);
	popup_position(popup);
}

void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct pudu_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit &&
			popup->xdg_popup->base->initialized) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
	popup_position(popup);
}

void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct pudu_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->link);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->map.link);
	wl_list_remove(&popup->reposition.link);
	free(popup);
}

void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct pudu_server *server =
		wl_container_of(listener, server, new_xdg_popup);

	struct wlr_scene_tree *parent_tree = NULL;

	struct wlr_xdg_surface *parent_xdg = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent_xdg) {
		parent_tree = parent_xdg->data;
	} else {
		struct wlr_layer_surface_v1 *parent_layer =
			wlr_layer_surface_v1_try_from_wlr_surface(xdg_popup->parent);
		if (parent_layer && parent_layer->data) {
			struct pudu_layer_surface *layer = parent_layer->data;
			parent_tree = layer->scene_layer->tree;
		}
	}

	if (parent_tree == NULL) {
		wlr_log(WLR_ERROR, "no parent tree for popup, unable to display");
		return;
	}

	struct pudu_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) return;
	popup->server = server;
	popup->xdg_popup = xdg_popup;

	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	if (xdg_popup->base->data == NULL) {
		free(popup);
		return;
	}

	wl_list_insert(&server->popups, &popup->link);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->map.notify = popup_handle_map;
	wl_signal_add(&xdg_popup->base->surface->events.map, &popup->map);
	popup->reposition.notify = popup_handle_reposition;
	wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void set_tiled(struct pudu_toplevel *t, int x, int y, int w, int h) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	int b = t->server->active_border_size;
	int cw = w - 2 * b;
	int ch = h - 2 * b;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;

	toplevel_animate_to(t, x, y);

	struct wlr_scene_tree *xdg_tree = t->xdg_toplevel->base->data;
	if (xdg_tree) {
		wlr_scene_node_set_position(&xdg_tree->node, b, b);
	}
	wlr_scene_node_set_position(&t->border_tree->node, b, b);

	wlr_xdg_toplevel_set_size(t->xdg_toplevel, (uint32_t)cw, (uint32_t)ch);
	wlr_xdg_toplevel_set_tiled(t->xdg_toplevel,
		WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
	t->allocated.x = x;
	t->allocated.y = y;
	t->allocated.width = w;
	t->allocated.height = h;
	wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
}

void arrange_workspace(struct pudu_server *server, int workspace) {
	struct wlr_box area;
	get_output_area_under_cursor(server, &area);
	if (area.width <= 0) return;

	int count = 0;
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->workspace == workspace && t->mapped && !t->fullscreen && !t->floating) {
			count++;
		}
	}

	if (count == 0) return;

	int ig = server->inner_gap;
	int og = server->outer_gap;

	int master_fw = 0, stack_fw = 0, master_ch = 0;
	int frame_h = 0;
	int N = count - 1;

	if (count == 1) {
		master_ch = area.height - 2 * og;
		if (master_ch < 1) master_ch = 1;
	} else {
		int frames_w = area.width - 2 * og - ig;
		if (frames_w < 1) frames_w = 1;
		master_fw = (int)(frames_w * server->master_ratio);
		stack_fw = frames_w - master_fw;
		master_ch = area.height - 2 * og;
		if (master_ch < 1) master_ch = 1;
		frame_h = (area.height - 2 * og - (N - 1) * ig) / N;
		if (frame_h < 1) frame_h = 1;
	}

	int i = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->workspace != workspace || !t->mapped || t->fullscreen || t->floating) continue;

		if (count == 1) {
			int fx = area.x + og;
			int fy = area.y + og;
			int fw = area.width - 2 * og;
			int fh = area.height - 2 * og;
			set_tiled(t, fx, fy, fw, fh);
		} else if (i == 0) {
			set_tiled(t,
				area.x + og,
				area.y + og,
				master_fw,
				master_ch);
		} else {
			int frame_y = area.y + og + (i - 1) * (frame_h + ig);
			int h = frame_h;
			if (i == count - 1) {
				h = area.y + area.height - og - frame_y;
			}
			set_tiled(t,
				area.x + og + master_fw + ig,
				frame_y,
				stack_fw,
				h);
		}
		i++;
	}
}

void cycle_focus(struct pudu_server *server) {
	if (wl_list_empty(&server->toplevels)) return;

	int ws = server->current_workspace;
	struct pudu_toplevel *focused = server->focused_toplevel_ptr;

	struct pudu_toplevel *next = NULL;
	struct pudu_toplevel *t;
	bool found_focused = false;

	if (focused && focused->workspace == ws && focused->mapped) {
		struct wl_list *link = focused->link.next;
		while (link != &server->toplevels) {
			struct pudu_toplevel *candidate = wl_container_of(link, candidate, link);
			if (candidate->workspace == ws && candidate->mapped) {
				next = candidate;
				found_focused = true;
				break;
			}
			link = link->next;
		}
	}

	if (!next) {
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->workspace == ws && t->mapped) {
				next = t;
				break;
			}
		}
	}

	if (next) {
		focus_toplevel(next);
	}
}

void swap_master(struct pudu_server *server) {
	struct pudu_toplevel *focused = server->focused_toplevel_ptr;
	if (!focused || !focused->mapped) return;

	int ws = server->current_workspace;
	struct pudu_toplevel *first = NULL;
	wl_list_for_each(first, &server->toplevels, link) {
		if (first->workspace == ws && first->mapped && !first->fullscreen) {
			break;
		}
	}
	if (!first || first == focused) return;

	struct wl_list *focused_prev = focused->link.prev;
	struct wl_list *focused_next = focused->link.next;
	struct wl_list *first_prev = first->link.prev;
	struct wl_list *first_next = first->link.next;

	if (focused_next == &first->link) {
		wl_list_remove(&focused->link);
		wl_list_insert(&first->link, &focused->link);
	} else if (first_next == &focused->link) {
		wl_list_remove(&first->link);
		wl_list_insert(&focused->link, &first->link);
	} else {
		wl_list_remove(&focused->link);
		wl_list_remove(&first->link);
		wl_list_insert(focused_prev, &first->link);
		wl_list_insert(first_prev, &focused->link);
	}

	arrange_workspace(server, ws);
	focus_toplevel(focused);
}

void get_output_area_under_cursor(struct pudu_server *server, struct wlr_box *area) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (!wlr_output) {
		wlr_output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (wlr_output) {
		struct pudu_output *output = output_from_wlr_output(server, wlr_output);
		if (output) {
			*area = output->usable_area;
		} else {
			wlr_output_layout_get_box(server->output_layout, wlr_output, area);
		}
	}
}

void focus_first_toplevel_in_workspace(struct pudu_server *server, int workspace) {
	struct pudu_toplevel *to_focus = NULL;
	struct pudu_toplevel *node;
	wl_list_for_each(node, &server->toplevels, link) {
		if (node->workspace == workspace) {
			to_focus = node;
			break;
		}
	}
	if (to_focus) {
		focus_toplevel(to_focus);
	} else {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}
}

int workspace_window_count(struct pudu_server *server, int workspace) {
	struct pudu_toplevel *t;
	int count = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->workspace == workspace && t->mapped) {
			count++;
		}
	}
	return count;
}

static void destroy_workspace(struct pudu_server *server, struct pudu_workspace *ws) {
	struct pudu_manager_client *mc, *mc_tmp;
	wl_list_for_each_safe(mc, mc_tmp, &server->manager_clients, link) {
		if (!mc->group_resource) continue;
		struct pudu_workspace_resource *wsr, *wsr_tmp;
		wl_list_for_each_safe(wsr, wsr_tmp, &ws->resources, link) {
			if (wl_resource_get_client(wsr->resource) == wl_resource_get_client(mc->manager_resource)) {
				ext_workspace_group_handle_v1_send_workspace_leave(
					mc->group_resource, wsr->resource);
			}
		}
	}

	struct pudu_workspace_resource *wsr, *wsr_tmp;
	wl_list_for_each_safe(wsr, wsr_tmp, &ws->resources, link) {
		wl_resource_set_user_data(wsr->resource, NULL);
		ext_workspace_handle_v1_send_removed(wsr->resource);
		wl_resource_destroy(wsr->resource);
		/* el destroy listener libera wsr */
	}

	wl_list_remove(&ws->link);
	free(ws);
}

void sync_dynamic_workspaces(struct pudu_server *server) {
	/* Count actual workspaces */
	int actual_count = 0;
	struct pudu_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		actual_count++;
	}

	/* Add missing workspaces up to workspace_count */
	for (int i = actual_count + 1; i <= server->workspace_count; i++) {
		get_or_create_workspace(server, i);
	}

	/* Remove excess empty workspaces from the end */
	while (actual_count > server->workspace_count) {
		struct pudu_workspace *last = NULL;
		wl_list_for_each(ws, &server->workspaces, link) {
			last = ws;
		}
		if (!last) break;
		/* Only remove if empty to avoid losing windows */
		bool empty = true;
		struct pudu_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->mapped && t->workspace == last->number) {
				empty = false;
				break;
			}
		}
		if (!empty) break;
		destroy_workspace(server, last);
		actual_count--;
	}

	/* Clamp current workspace to valid range */
	if (server->current_workspace > server->workspace_count) {
		server->current_workspace = server->workspace_count;
	}

	update_workspace_ipc(server);
	workspace_update_toplevel_visibility(server);
}

void update_workspace_ipc(struct pudu_server *server) {
	char path[128];
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) runtime_dir = "/tmp";
	snprintf(path, sizeof(path), "%s/pudu_workspace", runtime_dir);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d", server->current_workspace);
		fclose(f);
	}
}

void workspace_update_toplevel_visibility(struct pudu_server *server) {
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		bool visible = (t->workspace == server->current_workspace);
		wlr_scene_node_set_enabled(&t->scene_tree->node, visible);
	}
}

#define ANIM_DURATION_MS 220
static double ease_out_cubic(double t) {
	return 1.0 - pow(1.0 - t, 3.0);
}
int workspace_animation_tick(struct pudu_server *server) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	uint32_t elapsed = now - server->anim_start_time;

	double progress = (double)elapsed / ANIM_DURATION_MS;
	if (progress > 1.0) progress = 1.0;
	double eased = ease_out_cubic(progress);

	double slide = server->anim_slide_dist;
	int dir = server->anim_direction;

	for (int i = 0; i < server->anim_old_count; i++) {
		struct pudu_toplevel *t = server->anim_old_list[i];
		if (!t) continue;
		double x = server->anim_old_x[i] + (-slide * eased * dir);
		wlr_scene_node_set_position(&t->scene_tree->node, x, server->anim_old_y[i]);
	}
	for (int i = 0; i < server->anim_new_count; i++) {
		struct pudu_toplevel *t = server->anim_new_list[i];
		if (!t) continue;
		double x = server->anim_new_x[i] + (slide * (1.0 - eased) * dir);
		wlr_scene_node_set_position(&t->scene_tree->node, x, server->anim_new_y[i]);
	}

	if (progress >= 1.0) {
		workspace_animation_finish(server);
		return 0;
	}
	return 1;
}

void workspace_animation_finish(struct pudu_server *server) {
	if (!server->animating) return;

	int new_ws = server->anim_new_workspace;

	/* Restore original positions and visibility */
	for (int i = 0; i < server->anim_old_count; i++) {
		struct pudu_toplevel *t = server->anim_old_list[i];
		if (!t) continue;
		wlr_scene_node_set_position(&t->scene_tree->node,
			server->anim_old_x[i], server->anim_old_y[i]);
		wlr_scene_node_set_enabled(&t->scene_tree->node, false);
		server->anim_old_list[i] = NULL;
	}
	for (int i = 0; i < server->anim_new_count; i++) {
		struct pudu_toplevel *t = server->anim_new_list[i];
		if (!t) continue;
		wlr_scene_node_set_position(&t->scene_tree->node,
			server->anim_new_x[i], server->anim_new_y[i]);
		server->anim_new_list[i] = NULL;
	}

	workspace_update_toplevel_visibility(server);
	server_update_layer_visibility(server);
	arrange_workspace(server, new_ws);
	focus_first_toplevel_in_workspace(server, new_ws);

	free(server->anim_old_list);
	free(server->anim_new_list);
	free(server->anim_old_x);
	free(server->anim_old_y);
	free(server->anim_new_x);
	free(server->anim_new_y);
	server->anim_old_list = NULL;
	server->anim_new_list = NULL;
	server->anim_old_x = NULL;
	server->anim_old_y = NULL;
	server->anim_new_x = NULL;
	server->anim_new_y = NULL;

	server->animating = false;
}

void view_workspace(struct pudu_server *server, int workspace) {
	int old_ws = server->current_workspace;
	if (old_ws == workspace) return;

	server->current_workspace = workspace;
	update_workspace_ipc(server);

	/* Workspace protocol events */
	struct pudu_workspace *ws;
	ws = find_workspace(server, old_ws);
	if (ws) {
		ws->active = false;
		struct pudu_workspace_resource *wsr;
		wl_list_for_each(wsr, &ws->resources, link) {
			ext_workspace_handle_v1_send_state(wsr->resource, 0);
		}
	}
	get_or_create_workspace(server, workspace);
	ws = find_workspace(server, workspace);
	if (ws) {
		ws->active = true;
		struct pudu_workspace_resource *wsr;
		wl_list_for_each(wsr, &ws->resources, link) {
			ext_workspace_handle_v1_send_state(wsr->resource,
				EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE);
		}
	}
	workspace_send_done_all(server);
	{
		struct pudu_manager_client *mc, *mc_tmp;
		wl_list_for_each_safe(mc, mc_tmp, &server->manager_clients, link) {
			wl_client_flush(wl_resource_get_client(mc->manager_resource));
		}
	}

	/* Cancel any in-progress animation */
	if (server->animating) {
		workspace_animation_finish(server);
	}

	/* Build old/new toplevel lists */
	server->anim_old_count = 0;
	server->anim_new_count = 0;

	{
		struct pudu_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->workspace == old_ws && t->mapped) {
				server->anim_old_count++;
			} else if (t->workspace == workspace && t->mapped) {
				server->anim_new_count++;
			}
		}
	}

	if (server->anim_old_count > 0) {
		server->anim_old_list = calloc(server->anim_old_count, sizeof(*server->anim_old_list));
		server->anim_old_x = calloc(server->anim_old_count, sizeof(*server->anim_old_x));
		server->anim_old_y = calloc(server->anim_old_count, sizeof(*server->anim_old_y));
	}
	if (server->anim_new_count > 0) {
		server->anim_new_list = calloc(server->anim_new_count, sizeof(*server->anim_new_list));
		server->anim_new_x = calloc(server->anim_new_count, sizeof(*server->anim_new_x));
		server->anim_new_y = calloc(server->anim_new_count, sizeof(*server->anim_new_y));
	}

	/* If allocation failed, do instant switch */
	if ((server->anim_old_count > 0 && (!server->anim_old_list || !server->anim_old_x || !server->anim_old_y)) ||
	    (server->anim_new_count > 0 && (!server->anim_new_list || !server->anim_new_x || !server->anim_new_y))) {
		free(server->anim_old_list); server->anim_old_list = NULL;
		free(server->anim_old_x); server->anim_old_x = NULL;
		free(server->anim_old_y); server->anim_old_y = NULL;
		free(server->anim_new_list); server->anim_new_list = NULL;
		free(server->anim_new_x); server->anim_new_x = NULL;
		free(server->anim_new_y); server->anim_new_y = NULL;
		workspace_update_toplevel_visibility(server);
		server_update_layer_visibility(server);
		arrange_workspace(server, workspace);
		focus_first_toplevel_in_workspace(server, workspace);
		return;
	}

	{
		int old_i = 0, new_i = 0;
		struct pudu_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->workspace == old_ws && t->mapped) {
				server->anim_old_list[old_i++] = t;
			} else if (t->workspace == workspace && t->mapped) {
				server->anim_new_list[new_i++] = t;
			}
		}
	}

	/* Determine slide direction */
	int direction = (workspace > old_ws) ? 1 : -1;
	server->anim_direction = direction;

	/* Nothing to animate — instant switch */
	if (server->anim_old_count == 0 && server->anim_new_count == 0) {
		workspace_update_toplevel_visibility(server);
		server_update_layer_visibility(server);
		arrange_workspace(server, workspace);
		focus_first_toplevel_in_workspace(server, workspace);
		return;
	}

	/* Slide distance = width of output under cursor */
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
	double slide_dist = wlr_output ? wlr_output->width : 1920;
	server->anim_slide_dist = slide_dist;
	server->anim_new_workspace = workspace;

	/* Save original positions and bring animated toplevels to the front */
	for (int i = 0; i < server->anim_old_count; i++) {
		struct pudu_toplevel *t = server->anim_old_list[i];
		server->anim_old_x[i] = t->scene_tree->node.x;
		server->anim_old_y[i] = t->scene_tree->node.y;
		wlr_scene_node_raise_to_top(&t->scene_tree->node);
	}
	for (int i = 0; i < server->anim_new_count; i++) {
		struct pudu_toplevel *t = server->anim_new_list[i];
		server->anim_new_x[i] = t->scene_tree->node.x;
		server->anim_new_y[i] = t->scene_tree->node.y;
		wlr_scene_node_set_enabled(&t->scene_tree->node, true);
		wlr_scene_node_raise_to_top(&t->scene_tree->node);
	}

	/* Start animation — synchronized with vsync via output_frame */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	server->anim_start_time = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	server->animating = true;
	if (wlr_output) {
		wlr_output_schedule_frame(wlr_output);
	}
}

/*
 * ext_workspace_manager_v1 protocol implementation
 */

/* ext_workspace_handle_v1 request handlers */

static void workspace_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void workspace_handle_activate(struct wl_client *client,
		struct wl_resource *resource) {
	struct pudu_workspace *ws = wl_resource_get_user_data(resource);
	if (ws && ws->server && !ws->active) {
		view_workspace(ws->server, ws->number);
	}
}

static void workspace_resource_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_workspace_resource *wsr = wl_container_of(listener, wsr, destroy);
	wl_list_remove(&wsr->link);
	free(wsr);
}

static const struct ext_workspace_handle_v1_interface workspace_handle_impl = {
	.destroy = workspace_handle_destroy,
	.activate = workspace_handle_activate,
};

/* ext_workspace_group_handle_v1 request handlers */

static void group_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void group_handle_create_workspace(struct wl_client *client,
		struct wl_resource *resource, const char *name) {
	/* No-op — compositor does not advertise create_workspace capability */
}

void group_handle_resource_destroyed(struct wl_resource *resource) {
	struct pudu_manager_client *mc = wl_resource_get_user_data(resource);
	if (!mc) return;
	mc->group_resource = NULL;
}

static const struct ext_workspace_group_handle_v1_interface group_handle_impl = {
	.create_workspace = group_handle_create_workspace,
	.destroy = group_handle_destroy,
};

/* ext_workspace_manager_v1 request handlers */

static void manager_handle_commit(struct wl_client *client,
		struct wl_resource *resource) {
	/* No-op — changes are applied immediately in this compositor */
}

static void manager_handle_stop(struct wl_client *client,
		struct wl_resource *resource) {
	ext_workspace_manager_v1_send_finished(resource);
}

void manager_handle_resource_destroyed(struct wl_resource *resource) {
	struct pudu_manager_client *mc = wl_resource_get_user_data(resource);
	if (!mc) return;
	if (mc->group_resource) {
		wl_resource_destroy(mc->group_resource);
	}
	wl_list_remove(&mc->link);
	free(mc);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = manager_handle_commit,
	.stop = manager_handle_stop,
};

void workspace_send_done_all(struct pudu_server *server) {
	struct pudu_manager_client *mc;
	wl_list_for_each(mc, &server->manager_clients, link) {
		ext_workspace_manager_v1_send_done(mc->manager_resource);
	}
}

struct wl_resource *workspace_send_to_client(struct pudu_workspace *ws, struct pudu_manager_client *mc, uint32_t version) {
	struct wl_client *client = wl_resource_get_client(mc->manager_resource);
	struct wl_resource *ws_res = wl_resource_create(client,
		&ext_workspace_handle_v1_interface, version, 0);
	if (!ws_res) return NULL;
	wl_resource_set_implementation(ws_res, &workspace_handle_impl,
		ws, NULL);

	ext_workspace_manager_v1_send_workspace(mc->manager_resource, ws_res);

	char name[16];
	snprintf(name, sizeof(name), "%d", ws->number);
	ext_workspace_handle_v1_send_name(ws_res, name);
	if (ws->active) {
		ext_workspace_handle_v1_send_state(ws_res,
			EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE);
	}
	ext_workspace_handle_v1_send_capabilities(ws_res,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);

	if (mc->group_resource) {
		ext_workspace_group_handle_v1_send_workspace_enter(
			mc->group_resource, ws_res);
	}

	struct pudu_workspace_resource *wsr = calloc(1, sizeof(*wsr));
	wsr->resource = ws_res;
	wsr->destroy.notify = workspace_resource_handle_destroy;
	wl_resource_add_destroy_listener(ws_res, &wsr->destroy);
	wl_list_insert(&ws->resources, &wsr->link);
	return ws_res;
}

struct pudu_workspace *find_workspace(struct pudu_server *server, int number) {
	struct pudu_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->number == number) return ws;
	}
	return NULL;
}

void create_workspace(struct pudu_server *server, int number) {
	struct pudu_workspace *ws = calloc(1, sizeof(*ws));
	if (!ws) return;
	ws->server = server;
	ws->number = number;
	ws->active = (number == server->current_workspace);
	wl_list_init(&ws->resources);
	wl_list_insert(server->workspaces.prev, &ws->link);

	struct pudu_manager_client *mc, *mc_tmp;
	wl_list_for_each_safe(mc, mc_tmp, &server->manager_clients, link) {
		workspace_send_to_client(ws, mc, wl_resource_get_version(mc->manager_resource));
	}

	if (!wl_list_empty(&server->manager_clients)) {
		workspace_send_done_all(server);
		wl_list_for_each_safe(mc, mc_tmp, &server->manager_clients, link) {
			wl_client_flush(wl_resource_get_client(mc->manager_resource));
		}
	}
}

struct pudu_workspace *get_or_create_workspace(struct pudu_server *server, int number) {
	struct pudu_workspace *ws = find_workspace(server, number);
	if (ws) return ws;
	create_workspace(server, number);
	return find_workspace(server, number);
}

void workspace_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct pudu_server *server = data;

	struct pudu_manager_client *mc = calloc(1, sizeof(*mc));
	if (!mc) return;

	mc->manager_resource = wl_resource_create(
		client, &ext_workspace_manager_v1_interface, version, id);
	if (!mc->manager_resource) {
		free(mc);
		return;
	}
	wl_resource_set_implementation(mc->manager_resource, &manager_impl,
		mc, manager_handle_resource_destroyed);

	wl_list_insert(&server->manager_clients, &mc->link);

	mc->group_resource = wl_resource_create(
		client, &ext_workspace_group_handle_v1_interface, version, 0);
	if (!mc->group_resource) {
		wl_resource_destroy(mc->manager_resource);
		return;
	}
	wl_resource_set_implementation(mc->group_resource, &group_handle_impl, mc, group_handle_resource_destroyed);

	if (mc->group_resource) {
		ext_workspace_manager_v1_send_workspace_group(mc->manager_resource, mc->group_resource);
		ext_workspace_group_handle_v1_send_capabilities(mc->group_resource, 0);

		struct pudu_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			struct pudu_output_resource *ores;
			wl_list_for_each(ores, &output->output_resources, link) {
				if (wl_resource_get_client(ores->resource) == client) {
					ext_workspace_group_handle_v1_send_output_enter(
						mc->group_resource, ores->resource);
					break;
				}
			}
		}
	}

	struct pudu_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		workspace_send_to_client(ws, mc, version);
	}

	ext_workspace_manager_v1_send_done(mc->manager_resource);
	wl_client_flush(client);
}

void execute_binding(struct pudu_server *server,
		struct pudu_binding *b) {
	switch (b->action) {
	case PUDU_ACTION_CLOSE: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		}
		break;
	}
	case PUDU_ACTION_EXEC:
		spawn(b->command);
		break;
	case PUDU_ACTION_CYCLE_TOPLEVELS: {
		cycle_focus(server);
		break;
	}
	case PUDU_ACTION_SWAP_MASTER: {
		swap_master(server);
		break;
	}
	case PUDU_ACTION_WORKSPACE_NEXT:
	case PUDU_ACTION_WORKSPACE_PREV: {
		int delta = (b->action == PUDU_ACTION_WORKSPACE_NEXT) ? 1 : -1;
		int ws = server->current_workspace + delta;
		if (ws > server->workspace_count) ws = 1;
		if (ws < 1) ws = server->workspace_count;
		view_workspace(server, ws);
		break;
	}
	case PUDU_ACTION_MOVE_WORKSPACE_NEXT:
	case PUDU_ACTION_MOVE_WORKSPACE_PREV: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			int old_ws = toplevel->workspace;
			int delta = (b->action == PUDU_ACTION_MOVE_WORKSPACE_NEXT) ? 1 : -1;
			int ws = toplevel->workspace + delta;
			if (ws > server->workspace_count) ws = 1;
			if (ws < 1) ws = server->workspace_count;
			toplevel->workspace = ws;
			if (!wl_list_empty(&toplevel->link)) {
				wl_list_remove(&toplevel->link);
				wl_list_init(&toplevel->link);
			}
			if (server->new_is_master) {
				wl_list_insert(&server->toplevels, &toplevel->link);
			} else {
				wl_list_insert(server->toplevels.prev, &toplevel->link);
			}
			arrange_workspace(server, old_ws);
			arrange_workspace(server, toplevel->workspace);
			view_workspace(server, toplevel->workspace);
		}
		break;
	}
	case PUDU_ACTION_EXIT:
		wl_display_terminate(server->wl_display);
		break;
	default:
		break;
	}
}

bool handle_keybinding(struct pudu_server *server,
		xkb_keysym_t sym, uint32_t mods) {
	struct pudu_binding *b;
	uint32_t mask = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	wl_list_for_each(b, &server->bindings, link) {
		if (xkb_keysym_to_lower(b->sym) == xkb_keysym_to_lower(sym) && b->mods == (mods & mask)) {
			execute_binding(server, b);
			return true;
		}
	}
	return false;
}
static char *trim(char *str) {
	while (isspace(*str)) str++;
	if (*str == 0) return str;
	char *end = str + strlen(str) - 1;
	while (end > str && isspace(*end)) end--;
	*(end + 1) = 0;
	return str;
}

static xkb_keysym_t keysym_from_name(const char *name) {
	return xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
}

static uint32_t modifier_from_name(const char *name) {
	static const struct { const char *name; uint32_t mod; } map[] = {
		{"Shift", WLR_MODIFIER_SHIFT}, {"Shift_L", WLR_MODIFIER_SHIFT},
		{"Shift_R", WLR_MODIFIER_SHIFT}, {"S", WLR_MODIFIER_SHIFT},
		{"Control", WLR_MODIFIER_CTRL}, {"Control_L", WLR_MODIFIER_CTRL},
		{"Control_R", WLR_MODIFIER_CTRL}, {"Ctrl", WLR_MODIFIER_CTRL},
		{"Ctrl_L", WLR_MODIFIER_CTRL}, {"Ctrl_R", WLR_MODIFIER_CTRL},
		{"C", WLR_MODIFIER_CTRL},
		{"Alt", WLR_MODIFIER_ALT}, {"Alt_L", WLR_MODIFIER_ALT},
		{"Alt_R", WLR_MODIFIER_ALT}, {"Mod1", WLR_MODIFIER_ALT},
		{"A", WLR_MODIFIER_ALT},
		{"Super", WLR_MODIFIER_LOGO}, {"Super_L", WLR_MODIFIER_LOGO},
		{"Super_R", WLR_MODIFIER_LOGO}, {"Logo", WLR_MODIFIER_LOGO},
		{"Mod4", WLR_MODIFIER_LOGO}, {"Win", WLR_MODIFIER_LOGO},
		{"Win_L", WLR_MODIFIER_LOGO}, {"Win_R", WLR_MODIFIER_LOGO},
		{"Command", WLR_MODIFIER_LOGO}, {"Cmd", WLR_MODIFIER_LOGO},
		{"W", WLR_MODIFIER_LOGO},
		{"Mod2", WLR_MODIFIER_MOD2}, {"Mod3", WLR_MODIFIER_MOD3},
		{"Mod5", WLR_MODIFIER_MOD5},
		{"Caps_Lock", WLR_MODIFIER_CAPS}, {"Caps", WLR_MODIFIER_CAPS},
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcasecmp(name, map[i].name) == 0) return map[i].mod;
	}
	return 0;
}

static const char *config_get_var(const char *name);

static uint32_t parse_mods(struct pudu_server *server, const char *str) {
	uint32_t mods = 0;
	char *copy = strdup(str);
	if (!copy) return 0;
	char *token = strtok(copy, "+");
	while (token) {
		char *t = trim(token);
		if (strcasecmp(t, "Mod") == 0) {
			mods |= server->mod_modifier;
		} else if (t[0] == '$') {
			const char *val = config_get_var(t);
			uint32_t mod = modifier_from_name(val);
			if (mod) mods |= mod;
		} else {
			mods |= modifier_from_name(t);
		}
		token = strtok(NULL, "+");
	}
	free(copy);
	return mods;
}

static bool parse_color(const char *str, float color[4]) {
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

static void config_add_binding(struct pudu_server *server,
		const char *spec, const char *action_str_orig) {
	struct pudu_binding *b = calloc(1, sizeof(*b));
	if (!b) return;

	char *copy = strdup(spec);
	if (!copy) {
		free(b);
		return;
	}
	char *last_plus = strrchr(copy, '+');
	char *key_name;
	if (last_plus) {
		*last_plus = '\0';
		b->mods = parse_mods(server, copy);
		key_name = trim(last_plus + 1);
	} else {
		b->mods = 0;
		key_name = trim(copy);
	}

	b->sym = keysym_from_name(key_name);
	if (b->sym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "unknown keysym: %s", key_name);
		free(copy);
		free(b);
		return;
	}

	char *action_str_copy = strdup(action_str_orig);
	if (!action_str_copy) {
		free(copy);
		free(b);
		return;
	}
	char *action_str = trim(action_str_copy);

	static const struct { const char *name; enum pudu_action action; } action_map[] = {
		{"close", PUDU_ACTION_CLOSE},
		{"exit", PUDU_ACTION_EXIT},
		{"cycle_toplevels", PUDU_ACTION_CYCLE_TOPLEVELS},
		{"swap_master", PUDU_ACTION_SWAP_MASTER},
		{"workspace_next", PUDU_ACTION_WORKSPACE_NEXT},
		{"workspace_prev", PUDU_ACTION_WORKSPACE_PREV},
		{"move_workspace_next", PUDU_ACTION_MOVE_WORKSPACE_NEXT},
		{"move_workspace_prev", PUDU_ACTION_MOVE_WORKSPACE_PREV},
	};
	bool found = false;
	for (size_t i = 0; i < sizeof(action_map) / sizeof(action_map[0]); i++) {
		if (strcmp(action_str, action_map[i].name) == 0) {
			b->action = action_map[i].action;
			found = true;
			break;
		}
	}
	if (!found) {
		if (strncmp(action_str, "exec ", 5) == 0) {
			b->action = PUDU_ACTION_EXEC;
			char *cmd = strdup(trim(action_str + 5));
			if (cmd) b->command = cmd;
		} else {
			wlr_log(WLR_ERROR, "unknown action: %s", action_str);
			free(action_str_copy);
			free(copy);
			free(b);
			return;
		}
	}

	wl_list_insert(&server->bindings, &b->link);
	free(action_str_copy);
	free(copy);
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
		wl_list_remove(&as->link);
		free(as->command);
		free(as);
	}
}

extern const unsigned char default_config[];
extern const unsigned int default_config_len;

static char *config_vars[32][2];
static int config_var_count = 0;

static void config_clear_vars(void) {
	for (int i = 0; i < config_var_count; i++) {
		free(config_vars[i][0]);
		free(config_vars[i][1]);
	}
	config_var_count = 0;
}

static const char *config_get_var(const char *name) {
	for (int i = 0; i < config_var_count; i++) {
		if (strcmp(config_vars[i][0], name) == 0) {
			return config_vars[i][1];
		}
	}
	return name;
}

static void config_set_var(const char *name, const char *value) {
	if (config_var_count >= 32) return;
	char *n = strdup(name);
	char *v = strdup(value);
	if (n && v) {
		config_vars[config_var_count][0] = n;
		config_vars[config_var_count][1] = v;
		config_var_count++;
	} else {
		free(n);
		free(v);
	}
}

static char *strip_comment(char *line) {
	for (char *p = line; *p; p++) {
		if (*p == '#') {
			/* Don't treat # as comment if it looks like a hex color (#RRGGBB) */
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

static void config_parse_line(struct pudu_server *server, char *line) {
	line = strip_comment(line);
	line = trim(line);
	if (line[0] == '\0') return;

	/* Variable: $name = value */
	if (line[0] == '$') {
		char *eq = strchr(line, '=');
		if (eq) {
			*eq = '\0';
			char *name = trim(line);
			char *value = trim(eq + 1);
			config_set_var(name, value);
		}
		return;
	}

	/* Section start: name { */
	char *brace = strchr(line, '{');
	if (brace) {
		*brace = '\0';
		char *section = trim(line);
		return; /* Sections handled during line reading, values parsed below */
	}

	/* Section end */
	if (strchr(line, '}')) return;

	/* exec = command */
	if (strncmp(line, "exec", 4) == 0 && (line[4] == ' ' || line[4] == '\t' || line[4] == '=')) {
		char *val = line + 4;
		while (*val == ' ' || *val == '\t' || *val == '=') val++;
		val = trim(val);
		struct pudu_autostart *as = calloc(1, sizeof(*as));
		if (!as) return;
		as->command = strdup(val);
		if (!as->command) {
			free(as);
			return;
		}
		wl_list_insert(&server->autostarts, &as->link);
		return;
	}

	/* bind = mod, key, action or bind = mod, key, exec, cmd... */
	if (strncmp(line, "bind", 4) == 0 && (line[4] == ' ' || line[4] == '\t' || line[4] == '=')) {
		char *val = line + 4;
		while (*val == ' ' || *val == '\t' || *val == '=') val++;
		val = trim(val);

		/* Split by comma into up to 5 parts */
		char *parts[5];
		int n = 0;
		char *rest = val;
		while (n < 5) {
			char *comma = strchr(rest, ',');
			if (!comma) {
				parts[n++] = trim(rest);
				break;
			}
			*comma = '\0';
			parts[n++] = trim(rest);
			rest = comma + 1;
		}
		if (n < 3) return;

		const char *mod_part = config_get_var(parts[0]);
		char modspec[128] = "";
		if (mod_part && mod_part[0]) {
			snprintf(modspec, sizeof(modspec), "%s+%s", mod_part, parts[1]);
		} else {
			snprintf(modspec, sizeof(modspec), "%s", parts[1]);
		}

		if (n >= 4 && strcmp(parts[2], "exec") == 0) {
			/* Reconstruct exec command from parts 3..n-1 */
			char cmd[512] = "";
			int pos = 0;
			for (int i = 3; i < n; i++) {
				if (i > 3) {
					if (pos < sizeof(cmd) - 1) cmd[pos++] = ',';
				}
				int len = strlen(parts[i]);
				if (pos + len < sizeof(cmd)) {
					memcpy(cmd + pos, parts[i], len);
					pos += len;
				}
			}
			cmd[pos] = '\0';
			char action[520];
			snprintf(action, sizeof(action), "exec %s", cmd);
			config_add_binding(server, modspec, action);
		} else {
			config_add_binding(server, modspec, parts[2]);
		}
		return;
	}

	/* Section value: key = value (inside ui or input block) */
	char *eq = strchr(line, '=');
	if (!eq) return;
	*eq = '\0';
	char *key = trim(line);
	char *value = trim(eq + 1);

	if (strcmp(key, "mod") == 0) {
		uint32_t mod = modifier_from_name(value);
		if (mod) server->mod_modifier = mod;
	} else if (strcmp(key, "inner_gap") == 0) {
		server->inner_gap = atoi(value);
	} else if (strcmp(key, "outer_gap") == 0) {
		server->outer_gap = atoi(value);
	} else if (strcmp(key, "active_border_size") == 0) {
		int n = atoi(value);
		if (n >= 0) server->active_border_size = n;
	} else if (strcmp(key, "active_border_color") == 0) {
		parse_color(value, server->active_border_color);
	} else if (strcmp(key, "inactive_border_color") == 0) {
		parse_color(value, server->inactive_border_color);
	} else if (strcmp(key, "new_is_master") == 0) {
		server->new_is_master = config_parse_bool(value);
	} else if (strcmp(key, "natural_scroll") == 0) {
		server->natural_scroll = config_parse_bool(value);
	} else if (strcmp(key, "workspace_count") == 0) {
		int n = atoi(value);
		if (n >= 1) server->workspace_count = n;
	}
}

void load_config(struct pudu_server *server) {
	clear_bindings(server);
	clear_autostarts(server);
	config_clear_vars();

	const char *home = getenv("HOME");
	if (!home) return;

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/.config/pudu/config", home);
	FILE *f = fopen(config_path, "r");
	if (!f) {
		char config_dir[512];
		snprintf(config_dir, sizeof(config_dir), "%s/.config/pudu", home);
		mkdir(config_dir, 0755);

		f = fopen(config_path, "w");
		if (f) {
			fwrite(default_config, 1, default_config_len, f);
			fclose(f);
			f = fopen(config_path, "r");
		}
	}

	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			/* Remove trailing newline */
			size_t len = strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
				line[--len] = '\0';
			}

			char *trimmed = trim(line);
			if (trimmed[0] == '\0') continue;
			config_parse_line(server, trimmed);
		}
		fclose(f);
	}

	config_clear_vars();

	/* Force border geometry and color update for all mapped toplevels */
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->mapped) continue;
		t->border_w = 0; /* force geometry update on next commit */
		const float *target = (server->focused_toplevel_ptr == t) ?
			server->active_border_color : server->inactive_border_color;
		set_border_target(t, target);
	}

	sync_dynamic_workspaces(server);
}

struct pudu_output *output_from_wlr_output(struct pudu_server *server, struct wlr_output *wlr_output) {
	struct pudu_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) return output;
	}
	return NULL;
}

void output_set_layer_shell_visible(struct pudu_output *output, bool visible) {
	struct pudu_server *server = output->server;
	struct pudu_layer_surface *surf;
	wl_list_for_each(surf, &server->layer_surfaces, link) {
		if (surf->output != output->wlr_output) continue;
		if (surf->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP || surf->layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
			wlr_scene_node_set_enabled(&surf->scene_layer->tree->node, visible);
		}
	}
}

void arrange_layers(struct pudu_output *output) {
	struct wlr_box full_area = {0};
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &full_area);
	struct wlr_box usable_area = full_area;

	struct pudu_layer_surface *surf;
	wl_list_for_each(surf, &output->server->layer_surfaces, link) {
		if (surf->output != output->wlr_output) continue;
		wlr_scene_layer_surface_v1_configure(surf->scene_layer, &full_area, &usable_area);
	}

	output->usable_area = usable_area;
}

static struct wlr_scene_tree *layer_tree_for_surface(struct pudu_server *server, enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->background_tree;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->bottom_tree;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->top_tree;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return server->overlay_tree;
	}
	return server->top_tree;
}

static void layer_surface_handle_commit(struct wl_listener *listener, void *data) {
	struct pudu_layer_surface *surf = wl_container_of(listener, surf, commit);
	struct wlr_layer_surface_v1 *layer_surface = surf->scene_layer->layer_surface;

	uint32_t committed = layer_surface->current.committed;
	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		if (surf->layer != layer_surface->current.layer) {
			surf->layer = layer_surface->current.layer;
			struct wlr_scene_tree *new_tree = layer_tree_for_surface(surf->server, surf->layer);
			wlr_scene_node_reparent(&surf->scene_layer->tree->node, new_tree);
		}
	}
	if (!surf->configured || committed) {
		surf->configured = true;
		struct pudu_output *output = output_from_wlr_output(surf->server, surf->output);
		if (output) {
			arrange_layers(output);
		}
	}
}

static void layer_surface_handle_map(struct wl_listener *listener, void *data) {
	struct pudu_layer_surface *surf = wl_container_of(listener, surf, map);
	struct wlr_layer_surface_v1 *layer_surface = surf->scene_layer->layer_surface;

	struct pudu_output *output = output_from_wlr_output(surf->server, surf->output);
	if (output) {
		arrange_layers(output);
	}

	enum zwlr_layer_surface_v1_keyboard_interactivity ki =
		layer_surface->current.keyboard_interactive;
	if (ki != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		struct pudu_server *server = surf->server;
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		if (kb) {
			wlr_seat_keyboard_notify_enter(server->seat,
				layer_surface->surface,
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
		}
	}
}

static void layer_surface_handle_unmap(struct wl_listener *listener, void *data) {
	struct pudu_layer_surface *surf = wl_container_of(listener, surf, unmap);
	struct wlr_layer_surface_v1 *layer_surface = surf->scene_layer->layer_surface;
	struct pudu_server *server = surf->server;
	struct wlr_seat *seat = server->seat;
	if (seat->keyboard_state.focused_surface == layer_surface->surface) {
		wlr_seat_keyboard_notify_clear_focus(seat);
	}

	struct pudu_output *output = output_from_wlr_output(server, surf->output);
	if (output) {
		arrange_layers(output);
	}
}

static void layer_surface_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_layer_surface *surf = wl_container_of(listener, surf, destroy);
	wl_list_remove(&surf->link);
	wl_list_remove(&surf->commit.link);
	wl_list_remove(&surf->destroy.link);
	wl_list_remove(&surf->map.link);
	wl_list_remove(&surf->unmap.link);
	free(surf);
}

void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	struct wlr_output *output = layer_surface->output;
	if (!output) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (!output) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	struct wlr_scene_tree *layer_tree;
	switch (layer_surface->pending.layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		layer_tree = server->background_tree;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		layer_tree = server->bottom_tree;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		layer_tree = server->top_tree;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		layer_tree = server->overlay_tree;
		break;
	default:
		layer_tree = server->top_tree;
		break;
	}

	struct pudu_layer_surface *surf = calloc(1, sizeof(*surf));
	if (!surf) return;
	surf->server = server;
	surf->scene_layer = wlr_scene_layer_surface_v1_create(
		layer_tree, layer_surface);
	surf->output = output;
	surf->layer = layer_surface->pending.layer;
	layer_surface->data = surf;

	surf->commit.notify = layer_surface_handle_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &surf->commit);
	surf->map.notify = layer_surface_handle_map;
	wl_signal_add(&layer_surface->surface->events.map, &surf->map);
	surf->unmap.notify = layer_surface_handle_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &surf->unmap);

	surf->destroy.notify = layer_surface_handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &surf->destroy);

	wl_list_insert(&server->layer_surfaces, &surf->link);
}

void session_lock_handle_new_surface(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, session_lock_new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct wlr_scene_tree *tree = wlr_scene_tree_create(server->lock_tree);
	wlr_scene_surface_create(tree, lock_surface->surface);

	int width = 1920, height = 1080;
	if (lock_surface->output) {
		width = lock_surface->output->width;
		height = lock_surface->output->height;
	} else {
		struct wlr_output *output = wlr_output_layout_get_center_output(server->output_layout);
		if (output) {
			width = output->width;
			height = output->height;
		}
	}
	wlr_session_lock_surface_v1_configure(lock_surface, width, height);
}

void session_lock_handle_unlock(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, session_lock_unlock);
	wlr_scene_node_set_enabled(&server->lock_tree->node, false);
	server->cur_lock = NULL;
}

void session_lock_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, session_lock_destroy);
	if (server->cur_lock) {
		wlr_scene_node_set_enabled(&server->lock_tree->node, false);
		server->cur_lock = NULL;
	}
}

void server_new_session_lock(struct wl_listener *listener, void *data) {
	struct pudu_server *server = wl_container_of(listener, server, new_session_lock);
	struct wlr_session_lock_v1 *lock = data;
	if (server->cur_lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	server->cur_lock = lock;

	server->session_lock_new_surface.notify = session_lock_handle_new_surface;
	wl_signal_add(&lock->events.new_surface, &server->session_lock_new_surface);
	server->session_lock_unlock.notify = session_lock_handle_unlock;
	wl_signal_add(&lock->events.unlock, &server->session_lock_unlock);
	server->session_lock_destroy.notify = session_lock_handle_destroy;
	wl_signal_add(&lock->events.destroy, &server->session_lock_destroy);

	wlr_session_lock_v1_send_locked(lock);
	wlr_scene_node_set_enabled(&server->lock_tree->node, true);
	wlr_scene_node_raise_to_top(&server->lock_tree->node);
}

/* Config hot-reload via inotify */
int config_watch_cb(int fd, uint32_t mask, void *data) {
	struct pudu_server *server = data;
	char buf[4096];
	bool should_reload = false;

	/* Read in a loop to drain the inotify buffer and avoid losing events */
	while (true) {
		ssize_t len = read(fd, buf, sizeof(buf));
		if (len <= 0) {
			if (len < 0 && errno == EAGAIN) {
				break;
			}
			return 0;
		}

		for (char *ptr = buf; ptr < buf + len; ) {
			struct inotify_event *event = (struct inotify_event *)ptr;
			if (event->len > 0 && strcmp(event->name, "config") == 0) {
				if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
					should_reload = true;
				}
			}
			ptr += sizeof(struct inotify_event) + event->len;
		}
	}

	if (should_reload) {
		wlr_log(WLR_INFO, "Config file changed, reloading...");
		load_config(server);
		arrange_workspace(server, server->current_workspace);
	}
	return 0;
}

void setup_config_watcher(struct pudu_server *server) {
	server->config_watch_fd = -1;
	server->config_watch_source = NULL;

	const char *home = getenv("HOME");
	if (!home) return;

	char config_dir[512];
	snprintf(config_dir, sizeof(config_dir), "%s/.config/pudu", home);

	server->config_watch_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (server->config_watch_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to create inotify instance");
		return;
	}

	int wd = inotify_add_watch(server->config_watch_fd, config_dir,
		IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (wd < 0) {
		wlr_log(WLR_ERROR, "Failed to add inotify watch for config directory");
		close(server->config_watch_fd);
		server->config_watch_fd = -1;
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	server->config_watch_source = wl_event_loop_add_fd(loop, server->config_watch_fd,
		WL_EVENT_READABLE, config_watch_cb, server);

	wlr_log(WLR_INFO, "Config hot-reload watcher started");
}

void cleanup_config_watcher(struct pudu_server *server) {
	if (server->config_watch_source) {
		wl_event_source_remove(server->config_watch_source);
		server->config_watch_source = NULL;
	}
	if (server->config_watch_fd >= 0) {
		close(server->config_watch_fd);
		server->config_watch_fd = -1;
	}
}
