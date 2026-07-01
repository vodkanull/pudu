#include "pudu.h"
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>

static struct wlr_buffer *kawase_blur(struct pudu_server *server,
		struct wlr_texture *src_tex, int w, int h, int levels) {
	wlr_log(WLR_DEBUG, "BLUR:  %dx%d levels=%d", w, h, levels);
	if (levels < 1) levels = 1;

	int sizes[10][2];
	sizes[0][0] = w;  sizes[0][1] = h;
	for (int i = 1; i <= levels; i++) {
		sizes[i][0] = sizes[i-1][0] / 2;
		sizes[i][1] = sizes[i-1][1] / 2;
		if (sizes[i][0] < 1) sizes[i][0] = 1;
		if (sizes[i][1] < 1) sizes[i][1] = 1;
	}

	const struct wlr_drm_format *fmt = wlr_drm_format_set_get(
		wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DMABUF),
		DRM_FORMAT_XRGB8888);
	if (!fmt) {
		wlr_log(WLR_DEBUG, "BLUR:  no dmabuf fmt, trying SHM");
		fmt = wlr_drm_format_set_get(
			wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_SHM),
			DRM_FORMAT_XRGB8888);
	}
	if (!fmt) {
		wlr_log(WLR_ERROR, "kawase_blur: no format found");
		return NULL;
	}

	struct wlr_buffer *bufs[10];
	for (int i = 1; i <= levels; i++) {
		bufs[i] = wlr_allocator_create_buffer(
			server->allocator, sizes[i][0], sizes[i][1], fmt);
		if (!bufs[i]) {
			for (int j = 1; j < i; j++) wlr_buffer_drop(bufs[j]);
			wlr_log(WLR_ERROR, "kawase_blur: buf[%d] alloc failed", i);
			return NULL;
		}
	}

	struct wlr_buffer *result = wlr_allocator_create_buffer(
		server->allocator, w, h, fmt);
	if (!result) {
		for (int i = 1; i <= levels; i++) wlr_buffer_drop(bufs[i]);
		wlr_log(WLR_ERROR, "kawase_blur: result alloc failed");
		return NULL;
	}

	struct wlr_texture *tex = NULL;
	struct wlr_render_pass *pass = NULL;
	for (int i = 1; i <= levels; i++) {
		struct wlr_texture *src_i;
		if (i > 1) {
			src_i = wlr_texture_from_buffer(server->renderer, bufs[i-1]);
		} else {
			src_i = src_tex;
		}
		pass = wlr_renderer_begin_buffer_pass(server->renderer, bufs[i], NULL);
		if (src_i && pass) {
			wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
				.texture = src_i,
				.src_box = (struct wlr_fbox){0, 0, (double)sizes[i-1][0], (double)sizes[i-1][1]},
				.dst_box = {0, 0, sizes[i][0], sizes[i][1]},
				.filter_mode = WLR_SCALE_FILTER_BILINEAR,
				.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			});
			wlr_render_pass_submit(pass);
		} else {
			wlr_log(WLR_ERROR, "kawase_blur: down pass %d failed", i);
		}
		if (i > 1 && src_i) wlr_texture_destroy(src_i);
	}

	for (int i = levels; i >= 1; i--) {
		struct wlr_buffer *dst = (i > 1) ? bufs[i-1] : result;
		int dw = (i > 1) ? sizes[i-1][0] : sizes[0][0];
		int dh = (i > 1) ? sizes[i-1][1] : sizes[0][1];
		tex = wlr_texture_from_buffer(server->renderer, bufs[i]);
		pass = wlr_renderer_begin_buffer_pass(server->renderer, dst, NULL);
		if (tex && pass) {
			wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
				.texture = tex,
				.src_box = (struct wlr_fbox){0, 0, (double)sizes[i][0], (double)sizes[i][1]},
				.dst_box = {0, 0, dw, dh},
				.filter_mode = WLR_SCALE_FILTER_BILINEAR,
				.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			});
			wlr_render_pass_submit(pass);
		} else {
			wlr_log(WLR_ERROR, "kawase_blur: up pass %d failed", i);
		}
		if (tex) wlr_texture_destroy(tex);
	}

	for (int i = 1; i <= levels; i++) wlr_buffer_drop(bufs[i]);
	return result;
}

