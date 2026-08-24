import SwiftUI
import UIKit

// Replaces MobileOverlayLayer (src/platform/ios/mobile_overlay.cpp), which renders this same
// toggle-button + console-panel overlay inside the Vulkan/ImGui frame loop instead. That ImGui
// path exists specifically because a genuinely separate UIWindow hosting SwiftUI was already
// confirmed on-device to not repaint reliably once shadps4_run() takes the main thread's SDL
// run loop -- see shadps4_get_uikit_window()'s doc comment in shadps4_ios_api.h for that
// history. This attaches to SDL's OWN UIWindow as a subview instead of a second window, on the
// theory that content sharing the same window as SDL's actively-rendering view has a much
// better chance of getting its Core Animation commits serviced by whatever run-loop tick SDL's
// own window is already getting -- but this genuinely needs on-device confirmation; it hasn't
// been proven to work yet the way the ImGui path has.
@MainActor
enum GameOverlayHost {
    private static var hostingController: UIHostingController<GameOverlayView>?

    /// Call once SDL's window exists (from a shadps4_register_first_frame_callback()
    /// callback, hopped to the main thread -- see EmulatorProcess.launch()).
    static func attach() {
        guard hostingController == nil else { return }
        guard let raw = shadps4_get_uikit_window() else {
            // print(), not aelog()/NSLog -- confirmed via a real crash log that print()
            // reliably reaches aether_crash.log (CrashLogger.swift's stdout freopen), while
            // whether NSLog's output does too was unconfirmed and mattered here: an earlier
            // test showed zero GameOverlayHost lines despite 172 frames rendering, and
            // without a reliable log there was no way to tell "never ran" from "ran, logged
            // via a channel that isn't captured."
            print("[AetherPS4] GameOverlayHost: shadps4_get_uikit_window() returned nil, cannot attach")
            return
        }
        let window = Unmanaged<UIWindow>.fromOpaque(raw).takeUnretainedValue()

        let controller = UIHostingController(rootView: GameOverlayView())
        controller.view.backgroundColor = .clear
        controller.view.translatesAutoresizingMaskIntoConstraints = false

        if let rootVC = window.rootViewController {
            rootVC.addChild(controller)
        }
        window.addSubview(controller.view)
        NSLayoutConstraint.activate([
            controller.view.topAnchor.constraint(equalTo: window.topAnchor),
            controller.view.bottomAnchor.constraint(equalTo: window.bottomAnchor),
            controller.view.leadingAnchor.constraint(equalTo: window.leadingAnchor),
            controller.view.trailingAnchor.constraint(equalTo: window.trailingAnchor),
        ])
        if let rootVC = window.rootViewController {
            controller.didMove(toParent: rootVC)
        }

        hostingController = controller
        print("[AetherPS4] GameOverlayHost: attached SwiftUI overlay to SDL's UIWindow")
    }

    /// Call once the game session ends (shadps4_run() returning in EmulatorProcess.launch()),
    /// so a stale controller doesn't linger pointing at a torn-down window, and a poll timer
    /// that never found a window (e.g. the game crashed before SDL's window ever appeared)
    /// doesn't keep firing into the next launch.
    static func detach() {
        pollTimer?.invalidate()
        pollTimer = nil
        guard let controller = hostingController else { return }
        controller.willMove(toParent: nil)
        controller.view.removeFromSuperview()
        controller.removeFromParent()
        hostingController = nil
    }
}

extension GameOverlayHost {
    private static var pollTimer: Timer?
    private static var pollAttempt = 0

