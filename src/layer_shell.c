#include "pudu.h"

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
