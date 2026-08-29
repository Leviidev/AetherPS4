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
    private static var progressModel: LoadingProgressModel?

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

        let model = LoadingProgressModel()
        progressModel = model

        let overlayWindow = UIWindow(windowScene: scene)
        // .alert + 1: above everything UIKit itself puts up (alerts, action sheets), and
        // above SDL's own window regardless of which one is "key" -- windowLevel ordering
        // is independent of key-window status, unlike relying on presentation z-order.
        overlayWindow.windowLevel = .alert + 1
        overlayWindow.backgroundColor = .clear
        overlayWindow.rootViewController =
            UIHostingController(rootView: LoadingOverlayView(gameName: gameName, model: model))
        overlayWindow.rootViewController?.view.backgroundColor = .clear
        overlayWindow.isHidden = false
        window = overlayWindow
        print("[AetherPS4] LoadingOverlayWindow: shown")

        // shadps4_has_presented_frame() flips true on the presenter's very FIRST successful
        // swapchain present -- confirmed via a boot log to still be loading/splash content at
        // that point, not the real game (thousands of presents happen between it and a game
        // actually reaching its title screen). Same false-positive "ready" signal that made
        // the ImGui touch overlay show up too early before it was disabled. So this doesn't
        // dismiss the instant that flag flips: it starts a fixed grace period THEN, during
        // which the progress bar visibly finishes filling, then hides.
        let timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { _ in
            model.tick(framePresented: shadps4_has_presented_frame() != 0)
            if model.isDone {
                hide()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
    }

    /// Call once the game session ends, as a safety net for a game that crashed or exited
    /// before ever presenting a frame (the timer above already hides it once loading
    /// actually finishes).
    static func hide() {
        pollTimer?.invalidate()
        pollTimer = nil
        progressModel = nil
        window?.isHidden = true
        window = nil
        print("[AetherPS4] LoadingOverlayWindow: hidden")
    }
}

// Drives the progress bar in two phases, since there's no real percentage signal from the
// engine to show: before the first frame presents, progress creeps toward (but never
// reaches) 90% -- always visibly moving, never claiming to be done. Once the first frame
// presents, it's a fixed 3s countdown from wherever it was up to 100%, which is also the
// actual dismiss timer -- so the bar visually finishes filling right as the loading screen
// goes away, rather than being disconnected from what's actually happening.
@MainActor
private final class LoadingProgressModel: ObservableObject {
    @Published var progress: Double = 0
    private(set) var isDone = false
    private var graceElapsed: TimeInterval = 0
    private let tickInterval: TimeInterval = 0.1
    private let gracePeriod: TimeInterval = 3.0
    private var framePresentedAt: TimeInterval?

    func tick(framePresented: Bool) {
        guard !isDone else { return }
        if framePresented {
            if framePresentedAt == nil {
                framePresentedAt = graceElapsed
            }
            graceElapsed += tickInterval
            let sinceFirstFrame = graceElapsed - (framePresentedAt ?? graceElapsed)
            let base = progress
            let target = min(1.0, sinceFirstFrame / gracePeriod)
            progress = base + (1.0 - base) * min(1.0, target)
            if sinceFirstFrame >= gracePeriod {
                progress = 1.0
                isDone = true
            }
        } else {
            // Asymptotic creep toward 90% -- always visibly making progress without ever
            // claiming to be finished before the game has even presented anything.
            progress += (0.9 - progress) * 0.03
        }
    }
}

private struct LoadingOverlayView: View {
    let gameName: String
    @ObservedObject var model: LoadingProgressModel

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 20) {
                Text("Loading \(gameName)…")
                    .font(.title3.bold())
                    .foregroundStyle(.white)
                ProgressView(value: model.progress, total: 1.0)
                    .progressViewStyle(.linear)
                    .tint(.white)
                    .frame(maxWidth: 260)
                Text("Do not tap or leave the app")
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.7))
            }
        }
    }
}
