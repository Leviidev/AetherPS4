import SwiftUI

struct SettingsNetworkView: View {
    @AppStorage("networkEnabled") private var networkEnabled: Bool = true

    var body: some View {
        Form {
            Section {
                Toggle("Enable Network (ShadNet)", isOn: $networkEnabled)
                Text("Required for online games (Rocket League, etc.).")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Network")
        .navigationBarTitleDisplayMode(.inline)
    }
}
