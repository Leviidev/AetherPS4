import SwiftUI

struct SettingsAdvancedView: View {
    private let store = ConfigStore.shared

    @State private var debugDump = false
    @State private var shaderCollect = false

    // Debug export: installed games live under Application Support, which Files app doesn't
    // expose (UIFileSharingEnabled only surfaces Documents) -- this copies a chosen game's
    // eboot.bin into Documents so it becomes reachable for troubleshooting. Temporary debug
    // tool, not meant to stay long-term.
    @State private var installedGameDirs: [String] = []
    @State private var ebootExportMessage: String?

    var body: some View {
        Form {
            Section("Debug") {
                Toggle("Debug Register Dump", isOn: $debugDump)
                    .onChange(of: debugDump) { _, v in store.setBool("Debug", "debug_dump", v) }
                Toggle("Collect Shaders", isOn: $shaderCollect)
                    .onChange(of: shaderCollect) { _, v in store.setBool("Debug", "shader_collect", v) }
            }

            Section("Export Game Executable") {
                if installedGameDirs.isEmpty {
                    Text("No installed games found.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(installedGameDirs, id: \.self) { contentId in
                        Button(contentId) {
                            exportEboot(for: contentId)
                        }
                    }
                }
                if let ebootExportMessage {
                    Text(ebootExportMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("Advanced")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            store.reload()
            debugDump = store.bool("Debug", "debug_dump", default: false)
            shaderCollect = store.bool("Debug", "shader_collect", default: false)
            loadInstalledGameDirs()
        }
    }

    private func loadInstalledGameDirs() {
        let fm = FileManager.default
        guard let appSupport = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
            installedGameDirs = []
            return
        }
        let gamesDir = appSupport.appendingPathComponent("AetherPS4/games")
        guard let entries = try? fm.contentsOfDirectory(at: gamesDir, includingPropertiesForKeys: nil) else {
            installedGameDirs = []
            return
        }
        installedGameDirs = entries
            .filter { fm.fileExists(atPath: $0.appendingPathComponent("eboot.bin").path) }
            .map(\.lastPathComponent)
            .sorted()
    }

    private func exportEboot(for contentId: String) {
        let fm = FileManager.default
        guard let appSupport = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first,
              let documents = fm.urls(for: .documentDirectory, in: .userDomainMask).first
        else {
            ebootExportMessage = "Failed: could not resolve app directories."
            return
        }
        let source = appSupport.appendingPathComponent("AetherPS4/games/\(contentId)/eboot.bin")
        let destination = documents.appendingPathComponent("\(contentId)_eboot.bin")
        do {
            if fm.fileExists(atPath: destination.path) {
                try fm.removeItem(at: destination)
            }
            try fm.copyItem(at: source, to: destination)
            ebootExportMessage = "Exported to Files app as \(destination.lastPathComponent)"
        } catch {
            ebootExportMessage = "Failed: \(error.localizedDescription)"
        }
    }
}
