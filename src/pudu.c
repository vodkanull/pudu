#include "pudu.h"
#include <signal.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

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
	long maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd < 0) maxfd = 1024;
	for (int fd = 3; fd < maxfd; fd++) {
		close(fd);
	}
}

static pid_t spawn(const char *cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		close_all_fds();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(1);
	}
	return pid;
}

static double ease_out_back(double t);
static int arrange_anim_cb(void *data);

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);
	signal(SIGPIPE, SIG_IGN);
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
		return 0;
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
	server.border_transition_ms = 200;
	server.border_radius = 0;
	server.arrange_anim_ms = 300;
	server.arrange_anim = true;
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

	wl_list_init(&server.toplevels);
	wl_list_init(&server.layer_surfaces);
	wl_list_init(&server.popups);
	wl_list_init(&server.workspaces);
	wl_list_init(&server.manager_clients);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
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
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
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

	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	server.arrange_timer = wl_event_loop_add_timer(loop, arrange_anim_cb, &server);

	struct pudu_autostart *as;
	wl_list_for_each(as, &server.autostarts, link) {
		as->pid = spawn(as->command);
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
	wl_display_run(server.wl_display);

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

	wlr_scene_node_destroy(&server.scene->tree.node);
	if (server.arrange_timer) {
		wl_event_source_remove(server.arrange_timer);
	}
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}

void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

