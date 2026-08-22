import SwiftUI

@main
struct AetherPS4App: App {
    @State private var library = GameLibrary()
    @State private var emulator = EmulatorProcess()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(library)
                .environment(emulator)
                .frame(minWidth: 820, minHeight: 560)
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
