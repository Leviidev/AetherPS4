import SwiftUI

/// Top-level Settings screen: a segmented switcher over three focused sub-screens (Config,
/// Personalization, Socials) instead of one long scrolling Form -- the single-Form layout
/// mixed technical engine knobs, appearance/profile settings, and community links in one
/// undifferentiated list that kept growing as features were added.
struct SettingsView: View {
    private enum Tab: String, CaseIterable, Identifiable {
        case config = "Config"
        case personalization = "Personalization"
        case socials = "Socials"

        var id: String { rawValue }
    }

    @State private var selectedTab: Tab = .config

    var body: some View {
        VStack(spacing: 0) {
            Picker("Settings Section", selection: $selectedTab) {
                ForEach(Tab.allCases) { tab in
                    Text(tab.rawValue).tag(tab)
                }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal)
            .padding(.top, 8)
            .padding(.bottom, 4)

            switch selectedTab {
            case .config:
                SettingsConfigView()
            case .personalization:
                SettingsPersonalizationView()
            case .socials:
                SettingsSocialsView()
            }
        }
        .navigationTitle("Settings")
    }
}
