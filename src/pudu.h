#pragma once

#include <math.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/edges.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
#include <cairo.h>

#include <wlr/util/region.h>

#include "ext-workspace-v1-protocol.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define MAX_ANIM_TOPLEVELS 64
#define MASTER_RATIO 0.5f

enum pudu_cursor_mode {
	PUDU_CURSOR_PASSTHROUGH,
	PUDU_CURSOR_MOVE,
	PUDU_CURSOR_RESIZE_H,
};

enum pudu_action {
	PUDU_ACTION_NONE,
	PUDU_ACTION_CLOSE,
	PUDU_ACTION_EXEC,
	PUDU_ACTION_CYCLE_TOPLEVELS,
	PUDU_ACTION_SWAP_MASTER,
	PUDU_ACTION_WORKSPACE_NEXT,
	PUDU_ACTION_WORKSPACE_PREV,
	PUDU_ACTION_MOVE_WORKSPACE_NEXT,
	PUDU_ACTION_MOVE_WORKSPACE_PREV,
	PUDU_ACTION_EXIT,
	PUDU_ACTION_RELOAD,
};

struct pudu_binding {
	struct wl_list link;
	uint32_t mods;
	xkb_keysym_t sym;
	enum pudu_action action;
	char *command;
};

struct pudu_toplevel {
	struct wl_list link;
	struct pudu_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener set_parent;
	struct wl_listener ft_handle_request_maximize;
	struct wl_listener ft_handle_request_fullscreen;
	struct wl_listener ft_handle_request_activate;
	struct wl_listener ft_handle_request_close;
	struct wl_listener ft_handle_destroy;
	struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;
	struct wlr_scene_tree *border_tree;
	struct wlr_scene_rect *border_rects[4];
	struct wlr_scene_buffer *border_buffer;
	int border_w;
	int border_h;
	int border_b;
	int border_r;
	float border_color[4];
	float border_start[4];
	float border_target[4];
	uint64_t border_anim_start;
	bool border_animating;
	struct wl_event_source *border_anim_timer;
	int workspace;
	bool mapped;
	bool fullscreen;
	bool floating;
	bool dialog;
	struct wlr_box allocated;
	/* Arrange animation (jelly snap) */
	double arrange_from_x, arrange_from_y;
	double arrange_to_x, arrange_to_y;
	double arrange_from_w, arrange_from_h;
	double arrange_to_w, arrange_to_h;
	bool arrange_animating;
	uint64_t arrange_anim_start;
};

struct pudu_popup {
	struct wl_list link;
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct pudu_autostart {
	struct wl_list link;
	char *command;
	pid_t pid;
};

struct pudu_workspace {
	struct wl_list link;
	struct pudu_server *server;
	struct wl_list resources;
	int number;
	bool active;
};

struct pudu_output_resource {
	struct wl_list link;
	struct wl_resource *resource;
	struct wl_listener destroy;
};

struct pudu_manager_client {
	struct wl_list link;
	struct wl_resource *manager_resource;
	struct wl_resource *group_resource;
};

struct pudu_keyboard {
	struct wl_list link;
	struct pudu_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct pudu_layer_surface {
	struct wl_list link;
	struct pudu_server *server;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wlr_output *output;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	bool configured;
	enum zwlr_layer_shell_v1_layer layer;
};

struct pudu_output;
struct wlr_swapchain;

struct pudu_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *background_tree;
	struct wlr_scene_tree *bottom_tree;
	struct wlr_scene_tree *toplevel_tree;
	struct wlr_scene_tree *top_tree;
	struct wlr_scene_tree *overlay_tree;
	struct wlr_scene_tree *lock_tree;
	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
	struct wl_listener new_toplevel_decoration;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
	struct wl_listener new_layer_surface;
	struct wl_list toplevels;
	struct wl_list layer_surfaces;
	struct wl_list popups;
	int current_workspace;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	enum pudu_cursor_mode cursor_mode;
	double grab_x, grab_y;
	double cursor_prev_x, cursor_prev_y;
	struct pudu_toplevel *grabbed_toplevel;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	struct pudu_toplevel *focused_toplevel_ptr;

	int active_border_size;
	float active_border_color[4];
	float inactive_border_color[4];
	int border_transition_ms;
	int border_radius;
	int inner_gap;
	int outer_gap;
	float master_ratio;
	int arrange_anim_ms;
	bool arrange_anim;
	bool natural_scroll;
	bool new_is_master;
	uint32_t mod_modifier;
	int workspace_count;
	char keyboard_layout[64];
	struct wl_list bindings;
	struct wl_list autostarts;

	/* Arrange animation (jelly snap on tile) */
	struct wl_event_source *arrange_timer;

	/* Workspace animation */
	struct wl_event_source *anim_timer;
	bool animating;
	int anim_old_count;
	int anim_new_count;
	struct pudu_toplevel *anim_old_list[MAX_ANIM_TOPLEVELS];
	struct pudu_toplevel *anim_new_list[MAX_ANIM_TOPLEVELS];
	double anim_old_x[MAX_ANIM_TOPLEVELS];
	double anim_old_y[MAX_ANIM_TOPLEVELS];
	double anim_new_x[MAX_ANIM_TOPLEVELS];
	double anim_new_y[MAX_ANIM_TOPLEVELS];
	uint64_t anim_start_time;
	double anim_slide_dist;
	int anim_new_workspace;
	int anim_direction;

