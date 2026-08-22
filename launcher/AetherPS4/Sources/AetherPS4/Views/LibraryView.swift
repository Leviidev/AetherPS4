import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @Environment(GameLibrary.self) private var library
    @Binding var selectedGame: Game?

    @State private var isDropTargeted = false
    @State private var isImporterPresented = false

    private let columns = [GridItem(.adaptive(minimum: 150, maximum: 180), spacing: 20)]

    var body: some View {
        HSplitView {
            VStack(spacing: 0) {
                header
                Divider()
                if library.games.isEmpty {
                    emptyState
                } else {
                    ScrollView {
                        LazyVGrid(columns: columns, spacing: 20) {
                            ForEach(library.games) { game in
                                GameCardView(game: game, isSelected: game.id == selectedGame?.id)
                                    .onTapGesture { selectedGame = game }
                            }
                        }
                        .padding(20)
                    }
                }
            }
            .frame(minWidth: 420)
            .overlay {
                if isDropTargeted {
                    RoundedRectangle(cornerRadius: 12)
                        .strokeBorder(Color.accentColor, lineWidth: 3)
                        .background(Color.accentColor.opacity(0.08))
                        .padding(8)
                        .allowsHitTesting(false)
                }
            }
            .dropDestination(for: URL.self) { urls, _ in
                handleDrop(urls: urls)
            } isTargeted: { targeted in
                isDropTargeted = targeted
            }

            if let selectedGame, library.games.contains(where: { $0.id == selectedGame.id }) {
                GameDetailView(game: selectedGame) {
                    self.selectedGame = nil
                }
                .frame(minWidth: 260, idealWidth: 300)
            } else {
                emptyDetail
                    .frame(minWidth: 260, idealWidth: 300)
            }
        }
        .fileImporter(
            isPresented: $isImporterPresented,
            allowedContentTypes: [.pkgPackage],
            allowsMultipleSelection: true
        ) { result in
            if case .success(let urls) = result {
                for url in urls {
                    addGame(at: url)
                }
            }
        }
    }

    private var header: some View {
        HStack {
            Text("Library")
                .font(.title2.bold())
            Spacer()
            Button {
                library.refresh()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)

            Button {
                isImporterPresented = true
            } label: {
                Label("Add Game", systemImage: "plus")
            }
            .buttonStyle(.borderedProminent)
        }
        .padding(16)
    }

    private var emptyState: some View {
        VStack(spacing: 12) {
            Image(systemName: "shippingbox")
                .font(.system(size: 44))
                .foregroundStyle(.secondary)
            Text("No games yet")
                .font(.headline)
            Text("Drag a .pkg file here, or use Add Game.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var emptyDetail: some View {
        VStack(spacing: 8) {
            Image(systemName: "gamecontroller")
                .font(.system(size: 32))
                .foregroundStyle(.tertiary)
            Text("Select a game")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(.background.secondary)
    }

    private func handleDrop(urls: [URL]) -> Bool {
        let pkgURLs = urls.filter { $0.pathExtension.lowercased() == "pkg" }
        guard !pkgURLs.isEmpty else { return false }
        for url in pkgURLs {
            addGame(at: url)
        }
        return true
    }

    private func addGame(at url: URL) {
        guard let added = library.addGame(pkgPath: url.path) else { return }
        selectedGame = added
    }
}

extension UTType {
    /// shadPS4/PS4 package files use the `.pkg` extension. No installed
    /// system UTI declares this, so declare a minimal one from the extension
    /// alone rather than relying on a nonexistent system type.
    static var pkgPackage: UTType {
        UTType(filenameExtension: "pkg") ?? .data
    }
}
