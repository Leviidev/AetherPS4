import SwiftUI
import UIKit

// Puts a live SwiftUI loading screen on top of SDL's own window using the simplest tool
// UIKit has for exactly this -- a second UIWindow with a higher windowLevel -- instead of
// GameOverlayHost's approach (attaching as a subview inside SDL's own window, requiring
// child-view-controller containment and constraints anchored to a window this app doesn't
// own). That approach was never tested on-device before it shipped and crashed on the very
// first real run with an uncaught exception right after being attached; this one is a much
// more standard, lower-risk pattern with no shared view hierarchy at all. Only viable now
// that Emulator::Run() is split (see emulator.h): the main thread stays free for the whole
// game session instead of freezing the instant shadps4_run() used to take it over, so a
// Timer here (polling shadps4_has_presented_frame()) actually runs.
@MainActor
enum LoadingOverlayWindow {
    private static var window: UIWindow?
    private static var pollTimer: Timer?

    /// Call once shadps4_prepare_window() has returned successfully -- SDL's window
    /// unconditionally exists by then (see EmulatorProcess.launch()), though this window
    /// doesn't actually need that to be true; it's fully independent.
    static func show(gameName: String) {
        guard window == nil else { return }
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else {
            print("[AetherPS4] LoadingOverlayWindow: no active window scene, cannot show")
            return
        }

        let overlayWindow = UIWindow(windowScene: scene)
        // .alert + 1: above everything UIKit itself puts up (alerts, action sheets), and
        // above SDL's own window regardless of which one is "key" -- windowLevel ordering
        // is independent of key-window status, unlike relying on presentation z-order.
        overlayWindow.windowLevel = .alert + 1
        overlayWindow.backgroundColor = .clear
        overlayWindow.rootViewController = UIHostingController(rootView: LoadingOverlayView(gameName: gameName))
        overlayWindow.rootViewController?.view.backgroundColor = .clear
        overlayWindow.isHidden = false
        window = overlayWindow
        print("[AetherPS4] LoadingOverlayWindow: shown")

        let timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { _ in
            if shadps4_has_presented_frame() != 0 {
                hide()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
    }

    /// Call once the game session ends, as a safety net for a game that crashed or exited
    /// before ever presenting a frame (the poll above already hides it as soon as one does).
    static func hide() {
        pollTimer?.invalidate()
        pollTimer = nil
        window?.isHidden = true
        window = nil
        print("[AetherPS4] LoadingOverlayWindow: hidden")
    }
}

private struct LoadingOverlayView: View {
    let gameName: String

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 20) {
                ProgressView()
                    .controlSize(.large)
                    .tint(.white)
                Text("Loading \(gameName)…")
                    .font(.title3.bold())
                    .foregroundStyle(.white)
                Text("Do not tap or leave the app")
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.7))
            }
        }
    }
}
