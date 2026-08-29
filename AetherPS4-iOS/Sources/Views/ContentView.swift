import SwiftUI

struct ContentView: View {
    // No persistence across launches: the user asked for this check every time the
    // app launches, not once ever -- resetting to false on every fresh ContentView
    // means every cold launch re-verifies rather than trusting a stale prior result.
    @State private var setupVerified = false

    var body: some View {
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
    }
}