void keyboard_handle_key(
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

	if (!keyboard->wlr_keyboard->xkb_state) {
		wlr_log(WLR_ERROR, "key event but xkb_state is NULL");
		return;
	}

	wlr_log(WLR_DEBUG, "key event: keycode=%u nsyms=%d state=%s",
		keycode, nsyms,
		event->state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release");

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			for (int i = 0; i < nsyms; i++) {
				wlr_log(WLR_DEBUG, "  sym[%d]=0x%x mods=0x%x count=%d", i, syms[i], modifiers,
					workspace_window_count(server, server->current_workspace));
				handled = handle_keybinding(server, syms[i], modifiers);
			}
			if (handled) {
				wlr_log(WLR_DEBUG, "  binding handled -> exec");
			}
		}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

void server_new_keyboard(struct pudu_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	wlr_log(WLR_INFO, "new keyboard device: %s", device->name ? device->name : "unknown");

	struct pudu_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (!keymap) {
		wlr_log(WLR_ERROR, "failed to load system keymap, falling back to US layout");
		struct xkb_rule_names rules = {
			.rules = NULL,
			.model = NULL,
			.layout = "us",
			.variant = NULL,
			.options = NULL,
		};
		keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!keymap) {
			wlr_log(WLR_ERROR, "failed to load US fallback keymap either");
			xkb_context_unref(context);
			free(keyboard);
			return;
		}
	}

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

void server_new_pointer(struct pudu_server *server,
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

struct pudu_toplevel *desktop_toplevel_at(
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
	pc->server = server;
	pc->constraint = constraint;
	pc->destroy.notify = pointer_constraint_handle_destroy;
	wl_signal_add(&constraint->events.destroy, &pc->destroy);
	constraint->data = pc;
	update_active_pointer_constraint(server, server->seat->pointer_state.focused_surface);
}

void process_cursor_motion(struct pudu_server *server, uint32_t time) {
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);

	if (server->cursor_mode == NULLWC_CURSOR_MOVE) {
		struct pudu_toplevel *t = server->grabbed_toplevel;
		if (t) {
			double new_x = server->cursor->x - server->grab_x;
			double new_y = server->cursor->y - server->grab_y;
			wlr_scene_node_set_position(&t->scene_tree->node, new_x, new_y);
		}
		return;
	}

	if (server->cursor_mode == NULLWC_CURSOR_RESIZE_H) {
		struct wlr_output *wlr_output = wlr_output_layout_output_at(
				server->output_layout, server->cursor->x, server->cursor->y);
		if (!wlr_output) {
			wlr_output = wlr_output_layout_get_center_output(server->output_layout);
		}
		if (wlr_output) {
			struct pudu_output *output = output_from_wlr_output(server, wlr_output);
			struct wlr_box area;
			if (output) {
				area = output->usable_area;
			} else {
				wlr_output_layout_get_box(server->output_layout, wlr_output, &area);
			}
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

	if (toplevel && toplevel != server->focused_toplevel_ptr) {
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
	server->cursor_mode = NULLWC_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;

	if (!t) return;

	if (!t->dialog) t->floating = false;

	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (!wlr_output) {
		wlr_output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (!wlr_output) return;

	struct pudu_output *output = output_from_wlr_output(server, wlr_output);
	struct wlr_box area;
	if (output) {
		area = output->usable_area;
	} else {
		wlr_output_layout_get_box(server->output_layout, wlr_output, &area);
	}
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
		if (server->cursor_mode == NULLWC_CURSOR_MOVE) {
			finish_move(server);
			return;
		}
		if (server->cursor_mode == NULLWC_CURSOR_RESIZE_H) {
			server->cursor_mode = NULLWC_CURSOR_PASSTHROUGH;
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
			struct wlr_output *wlr_output = wlr_output_layout_output_at(
					server->output_layout, server->cursor->x, server->cursor->y);
			if (!wlr_output) {
				wlr_output = wlr_output_layout_get_center_output(server->output_layout);
			}
			if (wlr_output) {
				struct pudu_output *output = output_from_wlr_output(server, wlr_output);
				struct wlr_box area;
				if (output) {
					area = output->usable_area;
				} else {
					wlr_output_layout_get_box(server->output_layout, wlr_output, &area);
				}
				int ig = server->inner_gap;
				int og = server->outer_gap;
				int frames_w = area.width - 2 * og - ig;
				if (frames_w < 1) frames_w = 1;
				int master_fw = (int)(frames_w * server->master_ratio);
				int border_x = area.x + og + master_fw;
				if (server->cursor->x >= border_x - 8 && server->cursor->x <= border_x + ig + 8) {
					server->cursor_mode = NULLWC_CURSOR_RESIZE_H;
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
			server->cursor_mode = NULLWC_CURSOR_MOVE;
			server->grabbed_toplevel = toplevel;
			server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
			server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
		}
		return;
	}

	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);

	if (toplevel == NULL) {
		toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	}

	if (toplevel == NULL) {
		return;
	}

	focus_toplevel(toplevel);
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

void output_frame(struct wl_listener *listener, void *data) {
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

void output_resource_destroyed(struct wl_listener *listener, void *data) {
	struct pudu_output_resource *ores = wl_container_of(listener, ores, destroy);
	wl_list_remove(&ores->link);
	free(ores);
}

void output_bind(struct wl_listener *listener, void *data) {
	struct pudu_output *output = wl_container_of(listener, output, bind);
	struct wlr_output_event_bind *event = data;

	struct pudu_output_resource *ores = calloc(1, sizeof(*ores));
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

void output_destroy(struct wl_listener *listener, void *data) {
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

	/* Initialize usable_area and arrange any existing layer surfaces */
	arrange_layers(output);
}

/* Custom wlr_buffer implementation for Cairo-drawn rounded borders */
struct pudu_buffer {
	struct wlr_buffer base;
	void *data;
	size_t stride;
};

static void pudu_buffer_destroy(struct wlr_buffer *buffer) {
	struct pudu_buffer *nb = wl_container_of(buffer, nb, base);
	free(nb->data);
	free(nb);
}

static bool pudu_buffer_begin_data_ptr_access(struct wlr_buffer *buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct pudu_buffer *nb = wl_container_of(buffer, nb, base);
	*data = nb->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = nb->stride;
	return true;
}

static void pudu_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {
	/* no-op */
}

static const struct wlr_buffer_impl pudu_buffer_impl = {
	.destroy = pudu_buffer_destroy,
	.begin_data_ptr_access = pudu_buffer_begin_data_ptr_access,
	.end_data_ptr_access = pudu_buffer_end_data_ptr_access,
};

static struct wlr_buffer *create_rounded_border_buffer(int w, int h,
		const float color[4], int border_size, int radius) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	size_t stride = w * 4;
	void *data = calloc(1, stride * h);
	if (!data) return NULL;

	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, w, h, stride);
	cairo_t *cr = cairo_create(surf);

	/* Clear transparent */
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

	/* Draw outer rounded rectangle */
	double r = radius;
	if (r > w / 2.0) r = w / 2.0;
	if (r > h / 2.0) r = h / 2.0;

	cairo_new_sub_path(cr);
	cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
	cairo_arc(cr, w - r, r, r, 1.5 * M_PI, 2 * M_PI);
	cairo_arc(cr, w - r, h - r, r, 0, 0.5 * M_PI);
	cairo_arc(cr, r, h - r, r, 0.5 * M_PI, M_PI);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_fill(cr);

	/* Cut out inner transparent area */
	int b = border_size;
	if (w > 2 * b && h > 2 * b) {
		double ir = fmax(0, r - b);
		int iw = w - 2 * b;
		int ih = h - 2 * b;

		cairo_new_sub_path(cr);
		cairo_arc(cr, b + ir, b + ir, ir, M_PI, 1.5 * M_PI);
		cairo_arc(cr, b + iw - ir, b + ir, ir, 1.5 * M_PI, 2 * M_PI);
		cairo_arc(cr, b + iw - ir, b + ih - ir, ir, 0, 0.5 * M_PI);
		cairo_arc(cr, b + ir, b + ih - ir, ir, 0.5 * M_PI, M_PI);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, 0, 0, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_fill(cr);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(surf);

	struct pudu_buffer *nb = calloc(1, sizeof(*nb));
	if (!nb) {
		free(data);
		return NULL;
	}
	nb->data = data;
	nb->stride = stride;
	wlr_buffer_init(&nb->base, &pudu_buffer_impl, w, h);
	return &nb->base;
}

static bool border_buffer_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	return false;
}

static void toplevel_destroy_border(struct pudu_toplevel *toplevel) {
	for (int i = 0; i < 4; i++) {
		if (toplevel->border_rects[i]) {
			wlr_scene_node_destroy(&toplevel->border_rects[i]->node);
			toplevel->border_rects[i] = NULL;
		}
	}
	if (toplevel->border_buffer) {
		wlr_scene_node_destroy(&toplevel->border_buffer->node);
		toplevel->border_buffer = NULL;
	}
}

static void toplevel_update_border(struct pudu_toplevel *toplevel) {
	struct pudu_server *server = toplevel->server;
	int b = server->active_border_size;
	int r = server->border_radius;
	int w = toplevel->xdg_toplevel->base->geometry.width;
	int h = toplevel->xdg_toplevel->base->geometry.height;

	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	if (toplevel->border_w == w && toplevel->border_h == h &&
			toplevel->border_b == b && toplevel->border_r == r) {
		if (r == 0) {
			for (int i = 0; i < 4; i++) {
				if (toplevel->border_rects[i]) {
					wlr_scene_rect_set_color(toplevel->border_rects[i], toplevel->border_color);
				}
			}
		} else {
			int fw = w + 2 * b;
			int fh = h + 2 * b;
			struct wlr_buffer *buf = create_rounded_border_buffer(
				fw, fh, toplevel->border_color, b, r);
			if (buf) {
				wlr_scene_buffer_set_buffer(toplevel->border_buffer, buf);
				wlr_buffer_drop(buf);
			}
		}
		return;
	}

	if (r == 0) {
		/* Rect mode */
		if (toplevel->border_buffer) {
			wlr_scene_node_destroy(&toplevel->border_buffer->node);
			toplevel->border_buffer = NULL;
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
	} else {
		/* Rounded buffer mode */
		for (int i = 0; i < 4; i++) {
			if (toplevel->border_rects[i]) {
				wlr_scene_node_destroy(&toplevel->border_rects[i]->node);
				toplevel->border_rects[i] = NULL;
			}
		}
		if (!toplevel->border_buffer) {
			toplevel->border_buffer = wlr_scene_buffer_create(toplevel->border_tree, NULL);
			toplevel->border_buffer->point_accepts_input = border_buffer_accepts_input;
		}
		wlr_scene_node_raise_to_top(&toplevel->border_tree->node);

		int fw = w + 2 * b;
		int fh = h + 2 * b;
		if (fw < 1) fw = 1;
		if (fh < 1) fh = 1;
		struct wlr_buffer *buf = create_rounded_border_buffer(
			fw, fh, toplevel->border_color, b, r);
		if (buf) {
			wlr_scene_buffer_set_buffer(toplevel->border_buffer, buf);
			wlr_scene_node_set_position(&toplevel->border_buffer->node, -b, -b);
			wlr_buffer_drop(buf);
		}
	}

	toplevel->border_w = w;
	toplevel->border_h = h;
	toplevel->border_b = b;
	toplevel->border_r = r;
}

static void apply_border_color(struct pudu_toplevel *toplevel) {
	toplevel_update_border(toplevel);
}

static int border_anim_cb(void *data) {
	struct pudu_toplevel *toplevel = data;
	if (!toplevel->border_animating) return 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	uint32_t elapsed = now - toplevel->border_anim_start;
	int duration = toplevel->server->border_transition_ms;
	if (duration <= 0) duration = 1;

	double t = (double)elapsed / duration;
	if (t > 1.0) t = 1.0;

	for (int i = 0; i < 4; i++) {
		float start = toplevel->border_start[i];
		float target = toplevel->border_target[i];
		toplevel->border_color[i] = start + (target - start) * t;
	}
	apply_border_color(toplevel);

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
	if (toplevel->server->border_transition_ms <= 0) {
		memcpy(toplevel->border_color, target, sizeof(float) * 4);
		apply_border_color(toplevel);
		toplevel->border_animating = false;
		if (toplevel->border_anim_timer) {
			wl_event_source_remove(toplevel->border_anim_timer);
			toplevel->border_anim_timer = NULL;
		}
		return;
	}
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

	/* Raise child dialogs so they always stay above their parent */
	struct pudu_toplevel *child;
	wl_list_for_each(child, &server->toplevels, link) {
		if (child->dialog && child->mapped &&
				child->xdg_toplevel->parent == toplevel->xdg_toplevel) {
			wlr_scene_node_raise_to_top(&child->scene_tree->node);
		}
	}

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

struct pudu_toplevel *focused_toplevel(struct pudu_server *server) {
	return server->focused_toplevel_ptr;
}

static void center_toplevel(struct pudu_toplevel *toplevel) {
	struct pudu_server *server = toplevel->server;
	struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
	int w = geo.width;
	int h = geo.height;
	if (w < 1) w = 400;
	if (h < 1) h = 300;

	int b = server->active_border_size;
	int cx, cy;
	struct wlr_xdg_toplevel *parent = toplevel->xdg_toplevel->parent;
	if (parent) {
		struct wlr_scene_tree *parent_xdg_tree = parent->base->data;
		if (parent_xdg_tree && parent_xdg_tree->node.parent) {
			struct pudu_toplevel *parent_tl = parent_xdg_tree->node.parent->node.data;
			if (parent_tl && parent_tl->mapped) {
				double px = parent_tl->scene_tree->node.x;
				double py = parent_tl->scene_tree->node.y;
				struct wlr_box pgeo = parent->base->geometry;
				cx = px + pgeo.width / 2;
				cy = py + pgeo.height / 2;
				int fx = cx - w / 2;
				int fy = cy - h / 2;
				wlr_scene_node_set_position(&toplevel->scene_tree->node, fx, fy);
				struct wlr_scene_tree *xdg_tree = toplevel->xdg_toplevel->base->data;
				if (xdg_tree) {
					wlr_scene_node_set_position(&xdg_tree->node, b, b);
				}
				wlr_scene_node_set_position(&toplevel->border_tree->node, b, b);
				toplevel->allocated.x = fx;
				toplevel->allocated.y = fy;
				toplevel->allocated.width = w + 2 * b;
				toplevel->allocated.height = h + 2 * b;
				return;
			}
		}
	}

	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (!wlr_output) {
		wlr_output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (wlr_output) {
		struct pudu_output *output = output_from_wlr_output(server, wlr_output);
		struct wlr_box area;
		if (output) {
			area = output->usable_area;
		} else {
			wlr_output_layout_get_box(server->output_layout, wlr_output, &area);
		}
		cx = area.x + area.width / 2;
		cy = area.y + area.height / 2;
		int fx = cx - w / 2;
		int fy = cy - h / 2;
		wlr_scene_node_set_position(&toplevel->scene_tree->node, fx, fy);
		struct wlr_scene_tree *xdg_tree = toplevel->xdg_toplevel->base->data;
		if (xdg_tree) {
			wlr_scene_node_set_position(&xdg_tree->node, b, b);
		}
		wlr_scene_node_set_position(&toplevel->border_tree->node, b, b);
		toplevel->allocated.x = fx;
		toplevel->allocated.y = fy;
		toplevel->allocated.width = w + 2 * b;
		toplevel->allocated.height = h + 2 * b;
	}
}

void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wlr_log(WLR_DEBUG, "map: title='%s' app_id='%s'",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
		toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");

	if (toplevel->xdg_toplevel->parent != NULL) {
		toplevel->dialog = true;
		toplevel->floating = true;
		wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, WLR_EDGE_NONE);
	}

	if (toplevel->server->new_is_master) {
		wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	} else {
		wl_list_insert(toplevel->server->toplevels.prev, &toplevel->link);
	}
	toplevel->mapped = true;
	arrange_workspace(toplevel->server, toplevel->workspace);
	if (toplevel->dialog) {
		center_toplevel(toplevel);
	}
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);

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

void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
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
	if (toplevel->dialog && ws == toplevel->server->current_workspace) {
		struct wlr_xdg_toplevel *parent = toplevel->xdg_toplevel->parent;
		if (parent) {
			struct wlr_scene_tree *parent_xdg_tree = parent->base->data;
			if (parent_xdg_tree && parent_xdg_tree->node.parent) {
				struct pudu_toplevel *parent_tl = parent_xdg_tree->node.parent->node.data;
				if (parent_tl && parent_tl->mapped && parent_tl->workspace == ws) {
					focus_toplevel(parent_tl);
				}
			}
		}
	}
}



void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
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
			toplevel->border_b != toplevel->server->active_border_size ||
			toplevel->border_r != toplevel->server->border_radius) {
		toplevel_update_border(toplevel);
	}
}

void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	struct pudu_server *server = toplevel->server;
	wlr_log(WLR_INFO, "DESTROY xdg=%p toplevel=%p server=%p link={%p,%p}",
		(void*)toplevel->xdg_toplevel, (void*)toplevel, (void*)server,
		(void*)toplevel->link.prev, (void*)toplevel->link.next);
	toplevel->arrange_animating = false;
	if (toplevel->border_anim_timer) {
		wl_event_source_remove(toplevel->border_anim_timer);
		toplevel->border_anim_timer = NULL;
	}
	toplevel_destroy_border(toplevel);
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->set_app_id.link);
	wl_list_remove(&toplevel->set_parent.link);
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

void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
}

void xdg_toplevel_request_maximize(
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

void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (xdg->base->initialized) {
		apply_fullscreen_state(toplevel, xdg->requested.fullscreen, xdg->requested.fullscreen_output);
	}
}

void ft_handle_request_maximize(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, event->maximized);
	}
}

void ft_handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (xdg->base->initialized) {
		apply_fullscreen_state(toplevel, event->fullscreen, NULL);
	}
}

