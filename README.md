<div align="center">
  <img src="media/pudu.png" alt="Logo" width="140"/>
</div>

> [!NOTE]
>🦌 **pudu** is a minimal tiling Wayland compositor built on top of wlroots.
>
>It's early-stage and a solo hobby project, **so bugs are part of the deal**.
>Take it easy and file an issue if something breaks.

<div align="center">
  <img src="media/ss.png" alt="Screenshot" width="100%"/>
</div>

## Installation  

> [!CAUTION]
> XWayland is **not supported** and this is intentional. pudu is a pure Wayland compositor.

> [!IMPORTANT]  
> Only tested on **Arch Linux**. Other distributions may work but are untested.  
  
```bash  
# Dependencies  
sudo pacman -S base-devel git wlroots0.19 wayland libxkbcommon libinput cairo kitty seatd  
```  
```bash  
# Build  
git clone https://github.com/vodkanull/pudu.git  
cd pudu/src  
make  
```
```bash  
# Install
sudo cp build/pudu /usr/local/bin/  
sudo mkdir -p /usr/share/wayland-sessions/
sudo cp pudu.desktop /usr/share/wayland-sessions/ 
```  
```bash  
# Seatd (required when running from tty)
sudo systemctl enable --now seatd
sudo usermod -aG input,seat $USER
```  
> [!IMPORTANT]  
> After adding the groups, **log out and back in** for the changes to take effect.   
```bash  
# Run
# Select pudu in your DM or run it from tty with:
pudu
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

----

> [!NOTE]
>  **👇 Contributing**
>
> If you find a bug, have a suggestion, or just want to share your thoughts, feel free to **open an issue**.
> Pull requests are **not accepted** (this is a personal hobby project).

<video src="https://github.com/user-attachments/assets/c6b639f0-cfee-4a8a-bd6e-3c1cb852b7e8" autoplay loop muted playsinline width="100%"></video>

## License
🦌 Pudu is made in 🇨🇱 and is under the GPL v3.0 license.
