<div align="center">
  <img src="media/pudu.png" alt="Logo" width="140"/>
</div>

> [!NOTE]
>🦌 **pudu** is a minimalist, lightweight tiling Wayland compositor based on wlroots, with a dash of eye candy.
>
>It's early-stage and a solo hobby project, **so bugs are part of the deal**.
>Take it easy and file an issue if something breaks.

<div align="center">

<img src="media/ss1.png" width="100%" alt="ss1">

</div>

## Installation  

> [!CAUTION]
> XWayland is **not supported** and this is intentional. pudu is a pure Wayland compositor.

> [!IMPORTANT]  
> Only tested on **Arch Linux**. Other distributions may work but are untested.  

### AUR (recommended)

With an **AUR helper** (`yay/paru`):

```bash
yay -S pudu-git
```
See more [here](https://aur.archlinux.org/packages/pudu-git)

## Configuration

> [!TIP]
> - Move window: **Super** + **Left click** + drag
> - Close window: **Super** + **C**
> - Reload config: **Super** + **R** (kills autostarts, reloads config, re-runs autostarts)
> - Open terminal: **Super** + **Enter** (default: kitty, change it in the config file)
>
> The configuration file is located at **`~/.config/pudu/config`**.
>
> See an example config 👉 [here](https://github.com/vodkanull/pudu/blob/main/src/config) 👈

<div align="center">

<img src="media/ss2.png" width="49%" alt="ss2">
<img src="media/ss3.png" width="49%" alt="ss3">

</div>

> [!NOTE]
>  **👇 Contributing**
>
> If you find a bug, have a suggestion, or just want to share your thoughts, feel free to **open an issue**.
> Pull requests are **not accepted** (this is a personal hobby project).
>
> For a list of supported Wayland protocols see [PROTOCOLS.md](./PROTOCOLS.md).
> For the full release history see [CHANGELOGS.md](./CHANGELOGS.md).

<!--
<video src="https://github.com/user-attachments/assets/c6b639f0-cfee-4a8a-bd6e-3c1cb852b7e8" autoplay loop muted playsinline width="100%"></video>
-->


## License
🦌 Pudu is made in 🇨🇱 and is under the GPL v3.0 license.