static void output_frame_blur(struct pudu_output *output,
		struct wlr_scene_output *scene_output) {
	struct pudu_server *server = output->server;
	int ow = output->wlr_output->width;
	int oh = output->wlr_output->height;
	float scale = output->wlr_output->scale;

	const struct wlr_drm_format *fmt = wlr_drm_format_set_get(
		wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DMABUF),
		DRM_FORMAT_XRGB8888);
	if (!fmt) {
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}

	struct wlr_scene_tree *fg_trees[] = {
		server->toplevel_tree, server->top_tree,
		server->overlay_tree, server->lock_tree,
	};
	int n_fg = 4;

	struct wlr_swapchain *sc = wlr_swapchain_create(
		server->allocator, ow, oh, fmt);
	if (!sc) {
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}
	struct wlr_scene_output_state_options opts = { .swapchain = sc };

	for (int i = 0; i < n_fg; i++)
		wlr_scene_node_set_enabled(&fg_trees[i]->node, false);
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	if (!wlr_scene_output_build_state(scene_output, &st, &opts)) {
		for (int i = 0; i < n_fg; i++)
			wlr_scene_node_set_enabled(&fg_trees[i]->node, true);
		wlr_output_state_finish(&st);
		wlr_swapchain_destroy(sc);
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}
	struct wlr_texture *bg_tex = wlr_texture_from_buffer(
		server->renderer, st.buffer);
	wlr_output_state_finish(&st);
	for (int i = 0; i < n_fg; i++)
		wlr_scene_node_set_enabled(&fg_trees[i]->node, true);

	if (!bg_tex) {
		wlr_swapchain_destroy(sc);
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}

	struct wlr_buffer *bg_copy = wlr_allocator_create_buffer(
		server->allocator, ow, oh, fmt);
	if (!bg_copy) {
		wlr_texture_destroy(bg_tex);
		wlr_swapchain_destroy(sc);
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}
	struct wlr_render_pass *cp = wlr_renderer_begin_buffer_pass(
		server->renderer, bg_copy, NULL);
	if (cp) {
		wlr_render_pass_add_texture(cp, &(struct wlr_render_texture_options){
			.texture = bg_tex,
			.dst_box = {0, 0, ow, oh},
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		wlr_render_pass_submit(cp);
	}
	wlr_texture_destroy(bg_tex);
	struct wlr_texture *sharp_bg = wlr_texture_from_buffer(
		server->renderer, bg_copy);
	wlr_buffer_drop(bg_copy);
	if (!sharp_bg) {
		wlr_swapchain_destroy(sc);
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}

	struct wlr_buffer *blur_buf = kawase_blur(server, sharp_bg, ow, oh, server->blur_strength);
	if (!blur_buf) {
		wlr_texture_destroy(sharp_bg);
		wlr_swapchain_destroy(sc);
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}
	struct wlr_texture *blur_tex = wlr_texture_from_buffer(
		server->renderer, blur_buf);
	if (!blur_tex) {
		wlr_texture_destroy(sharp_bg);
		wlr_buffer_drop(blur_buf);
		wlr_swapchain_destroy(sc);
		wlr_scene_output_commit(scene_output, NULL);
		return;
	}

	int b = server->active_border_size;
	int sx = scene_output->x, sy = scene_output->y;
	struct pudu_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->mapped || t->workspace != server->current_workspace ||
				!t->blur_bg || t->fullscreen) {
			if (t->blur_bg)
				wlr_scene_node_set_enabled(&t->blur_bg->node, false);
			continue;
		}

		struct wlr_box geo = t->xdg_toplevel->base->geometry;
		int ww = geo.width, wh = geo.height;
		if (ww < 1 || wh < 1) {
			wlr_scene_node_set_enabled(&t->blur_bg->node, false);
			continue;
		}

		int wx = t->scene_tree->node.x + b;
		int wy = t->scene_tree->node.y + b;

		int px = (int)((wx - sx) * scale + 0.5f);
		int py = (int)((wy - sy) * scale + 0.5f);
		int pw = (int)(ww * scale + 0.5f);
		int ph = (int)(wh * scale + 0.5f);

		if (pw < 1 || ph < 1) {
			wlr_scene_node_set_enabled(&t->blur_bg->node, false);
			continue;
		}

		if (px < 0) { pw += px; px = 0; }
		if (py < 0) { ph += py; py = 0; }
		if (px + pw > ow) pw = ow - px;
		if (py + ph > oh) ph = oh - py;
		if (pw < 1 || ph < 1) {
			wlr_scene_node_set_enabled(&t->blur_bg->node, false);
			continue;
		}

		struct wlr_buffer *win_buf = wlr_allocator_create_buffer(
			server->allocator, pw, ph, fmt);
		if (!win_buf) {
			wlr_scene_node_set_enabled(&t->blur_bg->node, false);
			continue;
		}

		cp = wlr_renderer_begin_buffer_pass(server->renderer, win_buf, NULL);
		if (!cp) {
			wlr_buffer_drop(win_buf);
			wlr_scene_node_set_enabled(&t->blur_bg->node, false);
			continue;
		}

		struct wlr_fbox sbox = {(double)px, (double)py, (double)pw, (double)ph};
		struct wlr_box dbox = {0, 0, pw, ph};

		wlr_render_pass_add_texture(cp, &(struct wlr_render_texture_options){
			.texture = sharp_bg, .src_box = sbox, .dst_box = dbox,
			.filter_mode = WLR_SCALE_FILTER_NEAREST,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		wlr_render_pass_add_texture(cp, &(struct wlr_render_texture_options){
			.texture = blur_tex, .src_box = sbox, .dst_box = dbox,
			.alpha = &server->blur_opacity,
			.filter_mode = WLR_SCALE_FILTER_NEAREST,
			.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		});
		wlr_render_pass_submit(cp);

		wlr_scene_node_set_position(&t->blur_bg->node, b, b);
		wlr_scene_buffer_set_source_box(t->blur_bg,
			&(struct wlr_fbox){0, 0, (double)pw, (double)ph});
		wlr_scene_buffer_set_dest_size(t->blur_bg, ww, wh);
		wlr_scene_buffer_set_buffer(t->blur_bg, win_buf);
		wlr_scene_node_set_enabled(&t->blur_bg->node, true);
		wlr_buffer_drop(win_buf);
	}

	wlr_texture_destroy(blur_tex);
	wlr_texture_destroy(sharp_bg);
	wlr_buffer_drop(blur_buf);
	wlr_swapchain_destroy(sc);

	wlr_scene_output_commit(scene_output, NULL);
}

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

	if (server->blur && server->blur_opacity > 0.0f) {
		output_frame_blur(output, scene_output);
	} else if (!wlr_scene_output_commit(scene_output, NULL)) {
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
