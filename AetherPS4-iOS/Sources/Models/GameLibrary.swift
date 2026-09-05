import Foundation
import Observation

private struct PkgImportError: Error {
    let message: String
}

/// Owns the persisted list of games. Games are stored by reference (path
/// only); game metadata and cover art are dynamically probed and resolved.
@MainActor
@Observable
final class GameLibrary {
    private(set) var games: [Game] = []
    private(set) var isImporting = false
    var lastImportError: String?

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
    func addGame(pkgPath: String, storageLocation: GameStorageLocation? = nil) -> Game? {
        guard !games.contains(where: { $0.pkgPath == pkgPath }) else { return nil }
        let initialName = displayName(forPkgPath: pkgPath)
        var game = Game(name: initialName, pkgPath: pkgPath, storageLocation: storageLocation)
        game = resolveMetadata(for: game)
        games.append(game)
        save()
        return game
    }

    /// Decrypts and extracts a user-picked, raw `.pkg` file into a plain game directory
    /// (eboot.bin + sce_sys/) under either this app's own Application Support/AetherPS4/games
    /// directory, or -- if an external drive is configured and currently reachable (see
    /// ExternalStorageStore) -- that drive's AetherPS4Games directory instead, then adds it
    /// to the library. A PS4 game's *installed* footprint (after PFS decompression) commonly
    /// runs several times its package size, easily 80GB+ for a modern title, which is why
    /// this exists at all: plenty of devices don't have that much free internal storage.
    /// shadps4_run() can only open an already-extracted eboot.bin -- it has no
    /// PKG-decryption code of its own (see bachata_pkg_extract, ported from
    /// tools/pkg-extract/, which does the actual PFS/RSA/AES work). Extraction runs on a
    /// background thread since a real PS4 package can be several GB; only the C calls
    /// themselves are off the main actor, everything touching `games`/`isImporting` still
    /// happens on it.
    @discardableResult
    func importPkg(from sourceURL: URL) async -> Game? {
        lastImportError = nil
        isImporting = true
        defer { isImporting = false }

        let didStartAccess = sourceURL.startAccessingSecurityScopedResource()
        defer { if didStartAccess { sourceURL.stopAccessingSecurityScopedResource() } }

        // External storage's own security-scoped access is already held open for the whole
        // app session by ExternalStorageStore itself (see its own comment on why) -- this
        // just decides which root to extract into, on the main actor since ExternalStorageStore
        // is @MainActor.
        let useExternal = ExternalStorageStore.shared.gamesRootURL != nil
        let externalRoot = ExternalStorageStore.shared.gamesRootURL
        let appSupportURL = FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let destinationRoot = useExternal ? externalRoot! : appSupportURL
        let sourcePath = sourceURL.path

        let result = await Task.detached(priority: .userInitiated) { () -> Result<String, PkgImportError> in
            let fd = open(sourcePath, O_RDONLY)
            guard fd >= 0 else { return .failure(PkgImportError(message: "Could not open the selected file.")) }
            defer { close(fd) }

            var probe = BachataPkgProbe()
            let probeStatus = bachata_pkg_probe(fd, &probe)
            guard probeStatus == 0 else {
                return .failure(PkgImportError(message: probeStatus == 1
                    ? "This package needs a passcode, which isn't supported yet."
                    : "This file isn't a valid, supported PS4 package."))
            }
            let contentId = GameLibrary.cString(from: probe.content_id)
            guard !contentId.isEmpty else {
                return .failure(PkgImportError(message: "Could not read the package's content ID."))
            }

            let relativeGameDir = useExternal ? "\(contentId)" : "AetherPS4/games/\(contentId)"
            let gameDir = destinationRoot.appendingPathComponent(relativeGameDir)
            try? FileManager.default.createDirectory(at: destinationRoot, withIntermediateDirectories: true)
            try? FileManager.default.removeItem(at: gameDir)

            let extractStatus = bachata_pkg_extract(fd, gameDir.path, nil, nil, nil)
            guard extractStatus == 0 else {
                try? FileManager.default.removeItem(at: gameDir)
                return .failure(PkgImportError(message: extractStatus == 1
                    ? "This package needs a passcode, which isn't supported yet."
                    : "Extraction failed."))
            }

            return .success("\(relativeGameDir)/eboot.bin")
        }.value

        switch result {
        case .success(let relativeEbootPath):
            return addGame(pkgPath: relativeEbootPath, storageLocation: useExternal ? .external : nil)
        case .failure(let error):
            lastImportError = error.message
            return nil
        }
    }

    /// Reads a fixed-size C char buffer (from a bridged C struct) as a Swift String.
    private nonisolated static func cString<T>(from tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }
    }

    func removeGame(_ game: Game) {
        games.removeAll { $0.id == game.id }
        try? fileManager.removeItem(atPath: game.absolutePkgPath)
        save()
    }

    /// Re-checks on-disk state and metadata for all games.
    func refresh() {
        games = games.map { resolveMetadata(for: $0) }
        save()
    }

    // MARK: - Metadata Resolution

    /// Resolves title, title ID, version, and cover images from the game's
    /// own extracted sce_sys/ directory (a sibling of its eboot.bin).
    func resolveMetadata(for game: Game) -> Game {
        var updated = game
        let extractedSysDir = game.pkgURL
            .deletingLastPathComponent()
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