    // shadps4_register_first_frame_callback() looks like the natural fit here, but its
    // underlying flag is scoped to the whole process's lifetime, not one shadps4_run() call
    // -- "never resets to 0 again, even across game restarts within the same process" (its
    // own doc comment). Confirmed on-device: it silently never fires for a second-or-later
    // game launch in the same app session, which is exactly what a launch-time overlay
    // needs. Polling shadps4_get_uikit_window() instead sidesteps that entirely -- it's a
    // plain pointer read with no per-process state, safe to call as often as needed, and
    // naturally becomes non-nil exactly once SDL's window exists for THIS run.
    //
    // Uses an actual Timer added to RunLoop.main in .common modes, not a chain of
    // DispatchQueue.main.asyncAfter calls: a real on-device test showed zero evidence this
    // ever ran (not even the "started polling" breadcrumb) despite the game rendering 172
    // frames -- plenty of time for a 100ms-interval poll to have found the window. That's
    // consistent with shadps4_run()'s internal SDL loop only servicing certain run-loop
    // modes on the main thread once it takes over, in which case .asyncAfter's GCD-timer
    // path might just never get drained. RunLoop.common explicitly covers the modes UIKit
    // itself relies on for its own tracking/animation work, so this is the most likely
    // alternative to actually fire if that's really what's happening -- still needs
    // confirming with a fresh log now that logging goes through print(), not NSLog.
    static func pollUntilAttached() {
        guard hostingController == nil, pollTimer == nil else { return }
        pollAttempt = 0
        print("[AetherPS4] GameOverlayHost: starting to poll for SDL's UIWindow (Timer/.common)")
        let timer = Timer(timeInterval: 0.1, repeats: true) { _ in
            pollTick()
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
    }

    private static func pollTick() {
        pollAttempt += 1
        if shadps4_get_uikit_window() != nil {
            pollTimer?.invalidate()
            pollTimer = nil
            attach()
            return
        }
        // Breadcrumb every ~2s (not every 100ms) so a run that never finds the window
        // leaves clear evidence in the log without spamming it.
        if pollAttempt % 20 == 0 {
            print("[AetherPS4] GameOverlayHost: still polling for SDL's UIWindow (attempt \(pollAttempt))")
        }
    }
}

private struct GameOverlayView: View {
    @State private var showPanel = true
    @State private var consoleTail = ""
    @State private var isRunning = false
    private let refreshTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                if showPanel {
                    panel
                        .frame(
                            maxWidth: min(geometry.size.width - 32, 520),
                            maxHeight: min(geometry.size.height - 120, 420)
                        )
                        .position(x: geometry.size.width / 2, y: geometry.size.height / 2)
                }

                VStack {
                    HStack {
                        Spacer()
                        Button(showPanel ? "Hide" : "Console") {
                            showPanel.toggle()
                        }
                        .buttonStyle(.borderedProminent)
                        .padding(.top, 12)
                        .padding(.trailing, 12)
                    }
                    Spacer()
                }
            }
        }
        .allowsHitTesting(true)
        .onReceive(refreshTimer) { _ in
            refresh()
        }
        .onAppear { refresh() }
    }

    private var panel: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(isRunning ? "Running" : "Loading game…")
                .foregroundStyle(isRunning ? .green : .white)
                .font(.headline)
            Divider()
            Text("Console")
                .font(.subheadline)
                .foregroundStyle(.white)
            ScrollView {
                Text(consoleTail)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
        .background(Color.black.opacity(0.92))
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    // Last N bytes of the log, tailed live -- mirrors mobile_overlay.cpp's ReadTail exactly
    // (same file, same 32KB window, same refresh cadence), just read from the Swift side
    // instead of the C++ side.
    private func refresh() {
        isRunning = shadps4_has_presented_frame() != 0
        guard let documentsURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        else { return }
        let logURL = documentsURL.appendingPathComponent("aether_crash.log")
        guard let handle = try? FileHandle(forReadingFrom: logURL) else { return }
        defer { try? handle.close() }
        let fileSize = (try? handle.seekToEnd()) ?? 0
        let maxBytes: UInt64 = 32 * 1024
        let start = fileSize > maxBytes ? fileSize - maxBytes : 0
        try? handle.seek(toOffset: start)
        if let data = try? handle.readToEnd() {
            consoleTail = String(decoding: data, as: UTF8.self)
        }
    }
}
