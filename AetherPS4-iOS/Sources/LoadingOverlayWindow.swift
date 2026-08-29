import SwiftUI
import UIKit

// Puts a live SwiftUI loading popup on top of SDL's own window using the simplest tool
// UIKit has for exactly this -- a second UIWindow with a higher windowLevel -- instead of
// GameOverlayHost's approach (attaching as a subview inside SDL's own window, requiring
// child-view-controller containment and constraints anchored to a window this app doesn't
// own). That approach was never tested on-device before it shipped and crashed on the very
// first real run with an uncaught exception right after being attached; this one is a much
// more standard, lower-risk pattern with no shared view hierarchy at all. Only viable now
// that Emulator::Run() is split (see emulator.h): the main thread stays free for the whole
// game session instead of freezing the instant shadps4_run() used to take it over.
//
// User-controlled open/close, not auto-dismissed: shadps4_has_presented_frame() (the only
// engine signal available) flips true on the presenter's very first swapchain present, which
// is confirmed to still be empty/loading/splash content, not the real game -- there's no
// reliable "loading is actually done" signal to time an automatic dismiss against. So this
// popup just stays open until the user closes it, and can be reopened any time via a small
// floating button.
@MainActor
enum LoadingOverlayWindow {
    private static var window: UIWindow?
    private static var uiState: LoadingOverlayUIState?

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

        let state = LoadingOverlayUIState()
        uiState = state

        let overlayWindow = UIWindow(windowScene: scene)
        // .alert + 1: above everything UIKit itself puts up (alerts, action sheets), and
        // above SDL's own window regardless of which one is "key" -- windowLevel ordering
        // is independent of key-window status, unlike relying on presentation z-order.
        overlayWindow.windowLevel = .alert + 1
        overlayWindow.backgroundColor = .clear
        overlayWindow.rootViewController =
            UIHostingController(rootView: LoadingOverlayRootView(gameName: gameName, state: state))
        overlayWindow.rootViewController?.view.backgroundColor = .clear
        overlayWindow.isHidden = false
        window = overlayWindow
        print("[AetherPS4] LoadingOverlayWindow: shown")
    }

    /// Call once the game session actually ends -- tears the whole overlay window down
    /// (not just the popup content), regardless of whether the user had left it open or
    /// closed at the time.
    static func teardown() {
        uiState = nil
        window?.isHidden = true
        window = nil
        print("[AetherPS4] LoadingOverlayWindow: torn down")
    }
}

@MainActor
private final class LoadingOverlayUIState: ObservableObject {
    @Published var isPopupVisible = true
}

private struct LoadingOverlayRootView: View {
    let gameName: String
    @ObservedObject var state: LoadingOverlayUIState

    var body: some View {
        ZStack(alignment: .topTrailing) {
            if state.isPopupVisible {
                Color.black.opacity(0.85)
                    .ignoresSafeArea()
                popup
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                // No background layer at all here -- SwiftUI only claims touches over
                // views that actually draw something, so everywhere except this button
                // passes taps through to SDL's window underneath.
                reopenButton
                    .padding(.top, 50)
                    .padding(.trailing, 16)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: state.isPopupVisible)
    }

    private var popup: some View {
        VStack(spacing: 20) {
            ProgressView()
                .controlSize(.large)
                .tint(.white)
            Text("Loading \(gameName)…")
                .font(.title3.bold())
                .foregroundStyle(.white)
            Text("Boot time varies and isn't reported by the engine -- close this any time and reopen it later from the corner button.")
                .font(.caption)
                .foregroundStyle(.white.opacity(0.6))
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
            Button("Close") {
                state.isPopupVisible = false
            }
            .buttonStyle(.borderedProminent)
            .padding(.top, 8)
        }
        .padding(32)
    }

    private var reopenButton: some View {
        Button {
            state.isPopupVisible = true
        } label: {
            Image(systemName: "hourglass")
                .font(.title3)
                .foregroundStyle(.white)
                .padding(12)
                .background(Color.black.opacity(0.6), in: Circle())
        }
    }
}
