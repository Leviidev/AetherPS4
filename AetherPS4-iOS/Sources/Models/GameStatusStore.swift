import Foundation
import Observation

/// One game's compatibility report, sourced 1:1 from a GitHub issue in
/// c4ndyf1sh/aetherPS4-Compatibility-List -- that repo tracks one issue per game, titled with
/// the game's name, with a single compatibility-tier label attached (see `CompatibilityTier`).
struct GameStatus: Identifiable, Codable, Hashable {
    let id: Int
    let title: String
    let state: String
    let htmlURL: URL
    let updatedAt: Date
    let labels: [Label]

    struct Label: Codable, Hashable {
        let name: String
        let color: String
    }

    enum CodingKeys: String, CodingKey {
        case id, title, state, labels
        case htmlURL = "html_url"
        case updatedAt = "updated_at"
    }

    /// The repo's five compatibility labels, worst to best. Falls back to `.untested` when an
    /// issue hasn't been triaged with one of them yet (label lists don't include "Untested" as
    /// a real GitHub label -- it's this app's own placeholder for "no tier label present").
    var tier: CompatibilityTier {
        for label in labels {
            if let tier = CompatibilityTier(labelName: label.name) {
                return tier
            }
        }
        return .untested
    }
}

/// Matches the repo's actual label names/colors (fetched from the labels API) so the tier badge
/// in-app looks like the label on GitHub.
enum CompatibilityTier: String, CaseIterable {
    case playable = "Playable"
    case ingame = "Ingame"
    case menus = "Menus"
    case boots = "Boots"
    case nothing = "Nothing"
    case untested = "Untested"

    init?(labelName: String) {
        guard let match = CompatibilityTier.allCases.first(where: { $0.rawValue == labelName }) else {
            return nil
        }
        self = match
    }

    var colorHex: String {
        switch self {
        case .playable: "1ecc0c"
        case .ingame: "30b5c0"
        case .menus: "ebe54d"
        case .boots: "c7432e"
        case .nothing: "465339"
        case .untested: "8e8e93"
        }
    }

    var summary: String {
        switch self {
        case .playable: "Can be played without any major issue."
        case .ingame: "Can reach gameplay but has game-breaking issues."
        case .menus: "Can reach the menu but freezes/crashes when trying to proceed further."
        case .boots: "Shows visual or audio output but freezes or crashes before reaching the menu."
        case .nothing: "Crashes when trying to launch, or hangs on a black screen."
        case .untested: "Not yet triaged in the compatibility list."
        }
    }

    /// Best-to-worst ordering for sorting the list.
    var sortRank: Int {
        switch self {
        case .playable: 0
        case .ingame: 1
        case .menus: 2
        case .boots: 3
        case .nothing: 4
        case .untested: 5
        }
    }
}

/// Fetches and caches the community compatibility list from
/// github.com/c4ndyf1sh/aetherPS4-Compatibility-List's issues. Unauthenticated GitHub API calls
/// are capped at 60/hour per IP, so results are cached to disk and only refetched when stale
/// (or when the user explicitly pulls to refresh) rather than on every tab visit.
@Observable
final class GameStatusStore {
    static let shared = GameStatusStore()

    private static let apiURL = URL(
        string: "https://api.github.com/repos/c4ndyf1sh/aetherPS4-Compatibility-List/issues?per_page=100&state=all")!
    private static let cacheURL = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("game_status_cache.json")
    private static let minRefreshInterval: TimeInterval = 15 * 60

    private(set) var games: [GameStatus] = []
    private(set) var lastUpdated: Date?
    private(set) var isLoading = false
    private(set) var errorMessage: String?

    private init() {
        loadCache()
    }

    /// Fetches fresh data unless the cache is still recent and `force` isn't set (pull-to-refresh
    /// passes `force: true`).
    func refreshIfNeeded(force: Bool = false) async {
        if !force, let lastUpdated, Date().timeIntervalSince(lastUpdated) < Self.minRefreshInterval {
            return
        }
        await refresh()
    }

    private func refresh() async {
        isLoading = true
        errorMessage = nil
        defer { isLoading = false }

        do {
            var allIssues: [GameStatus] = []
            var nextURL: URL? = Self.apiURL
            let decoder = JSONDecoder()
            decoder.dateDecodingStrategy = .iso8601

            while let url = nextURL {
                let (data, response) = try await URLSession.shared.data(from: url)
                let issues = try decoder.decode([GameStatus].self, from: data)
                allIssues.append(contentsOf: issues)
                nextURL = Self.nextPageURL(from: response)
            }

            games = allIssues.sorted {
                $0.tier.sortRank == $1.tier.sortRank
                    ? $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending
                    : $0.tier.sortRank < $1.tier.sortRank
            }
            lastUpdated = Date()
            saveCache()
        } catch {
            errorMessage = games.isEmpty
                ? "Couldn't load the compatibility list. Check your connection and try again."
                : "Couldn't refresh (showing last known results): \(error.localizedDescription)"
        }
    }

    /// GitHub paginates via an RFC 5988 `Link` header rather than a body field.
    private static func nextPageURL(from response: URLResponse) -> URL? {
        guard let http = response as? HTTPURLResponse, let link = http.value(forHTTPHeaderField: "Link") else {
            return nil
        }
        for part in link.split(separator: ",") {
            let segments = part.split(separator: ";").map { $0.trimmingCharacters(in: .whitespaces) }
            guard segments.count == 2, segments[1] == "rel=\"next\"" else { continue }
            let urlString = segments[0].trimmingCharacters(in: CharacterSet(charactersIn: "<>"))
            return URL(string: urlString)
        }
        return nil
    }

    private struct Cache: Codable {
        let games: [GameStatus]
        let lastUpdated: Date
    }

    private func loadCache() {
        guard let data = try? Data(contentsOf: Self.cacheURL) else { return }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        guard let cache = try? decoder.decode(Cache.self, from: data) else { return }
        games = cache.games
        lastUpdated = cache.lastUpdated
    }

    private func saveCache() {
        guard let lastUpdated else { return }
        let cache = Cache(games: games, lastUpdated: lastUpdated)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        guard let data = try? encoder.encode(cache) else { return }
        try? data.write(to: Self.cacheURL, options: .atomic)
    }
}