void ft_handle_request_activate(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_activate);
	focus_toplevel(toplevel);
}

void ft_handle_request_close(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, ft_handle_request_close);
	wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

void ft_handle_destroy(struct wl_listener *listener, void *data) {
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

void decoration_handle_request_mode(struct wl_listener *listener, void *data) {
	struct pudu_decoration *dec = wl_container_of(listener, dec, request_mode);
	struct wlr_xdg_toplevel_decoration_v1 *deco = dec->decoration;

	if (dec->toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (toplevel->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_set_title(
			toplevel->foreign_handle, xdg->title ? xdg->title : "");
	}
}

void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, set_app_id);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	if (toplevel->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_set_app_id(
			toplevel->foreign_handle, xdg->app_id ? xdg->app_id : "");
	}
}

void xdg_toplevel_set_parent(struct wl_listener *listener, void *data) {
	struct pudu_toplevel *toplevel = wl_container_of(listener, toplevel, set_parent);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
	bool had_parent = toplevel->dialog;
	toplevel->dialog = (xdg->parent != NULL);
	if (toplevel->dialog && toplevel->mapped) {
		toplevel->floating = true;
		wlr_xdg_toplevel_set_tiled(xdg, WLR_EDGE_NONE);
		center_toplevel(toplevel);
		focus_toplevel(toplevel);
		arrange_workspace(toplevel->server, toplevel->workspace);
	} else if (!toplevel->dialog && had_parent && toplevel->mapped) {
		toplevel->floating = false;
		arrange_workspace(toplevel->server, toplevel->workspace);
	}
}

