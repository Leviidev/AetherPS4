import SwiftUI
import UniformTypeIdentifiers

enum LibrarySortOrder: String, CaseIterable {
    case nameAscending = "Name (A–Z)"
    case nameDescending = "Name (Z–A)"
    case dateAddedNewest = "Recently Added"
    case dateAddedOldest = "Oldest Added"
}

struct LibraryView: View {
    @Environment(GameLibrary.self) private var library
    @State private var searchText = ""
    @State private var sortOrder: LibrarySortOrder = .dateAddedNewest

    private let columns = [GridItem(.adaptive(minimum: 140, maximum: 170), spacing: 22)]

    private var visibleGames: [Game] {
        let filtered: [Game]
        if searchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            filtered = library.games
        } else {
            filtered = library.games.filter { game in
                game.name.localizedCaseInsensitiveContains(searchText)
                    || (game.titleId?.localizedCaseInsensitiveContains(searchText) ?? false)
            }
        }
        switch sortOrder {
        case .nameAscending:
            return filtered.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        case .nameDescending:
            return filtered.sorted { $0.name.localizedStandardCompare($1.name) == .orderedDescending }
        case .dateAddedNewest:
            return filtered.sorted { $0.dateAdded > $1.dateAdded }
        case .dateAddedOldest:
            return filtered.sorted { $0.dateAdded < $1.dateAdded }
        }
    }

    var body: some View {
        Group {
            if library.isImporting {
                importingState
            } else if library.games.isEmpty {
                emptyState
            } else if visibleGames.isEmpty {
                noResultsState
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 26) {
                        ForEach(visibleGames) { game in
                            NavigationLink(value: game) {
                                GameCardView(game: game, isSelected: false)
                            }
                            .buttonStyle(CardPressStyle())
                        }
                    }
                    .padding(.horizontal, 20)
                    .padding(.vertical, 24)
                }
            }
        }
        .navigationTitle("Library")
        .navigationDestination(for: Game.self) { game in
            GameDetailView(game: game)
        }
        .searchable(text: $searchText, placement: .navigationBarDrawer(displayMode: .always), prompt: "Search games")
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Menu {
                    Picker("Sort", selection: $sortOrder) {
                        ForEach(LibrarySortOrder.allCases, id: \.self) { order in
                            Text(order.rawValue).tag(order)
                        }
                    }
                } label: {
                    Label("Sort", systemImage: "arrow.up.arrow.down")
                }
            }
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    library.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
            }
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    isImporterPresented = true
                } label: {
                    Label("Add Game", systemImage: "plus")
                }
                .buttonStyle(.borderedProminent)
                .disabled(library.isImporting)
            }
        }
        .fileImporter(
            isPresented: $isImporterPresented,
            allowedContentTypes: [.pkgPackage],
            allowsMultipleSelection: false
        ) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            Task { await library.importPkg(from: url) }
        }
        .alert(
            "Couldn't Add Game",
            isPresented: Binding(
                get: { library.lastImportError != nil },
                set: { if !$0 { library.lastImportError = nil } }
            )
        ) {
            Button("OK") { library.lastImportError = nil }
        } message: {
            Text(library.lastImportError ?? "")
        }
    }

    @State private var isImporterPresented = false

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "square.grid.2x2")
                .font(.system(size: 40))
                .foregroundStyle(.tertiary)
            VStack(spacing: 4) {
                Text("No games yet")
                    .font(.title3.weight(.semibold))
                Text("Import a .pkg file to add your first game.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
            Button {
                isImporterPresented = true
            } label: {
                Label("Add Game", systemImage: "plus")
                    .font(.subheadline.weight(.semibold))
                    .padding(.horizontal, 6)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.horizontal, 32)
    }

    private var importingState: some View {
        VStack(spacing: 14) {
            ProgressView()
                .controlSize(.large)
            Text("Extracting game…")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var noResultsState: some View {
        VStack(spacing: 12) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 40))
                .foregroundStyle(.tertiary)
            Text("No games match \"\(searchText)\"")
                .font(.title3.weight(.semibold))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.horizontal, 32)
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
