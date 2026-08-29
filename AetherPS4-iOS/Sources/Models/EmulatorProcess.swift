import Foundation
import Observation
import UIKit

enum EmulatorRunState: Equatable {
    case idle
    case extracting
    case running
    case exited(status: Int32)
}

struct ConsoleLine: Identifiable {
    enum Stream { case stdout, stderr }
    let id = UUID()
    let stream: Stream
    let text: String
}

@MainActor
@Observable
final class EmulatorProcess {
    private(set) var state: EmulatorRunState = .idle
    private(set) var consoleLines: [ConsoleLine] = []
    private(set) var runningGameName: String?

    private var didInit = false
    // Set the instant launch() is called, cleared once the rotation-wait below hands off
    // to the rest of launch -- guards against a second launch() call re-entering during
    // that window, since state itself doesn't flip to .running until after it (see
    // launch()'s own comment for why the wait has to come first).
    private var isPreparingToLaunch = false

    var isRunning: Bool {
        if case .running = state { return true }
        return false
    }

    var isBusy: Bool {
        if isPreparingToLaunch { return true }
        switch state {
        case .running, .extracting: return true
        case .idle, .exited: return false
        }
    }

    func launch(pkgPath: String, gameName: String) {
        guard !isBusy else { return }

        // Setup JIT if needed
        if !checkDebugged() {
            JITEnabler.requestStikDebugJIT()
            appendLine(.stderr, "[AetherPS4] Waiting for StikDebug JIT to attach...")
            return
        }

        configureJITEnvVars()
        isPreparingToLaunch = true

        // Request the rotation and actually wait for it to finish animating before
        // presenting the loading cover (state = .running below) or doing anything else --
        // confirmed on-device, twice, that presenting the cover concurrently with an
        // in-flight forced rotation is what breaks it (visible while it's happening, gone
        // once it settles), regardless of whether the rotation was requested before the
        // cover, after it, or from the cover's own onAppear. requestGeometryUpdate has no
        // real "did finish" callback (its closure is error-only), so this uses a fixed
        // delay comfortably longer than iOS's own interface rotation animation (~0.3s).
        lockToLandscape()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.continueLaunch(pkgPath: pkgPath, gameName: gameName)
        }
    }

    private func continueLaunch(pkgPath: String, gameName: String) {
        isPreparingToLaunch = false
        self.runningGameName = gameName
        self.state = .running

        if !didInit {
            appendLine(.stdout, "[AetherPS4] Initializing shadPS4 Engine...")

            let documentsPath = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
            let initStatus = documentsPath.withCString { dirPtr -> Int32 in
                var options = ShadPS4Options()
                options.user_dir = dirPtr
                options.show_fps = UserDefaults.standard.bool(forKey: "showFpsCounter") ? 1 : 0
                options.network_enabled = UserDefaults.standard.bool(forKey: "networkEnabled") ? 1 : 0
                options.fullscreen = 1
                return shadps4_init(&options)
            }
            if initStatus != 0 {
                appendLine(.stderr, "[AetherPS4] Failed to initialize shadPS4")
                self.state = .exited(status: initStatus)
                return
            }
            didInit = true
        }

        appendLine(.stdout, "[AetherPS4] Launching \(gameName)...")

        // Split from the old single blocking shadps4_run() call: shadps4_prepare_window()
        // does the same UIKit-touching setup (SDL creates its own real UIWindow) that used
        // to block the WHOLE game session for, but returns as soon as that window exists --
        // still has to run on the main thread for the same reason shadps4_run() did, but
        // continueLaunch() is already on the main actor, so this can just call it directly.
        // See shadps4_ios_api.h's own comment on both halves for the full reasoning.
        let prepareStatus = pkgPath.withCString { shadps4_prepare_window($0) }
        guard prepareStatus == 0 else {
            appendLine(.stderr, "[AetherPS4] Failed to prepare game window")
            state = .exited(status: prepareStatus)
            unlockOrientation()
            return
        }

        // shadps4_run_loop() is everything shadps4_prepare_window() didn't already do:
        // starting guest execution, then SDL's blocking event loop. Unlike the old
        // shadps4_run(), this is safe on a background thread -- the window (and every
        // other UIKit object involved) already exists, SDL's own event queue is
        // thread-safe by design, and nothing left in this call touches UIKit directly.
        // This is what actually frees the main thread for the rest of the game session:
        // SwiftUI/UIKit keeps working normally the whole time instead of freezing the
        // instant the game starts (see GameLoadingCoverView's own header comment for the
        // years -- well, hours -- of workarounds that were needed before this existed).
        // Thread.detachNewThread (not DispatchQueue.global()) for a plain dedicated OS
        // thread that stays alive for exactly this one blocking call, same as the render
        // thread pattern used elsewhere in this codebase.
        Thread.detachNewThread { [weak self] in
            let result = shadps4_run_loop()
            DispatchQueue.main.async {
                guard let self else { return }
                self.appendLine(.stdout, "[AetherPS4] shadPS4 exited with status \(result)")
                self.state = .exited(status: result)
                self.unlockOrientation()
            }
        }
    }

    // PS4 games are landscape-only; lock and force the rotation now so SDL's window is
    // created in the right orientation from the start instead of the user having to
    // manually rotate to fix content that renders "off screen" in portrait. Goes through
    // the app delegate rather than a SwiftUI API -- see AppDelegate's doc comment
    // (AetherPS4App.swift) for why. Called from launch(), before the loading cover ever
    // presents -- see its own comment for why.
    func lockToLandscape() {
        AppDelegate.orientationLock = .landscape
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else { return }
        // Walk to the actual topmost presented view controller, not just the root: telling
        // only the root to update its supported orientations doesn't reliably propagate to
        // whatever is actually on screen (e.g. GameLoadingCoverView's own fullScreenCover),
        // confirmed on-device as part of why the loading cover stopped showing once a
        // forced rotation completed.
        var topController = scene.windows.first?.rootViewController
        while let presented = topController?.presentedViewController {
            topController = presented
        }
        topController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: .landscape)) { _ in
            // Best-effort: fails harmlessly if e.g. the user has Control Center's
            // portrait orientation lock enabled, in which case they'll need to rotate
            // manually same as before -- not worth surfacing as an error.
        }
    }

    private func unlockOrientation() {
        AppDelegate.orientationLock = .all
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else { return }
        scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
    }

    func stop() {
        guard isRunning else { return }
        // Safe to call from any thread (pushes an SDL_EVENT_QUIT); this is really the
        // only way to interrupt a running game from outside SDL's own window, since
        // the main thread is inside shadps4_run() for the whole session -- see launch().
        shadps4_stop()
    }

    func clearConsole() {
        consoleLines.removeAll()
    }

    func appendLine(_ stream: ConsoleLine.Stream, _ text: String) {
        consoleLines.append(ConsoleLine(stream: stream, text: text))
    }
}
