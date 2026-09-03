import SwiftUI

/// Community links and app version -- split out from the old single Settings Form so it
/// isn't buried at the bottom of a long list of unrelated engine toggles.
struct SettingsSocialsView: View {
    private var versionLabel: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
            as? String ?? "Unknown"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion")
            as? String ?? "Unknown"
        return "AetherPS4 \(version) (\(build))"
    }

    var body: some View {
        Form {
            Section("Community") {
                Link(destination: URL(string: "https://discord.gg/xApMHWAzkh")!) {
                    Label("Join the Discord", systemImage: "bubble.left.and.bubble.right")
                }
            }

            Section {
                Text(versionLabel)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("Version \(versionLabel)")
            }
        }
    }
}
