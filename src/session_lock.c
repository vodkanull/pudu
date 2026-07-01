#include "pudu.h"

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
