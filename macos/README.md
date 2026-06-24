# keyflare (macOS)

Native Swift/AppKit + SwiftUI build of keyflare.

## Build & run

```sh
cd macos
swift run            # or: swift build -c release  ->  .build/release/keyflare
```

Requires the Xcode command-line tools (`xcode-select --install`). No Xcode
project needed — this is a Swift Package.

## Permission (required)

keyflare reads keystrokes via a `CGEventTap`, which needs **Input Monitoring**:

1. Run it once. If it shows "Grant Input Monitoring →", open
   **System Settings → Privacy & Security → Input Monitoring**.
2. Add the `keyflare` binary (`.build/release/keyflare` or the path `swift run`
   uses) and enable it.
3. Relaunch.

(If a key tap still won't arm, also check **Accessibility** in the same panel.)

## Behaviour

Identical to the Linux build: glassy pill bottom-right, combos (`Cmd + Shift + A`),
heat "speedometer" → red after sustained typing, speed-scaled gunshot recoil,
idle fade, and **click the pill** to toggle password mode. Modifiers are shown
with macOS names (`Cmd`, `Option`, `Ctrl`, `Shift`).

## Config

```sh
KEYFLARE_HOT_MS=3000 swift run      # go red after 3s instead of 10s
```

`KEYFLARE_HOT_MS` (default 10000), `KEYFLARE_RESET_MS` (default 2000).

## Notes

- No dock icon (runs as an accessory/agent app).
- `swift run` is fine for personal use. To ship a double-clickable `.app`
  (proper TCC prompts, bundle id), wrap it in an Xcode app target later.
- Real glass here is `NSVisualEffectView` (native vibrancy), not a faked
  gradient like the GTK build.
