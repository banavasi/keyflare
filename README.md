# keyflare

A tiny floating keystroke HUD for Wayland — a glassy "pill" that sits in the
bottom-right corner and shows the last key (or combo) you pressed, with a
typing-heat "speedometer" and gunshot-style recoil on every key.

Built for **Pop!_OS COSMIC / wlroots-style Wayland** using the
[layer-shell](https://wayland.app/protocols/wlr-layer-shell-unstable-v1)
protocol, so the overlay is genuinely always-on-top and anchored with **no
manual "always on top" step** — unlike a normal window.

> _(Add a screen recording here — e.g. `docs/demo.gif`.)_

## Features

- **Fixed floating overlay** — always on top, anchored bottom-right,
  undecorated, off the taskbar, not user-movable. (layer-shell `OVERLAY`.)
- **Combos** — shows `Shift + A`, `Ctrl + Shift + C`, etc.
- **Heat / speedometer** — typing continuously warms the pill
  cool → amber → orange → **red** after ~10s of sustained typing; a pause resets it.
- **Gunshot recoil** — every keypress punches with a muzzle-flash glow; the
  recoil hits **harder the faster you type**.
- **Idle fade** — invisible when you're not typing; snaps to full the instant
  you press a key.
- **Password mode** — **click the pill** to pause the display (shows a 🔒) so
  your password keystrokes are never shown. Click again to resume.

## Platform support (read this)

keyflare is **Linux + Wayland only**, and only on compositors that implement
`wlr-layer-shell`:

| Environment | Works? | Notes |
|---|---|---|
| COSMIC, sway, Hyprland, wlroots | ✅ | the target |
| GNOME / Mutter (Wayland) | ⚠️ | Mutter has no layer-shell → falls back to a plain undecorated window (not pinned) |
| X11 | ❌ | no layer-shell |
| Windows / macOS | ❌ | different overlay + key-capture APIs entirely |

Two things make a key visualizer OS-specific: the **always-on-top overlay** and
**global key capture**. A cross-platform rewrite (e.g. Tauri + the `rdev`
crate, which would also enable real backdrop-blur glass) is the path to
Windows/macOS — see "Roadmap". Note that on **Wayland, global key capture
requires privilege by design** (anti-keylogger), which is why this reads keys
via `libinput` rather than snooping the compositor.

## Build

```sh
sudo apt install build-essential libgtk-3-dev libgtk-layer-shell-dev libinput-tools
make
make install            # symlinks ./keyflare into ~/.local/bin
```

## Run

```sh
keyflare
```

Reading raw key events needs access to `/dev/input`. If you're not in the
`input` group, keyflare escalates that one subprocess via `pkexec` (a password
prompt). To skip the prompt permanently:

```sh
sudo usermod -aG input $USER   # then log out and back in
```

## Configuration

Environment variables:

| Var | Default | Meaning |
|---|---|---|
| `KEYFLARE_HOT_MS` | `10000` | ms of sustained typing before the pill goes red |
| `KEYFLARE_RESET_MS` | `2000` | pause (ms) that resets the typing-heat streak |

```sh
KEYFLARE_HOT_MS=3000 keyflare   # go red after 3s instead of 10s
```

## How it works

- **Overlay**: a GTK3 window promoted to a `wlr-layer-shell` `OVERLAY` surface
  anchored bottom-right — always on top, no decorations, no taskbar entry.
- **Keys**: spawns `libinput debug-events --show-keycodes` (directly if you're
  in the `input` group, else via `pkexec`) and parses its output in the GLib
  main loop. No threads.

## Roadmap

- Cross-platform (Windows/macOS) via a Tauri rewrite with `rdev` for global
  key capture and a webview UI (real `backdrop-filter` glass + CSS animations).
- Configurable corner/anchor and colors.

## License

[MIT](LICENSE) © 2026 Shashank Shandilya
