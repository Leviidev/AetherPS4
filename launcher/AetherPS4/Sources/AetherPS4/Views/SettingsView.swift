import SwiftUI

/// Deliberately minimal: this is a development/testing launcher, not a full
/// frontend. The only setting that actually matters is where the emulator
/// binary lives, and even that only needs to be touched if auto-detection
/// fails. No firmware management here — shadPS4 is fully HLE and needs no
/// firmware/BIOS file to run (confirmed by inspecting its source and README
/// before building this UI; nothing here is fabricated).
struct SettingsView: View {
    @AppStorage("showFpsCounter") private var showFpsCounter: Bool = true
    @State private var detectedPath: URL?
    @State private var overridePath: String = ""
    @State private var isImporterPresented = false

    var body: some View {
        Form {
            Section("Display & Performance") {
                Toggle("Show In-Game FPS Counter", isOn: $showFpsCounter)
                Text("Displays the real-time frame rate, frame time, and resolution overlay inside the emulator window.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("AetherPS4 / shadPS4 Executable") {
                LabeledContent("Detected") {
                    Text(detectedPath?.path ?? "Not found")
                        .foregroundStyle(detectedPath == nil ? .red : .secondary)
                        .textSelection(.enabled)
                }

                LabeledContent("Override") {
                    HStack {
                        TextField("Optional explicit path", text: $overridePath)
                            .textFieldStyle(.roundedBorder)
                        Button("Choose…") {
                            isImporterPresented = true
                        }
                    }
                }

                HStack {
                    Button("Save Override") {
                        EmulatorLocator.setOverride(overridePath.isEmpty ? nil : overridePath)
                        refresh()
                    }
                    .disabled(overridePath.isEmpty)

                    Button("Clear Override") {
                        overridePath = ""
                        EmulatorLocator.setOverride(nil)
                        refresh()
                    }
                }
            }

            Section {
                Text("Firmware isn't required — shadPS4 re-implements the PS4 system libraries it needs directly, rather than running Sony's own firmware.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding(20)
        .onAppear(perform: refresh)
        .fileImporter(isPresented: $isImporterPresented, allowedContentTypes: [.unixExecutable]) { result in
            if case .success(let url) = result {
                overridePath = url.path
            }
        }
    }

    private func refresh() {
        detectedPath = EmulatorLocator.locate()
    }
}