void decoration_handle_destroy(struct wl_listener *listener, void *data) {
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
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->workspace = server->current_workspace;
	toplevel->dialog = (xdg_toplevel->parent != NULL);
	wl_list_init(&toplevel->link);

	/* Parent tree: positioned/cascade by us, used for hit-testing */
	toplevel->scene_tree = wlr_scene_tree_create(server->toplevel_tree);
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

	/* Border tree: holds either rects or a rounded buffer */
	toplevel->border_tree = wlr_scene_tree_create(toplevel->scene_tree);
	toplevel->border_buffer = NULL;
	for (int i = 0; i < 4; i++) {
		toplevel->border_rects[i] = NULL;
	}
	memcpy(toplevel->border_color, server->inactive_border_color, sizeof(float) * 4);
	toplevel->border_w = 0;
	toplevel->border_h = 0;
	toplevel->border_b = 0;
	toplevel->border_r = -1;
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

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
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

	/* Update foreign handle when title, app_id or parent changes */
	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
	toplevel->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);
	toplevel->set_parent.notify = xdg_toplevel_set_parent;
	wl_signal_add(&xdg_toplevel->events.set_parent, &toplevel->set_parent);
}

void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct pudu_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit &&
			popup->xdg_popup->base->initialized) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct pudu_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->link);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
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
	popup->xdg_popup = xdg_popup;

	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	if (xdg_popup->base->data == NULL) {
		free(popup);
		return;
	}

	wl_list_insert(&server->popups, &popup->link);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static double ease_out_back(double t) {
	double c1 = 1.70158;
	double c3 = c1 + 1;
	return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}

