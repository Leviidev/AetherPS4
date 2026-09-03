import SwiftUI

struct SettingsDisplayPerformanceView: View {
    @AppStorage("showFpsCounter") private var showFpsCounter: Bool = true
    @AppStorage("consoleLoggingEnabled") private var consoleLoggingEnabled: Bool = false
    @AppStorage("performanceOverlayEnabled") private var performanceOverlayEnabled: Bool = false

    var body: some View {
        Form {
            Section {
                Toggle("Show In-Game FPS Counter", isOn: $showFpsCounter)
                Toggle("Console Logging", isOn: $consoleLoggingEnabled)
                Toggle("Performance Overlay", isOn: $performanceOverlayEnabled)
            }
        }
        .navigationTitle("Display & Performance")
        .navigationBarTitleDisplayMode(.inline)
    }
}
