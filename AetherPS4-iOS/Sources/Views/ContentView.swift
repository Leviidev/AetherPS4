import SwiftUI

struct ContentView: View {
    // No persistence across launches: the user asked for this check every time the
    // app launches, not once ever -- resetting to false on every fresh ContentView
    // means every cold launch re-verifies rather than trusting a stale prior result.
    @State private var setupVerified = false
    @Environment(EmulatorProcess.self) private var emulator

    var body: some View {
        // @Observable's dependency tracking is fine-grained: it only tracks properties
        // actually read during body's OWN evaluation, not ones read later inside a closure
        // SwiftUI invokes separately (like a Binding's get). isRunning was only ever read
        // inside the fullScreenCover binding's get closure below, so changing
        // emulator.state never actually triggered a re-render of this view at all --
        // confirmed on-device as the real cause of the loading cover not appearing.
        // Reading it here, directly in body, makes it a tracked dependency.
        let isRunning = emulator.isRunning

        TabView {
            NavigationStack {
                LibraryView()
            }
            .tabItem {
                Label("Library", systemImage: "square.grid.2x2")
            }

            NavigationStack {
                ConsoleView()
            }
            .tabItem {
                Label("Console", systemImage: "terminal")
            }

            NavigationStack {
                SettingsView()
            }
            .tabItem {
                Label("Settings", systemImage: "gearshape")
            }
        }
        // fullScreenCover (not .sheet): no swipe-to-dismiss, so the only way past this
        // is actually passing both checks -- matches "only let them proceed if" working.
        .fullScreenCover(isPresented: Binding(get: { !setupVerified }, set: { _ in })) {
            SetupCheckView(onPassed: { setupVerified = true })
        }
        // Shown the instant EmulatorProcess.launch() flips state to .running, in this
        // window (not a separate one, and not attached to SDL's) -- see GameLoadingCoverView's
        // own header comment for why that's the one part of the "loading overlay" idea that
        // actually works reliably, after several attempts at a live-updating version that
        // depended on getting new work serviced on the main thread during shadps4_run()
        // did not.
        .fullScreenCover(isPresented: Binding(get: { isRunning }, set: { _ in })) {
            GameLoadingCoverView(gameName: emulator.runningGameName)
        }
    }
}
