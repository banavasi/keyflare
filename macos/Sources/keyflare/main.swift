// keyflare (macOS) — floating keystroke HUD
//
// Native counterpart of the Linux/Wayland keyflare. Same behaviour:
//   - glassy pill anchored bottom-right, always on top (NSPanel + NSVisualEffectView)
//   - shows the last key / combo ("Cmd + Shift + A")
//   - typing-heat "speedometer": cool -> warm -> orange -> RED after ~10s sustained
//   - gunshot recoil on each key; harder the faster you type
//   - idle => fades to invisible; snaps to full the instant you type
//   - click the pill => password mode (pauses display, shows a lock)
//
// Keys come from a CGEventTap (needs Input Monitoring permission). No dock icon.
//
// Build/run:  swift run            (from the macos/ directory)
// Tuning:     KEYFLARE_HOT_MS, KEYFLARE_RESET_MS (ms)

import AppKit
import SwiftUI
import Combine
import QuartzCore
import CoreGraphics

// MARK: - Model -------------------------------------------------------------

final class KeyModel: ObservableObject {
    @Published var text = " "
    @Published var heat = 0.0          // 0..1 toward red
    @Published var hot = false         // sustained-typing red state
    @Published var visible = true
    @Published var muted = false       // password mode
    @Published var recoil = 0          // bumped per key to trigger the bang
    var intensity = 0.0                // last recoil strength 0..1 (speed-scaled)

    private var lastKey: CFTimeInterval = 0      // ms
    private var streakStart: CFTimeInterval = 0  // ms
    private var held: [String] = []              // currently-held modifiers, ordered
    private var idleWork: DispatchWorkItem?

    private let hotMS  = Double(ProcessInfo.processInfo.environment["KEYFLARE_HOT_MS"]   ?? "") ?? 10000
    private let resetMS = Double(ProcessInfo.processInfo.environment["KEYFLARE_RESET_MS"] ?? "") ?? 2000
    private let slowMS = 350.0, fastMS = 55.0

    func pressKey(_ name: String) {
        if muted { return }
        let disp = held.isEmpty ? name : held.joined(separator: " + ") + " + " + name
        emit(disp)
    }

    /// `mods` = the full set of modifiers currently held, ordered.
    func setMods(_ mods: [String]) {
        if muted { return }
        if mods.count > held.count {            // a modifier was pressed -> show the held combo
            held = mods
            emit(mods.joined(separator: " + "))
        } else {                                 // a release -> just update state
            held = mods
        }
    }

    private func emit(_ disp: String) {
        let now = CACurrentMediaTime() * 1000.0
        let gap = lastKey == 0 ? 1e9 : now - lastKey
        if lastKey == 0 || gap > resetMS { streakStart = now }   // new typing streak
        lastKey = now

        let streak = now - streakStart
        hot = streak >= hotMS
        heat = hot ? 1.0 : min(1.0, streak / hotMS)
        intensity = max(0.0, min(1.0, (slowMS - gap) / (slowMS - fastMS)))  // faster => harder

        text = disp
        recoil &+= 1
        wake()
    }

    private func wake() {
        idleWork?.cancel()
        withAnimation(.easeOut(duration: 0.10)) { visible = true }   // snap visible
        let work = DispatchWorkItem { [weak self] in
            withAnimation(.easeInOut(duration: 0.35)) { self?.visible = false }
        }
        idleWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.4, execute: work)
    }

    func toggleMute() {
        muted.toggle()
        if muted {
            idleWork?.cancel()
            held = []
            withAnimation { visible = true; hot = false; heat = 0 }
            text = "🔒"
        } else {
            lastKey = 0; held = []; text = " "
            wake()
        }
    }
}

// MARK: - View --------------------------------------------------------------

/// Real macOS vibrancy (backdrop blur) behind the pill.
struct GlassEffect: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let v = NSVisualEffectView()
        v.material = .hudWindow
        v.blendingMode = .behindWindow
        v.state = .active
        return v
    }
    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {}
}

struct PillView: View {
    @ObservedObject var model: KeyModel
    @State private var scale: CGFloat = 1
    @State private var flash: Double = 0

    private var heatColor: Color {
        if model.hot { return Color(red: 0.91, green: 0.20, blue: 0.20) }
        if model.heat >= 0.66 { return Color(red: 0.90, green: 0.47, blue: 0.16) }
        if model.heat >= 0.33 { return Color(red: 0.78, green: 0.59, blue: 0.24) }
        return Color(red: 0.26, green: 0.27, blue: 0.36)
    }