static int arrange_anim_cb(void *data) {
	struct pudu_server *server = data;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

	bool any_active = false;
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->arrange_animating || !t->mapped) continue;

		uint32_t elapsed = now - t->arrange_anim_start;
		int duration = server->arrange_anim_ms;
		if (duration <= 0) duration = 1;

		double progress = (double)elapsed / duration;
		if (progress > 1.0) progress = 1.0;

		double eased = ease_out_back(progress);

		double x = t->arrange_from_x + (t->arrange_to_x - t->arrange_from_x) * eased;
		double y = t->arrange_from_y + (t->arrange_to_y - t->arrange_from_y) * eased;
		wlr_scene_node_set_position(&t->scene_tree->node, x, y);

		if (progress >= 1.0) {
			t->arrange_animating = false;
			wlr_scene_node_set_position(&t->scene_tree->node,
				t->arrange_to_x, t->arrange_to_y);
		} else {
			any_active = true;
		}
	}

	if (any_active) {
		wl_event_source_timer_update(server->arrange_timer, 16);
	}
	return 0;
}

static void set_tiled(struct pudu_toplevel *t, int x, int y, int w, int h) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	int b = t->server->active_border_size;
	int cw = w - 2 * b;
	int ch = h - 2 * b;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;

	struct pudu_server *server = t->server;
	double cur_x = t->scene_tree->node.x;
	double cur_y = t->scene_tree->node.y;
	double dx = fabs(cur_x - x);
	double dy = fabs(cur_y - y);
	bool skip_anim = !server->arrange_anim ||
		(server->arrange_anim_ms <= 0) ||
		(t->allocated.width == 0 && t->allocated.height == 0) ||
		(dx < 1.0 && dy < 1.0);

	if (skip_anim) {
		wlr_scene_node_set_position(&t->scene_tree->node, x, y);
		t->arrange_animating = false;
	} else {
		t->arrange_from_x = cur_x;
		t->arrange_from_y = cur_y;
		t->arrange_to_x = x;
		t->arrange_to_y = y;
		if (!t->arrange_animating) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			t->arrange_anim_start = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
		}
		t->arrange_animating = true;
		if (server->arrange_timer) {
			wl_event_source_timer_update(server->arrange_timer, 16);
		}
	}

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

static bool win_has_min(struct pudu_toplevel *t, int *min_w, int *min_h) {
	*min_w = t->xdg_toplevel->current.min_width;
	*min_h = t->xdg_toplevel->current.min_height;
	return *min_w > 0 || *min_h > 0;
}

