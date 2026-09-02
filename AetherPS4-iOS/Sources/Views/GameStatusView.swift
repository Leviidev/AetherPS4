import SwiftUI

/// Community game-compatibility list, sourced live from
/// github.com/c4ndyf1sh/aetherPS4-Compatibility-List's issues (one issue per game, labeled with
/// a compatibility tier). Read-only: tapping a game opens its GitHub issue for the full report.
struct GameStatusView: View {
    private var store = GameStatusStore.shared

    @State private var searchText = ""
    @State private var selectedTiers: Set<CompatibilityTier> = []

    private var filteredGames: [GameStatus] {
        store.games.filter { game in
            let matchesSearch = searchText.isEmpty
                || game.title.localizedCaseInsensitiveContains(searchText)
            let matchesTier = selectedTiers.isEmpty || selectedTiers.contains(game.tier)
            return matchesSearch && matchesTier
        }
    }

    var body: some View {
        Group {
            if store.games.isEmpty && store.isLoading {
                ProgressView("Loading compatibility list…")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if store.games.isEmpty, let errorMessage = store.errorMessage {
                ContentUnavailableView {
                    Label("Can't Load Game Status", systemImage: "wifi.slash")
                } description: {
                    Text(errorMessage)
                }
            } else {
                List {
                    if let errorMessage = store.errorMessage {
                        Section {
                            Label(errorMessage, systemImage: "exclamationmark.triangle")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    Section {
                        ForEach(filteredGames) { game in
                            Link(destination: game.htmlURL) {
                                GameStatusRow(game: game)
                            }
                            .tint(.primary)
                        }
                    } footer: {
                        if let lastUpdated = store.lastUpdated {
                            Text("Updated \(lastUpdated.formatted(.relative(presentation: .named)))")
                        }
                    }
                }
                .listStyle(.insetGrouped)
            }
        }
        .navigationTitle("Game Status")
        .searchable(text: $searchText, prompt: "Search games")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    ForEach(CompatibilityTier.allCases, id: \.self) { tier in
                        Toggle(tier.rawValue, isOn: Binding(
                            get: { selectedTiers.contains(tier) },
                            set: { isOn in
                                if isOn { selectedTiers.insert(tier) } else { selectedTiers.remove(tier) }
                            }
                        ))
                    }
                    if !selectedTiers.isEmpty {
                        Button("Clear Filters", role: .destructive) { selectedTiers.removeAll() }
                    }
                } label: {
                    Label(
                        "Filter", systemImage: selectedTiers.isEmpty
                            ? "line.3.horizontal.decrease.circle" : "line.3.horizontal.decrease.circle.fill")
                }
            }
        }
        .refreshable {
            await store.refreshIfNeeded(force: true)
        }
        .task {
            await store.refreshIfNeeded()
        }
    }
}

private struct GameStatusRow: View {
    let game: GameStatus

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text(game.title)
                    .font(.body)
                Text(game.tier.summary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer()
            Text(game.tier.rawValue)
                .font(.caption.weight(.semibold))
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(Color(hex: game.tier.colorHex).opacity(0.2))
                .foregroundStyle(Color(hex: game.tier.colorHex))
                .clipShape(Capsule())
        }
        .padding(.vertical, 2)
    }
}

private extension Color {
    /// Parses a bare 6-digit hex string (no leading `#`), matching the format GitHub's labels API
    /// returns (e.g. "1ecc0c"). Falls back to gray for anything malformed.
    init(hex: String) {
        var value: UInt64 = 0
        guard hex.count == 6, Scanner(string: hex).scanHexInt64(&value) else {
            self = .gray
            return
        }
        self.init(
            red: Double((value >> 16) & 0xFF) / 255,
            green: Double((value >> 8) & 0xFF) / 255,
            blue: Double(value & 0xFF) / 255
        )
    }
}
