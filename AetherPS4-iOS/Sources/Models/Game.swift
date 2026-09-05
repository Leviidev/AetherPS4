import Foundation

/// Where a `Game`'s extracted files live. Only ever `.external` in storage -- see
/// `Game.storageLocation`'s own comment for why `nil` (not an `.internal` case) represents
/// internal storage.
enum GameStorageLocation: String, Codable {
    case external
}

/// A reference to a `.pkg` file the user has added to their library.
///
/// Stores path reference along with probed / extracted metadata and cover art.
struct Game: Identifiable, Codable, Hashable {
    let id: UUID
    /// Display name (e.g. "Sonic Mania", "Minecraft: PlayStation®4 Edition").
    var name: String
    /// Path to the imported `.pkg` file, relative to whichever root
    /// `storageLocation` points at (e.g. "games/Foo.pkg"). Stored relative, not
    /// absolute: iOS container paths are not guaranteed stable across app
    /// launches or reinstalls, so persisting an absolute path can silently
    /// go stale even without the user doing anything.
    var pkgPath: String
    /// Where this game's extracted files actually live. Optional (rather than a
    /// non-optional field defaulting to `.internal`) specifically so decoding an
    /// existing library.json written before this field existed doesn't fail outright --
    /// Codable's auto-synthesis leaves a missing key as nil instead of throwing, but only
    /// for Optional properties. `nil` means internal, exactly matching every game's only
    /// possible location before external storage support existed.
    var storageLocation: GameStorageLocation?
    var dateAdded: Date
    /// Title ID (e.g. "CUSA07023", "CUSA00744").
    var titleId: String?
    /// Application version (e.g. "01.00").
    var appVersion: String?
    /// Path to cover art image (icon0.png).
    var iconPath: String?
    /// Path to backdrop banner image (pic1.png).
    var bannerPath: String?

    init(
        id: UUID = UUID(),
        name: String,
        pkgPath: String,
        storageLocation: GameStorageLocation? = nil,
        dateAdded: Date = .now,
        titleId: String? = nil,
        appVersion: String? = nil,
        iconPath: String? = nil,
        bannerPath: String? = nil
    ) {
        self.id = id
        self.name = name
        self.pkgPath = pkgPath
        self.storageLocation = storageLocation
        self.dateAdded = dateAdded
        self.titleId = titleId
        self.appVersion = appVersion
        self.iconPath = iconPath
        self.bannerPath = bannerPath
    }

    /// Resolves the relative `pkgPath` against whichever root this game actually lives
    /// under -- the app's own Application Support directory for internal games (always
    /// valid even if the container's absolute path has changed since the game was added),
    /// or the external drive's bookmarked root for external ones. Falls back to the
    /// internal root if a game is marked external but no external drive is currently
    /// reachable, so `isAvailable` below correctly reports it as missing rather than
    /// resolving to a nonsense path.
    var pkgURL: URL {
        if storageLocation == .external, let externalRoot = ExternalStorageStore.shared.gamesRootURL {
            return externalRoot.appendingPathComponent(pkgPath)
        }
        return FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(pkgPath)
    }

    /// Absolute, on-disk path — resolved fresh each access. Use this (not
    /// stored `pkgPath`) for anything doing actual file I/O.
    var absolutePkgPath: String {
        pkgURL.path
    }

    var iconURL: URL? {
        guard let iconPath, FileManager.default.fileExists(atPath: iconPath) else { return nil }
        return URL(fileURLWithPath: iconPath)
    }

    var bannerURL: URL? {
        guard let bannerPath, FileManager.default.fileExists(atPath: bannerPath) else { return nil }
        return URL(fileURLWithPath: bannerPath)
    }

    /// Whether the referenced file still exists. Computed on demand (not cached)
    /// so the library view always reflects current disk state.
    var isAvailable: Bool {
        FileManager.default.fileExists(atPath: absolutePkgPath)
    }
}