	struct wlr_session_lock_manager_v1 *session_lock_manager;
	struct wl_listener new_session_lock;
	struct wl_listener session_lock_new_surface;
	struct wl_listener session_lock_unlock;
	struct wl_listener session_lock_destroy;
	struct wlr_session_lock_v1 *cur_lock;

	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wl_listener new_pointer_constraint;
	struct wlr_pointer_constraint_v1 *active_pointer_constraint;

	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener xdg_activation_request_activate;

	struct wlr_idle_notifier_v1 *idle_notifier;

	struct wlr_scene_tree *drag_icon;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener destroy_drag;
	struct wl_listener drag_icon_commit;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wl_list workspaces;
	struct wl_list manager_clients;
	struct wl_global *workspace_manager_global;

	/* Config error overlay */
	struct wlr_scene_tree *config_error_tree;
	struct wlr_scene_buffer *config_error_buf;
	char config_error_msg[512];

};

struct pudu_output {
	struct wl_list link;
	struct pudu_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener bind;
	struct wl_list output_resources;
	struct wlr_box usable_area;
};

/* Server / Output / Input / Cursor */
void server_new_output(struct wl_listener *listener, void *data);
void server_new_input(struct wl_listener *listener, void *data);
void seat_request_cursor(struct wl_listener *listener, void *data);
void seat_pointer_focus_change(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);
void server_cursor_motion(struct wl_listener *listener, void *data);
void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
void server_cursor_button(struct wl_listener *listener, void *data);
void server_cursor_axis(struct wl_listener *listener, void *data);
void server_cursor_frame(struct wl_listener *listener, void *data);
/* Toplevel */
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void server_new_xdg_popup(struct wl_listener *listener, void *data);
void server_new_toplevel_decoration(struct wl_listener *listener, void *data);
void focus_toplevel(struct pudu_toplevel *toplevel);
struct pudu_toplevel *focused_toplevel(struct pudu_server *server);
void toplevel_set_fullscreen(struct pudu_toplevel *toplevel, bool fullscreen);

/* Layout */
void arrange_workspace(struct pudu_server *server, int workspace);
void arrange_workspace_on_output(struct pudu_server *server, int workspace, struct wlr_output *wlr_output);
void cycle_focus(struct pudu_server *server);
void swap_master(struct pudu_server *server);

/* Layer shell */
void server_new_layer_surface(struct wl_listener *listener, void *data);
struct pudu_output *output_from_wlr_output(struct pudu_server *server, struct wlr_output *wlr_output);
void arrange_layers(struct pudu_output *output);
void output_set_layer_shell_visible(struct pudu_output *output, bool visible);
void server_update_layer_visibility(struct pudu_server *server);
void apply_fullscreen_state(struct pudu_toplevel *toplevel, bool fullscreen, struct wlr_output *wlr_output);

/* Session lock */
void server_new_session_lock(struct wl_listener *listener, void *data);
void session_lock_handle_new_surface(struct wl_listener *listener, void *data);
void session_lock_handle_unlock(struct wl_listener *listener, void *data);
void session_lock_handle_destroy(struct wl_listener *listener, void *data);

/* Workspace */
void view_workspace(struct pudu_server *server, int workspace);
int workspace_window_count(struct pudu_server *server, int workspace);
void sync_dynamic_workspaces(struct pudu_server *server);
void server_new_keyboard(struct pudu_server *server, struct wlr_input_device *device);
void server_apply_keyboard_layout(struct pudu_server *server);
void server_new_pointer_constraint(struct wl_listener *listener, void *data);
void update_active_pointer_constraint(struct pudu_server *server, struct wlr_surface *surface);
void update_workspace_ipc(struct pudu_server *server);
void workspace_update_toplevel_visibility(struct pudu_server *server);
struct pudu_workspace *find_workspace(struct pudu_server *server, int number);
void create_workspace(struct pudu_server *server, int number);
struct pudu_workspace *get_or_create_workspace(struct pudu_server *server, int number);
void workspace_send_done_all(struct pudu_server *server);
void workspace_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);

/* XDG Activation */
void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data);

/* Drag and drop */
void seat_request_start_drag(struct wl_listener *listener, void *data);
void seat_start_drag(struct wl_listener *listener, void *data);
void seat_destroy_drag(struct wl_listener *listener, void *data);

/* Config */
bool load_config(struct pudu_server *server);
void render_config_error(struct pudu_server *server);
void clear_config_error(struct pudu_server *server);
bool handle_keybinding(struct pudu_server *server, xkb_keysym_t sym, uint32_t mods);
void execute_binding(struct pudu_server *server, struct pudu_binding *b);
int workspace_animation_tick(struct pudu_server *server);
void workspace_animation_finish(struct pudu_server *server);
int arrange_anim_cb(void *data);
void set_border_target(struct pudu_toplevel *toplevel, const float target[4]);
void center_toplevel(struct pudu_toplevel *toplevel);

