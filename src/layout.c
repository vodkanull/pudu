#include "pudu.h"

static double ease_out_back(double t) {
	double c1 = 1.70158;
	double c3 = c1 + 1;
	return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}

int arrange_anim_cb(void *data) {
	struct pudu_server *server = data;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	bool any_active = false;
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->arrange_animating || !t->mapped) continue;

		uint64_t elapsed = now - t->arrange_anim_start;
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
			t->arrange_anim_start = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
	struct pudu_toplevel **stack_wins = calloc(count, sizeof(*stack_wins));
	if (!stack_wins) return;
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
		if (floated) { free(stack_wins); arrange_workspace(server, workspace); return; }
	}

	int available_h = MAX(1, area.height - 2 * og);
	int master_ch = available_h;

	int *stack_heights = calloc(n_stack > 0 ? n_stack : 1, sizeof(*stack_heights));
	if (!stack_heights) { free(stack_wins); return; }
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
			if (floated) { free(stack_wins); free(stack_heights); arrange_workspace(server, workspace); return; }
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
	free(stack_wins);
	free(stack_heights);
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
