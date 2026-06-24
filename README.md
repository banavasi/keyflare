# keyflare

A tiny floating keystroke HUD — a glassy "pill" in the bottom-right corner that
shows the last key (or combo) you pressed, with a typing-heat "speedometer" and
gunshot-style recoil on every key.

Two native builds that behave the same:

| Platform | Status | Stack |
|---|---|---|
| **macOS** (13+) | ✅ | Swift / AppKit `NSPanel` + `CGEventTap` + `NSVisualEffectView` glass — [`macos/`](macos/) |
| **Linux** Wayland — COSMIC, sway, Hyprland (wlroots) | ✅ | C / GTK3 + `gtk-layer-shell` + `libinput` — [`linux/`](linux/) |
| Linux GNOME/Mutter (Wayland) | ⚠️ | runs, but no layer-shell → falls back to a plain window (not pinned) |
| Linux X11 | ❌ | no layer-shell |
| **Windows** | ⏳ | planned |

> Why two codebases? The two things that define a key visualizer — the
> always-on-top overlay and global key capture — share **no** implementation
> between macOS (`NSWindow`/`CGEventTap`) and Wayland (layer-shell/`libinput`).
> Each platform gets a small, idiomatic, native build instead of a leaky
> abstraction. (On Wayland, global key capture requires privilege by design.)

## Features (both platforms)

- **Fixed floating overlay** — always on top, bottom-right, undecorated, off the
  taskbar, not user-movable.
- **Combos** — `Shift + A`, `Cmd/Ctrl + Shift + C`, etc.
- **Heat / speedometer** — continuous typing warms the pill cool → amber →
  orange → **red** after ~10s; a pause resets it.
- **Gunshot recoil** — every keypress punches with a muzzle-flash glow, **harder
  the faster you type**.
- **Idle fade** — invisible when idle; snaps to full the instant you type.
- **Password mode** — **click the pill** to pause the display (🔒) so your
  password keystrokes are never shown.

## Quick start

**macOS** (needs Xcode command-line tools):
```sh
cd macos && swift run
```
Then grant **Input Monitoring** (System Settings → Privacy & Security) and
relaunch. See [`macos/README.md`](macos/README.md).

**Linux** (Wayland / wlroots):
```sh
sudo apt install build-essential libgtk-3-dev libgtk-layer-shell-dev libinput-tools
cd linux && make && make install        # -> ~/.local/bin/keyflare
keyflare
```
Reading keys needs `/dev/input` access; if you're not in the `input` group it
escalates via `pkexec`. Skip the prompt: `sudo usermod -aG input $USER` then
re-login. See [`linux/`](linux/).

## Configuration (both)

| Var | Default | Meaning |
|---|---|---|
| `KEYFLARE_HOT_MS` | `10000` | ms of sustained typing before the pill goes red |
| `KEYFLARE_RESET_MS` | `2000` | pause (ms) that resets the typing-heat streak |

```sh
KEYFLARE_HOT_MS=3000 keyflare   # red after 3s instead of 10s
```

## License

[MIT](LICENSE) © 2026 Shashank Shandilya
