# Changelogs

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
