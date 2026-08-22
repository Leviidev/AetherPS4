import Foundation
import Observation
import CryptoKit

/// Owns the persisted list of games. Games are stored by reference (path
/// only); game metadata and cover art are dynamically probed and resolved.
@MainActor
@Observable
final class GameLibrary {
    private(set) var games: [Game] = []

    private let storeURL: URL
    private let fileManager = FileManager.default

    init(storeURL: URL? = nil) {
        if let storeURL {
            self.storeURL = storeURL
        } else {
            let supportDir = FileManager.default
                .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("AetherPS4", isDirectory: true)
            try? FileManager.default.createDirectory(at: supportDir, withIntermediateDirectories: true)
            self.storeURL = supportDir.appendingPathComponent("library.json")
        }
        load()
    }

    // MARK: - Persistence

    private func load() {
        guard let data = try? Data(contentsOf: storeURL) else { return }
        guard let decoded = try? JSONDecoder().decode([Game].self, from: data) else { return }
        games = decoded.map { resolveMetadata(for: $0) }.sorted { $0.dateAdded < $1.dateAdded }
        save()
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(games) else { return }
        try? data.write(to: storeURL, options: .atomic)
    }

    // MARK: - Mutations

    /// Adds a `.pkg` reference to the library. Returns the new game, or nil if
    /// this exact path is already present (no duplicate entries).
    @discardableResult
    func addGame(pkgPath: String) -> Game? {
        guard !games.contains(where: { $0.pkgPath == pkgPath }) else { return nil }
        let initialName = displayName(forPkgPath: pkgPath)
        var game = Game(name: initialName, pkgPath: pkgPath)
        game = resolveMetadata(for: game)
        games.append(game)
        save()
        return game
    }

    func removeGame(_ game: Game) {
        games.removeAll { $0.id == game.id }
        save()
    }

    /// Re-checks on-disk state and metadata for all games.
    func refresh() {
        games = games.map { resolveMetadata(for: $0) }
        save()
    }

    // MARK: - Metadata Resolution

    /// Resolves title, title ID, version, and cover images from extracted files.
    func resolveMetadata(for game: Game) -> Game {
        var updated = game
        let hash = SHA256.hash(data: Data(game.pkgPath.utf8))
            .compactMap { String(format: "%02x", $0) }
            .joined()

        let supportDir = FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("AetherPS4", isDirectory: true)
        let extractedSysDir = supportDir
            .appendingPathComponent("extracted", isDirectory: true)
            .appendingPathComponent(hash, isDirectory: true)
            .appendingPathComponent("sce_sys", isDirectory: true)

        let sfoURL = extractedSysDir.appendingPathComponent("param.sfo")
        if let sfo = ParamSfo.parse(from: sfoURL) {
            if let title = sfo.title, !title.isEmpty {
                updated.name = title
            }
            if let titleId = sfo.titleId, !titleId.isEmpty {
                updated.titleId = titleId
            }
            if let appVersion = sfo.appVersion, !appVersion.isEmpty {
                updated.appVersion = appVersion
            }
        }

        let iconURL = extractedSysDir.appendingPathComponent("icon0.png")
        if fileManager.fileExists(atPath: iconURL.path) {
            updated.iconPath = iconURL.path
        }

        let bannerURL = extractedSysDir.appendingPathComponent("pic1.png")
        if fileManager.fileExists(atPath: bannerURL.path) {
            updated.bannerPath = bannerURL.path
        }

        return updated
    }

    /// Derives a display name from the filename as fallback.
    private func displayName(forPkgPath path: String) -> String {
        let filename = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
        return filename.isEmpty ? "Untitled Game" : filename
    }
}
