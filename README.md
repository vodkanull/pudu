<div align="center">
  <img src="media/pudu.png" alt="Logo" width="140"/>
</div>

> [!NOTE]
>🦌 **pudu** is a minimal tiling Wayland compositor built on top of wlroots.
>
>It's early-stage and a solo hobby project, **so bugs are part of the deal**.
>Take it easy and file an issue if something breaks.


<div align="center">

<img src="media/ss1" width="100%" alt="ss1">

<img src="ss2" width="49%" alt="ss2">
<img src="ss3" width="49%" alt="ss3">

</div>

## Installation  

> [!CAUTION]
> XWayland is **not supported** and this is intentional. pudu is a pure Wayland compositor.

> [!IMPORTANT]  
> Only tested on **Arch Linux**. Other distributions may work but are untested.  

### AUR (recommended)

With an **AUR helper** (`yay`):

```bash
yay -S pudu-git
```

## Configuration

> [!TIP]
> - Move window: **Super** + **Left click** + drag
> - Close window: **Super** + **C**
> - Reload config: **Super** + **R** (kills autostarts, reloads config, re-runs autostarts)
> - Open terminal: **Super** + **Enter** (default: kitty, change it in the config file)
>
> The configuration file is located at **`~/.config/pudu/config`**.
>
> See an example config 👉 [here](https://github.com/vodkanull/pudu/blob/main/src/config) 👈.
>
> [!CAUTION]
> The `blur` option requires GPU acceleration. On weak or integrated GPUs it may cause noticeable lag — disable it if you experience performance issues.

----

> [!NOTE]
>  **👇 Contributing**
>
> If you find a bug, have a suggestion, or just want to share your thoughts, feel free to **open an issue**.
> Pull requests are **not accepted** (this is a personal hobby project).

<!--
<video src="https://github.com/user-attachments/assets/c6b639f0-cfee-4a8a-bd6e-3c1cb852b7e8" autoplay loop muted playsinline width="100%"></video>
-->

## License
🦌 Pudu is made in 🇨🇱 and is under the GPL v3.0 license.
