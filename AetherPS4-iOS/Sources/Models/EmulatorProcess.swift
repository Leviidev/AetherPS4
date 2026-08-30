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

    // args: guest argv entries to pass to the eboot's entry point, in order. Only ever
    // non-empty when resuming after a restart request (see resumeIfRestartPending()) -- a
    // normal library launch has nothing to pass and lets shadps4_prepare_window_with_args's
    // empty-string case fall back to its own default argv[0].
    func launch(pkgPath: String, gameName: String, args: [String] = []) {
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
            self?.continueLaunch(pkgPath: pkgPath, gameName: gameName, args: args)
        }
    }

    private func continueLaunch(pkgPath: String, gameName: String, args: [String] = []) {
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
        // Always the _with_args variant (an empty joined string is equivalent to the plain
        // call) so there's only one code path to keep correct rather than two.
        let joinedArgs = args.joined(separator: "\n")
        let prepareStatus = pkgPath.withCString { pathPtr in
            joinedArgs.withCString { argsPtr in
                shadps4_prepare_window_with_args(pathPtr, argsPtr)
            }
        }
        guard prepareStatus == 0 else {
            appendLine(.stderr, "[AetherPS4] Failed to prepare game window")
            state = .exited(status: prepareStatus)
            unlockOrientation()
            return
        }

        // TouchControlsOverlayWindow also owns the compact, touch-to-hide boot progress
        // badge. Keeping both in one UIWindow avoids the old loading window's landscape
        // coordinate mismatch and guarantees the progress UI cannot intercept controls in
        // another window. The badge removes itself after sustained successful presentation.
        TouchControlsOverlayWindow.show()

        // shadps4_run_loop() is everything shadps4_prepare_window() didn't already do:
        // starting guest execution, then SDL's blocking event loop. Unlike the old
        // shadps4_run(), this is safe on a background thread -- the window (and every
        // other UIKit object involved) already exists, SDL's own event queue is
        // thread-safe by design, and nothing left in this call touches UIKit directly.
        // This is what actually frees the main thread for the rest of the game session:
        // SwiftUI/UIKit keeps working normally the whole time instead of freezing the
        // instant the game starts. (Diagnostic test: temporarily ran this on the main
        // thread instead to check whether off-main-thread scheduling was causing Sonic
        // Mania's Title-Screen heap-corruption crash -- confirmed innocent, exact same
        // crash happened either way, so back to the background thread.)
        // Thread.detachNewThread (not DispatchQueue.global()) for a plain dedicated OS
        // thread that stays alive for exactly this one blocking call, same as the render
        // thread pattern used elsewhere in this codebase.
        Thread.detachNewThread { [weak self] in
            let result = shadps4_run_loop()

            // A game calling sceSystemServiceLoadExec (e.g. a chapter/level transition --
            // confirmed on-device with Journey) asks the engine to restart with a different
            // eboot. Every other shadPS4 platform handles that by fork()/exec()-ing a whole
            // new process; iOS can't do that at all (a sandboxed app cannot fork()), so
            // Emulator::Restart() instead stops this session the normal way and records the
            // new path for exactly this check (see shadps4_ios_api.h's own comment). Without
            // this, shadps4_run_loop() returning here would be indistinguishable from the
            // player quitting -- the whole app would fall through to .exited below, and the
            // in-progress restart request would just be dropped.
            //
            // An earlier version tried relaunching in-process right here (calling
            // continueLaunch() again with the new path) -- confirmed on-device to hang, not
            // crash: PrepareWindow()/RunLoop() reuse the same Emulator/Linker/Memory
            // singletons the first launch used, and that whole layer was only ever built to
            // run one game per process lifetime, same assumption every other platform's
            // fork()/exec() relies on. Actually getting a fresh process on iOS means the
            // player has to be the one to close and reopen the app -- RestartRequiredOverlayWindow
            // makes that unmissable and gives them a one-tap way to do it, instead of quietly
            // reusing state that was never proven safe to reuse.
            //
            // The path and guest args both have to survive that process restart, since
            // there's nothing left in memory once exit(0) actually runs -- persisted to
            // UserDefaults here and picked back up by resumeIfRestartPending() on the next
            // cold launch. Confirmed on-device that dropping the args (an earlier version of
            // this fix did) lands the player back at the game's own main menu instead of
            // continuing: sceSystemServiceLoadExec's argv is how the game tells its
            // newly-loaded self where to resume, the same way it would on real hardware.
            var pathBuffer = [Int8](repeating: 0, count: 4096)
            var argsBuffer = [Int8](repeating: 0, count: 4096)
            var restartPath: String?
            var restartArgs: [String] = []
            pathBuffer.withUnsafeMutableBufferPointer { pathBuf in
                argsBuffer.withUnsafeMutableBufferPointer { argsBuf in
                    guard let pathBase = pathBuf.baseAddress, let argsBase = argsBuf.baseAddress,
                          shadps4_take_pending_restart(pathBase, Int32(pathBuf.count), argsBase,
                                                       Int32(argsBuf.count)) != 0
                    else { return }
                    restartPath = String(cString: pathBase)
                    let joined = String(cString: argsBase)
                    restartArgs = joined.isEmpty ? [] : joined.components(separatedBy: "\n")
                }
            }
            if let restartPath {
                DispatchQueue.main.async {
                    guard let self else { return }
                    let name = self.runningGameName ?? "The game"
                    self.appendLine(.stdout, "[AetherPS4] \(name) requested a restart -- prompting to restart the app")
                    UserDefaults.standard.set(restartPath, forKey: "pendingRestartEbootPath")
                    UserDefaults.standard.set(restartArgs, forKey: "pendingRestartArgs")
                    UserDefaults.standard.set(name, forKey: "pendingRestartGameName")
                    LoadingOverlayWindow.teardown()
                    TouchControlsOverlayWindow.teardown()
                    self.state = .exited(status: 0)
                    self.unlockOrientation()
                    RestartRequiredOverlayWindow.show(gameName: name)
                }
                return
            }

            DispatchQueue.main.async {
                guard let self else { return }
                LoadingOverlayWindow.teardown()
                TouchControlsOverlayWindow.teardown()
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

    // Not private: also called directly by TouchControlsLayoutEditorView, which locks to
    // landscape for the same reason a real game session does (see lockToLandscape's own
    // comment) but isn't itself a game session, so it can't rely on continueLaunch's own
    // unlock-on-exit path.
    func unlockOrientation() {
        AppDelegate.orientationLock = .all
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else { return }
        scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
    }

    // Call once on cold launch, after JIT/setup verification passes (see ContentView), to
    // pick back up a game that requested a restart last session (see the restart-detection
    // block in continueLaunch() above for why this can't just happen automatically without
    // the player closing and reopening the app). No-op if nothing's pending.
    func resumeIfRestartPending() {
        let defaults = UserDefaults.standard
        guard let path = defaults.string(forKey: "pendingRestartEbootPath") else { return }
        let args = defaults.stringArray(forKey: "pendingRestartArgs") ?? []
        let name = defaults.string(forKey: "pendingRestartGameName") ?? "the game"
        defaults.removeObject(forKey: "pendingRestartEbootPath")
        defaults.removeObject(forKey: "pendingRestartArgs")
        defaults.removeObject(forKey: "pendingRestartGameName")
        appendLine(.stdout, "[AetherPS4] Resuming \(name) after restart...")
        launch(pkgPath: path, gameName: name, args: args)
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
