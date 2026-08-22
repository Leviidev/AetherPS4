import SwiftUI

struct SettingsView: View {
    @AppStorage("showFpsCounter") private var showFpsCounter: Bool = true

    var body: some View {
        Form {
            Section("Display & Performance") {
                Toggle("Show In-Game FPS Counter", isOn: $showFpsCounter)
                Text("Displays the real-time frame rate, frame time, and resolution overlay inside the emulator window.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Text("Firmware isn't required — shadPS4 re-implements the PS4 system libraries it needs directly, rather than running Sony's own firmware.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
