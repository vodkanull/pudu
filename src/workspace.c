#include "pudu.h"

#define ANIM_DURATION_MS 220

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
	int actual_count = 0;
	struct pudu_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		actual_count++;
	}

	for (int i = actual_count + 1; i <= server->workspace_count; i++) {
		get_or_create_workspace(server, i);
	}

	while (actual_count > server->workspace_count) {
		struct pudu_workspace *last = NULL;
		wl_list_for_each(ws, &server->workspaces, link) {
			last = ws;
		}
		if (!last) break;
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

	if (server->animating) {
		workspace_animation_finish(server);
	}

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

	int direction = (workspace > old_ws) ? 1 : -1;
	server->anim_direction = direction;

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

	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
	double slide_dist = wlr_output ? wlr_output->width : 1920;
	server->anim_slide_dist = slide_dist;
	server->anim_new_workspace = workspace;

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
