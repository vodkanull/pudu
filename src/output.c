#include "pudu.h"

void output_frame(struct wl_listener *listener, void *data) {
	struct pudu_output *output = wl_container_of(listener, output, frame);
	struct pudu_server *server = output->server;
	struct wlr_scene *scene = server->scene;

	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(scene, output->wlr_output);

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

	arrange_layers(output);

	if (server->config_error_msg[0]) {
		render_config_error(server);
	}
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
