import SwiftUI
import UIKit

// Replaces MobileOverlayLayer (src/platform/ios/mobile_overlay.cpp), which renders this same
// loading-screen + console-panel overlay inside the Vulkan/ImGui frame loop instead. That ImGui
// path existed specifically because a genuinely separate UIWindow/subview hosting live SwiftUI
// was confirmed on-device to not get new main-thread work serviced at all once shadps4_run()
// took over the main thread for the whole game session -- see shadps4_get_uikit_window()'s doc
// comment in shadps4_ios_api.h for that history. That restriction is gone now that Emulator::Run()
// is split (see emulator.h): shadps4_prepare_window() (which creates this window) is synchronous
// and runs on the main thread, and only shadps4_run_loop() -- everything after -- moves to a
// background thread, so the main thread (and this subview's own Timer-driven refresh below) stay
// live for the whole session. Attaches to SDL's OWN UIWindow as a subview (not a second window)
// so it's genuinely composited on top of the game's own rendering, called right after
// shadps4_prepare_window() returns (see EmulatorProcess.launch()), by which point the window
// unconditionally exists -- no polling needed anymore either.
@MainActor
enum GameOverlayHost {
    private static var hostingController: UIHostingController<GameOverlayView>?

    /// Call once shadps4_prepare_window() has returned successfully -- SDL's window
    /// unconditionally exists by then (see EmulatorProcess.launch()).
    static func attach(gameName: String) {
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

        let controller = UIHostingController(rootView: GameOverlayView(gameName: gameName))
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

private struct GameOverlayView: View {
    let gameName: String
    @State private var showPanel = false
    @State private var consoleTail = ""
    @State private var isRunning = false
    private let refreshTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                // Full-screen loading state: visible the instant this attaches (SDL's window
                // exists but hasn't presented a real frame yet), replacing GameLoadingCoverView
                // for the whole loading window instead of just the split-second gap before SDL's
                // window exists -- this one lives inside that window and stays live-updating for
                // the whole session, so it doesn't need a separate static placeholder.
                if !isRunning {
                    Color.black
                        .ignoresSafeArea()
                    VStack(spacing: 16) {
                        ProgressView()
                            .progressViewStyle(.circular)
                            .tint(.white)
                            .scaleEffect(1.5)
                        Text("Loading \(gameName)…")
                            .font(.title3.weight(.semibold))
                            .foregroundStyle(.white)
                        Text("Do not tap or leave the app")
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.6))
                    }
                }

                if showPanel {
                    panel
                        .frame(
                            maxWidth: min(geometry.size.width - 32, 520),
                            maxHeight: min(geometry.size.height - 120, 420)
                        )
                        .position(x: geometry.size.width / 2, y: geometry.size.height / 2)
                }

                if isRunning {
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
        }
        .allowsHitTesting(true)
        .onReceive(refreshTimer) { _ in
            refresh()
        }
        .onAppear { refresh() }
    }

    private var panel: some View {
        VStack(alignment: .leading, spacing: 8) {
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
