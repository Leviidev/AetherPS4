import GameController
import SwiftUI
import UIKit

// PS4 games are inherently landscape-only (SDL's window/framebuffer is always landscape,
// matching PS4 native output) -- without this, a game session starts in whatever
// orientation the device happened to be in, and if that's portrait, SDL's landscape
// content ends up positioned/sized for a landscape screen inside a portrait one,
// appearing cut off ("off screen") until the user manually rotates. EmulatorProcess
// flips AppDelegate.orientationLock to .landscape for the duration of a game session
// (see launch()/its shadps4_run() completion handler) and back to .all once it's over,
// since the library/settings UI is fine in either orientation. SwiftUI itself has no
// per-screen orientation override API, so this goes through the one UIKit hook that
// actually works: UIApplicationDelegate's supportedInterfaceOrientationsFor.
final class AppDelegate: NSObject, UIApplicationDelegate {
    static var orientationLock: UIInterfaceOrientationMask = .all

    func application(_ application: UIApplication,
                      supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        AppDelegate.orientationLock
    }

    // SDL's own MFi joystick driver (SDL_mfijoystick.m) only ever listens passively for
    // GCControllerDidConnectNotification and enumerates GCController.controllers -- it
    // never calls startWirelessControllerDiscoveryWithCompletionHandler itself. That means
    // a Bluetooth controller iOS has never seen before (not previously paired via
    // Settings > Bluetooth) simply never appears to the app, no matter what buttons get
    // pressed on it -- discovery has to be actively started by someone. Kicking it off here
    // at launch puts the device into MFi/BLE discovery mode immediately, so a controller
    // already in pairing mode connects on its own; SDL's existing notification listener
    // then picks it up exactly like any other GCController connect.
    //
    // Discovery is a short-lived window, not something that stays active for the whole
    // process lifetime -- a controller paired *after* launch (e.g. the user backgrounds
    // the app to pair via Settings, or just powers the controller on well into a play
    // session) can land after that window already closed, or after it was stopped by an
    // unrelated earlier connect. Restarting it in applicationDidBecomeActive as well means
    // every time the app is actually in front of the user, it's back in discovery mode
    // (unless a controller is already connected), rather than only ever getting one shot
    // at cold launch.
    func application(_ application: UIApplication,
                      didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil,
                                                queue: .main) { _ in
            GCController.stopWirelessControllerDiscovery()
        }
        startDiscoveryIfNeeded()
        return true
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        startDiscoveryIfNeeded()
    }

    private func startDiscoveryIfNeeded() {
        guard GCController.controllers().isEmpty else { return }
        GCController.startWirelessControllerDiscovery(completionHandler: {})
    }
}

@main
struct AetherPS4App: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var library = GameLibrary()
    @State private var emulator = EmulatorProcess()

    init() {
        CrashLogger.shared.setup()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(library)
                .environment(emulator)
        }
    }
}
