#include "pudu.h"
#include <wlr/types/wlr_buffer.h>

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

	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

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
	uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	uint64_t elapsed = now - toplevel->border_anim_start;
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
	toplevel->border_anim_start = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	toplevel->border_animating = true;

	if (!toplevel->border_anim_timer) {
		struct wl_event_loop *loop = wl_display_get_event_loop(toplevel->server->wl_display);
		toplevel->border_anim_timer = wl_event_loop_add_timer(loop, border_anim_cb, toplevel);
	}
	wl_event_source_timer_update(toplevel->border_anim_timer, 16);
}

void set_border_target(struct pudu_toplevel *toplevel, const float target[4]) {
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

void center_toplevel(struct pudu_toplevel *toplevel) {
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
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->set_app_id.link);
	wl_list_remove(&toplevel->set_parent.link);
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

	toplevel->scene_tree = wlr_scene_tree_create(server->toplevel_tree);
	toplevel->scene_tree->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

	struct wlr_scene_tree *xdg_tree = wlr_scene_xdg_surface_create(
		toplevel->scene_tree, xdg_toplevel->base);
	if (xdg_tree == NULL) {
		wlr_scene_node_destroy(&toplevel->scene_tree->node);
		free(toplevel);
		return;
	}
	xdg_toplevel->base->data = xdg_tree;

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

	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);

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

	toplevel->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
	toplevel->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);
	toplevel->set_parent.notify = xdg_toplevel_set_parent;
	wl_signal_add(&xdg_toplevel->events.set_parent, &toplevel->set_parent);
}
