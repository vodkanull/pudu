#include "pudu.h"

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
		if (fork() == 0) {
			setsid();
			long maxfd = sysconf(_SC_OPEN_MAX);
			if (maxfd < 0) maxfd = 1024;
			for (int fd = 3; fd < maxfd; fd++) close(fd);
			execl("/bin/sh", "/bin/sh", "-c", b->command, (void *)NULL);
			_exit(1);
		}
		break;
	case PUDU_ACTION_CYCLE_TOPLEVELS: {
		cycle_focus(server);
		break;
	}
	case PUDU_ACTION_SWAP_MASTER: {
		swap_master(server);
		break;
	}
	case PUDU_ACTION_WORKSPACE_NEXT: {
		int next_ws = server->current_workspace + 1;
		if (next_ws > server->workspace_count) next_ws = 1;
		view_workspace(server, next_ws);
		break;
	}
	case PUDU_ACTION_WORKSPACE_PREV: {
		int prev_ws = server->current_workspace - 1;
		if (prev_ws < 1) prev_ws = server->workspace_count;
		view_workspace(server, prev_ws);
		break;
	}
	case PUDU_ACTION_MOVE_WORKSPACE_NEXT: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			int old_ws = toplevel->workspace;
			toplevel->workspace = toplevel->workspace + 1;
			if (toplevel->workspace > server->workspace_count) toplevel->workspace = 1;
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
	case PUDU_ACTION_MOVE_WORKSPACE_PREV: {
		struct pudu_toplevel *toplevel = focused_toplevel(server);
		if (toplevel) {
			int old_ws = toplevel->workspace;
			int prev_ws = toplevel->workspace - 1;
			if (prev_ws < 1) prev_ws = server->workspace_count;
			toplevel->workspace = prev_ws;
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
	case PUDU_ACTION_RELOAD: {
		wlr_log(WLR_INFO, "Reloading config...");
		if (load_config(server)) {
			struct pudu_autostart *as;
			wl_list_for_each(as, &server->autostarts, link) {
				long maxfd = sysconf(_SC_OPEN_MAX);
				if (maxfd < 0) maxfd = 1024;
				pid_t pid = fork();
				if (pid == 0) {
					setsid();
					for (int fd = 3; fd < maxfd; fd++) close(fd);
					execl("/bin/sh", "/bin/sh", "-c", as->command, (void *)NULL);
					_exit(1);
				}
				as->pid = pid;
			}
			arrange_workspace(server, server->current_workspace);
		}
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
