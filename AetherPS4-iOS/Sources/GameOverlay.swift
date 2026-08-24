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
    /// so a stale controller doesn't linger pointing at a torn-down window. Any in-flight
    /// background poll that never found a window (e.g. the game crashed or exited before
    /// SDL ever created one) is left to run out on its own -- see pollLoopNonisolated's
    /// bounded attempt count -- rather than torn down here, since doing that safely would
    /// need touching @MainActor state from the background poll again, the exact class of
    /// thing that caused the crash this whole redesign is working around.
    static func detach() {
        guard let controller = hostingController else { return }
        controller.willMove(toParent: nil)
        controller.view.removeFromSuperview()
        controller.removeFromParent()
        hostingController = nil
    }
}

extension GameOverlayHost {
    // shadps4_register_first_frame_callback() looks like the natural fit here, but its
    // underlying flag is scoped to the whole process's lifetime, not one shadps4_run() call
    // -- "never resets to 0 again, even across game restarts within the same process" (its
    // own doc comment). Confirmed on-device: it silently never fires for a second-or-later
    // game launch in the same app session, which is exactly what a launch-time overlay
    // needs. Polling shadps4_get_uikit_window() instead sidesteps that entirely -- it's a
    // plain pointer read with no per-process state, safe to call as often as needed, and
    // naturally becomes non-nil exactly once SDL's window exists for THIS run.
    //
    // Polls on a plain background thread (Thread.sleep, no RunLoop/@MainActor involvement
    // at all) rather than a Timer on RunLoop.main: an earlier Timer-based version (added to
    // RunLoop.main in .common mode specifically to survive shadps4_run() taking over the
    // main run loop) was confirmed on-device as the actual cause of a real crash with no
    // catchable signal -- disabling only that one call site was enough to make the exact
    // same crash disappear, with Sonic Mania then booting successfully. Best explanation:
    // repeatedly waking the main run loop for this, however cheap each individual tick is,
    // was enough extra main-thread contention against whatever shadps4_run() needs to push
    // it over some watchdog-style threshold. This version never touches the main thread (or
    // any @MainActor-isolated state) at all until it has *found* the window, and then only
    // once -- a single DispatchQueue.main.async to call attach(), which is already safely
    // idempotent (guard hostingController == nil) if this somehow overlaps a fresh poll.
    static func pollUntilAttached() {
        guard hostingController == nil else { return }
        print("[AetherPS4] GameOverlayHost: starting to poll for SDL's UIWindow (background thread)")
        // .userInitiated, not .utility: boot is exactly when the CPU-heavy JIT/module-loading
        // work this poll needs to survive is happening, and .utility threads are among the
        // first the scheduler starves under that kind of load -- a low-priority poll could
        // stretch its nominal 100ms interval out far longer right when it matters most.
        DispatchQueue.global(qos: .userInitiated).async {
            pollLoopNonisolated(attempt: 0)
        }
    }

    // Deliberately nonisolated and free of any @MainActor-isolated state (including
    // GameOverlayHost's own hostingController) -- see pollUntilAttached's doc comment for
    // why. Bounded at 300 attempts (~30s) rather than running forever: a game that crashes
    // or exits before SDL ever creates a window should let this loop give up instead of
    // polling for the rest of the app's lifetime.
    nonisolated private static func pollLoopNonisolated(attempt: Int) {
        if shadps4_get_uikit_window() != nil {
            DispatchQueue.main.async {
                attach()
            }
            return
        }
        guard attempt < 300 else {
            print("[AetherPS4] GameOverlayHost: giving up polling for SDL's UIWindow after \(attempt) attempts")
            return
        }
        // Breadcrumb every ~2s (not every 100ms) so a run that never finds the window
        // leaves clear evidence in the log without spamming it.
        if attempt > 0 && attempt % 20 == 0 {
            print("[AetherPS4] GameOverlayHost: still polling for SDL's UIWindow (attempt \(attempt))")
        }
        Thread.sleep(forTimeInterval: 0.1)
        pollLoopNonisolated(attempt: attempt + 1)
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
