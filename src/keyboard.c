#include "pudu.h"

void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct pudu_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);

	uint32_t keycode = event->keycode + 8;

	if (!keyboard->wlr_keyboard->xkb_state) {
		wlr_log(WLR_ERROR, "key event but xkb_state is NULL");
		return;
	}

	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	wlr_log(WLR_DEBUG, "key event: keycode=%u nsyms=%d state=%s",
		keycode, nsyms,
		event->state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release");

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && !server->cur_lock) {
		for (int i = 0; i < nsyms; i++) {
			wlr_log(WLR_DEBUG, "  sym[%d]=0x%x mods=0x%x count=%d", i, syms[i], modifiers,
				workspace_window_count(server, server->current_workspace));
			handled = handle_keybinding(server, syms[i], modifiers);
		}
		if (handled) {
			wlr_log(WLR_DEBUG, "  binding handled -> exec");
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct pudu_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

void server_new_keyboard(struct pudu_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	wlr_log(WLR_INFO, "new keyboard device: %s", device->name ? device->name : "unknown");

	struct pudu_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (!keymap) {
		wlr_log(WLR_ERROR, "failed to load system keymap, falling back to US layout");
		struct xkb_rule_names rules = {
			.rules = NULL,
			.model = NULL,
			.layout = "us",
			.variant = NULL,
			.options = NULL,
		};
		keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!keymap) {
			wlr_log(WLR_ERROR, "failed to load US fallback keymap either");
			xkb_context_unref(context);
			free(keyboard);
			return;
		}
	}

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	wl_list_insert(&server->keyboards, &keyboard->link);
}