void arrange_workspace(struct pudu_server *server, int workspace) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
	if (!wlr_output) {
		wlr_output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (!wlr_output) return;

	struct pudu_output *output = output_from_wlr_output(server, wlr_output);
	struct wlr_box area;
	if (output) {
		area = output->usable_area;
	} else {
		wlr_output_layout_get_box(server->output_layout, wlr_output, &area);
	}

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

	if (count == 1) {
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->workspace != workspace || !t->mapped || t->fullscreen || t->floating) continue;
			int fx = MAX(1, area.x + og);
			int fy = MAX(1, area.y + og);
			int fw = MAX(1, area.width - 2 * og);
			int fh = MAX(1, area.height - 2 * og);
			int mw, mh;
			if (win_has_min(t, &mw, &mh)) {
				if (mw > 0 && fw < mw) fw = mw;
				if (mh > 0 && fh < mh) fh = mh;
			}
			int mxw = t->xdg_toplevel->current.max_width;
			int mxh = t->xdg_toplevel->current.max_height;
			if (mxw > 0 && fw > mxw) fw = mxw;
			if (mxh > 0 && fh > mxh) fh = mxh;
			set_tiled(t, fx, fy, fw, fh);
			return;
		}
	}

	int frames_w = MAX(1, area.width - 2 * og - ig);
	float master_ratio = server->master_ratio;
	int master_fw = MAX(1, (int)(frames_w * master_ratio));
	int stack_fw = MAX(1, frames_w - master_fw);

	struct pudu_toplevel *master = NULL;
	struct pudu_toplevel *stack_wins[64];
	int n_stack = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->workspace != workspace || !t->mapped || t->fullscreen || t->floating) continue;
		if (!master) { master = t; continue; }
		stack_wins[n_stack++] = t;
	}

	int master_min_w = master->xdg_toplevel->current.min_width;
	int stack_min_w = 0;
	bool floated = false;
	for (int i = 0; i < n_stack; i++) {
		int mw = stack_wins[i]->xdg_toplevel->current.min_width;
		if (mw > stack_min_w) stack_min_w = mw;
	}

	if (stack_min_w > 0 && stack_fw < stack_min_w) {
		stack_fw = MIN(stack_min_w, frames_w - 1);
		master_fw = MAX(1, frames_w - stack_fw);
	}
	if (master_min_w > 0 && master_fw < master_min_w) {
		master_fw = MIN(master_min_w, frames_w - 1);
		stack_fw = MAX(1, frames_w - master_fw);
	}
	if ((master_min_w > 0 && master_fw < master_min_w) ||
		(stack_min_w > 0 && stack_fw < stack_min_w)) {
		if (master_min_w > 0 && master_fw < master_min_w) {
			master->floating = true;
			floated = true;
			wlr_xdg_toplevel_set_tiled(master->xdg_toplevel, WLR_EDGE_NONE);
			center_toplevel(master);
		} else {
			for (int i = 0; i < n_stack; i++) {
				if (stack_wins[i]->xdg_toplevel->current.min_width > stack_fw) {
					stack_wins[i]->floating = true;
					floated = true;
					wlr_xdg_toplevel_set_tiled(stack_wins[i]->xdg_toplevel, WLR_EDGE_NONE);
					center_toplevel(stack_wins[i]);
				}
			}
		}
		if (floated) { arrange_workspace(server, workspace); return; }
	}

	int available_h = MAX(1, area.height - 2 * og);
	int master_ch = available_h;

	int stack_heights[64];
	int total_min_h = 0;
	bool stack_has_min = false;
	for (int i = 0; i < n_stack; i++) {
		int mh = stack_wins[i]->xdg_toplevel->current.min_height;
		if (mh > 0) { total_min_h += mh; stack_has_min = true; }
		stack_heights[i] = 0;
	}

	if (stack_has_min) {
		int min_space = total_min_h + MAX(0, n_stack - 1) * ig;
		if (min_space <= available_h) {
			int extra = available_h - min_space;
			int base_extra = extra / n_stack;
			int rem = extra - base_extra * n_stack;
			for (int i = 0; i < n_stack; i++) {
				int mh = stack_wins[i]->xdg_toplevel->current.min_height;
				stack_heights[i] = MAX(1, mh + base_extra);
				if (rem > 0) { stack_heights[i]++; rem--; }
			}
		} else {
			floated = false;
			for (int i = 0; i < n_stack; i++) {
				int mh = stack_wins[i]->xdg_toplevel->current.min_height;
				if (mh > available_h / n_stack) {
					stack_wins[i]->floating = true;
					floated = true;
					wlr_xdg_toplevel_set_tiled(stack_wins[i]->xdg_toplevel, WLR_EDGE_NONE);
					center_toplevel(stack_wins[i]);
				}
			}
			if (floated) { arrange_workspace(server, workspace); return; }
			int frame_h = MAX(1, (available_h - (n_stack - 1) * ig) / n_stack);
			for (int i = 0; i < n_stack; i++) stack_heights[i] = frame_h;
		}
	} else {
		int frame_h = MAX(1, (available_h - (n_stack - 1) * ig) / n_stack);
		for (int i = 0; i < n_stack; i++) stack_heights[i] = frame_h;
	}

	set_tiled(master, area.x + og, area.y + og, master_fw, master_ch);

	int fy = area.y + og;
	for (int i = 0; i < n_stack; i++) {
		int h = stack_heights[i] > 0 ? stack_heights[i]
			: MAX(1, (available_h - (n_stack - 1) * ig) / n_stack);
		if (i == n_stack - 1) {
			h = MAX(1, area.y + area.height - og - fy);
		}
		set_tiled(stack_wins[i],
			area.x + og + master_fw + ig,
			fy, stack_fw, h);
		fy += h + ig;
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

	wl_list_remove(&focused->link);
	wl_list_insert(first->link.prev, &focused->link);

	arrange_workspace(server, ws);
	focus_toplevel(focused);
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
		struct wl_resource *ws_res, *ws_res_tmp;
		wl_list_for_each_safe(ws_res, ws_res_tmp, &ws->resources, link) {
			if (wl_resource_get_client(ws_res) == wl_resource_get_client(mc->manager_resource)) {
				ext_workspace_group_handle_v1_send_workspace_leave(
					mc->group_resource, ws_res);
			}
		}
	}

	struct wl_resource *ws_res, *res_tmp;
	wl_list_for_each_safe(ws_res, res_tmp, &ws->resources, link) {
		wl_resource_set_user_data(ws_res, NULL);
		ext_workspace_handle_v1_send_removed(ws_res);
		wl_resource_destroy(ws_res);
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

double ease_out_cubic(double t) {
	return 1.0 - pow(1.0 - t, 3.0);
}

#define ANIM_DURATION_MS 220
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

	struct pudu_toplevel *to_focus = NULL;
	struct pudu_toplevel *node;
	wl_list_for_each(node, &server->toplevels, link) {
		if (node->workspace == new_ws) {
			to_focus = node;
			break;
		}
	}
	if (to_focus) {
		focus_toplevel(to_focus);
	} else {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}

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
		struct wl_resource *ws_res;
		wl_list_for_each(ws_res, &ws->resources, link) {
			ext_workspace_handle_v1_send_state(ws_res, 0);
		}
	}
	get_or_create_workspace(server, workspace);
	ws = find_workspace(server, workspace);
	if (ws) {
		ws->active = true;
		struct wl_resource *ws_res;
		wl_list_for_each(ws_res, &ws->resources, link) {
			ext_workspace_handle_v1_send_state(ws_res,
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
				if (server->anim_old_count < MAX_ANIM_TOPLEVELS) {
					server->anim_old_list[server->anim_old_count++] = t;
				}
			} else if (t->workspace == workspace && t->mapped) {
				if (server->anim_new_count < MAX_ANIM_TOPLEVELS) {
					server->anim_new_list[server->anim_new_count++] = t;
				}
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
		struct pudu_toplevel *to_focus = NULL;
		struct pudu_toplevel *node;
		wl_list_for_each(node, &server->toplevels, link) {
			if (node->workspace == workspace) {
				to_focus = node;
				break;
			}
		}
		if (to_focus) focus_toplevel(to_focus);
		else wlr_seat_keyboard_notify_clear_focus(server->seat);
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

int get_dynamic_workspace_count(struct pudu_server *server) {
	return server->workspace_count;
}
/*
 * ext_workspace_manager_v1 protocol implementation
 */

/* ext_workspace_handle_v1 request handlers */

void workspace_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void workspace_handle_activate(struct wl_client *client,
		struct wl_resource *resource) {
	struct pudu_workspace *ws = wl_resource_get_user_data(resource);
	if (ws && !ws->active) {
		view_workspace(ws->server, ws->number);
	}
}

void workspace_handle_deactivate(struct wl_client *client,
		struct wl_resource *resource) {
}

void workspace_handle_remove(struct wl_client *client,
		struct wl_resource *resource) {
}

void workspace_handle_assign(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *workspace_group) {
}

void workspace_handle_resource_destroyed(struct wl_resource *resource) {
	wl_list_remove(&resource->link);
}

static const struct ext_workspace_handle_v1_interface workspace_handle_impl = {
	.destroy = workspace_handle_destroy,
	.activate = workspace_handle_activate,
	.deactivate = workspace_handle_deactivate,
	.remove = workspace_handle_remove,
	.assign = workspace_handle_assign,
};

/* ext_workspace_group_handle_v1 request handlers */

void group_handle_create_workspace(struct wl_client *client,
		struct wl_resource *resource, const char *name) {
}

void group_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void group_handle_resource_destroyed(struct wl_resource *resource) {
	struct pudu_manager_client *mc = wl_resource_get_user_data(resource);
	if (mc) {
		mc->group_resource = NULL;
	}
}

static const struct ext_workspace_group_handle_v1_interface group_handle_impl = {
	.create_workspace = group_handle_create_workspace,
	.destroy = group_handle_destroy,
};

/* ext_workspace_manager_v1 request handlers */

void manager_handle_commit(struct wl_client *client,
		struct wl_resource *resource) {
}

void manager_handle_stop(struct wl_client *client,
		struct wl_resource *resource) {
	ext_workspace_manager_v1_send_finished(resource);
}

void manager_handle_resource_destroyed(struct wl_resource *resource) {
	struct pudu_manager_client *mc = wl_resource_get_user_data(resource);
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

struct pudu_workspace *find_workspace(struct pudu_server *server, int number) {
	struct pudu_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->number == number) return ws;
	}
	return NULL;
}

void create_workspace(struct pudu_server *server, int number) {
	struct pudu_workspace *ws = calloc(1, sizeof(*ws));
	ws->server = server;
	ws->number = number;
	ws->active = (number == server->current_workspace);
	wl_list_init(&ws->resources);
	wl_list_insert(server->workspaces.prev, &ws->link);

	char name[16];
	snprintf(name, sizeof(name), "%d", number);

	struct pudu_manager_client *mc, *mc_tmp;
	wl_list_for_each_safe(mc, mc_tmp, &server->manager_clients, link) {
		struct wl_client *client = wl_resource_get_client(mc->manager_resource);
		struct wl_resource *ws_res = wl_resource_create(client,
			&ext_workspace_handle_v1_interface,
			wl_resource_get_version(mc->manager_resource), 0);
		wl_resource_set_implementation(ws_res, &workspace_handle_impl,
			ws, workspace_handle_resource_destroyed);

		ext_workspace_manager_v1_send_workspace(mc->manager_resource, ws_res);
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

		wl_list_insert(&ws->resources, &ws_res->link);
	}

	if (!wl_list_empty(&server->manager_clients)) {
		workspace_send_done_all(server);
		struct pudu_manager_client *mc, *mc_tmp;
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

	mc->manager_resource = wl_resource_create(
		client, &ext_workspace_manager_v1_interface, version, id);
	wl_resource_set_implementation(mc->manager_resource, &manager_impl,
		mc, manager_handle_resource_destroyed);

	wl_list_insert(&server->manager_clients, &mc->link);

	mc->group_resource = wl_resource_create(
		client, &ext_workspace_group_handle_v1_interface, version, 0);
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
		struct wl_resource *ws_resource = wl_resource_create(
			client, &ext_workspace_handle_v1_interface, version, 0);
		wl_resource_set_implementation(ws_resource, &workspace_handle_impl,
			ws, workspace_handle_resource_destroyed);

		ext_workspace_manager_v1_send_workspace(mc->manager_resource, ws_resource);

		char name[16];
		snprintf(name, sizeof(name), "%d", ws->number);
		ext_workspace_handle_v1_send_name(ws_resource, name);
		if (ws->active) {
			ext_workspace_handle_v1_send_state(ws_resource,
				EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE);
		}
		ext_workspace_handle_v1_send_capabilities(ws_resource,
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);

		if (mc->group_resource) {
			ext_workspace_group_handle_v1_send_workspace_enter(
				mc->group_resource, ws_resource);
		}

		wl_list_insert(&ws->resources, &ws_resource->link);
	}

	ext_workspace_manager_v1_send_done(mc->manager_resource);
	wl_client_flush(client);
}

void execute_binding(struct pudu_server *server,
		struct pudu_binding *b) {
	switch (b->action) {
	case NULLWC_ACTION_CLOSE: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		}
		break;
	}
	case NULLWC_ACTION_EXEC:
		if (fork() == 0) {
			setsid();
			close_all_fds();
			execl("/bin/sh", "/bin/sh", "-c", b->command, (void *)NULL);
			_exit(1);
		}
		break;
	case NULLWC_ACTION_CYCLE_TOPLEVELS: {
		cycle_focus(server);
		break;
	}
	case NULLWC_ACTION_SWAP_MASTER: {
		swap_master(server);
		break;
	}
	case NULLWC_ACTION_WORKSPACE_NEXT: {
		int next_ws = server->current_workspace + 1;
		if (next_ws > server->workspace_count) next_ws = 1;
		view_workspace(server, next_ws);
		break;
	}
	case NULLWC_ACTION_WORKSPACE_PREV: {
		int prev_ws = server->current_workspace - 1;
		if (prev_ws < 1) prev_ws = server->workspace_count;
		view_workspace(server, prev_ws);
		break;
	}
	case NULLWC_ACTION_MOVE_WORKSPACE_NEXT: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			int old_ws = toplevel->workspace;
			toplevel->workspace = toplevel->workspace + 1;
			if (toplevel->workspace > server->workspace_count) toplevel->workspace = 1;
			/* Re-insert according to new_is_master so layout order is correct */
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
	case NULLWC_ACTION_MOVE_WORKSPACE_PREV: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			int old_ws = toplevel->workspace;
			int prev_ws = toplevel->workspace - 1;
			if (prev_ws < 1) prev_ws = server->workspace_count;
			toplevel->workspace = prev_ws;
			/* Re-insert according to new_is_master so layout order is correct */
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
	case NULLWC_ACTION_EXIT:
		wl_display_terminate(server->wl_display);
		break;
	case NULLWC_ACTION_RELOAD: {
		wlr_log(WLR_INFO, "Reloading config...");
		load_config(server);
		struct pudu_autostart *as;
		wl_list_for_each(as, &server->autostarts, link) {
			as->pid = spawn(as->command);
		}
		arrange_workspace(server, server->current_workspace);
		break;
	}
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

	if (strcmp(action_str, "PUDU_CLOSE") == 0) b->action = NULLWC_ACTION_CLOSE;
	else if (strcmp(action_str, "PUDU_EXIT") == 0) b->action = NULLWC_ACTION_EXIT;
	else if (strcmp(action_str, "PUDU_CYCLE_TOPLEVELS") == 0) b->action = NULLWC_ACTION_CYCLE_TOPLEVELS;
	else if (strcmp(action_str, "PUDU_SWAP_MASTER") == 0) b->action = NULLWC_ACTION_SWAP_MASTER;
	else if (strcmp(action_str, "PUDU_WORKSPACE_NEXT") == 0) b->action = NULLWC_ACTION_WORKSPACE_NEXT;
	else if (strcmp(action_str, "PUDU_WORKSPACE_PREV") == 0) b->action = NULLWC_ACTION_WORKSPACE_PREV;
	else if (strcmp(action_str, "PUDU_WORKSPACE_MOVE_NEXT") == 0) b->action = NULLWC_ACTION_MOVE_WORKSPACE_NEXT;
	else if (strcmp(action_str, "PUDU_WORKSPACE_MOVE_PREV") == 0) b->action = NULLWC_ACTION_MOVE_WORKSPACE_PREV;
	else if (strcmp(action_str, "PUDU_RELOAD") == 0) b->action = NULLWC_ACTION_RELOAD;
	else {
		b->action = NULLWC_ACTION_EXEC;
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

#include "build/default_config.inc"

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

static void config_parse_kv(struct pudu_server *server, const char *key, const char *value) {
	if (strcmp(key, "mod") == 0) {
		uint32_t mod = modifier_from_name(value);
		if (mod) server->mod_modifier = mod;
	} else if (strcmp(key, "inner_gap") == 0) {
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
	} else if (strcmp(key, "workspace_count") == 0) {
		int n = atoi(value);
		if (n >= 1) server->workspace_count = n;
	}
}

static void config_parse_line(struct pudu_server *server, char *line) {
	line = strip_comment(line);
	line = trim(line);
	if (line[0] == '\0') return;

	/* Try key = value */
	char *eq = strchr(line, '=');
	if (eq) {
		*eq = '\0';
		char *key = trim(line);
		char *value = trim(eq + 1);
		config_parse_kv(server, key, value);
		return;
	}

	/* Try space-separated key value */
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
			/* Remove trailing newline */
			size_t len = strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
				line[--len] = '\0';
			}

			char *trimmed = trim(line);
			if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

			/* Detect section start */
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
				/* Split into spec + action by first whitespace */
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

	/* Force border geometry and color update for all mapped toplevels */
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->mapped) continue;
		t->border_w = 0; /* force geometry update on next commit */
		t->border_r = -1; /* force border mode switch if radius changed */
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

void layer_surface_handle_commit(struct wl_listener *listener, void *data) {
	struct pudu_layer_surface *surf = wl_container_of(listener, surf, commit);
	struct wlr_layer_surface_v1 *layer_surface = surf->scene_layer->layer_surface;

	uint32_t committed = layer_surface->current.committed;
	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		surf->layer = layer_surface->current.layer;
	}
	if (!surf->configured || committed) {
		surf->configured = true;
		struct pudu_output *output = output_from_wlr_output(surf->server, surf->output);
		if (output) {
			arrange_layers(output);
		}
	}
}

void layer_surface_handle_map(struct wl_listener *listener, void *data) {
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

void layer_surface_handle_unmap(struct wl_listener *listener, void *data) {
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

void layer_surface_handle_destroy(struct wl_listener *listener, void *data) {
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
	wlr_session_lock_surface_v1_configure(lock_surface,
			lock_surface->output->width, lock_surface->output->height);
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
