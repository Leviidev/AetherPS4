import Foundation
import Observation

/// Lets games (both freshly-imported PKGs and ones already in the library) live on an
/// external drive instead of this app's own internal container -- iOS apps are sandboxed to
/// their own Application Support directory by default, which sits on the device's internal
/// storage no matter what's plugged in, so reaching an external volume at all requires the
/// user to explicitly pick a folder on it through the system file picker (UIDocumentPicker,
/// which surfaces external drives connected over USB-C/Lightning the same way it surfaces
/// Files app locations) and this app persisting a *security-scoped bookmark* to it -- a raw
/// path to an external volume isn't guaranteed to still resolve, or be accessible without
/// re-requesting permission, the next time the app launches.
///
/// Access is granted once here, for the folder as a whole, and held open for as long as
/// `isConnected` is true (not acquired/released per file operation) -- this is standard
/// practice for a location the user has explicitly and persistently chosen as their game
/// library's home, and it means `Game.pkgURL` and everything that reads from it doesn't need
/// to know or care whether the game it's resolving lives internally or externally.
/// Deliberately not @MainActor: `Game.pkgURL` (a plain, non-isolated struct, since Game
/// instances cross Task boundaries during import) needs to read `gamesRootURL` synchronously,
/// and nothing here actually requires the main thread -- UserDefaults is internally
/// thread-safe, and the security-scoped resource calls carry no such requirement either. The
/// mutating methods (chooseFolder/forget/refresh) are only ever called from SwiftUI action
/// closures and scenePhase changes in practice, i.e. already on the main thread, without
/// needing the type system to enforce it.
@Observable
final class ExternalStorageStore {
    static let shared = ExternalStorageStore()

    private(set) var isConnected = false
    private(set) var displayName: String?

    private static let bookmarkKey = "externalGamesStorageBookmark"
    private var accessedURL: URL?

    private init() {
        resolveBookmark()
    }

    /// Call after the user picks a folder via a `.folder`-content-type file importer.
    func chooseFolder(_ pickedURL: URL) {
        let didAccess = pickedURL.startAccessingSecurityScopedResource()
        defer { if didAccess { pickedURL.stopAccessingSecurityScopedResource() } }
        guard let bookmark = try? pickedURL.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            return
        }
        UserDefaults.standard.set(bookmark, forKey: Self.bookmarkKey)
        resolveBookmark()
    }

    /// Drops the saved bookmark entirely. Games already recorded as external in the library
    /// stay recorded that way (and will show as unavailable) -- this only forgets the
    /// location, it doesn't touch any game data.
    func forget() {
        if let accessedURL {
            accessedURL.stopAccessingSecurityScopedResource()
        }
        accessedURL = nil
        UserDefaults.standard.removeObject(forKey: Self.bookmarkKey)
        isConnected = false
        displayName = nil
    }

    /// Re-attempts resolving the saved bookmark. External drives can be unplugged and
    /// reconnected independent of the app's own lifecycle (unlike internal storage, which is
    /// always there), so this is exposed for a manual "Check Connection" action and called
    /// automatically whenever the app returns to the foreground.
    func refresh() {
        resolveBookmark()
    }

    private func resolveBookmark() {
        if let accessedURL {
            accessedURL.stopAccessingSecurityScopedResource()
        }
        accessedURL = nil
        isConnected = false

        guard let data = UserDefaults.standard.data(forKey: Self.bookmarkKey) else {
            displayName = nil
            return
        }

        var isStale = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &isStale),
              url.startAccessingSecurityScopedResource() else {
            // Drive not currently reachable (unplugged, or the bookmark no longer resolves).
            // Deliberately not clearing the saved bookmark here -- this is very plausibly
            // transient (the drive just isn't plugged in right now), and forgetting the
            // location on every disconnect would make the user re-pick the folder every time.
            return
        }

        accessedURL = url
        isConnected = true
        displayName = url.lastPathComponent

        if isStale, let fresh = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            UserDefaults.standard.set(fresh, forKey: Self.bookmarkKey)
        }
    }

    /// Root directory for installed games on the external drive, or nil if none is
    /// configured/currently reachable. Callers don't need to (and shouldn't) create this --
    /// GameLibrary.importPkg creates the per-game subdirectory the same way it does under
    /// Application Support.
    var gamesRootURL: URL? {
        guard isConnected, let accessedURL else { return nil }
        return accessedURL.appendingPathComponent("AetherPS4Games", isDirectory: true)
    }
}
