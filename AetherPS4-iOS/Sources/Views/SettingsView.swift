import SwiftUI

struct SettingsView: View {
    @AppStorage("showFpsCounter") private var showFpsCounter: Bool = true
    @AppStorage("networkEnabled") private var networkEnabled: Bool = true
    @AppStorage("consoleLoggingEnabled") private var consoleLoggingEnabled: Bool = false

    private var versionLabel: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
            as? String ?? "Unknown"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion")
            as? String ?? "Unknown"
        return "AetherPS4 \(version) (\(build))"
    }

    var body: some View {
        Form {
            Section("Display & Performance") {
                Toggle("Show In-Game FPS Counter", isOn: $showFpsCounter)
                Text("Displays the real-time frame rate, frame time, and resolution overlay inside the emulator window.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("Console Logging", isOn: $consoleLoggingEnabled)
                Text("Shows a live, scrolling console in the loading card. Off by default -- tailing the log file and re-rendering it several times a second noticeably lags the app, so it's only worth turning on when you actually need to see what's happening during boot.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Network") {
                Toggle("Enable Network (ShadNet)", isOn: $networkEnabled)
                Text("Required for online games (Rocket League, etc.). Enables ShadNet P2P and system network access.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Link(destination: URL(string: "https://discord.gg/xApMHWAzkh")!) {
                    Label("Join the Discord", systemImage: "bubble.left.and.bubble.right")
                }
            } header: {
                Text("Community")
            } footer: {
                Text(versionLabel)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("Version \(versionLabel)")
            }
        }
    }
}
