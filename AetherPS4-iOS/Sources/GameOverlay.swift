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
            aelog("GameOverlayHost: shadps4_get_uikit_window() returned nil, cannot attach")
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
        aelog("GameOverlayHost: attached SwiftUI overlay to SDL's UIWindow")
    }

    /// Call once the game session ends (shadps4_run() returning in EmulatorProcess.launch()),
    /// so a stale controller doesn't linger pointing at a torn-down window.
    static func detach() {
        guard let controller = hostingController else { return }
        controller.willMove(toParent: nil)
        controller.view.removeFromSuperview()
        controller.removeFromParent()
        hostingController = nil
    }
}

// C-callable, non-capturing top-level function -- shadps4_register_first_frame_callback()
// takes a plain C function pointer, so this can't be a closure with captures. Fires on the
// Vulkan presenter's own render thread, never the main thread (see the C header's own doc
// comment), so this hops to main before touching any UIKit/SwiftUI state.
func aetherGameOverlayFirstFrameCallback() {
    DispatchQueue.main.async {
        GameOverlayHost.attach()
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
