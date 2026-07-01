#include "pudu.h"

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

struct pudu_pointer_constraint {
	struct wl_listener destroy;
	struct wlr_pointer_constraint_v1 *constraint;
	struct pudu_server *server;
};

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

static void process_cursor_motion(struct pudu_server *server, uint32_t time) {
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
