import Foundation

/// A reference to a `.pkg` file the user has added to their library.
///
/// Stores path reference along with probed / extracted metadata and cover art.
struct Game: Identifiable, Codable, Hashable {
    let id: UUID
    /// Display name (e.g. "Sonic Mania", "Minecraft: PlayStation®4 Edition").
    var name: String
    /// Absolute path to the `.pkg` file on disk.
    var pkgPath: String
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
        dateAdded: Date = .now,
        titleId: String? = nil,
        appVersion: String? = nil,
        iconPath: String? = nil,
        bannerPath: String? = nil
    ) {
        self.id = id
        self.name = name
        self.pkgPath = pkgPath
        self.dateAdded = dateAdded
        self.titleId = titleId
        self.appVersion = appVersion
        self.iconPath = iconPath
        self.bannerPath = bannerPath
    }

    var pkgURL: URL {
        URL(fileURLWithPath: pkgPath)
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
        FileManager.default.fileExists(atPath: pkgPath)
    }
}
