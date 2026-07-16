#include "pudu.h"
#include <signal.h>

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

static void setup_portals(void) {
	const char *home = getenv("HOME");
	if (!home) return;

	char path[1024];
	struct stat st;

	snprintf(path, sizeof(path), "%s/.config/xdg-desktop-portal", home);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.config/xdg-desktop-portal/portals.conf", home);
	if (stat(path, &st) != 0) {
		FILE *f = fopen(path, "w");
		if (f) {
			fprintf(f, "[preferred]\ndefault=wlr\n");
			fclose(f);
			wlr_log(WLR_INFO, "setup_portals: created %s", path);
		}
	}

	snprintf(path, sizeof(path), "%s/.config/xdg-desktop-portal-wlr", home);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.config/xdg-desktop-portal-wlr/config", home);
	if (stat(path, &st) != 0) {
		FILE *f = fopen(path, "w");
		if (f) {
			fprintf(f, "[output]\nname=\n[screencast]\noutput_name=\n");
			fclose(f);
			wlr_log(WLR_INFO, "setup_portals: created %s", path);
		}
	}

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		close_all_fds();
		static const char *paths[] = {
			"/usr/libexec/xdg-desktop-portal",
			"/usr/lib/xdg-desktop-portal",
			"/usr/local/libexec/xdg-desktop-portal",
			"/usr/local/lib/xdg-desktop-portal",
			NULL
		};
		for (int i = 0; paths[i]; i++) {
			execl(paths[i], "xdg-desktop-portal", "--replace", (char *)NULL);
		}
		wlr_log(WLR_ERROR, "setup_portals: failed to exec xdg-desktop-portal");
		_exit(1);
	}
	wlr_log(WLR_INFO, "setup_portals: started xdg-desktop-portal (pid=%d)", pid);

	pid = fork();
	if (pid == 0) {
		setsid();
		close_all_fds();
		static const char *paths[] = {
			"/usr/libexec/xdg-desktop-portal-wlr",
			"/usr/lib/xdg-desktop-portal-wlr",
			"/usr/local/libexec/xdg-desktop-portal-wlr",
			"/usr/local/lib/xdg-desktop-portal-wlr",
			NULL
		};
		for (int i = 0; paths[i]; i++) {
			execl(paths[i], "xdg-desktop-portal-wlr", "--replace", "--loglevel", "INFO", (char *)NULL);
		}
		wlr_log(WLR_ERROR, "setup_portals: failed to exec xdg-desktop-portal-wlr");
		_exit(1);
	}
	wlr_log(WLR_INFO, "setup_portals: started xdg-desktop-portal-wlr (pid=%d)", pid);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
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

	server.request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag,
			&server.request_start_drag);
	server.start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag,
			&server.start_drag);

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

	setup_portals();

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
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);
	wl_list_remove(&server.xdg_activation_request_activate.link);
	wl_list_remove(&server.new_pointer_constraint.link);

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