    var body: some View {
        Text(model.text)
            .font(.system(size: 30, weight: .semibold, design: .monospaced))
            .foregroundColor(.white)
            .shadow(color: .black.opacity(0.55), radius: 3, y: 1)
            .padding(.vertical, 12)
            .padding(.horizontal, 26)
            .frame(minWidth: 84, minHeight: 54)
            .background(
                ZStack {
                    GlassEffect()
                    heatColor.opacity(0.45)
                    Color.white.opacity(flash * 0.20)        // muzzle flash
                }
            )
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(.white.opacity(0.16 + flash * 0.5), lineWidth: 1)
            )
            .shadow(color: heatColor.opacity(0.55), radius: 16, y: 6)
            .scaleEffect(scale)
            .fixedSize()
            .opacity(model.visible ? 1 : 0)
            .onReceive(model.$recoil) { _ in fire() }
            .onTapGesture { model.toggleMute() }
    }

    private func fire() {
        let peak = 1.10 + 0.35 * model.intensity        // slow ~1.10x, fast ~1.45x
        scale = peak
        flash = 1
        withAnimation(.interpolatingSpring(stiffness: 700, damping: 22)) { scale = 1 }
        withAnimation(.easeOut(duration: 0.16)) { flash = 0 }
    }
}

// MARK: - Key capture (CGEventTap) -----------------------------------------

private let specialKeys: [Int: String] = [
    49: "␣", 36: "⏎", 76: "⏎", 48: "⇥", 51: "⌫", 117: "Del", 53: "Esc",
    123: "←", 124: "→", 125: "↓", 126: "↑",
    115: "Home", 119: "End", 116: "PgUp", 121: "PgDn",
    122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
    98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12",
]

private func modsFrom(_ flags: CGEventFlags) -> [String] {
    var m: [String] = []
    if flags.contains(.maskCommand)   { m.append("Cmd") }
    if flags.contains(.maskControl)   { m.append("Ctrl") }
    if flags.contains(.maskAlternate) { m.append("Option") }
    if flags.contains(.maskShift)     { m.append("Shift") }
    return m
}

final class TapCoordinator {
    let model: KeyModel
    var tap: CFMachPort?
    init(model: KeyModel) { self.model = model }

    func handle(type: CGEventType, event: CGEvent) {
        switch type {
        case .keyDown:
            let kc = Int(event.getIntegerValueField(.keyboardEventKeycode))
            model.pressKey(Self.keyName(keycode: kc, event: event))
        case .flagsChanged:
            model.setMods(modsFrom(event.flags))
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
            if let tap = tap { CGEvent.tapEnable(tap: tap, enable: true) }
        default:
            break
        }
    }

    static func keyName(keycode: Int, event: CGEvent) -> String {
        if let s = specialKeys[keycode] { return s }
        if let ns = NSEvent(cgEvent: event),
           let ch = ns.charactersIgnoringModifiers,
           let scalar = ch.unicodeScalars.first,
           scalar.value >= 0x20, scalar.value != 0x7f {
            return ch.uppercased()
        }
        return "·"
    }
}

// C-compatible callback (captures nothing; state arrives via userInfo).
private let tapCallback: CGEventTapCallBack = { _, type, event, userInfo in
    if let userInfo = userInfo {
        let coord = Unmanaged<TapCoordinator>.fromOpaque(userInfo).takeUnretainedValue()
        coord.handle(type: type, event: event)
    }
    return Unmanaged.passUnretained(event)   // listen-only: pass through unchanged
}

// MARK: - App ---------------------------------------------------------------

final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = KeyModel()
    var panel: NSPanel!
    var coord: TapCoordinator!

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)   // no dock icon / menu bar

        let size = NSSize(width: 320, height: 110)
        panel = NSPanel(contentRect: NSRect(origin: .zero, size: size),
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        panel.isFloatingPanel = true
        panel.level = .statusBar
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.isMovableByWindowBackground = false
        panel.hidesOnDeactivate = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]

        let root = PillView(model: model)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
            .padding(16)
        panel.contentView = NSHostingView(rootView: root)

        positionBottomRight()
        panel.orderFrontRegardless()

        startTap()
    }

    private func positionBottomRight() {
        guard let screen = NSScreen.main else { return }
        let vf = screen.visibleFrame
        let f = panel.frame
        panel.setFrameOrigin(NSPoint(x: vf.maxX - f.width, y: vf.minY))
    }

    private func startTap() {
        coord = TapCoordinator(model: model)
        let mask = (1 << CGEventType.keyDown.rawValue) | (1 << CGEventType.flagsChanged.rawValue)
        guard let tap = CGEvent.tapCreate(
                tap: .cgSessionEventTap, place: .headInsertEventTap, options: .listenOnly,
                eventsOfInterest: CGEventMask(mask), callback: tapCallback,
                userInfo: Unmanaged.passUnretained(coord).toOpaque()) else {
            // No Input Monitoring permission yet.
            model.text = "Grant Input Monitoring →"
            model.visible = true
            return
        }
        coord.tap = tap
        let src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
