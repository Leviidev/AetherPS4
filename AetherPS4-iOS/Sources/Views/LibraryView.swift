import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @Environment(GameLibrary.self) private var library

    private let columns = [GridItem(.adaptive(minimum: 150, maximum: 180), spacing: 20)]

    var body: some View {
        Group {
            if library.isImporting {
                importingState
            } else if library.games.isEmpty {
                emptyState
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 20) {
                        ForEach(library.games) { game in
                            NavigationLink(value: game) {
                                GameCardView(game: game, isSelected: false)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(20)
                }
            }
        }
        .navigationTitle("Library")
        .navigationDestination(for: Game.self) { game in
            GameDetailView(game: game)
        }
        .toolbar {
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
        VStack(spacing: 12) {
            Image(systemName: "shippingbox")
                .font(.system(size: 44))
                .foregroundStyle(.secondary)
            Text("No games yet")
                .font(.headline)
            Text("Use Add Game to import a .pkg file.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var importingState: some View {
        VStack(spacing: 12) {
            ProgressView()
            Text("Extracting game…")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
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
