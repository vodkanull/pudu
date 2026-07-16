# Changelogs

## v0.1.5

| |
|---|
| Blur temporarily removed |
| Fix crash when unlocking with Hyprlock (the lock-screen could now be any session-lock compatible program) |
| The session-lock listeners are now properly cleaned up on unlock or destroy |
| Fix NULL xkb_state dereference crash on keyboard key event |
| Fix config reload clearing keybindings before confirming the file was readable |
| Malformed configs are rejected atomically (no partial apply) |
| Config error overlay: persistent on-screen error message shown until config is fixed |

## v0.1.4

| |
|---|
| Overflow fix in animations (`uint32_t` → `uint64_t`) no longer break after ~50 days of uptime |
| The `xdg-desktop-portal` paths are no longer hardcoded to `/usr/lib/`, now it tries multiple locations |
| Dynamic window limit in stack (no longer limited to 64) |
| The lock screen no longer hides during blur rendering |

## v0.1.3

| |
|---|
| Kawase blur background behind windows (configurable via `blur`, `blur_strength`) |
| Fix flatpak apps not being able to open the system browser for OAuth/login |

## v0.1.2

| |
|---|
| Dialogs always on top of their parent window |
| Dialogs re-center and re-raise when the parent changes dynamically |
| Removed hot-reload on config change |
| Added PUDU_RELOAD action to reload config and restart autostarts |
| Autostart programs now keep their PID, killed on reload |

## v0.1.1

| |
|---|
| Modal windows and dialogs (with parent) now float centered over their parent |
| Floating windows maintain their floating state after being moved |
| Focus returns to the parent when a dialog is closed |
| Minimum sizes (min-width/min-height) are respected in tiling layout |
| Windows that don't fit with their minimums auto-float |

## v0.1.0

| |
|---|
| Master-stack layout with automatically organized windows |
| Tiling windows can be moved and resized with the mouse |
| Multiple workspaces with animated transitions |
| "Jelly snap" animation when organizing windows in tiling |
| Animated border colors on focus change |
| Configurable keyboard shortcuts |
| Support for panels and bars (layer-shell, e.g. waybar) |
| Autostart programs on startup |
| Session lock support |
| Display manager integration |
| Hot-reload configuration |
| Configurable natural scrolling |
| Multiple Wayland protocols |
