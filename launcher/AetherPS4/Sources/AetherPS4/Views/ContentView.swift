import SwiftUI

enum SidebarSection: String, CaseIterable, Identifiable {
    case library = "Library"
    case console = "Console"
    case settings = "Settings"

    var id: String { rawValue }

    var systemImage: String {
        switch self {
        case .library: "square.grid.2x2"
        case .console: "terminal"
        case .settings: "gearshape"
        }
    }
}

struct ContentView: View {
    @Environment(GameLibrary.self) private var library
    @Environment(EmulatorProcess.self) private var emulator

    @State private var selection: SidebarSection? = .library
    @State private var selectedGame: Game?

    var body: some View {
        NavigationSplitView {
            List(SidebarSection.allCases, selection: $selection) { section in
                Label(section.rawValue, systemImage: section.systemImage)
                    .tag(section)
            }
            .navigationSplitViewColumnWidth(min: 160, ideal: 180)
        } detail: {
            switch selection {
            case .library, .none:
                LibraryView(selectedGame: $selectedGame)
            case .console:
                ConsoleView()
            case .settings:
                SettingsView()
            }
        }
        .navigationTitle("AetherPS4")
    }
}
