import SwiftUI

struct ContentView: View {
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
    }
}
