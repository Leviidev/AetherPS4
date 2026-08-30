import SwiftUI
import UIKit

// Shown instead of silently trying to relaunch in-process when a game calls
// sceSystemServiceLoadExec (see Emulator::Restart()'s own doc comment). On every other
// platform that syscall's handler fork()/exec()s a whole new process, which iOS's sandbox
// never permits; an earlier attempt at reusing the same in-process Emulator/Linker/Memory
// singletons for a second PrepareWindow()/RunLoop() call hung instead of crashing --
// confirmed on-device -- because that whole layer of the engine was only ever exercised by
// one game per process lifetime, the same assumption fork()/exec() relies on everywhere
// else. Actually reproducing a fresh process on iOS means the player has to be the one who
// closes and reopens the app; this window just makes that unmissable and gives them a
// one-tap way to do the "close" half instead of hunting for the app switcher themselves.
//
// Deliberately opaque and NOT dismissible (no X button, no passthrough hit-testing like
// LoadingOverlayWindow's) -- this is the last screen of the session. Leaving any way to tap
// through to GameDetailView's "Launch Game" button underneath would let the player re-enter
// the exact same broken restart path a second time; blocking all touches until they either
// tap Restart App or background/kill the app themselves is what actually prevents that.
@MainActor
enum RestartRequiredOverlayWindow {
    private static var window: UIWindow?

    static func show(gameName: String) {
        guard window == nil else { return }
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else {
            print("[AetherPS4] RestartRequiredOverlayWindow: no active window scene, cannot show")
            return
        }

        let overlayWindow = UIWindow(windowScene: scene)
        overlayWindow.windowLevel = .alert + 1
        overlayWindow.backgroundColor = UIColor.black.withAlphaComponent(0.92)
        overlayWindow.rootViewController = UIHostingController(rootView: RestartRequiredView(gameName: gameName))
        overlayWindow.rootViewController?.view.backgroundColor = .clear
        overlayWindow.isHidden = false
        window = overlayWindow
        print("[AetherPS4] RestartRequiredOverlayWindow: shown")
    }

    static func teardown() {
        window?.isHidden = true
        window = nil
    }
}

private struct RestartRequiredView: View {
    let gameName: String

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "arrow.triangle.2.circlepath")
                .font(.system(size: 40))
                .foregroundColor(.white)

            Text("Restart Needed")
                .font(.title2.bold())
                .foregroundColor(.white)

            Text("\(gameName) needs to restart to continue -- this is normal, real PS4 hardware does a full reboot here too.\n\nTap Restart App below, then reopen AetherPS4 and launch \(gameName) again.")
                .font(.body)
                .foregroundColor(.white.opacity(0.8))
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)

            Button {
                exit(0)
            } label: {
                Text("Restart App")
                    .font(.headline)
                    .frame(maxWidth: 240)
                    .padding()
                    .background(Color.red, in: RoundedRectangle(cornerRadius: 12))
                    .foregroundColor(.white)
            }
            .padding(.top, 8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .contentShape(Rectangle())
    }
}
