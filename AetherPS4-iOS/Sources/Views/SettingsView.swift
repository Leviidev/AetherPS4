import SwiftUI

struct SettingsView: View {
    @AppStorage("showFpsCounter") private var showFpsCounter: Bool = true
    @AppStorage("networkEnabled") private var networkEnabled: Bool = true

    var body: some View {
        Form {
            Section("Display & Performance") {
                Toggle("Show In-Game FPS Counter", isOn: $showFpsCounter)
                Text("Displays the real-time frame rate, frame time, and resolution overlay inside the emulator window.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Network") {
                Toggle("Enable Network (ShadNet)", isOn: $networkEnabled)
                Text("Required for online games (Rocket League, etc.). Enables ShadNet P2P and system network access.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Community") {
                Link(destination: URL(string: "https://discord.gg/xApMHWAzkh")!) {
                    Label("Join the Discord", systemImage: "bubble.left.and.bubble.right")
                }
            }
        }
    }
}
