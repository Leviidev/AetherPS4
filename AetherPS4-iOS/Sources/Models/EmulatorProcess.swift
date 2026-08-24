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

    var isRunning: Bool {
        if case .running = state { return true }
        return false
    }

    var isBusy: Bool {
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

        // PS4 games are landscape-only; lock and force the rotation now so SDL's window
        // is created in the right orientation from the start instead of the user having
        // to manually rotate to fix content that renders "off screen" in portrait. See
        // AppDelegate's doc comment (AetherPS4App.swift) for why this goes through the
        // app delegate rather than a SwiftUI API.
        lockToLandscape()

        // Loading state and the console log attach as a SwiftUI subview of SDL's own UIWindow
        // once it exists (GameOverlay.swift's GameOverlayHost). Confirmed on-device that the
        // previous Timer-on-RunLoop.main version of this polling was the actual cause of a
        // real crash (disabling this one call site made it disappear); pollUntilAttached() now
        // polls on a plain background thread instead and never touches the main thread until
        // it has found the window -- see its own doc comment for the full story.
        GameOverlayHost.pollUntilAttached()

        // shadps4_run() MUST run on the main thread: SDL's UIKit backend creates the
        // window and drives its own event loop from whatever thread calls this, and
        // that has to be the app's real main thread. Once it starts, SDL owns the
        // screen/run loop for the whole game session -- SwiftUI won't be able to
        // process taps (including a "Stop" button) again until shadps4_run() returns,
        // the same way a native SDL app's own window is what's interactive during
        // play, not surrounding host-app UI. .main.async (not .global()) defers this
        // one turn so it doesn't re-enter SwiftUI's button-action call frame, while
        // still landing on the main thread.
        DispatchQueue.main.async { [weak self] in
            let result = pkgPath.withCString { shadps4_run($0) }
            guard let self else { return }
            GameOverlayHost.detach()
            self.appendLine(.stdout, "[AetherPS4] shadPS4 exited with status \(result)")
            self.state = .exited(status: result)
            self.unlockOrientation()
        }
    }

    private func lockToLandscape() {
        AppDelegate.orientationLock = .landscape
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else { return }
        scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
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
