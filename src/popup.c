#include "pudu.h"

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
