import SwiftUI
import UniformTypeIdentifiers

/// System module import: lets the user supply real, dumped PS4 system modules (.sprx files --
/// Sony's own code, which shadPS4 can't ship and falls back to its own, incomplete HLE
/// reimplementations of without them) as a ZIP, extracted directly into the directory the
/// engine already scans before falling back to HLE.
struct SettingsSystemModulesView: View {
    @State private var isSysModulesImporterPresented = false
    @State private var sysModulesImportMessage: String?

    var body: some View {
        Form {
            Section {
                Button("Import System Modules…") {
                    isSysModulesImporterPresented = true
                }
                if let sysModulesImportMessage {
                    Text(sysModulesImportMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    Text("Some games need real PS4 system modules (.sprx files) dumped from your own console -- import a ZIP of them here.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("System Modules")
        .navigationBarTitleDisplayMode(.inline)
        .fileImporter(
            isPresented: $isSysModulesImporterPresented,
            allowedContentTypes: [.zip],
            allowsMultipleSelection: false
        ) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            importSysModulesZip(from: url)
        }
    }

    private func importSysModulesZip(from sourceURL: URL) {
        let didStartAccess = sourceURL.startAccessingSecurityScopedResource()
        defer { if didStartAccess { sourceURL.stopAccessingSecurityScopedResource() } }

        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        else {
            sysModulesImportMessage = "Failed: could not resolve the app's Documents directory."
            return
        }
        // Matches EmulatorSettings::GetSysModulesDir()'s default (Common::FS::PathType::
        // SysModuleDir, path_util.cpp) -- user_dir/sys_modules, with user_dir set to this
        // same Documents directory at shadps4_init (EmulatorProcess.swift).
        let destDir = documents.appendingPathComponent("sys_modules", isDirectory: true)

        var result = BachataSysModulesImportResult()
        let status = sourceURL.path.withCString { zipPathPtr in
            destDir.path.withCString { destPathPtr in
                bachata_sysmodules_import_zip(zipPathPtr, destPathPtr, &result)
            }
        }
        let message = withUnsafeBytes(of: result.message) { raw in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }

        if status == 0 {
            sysModulesImportMessage = "Imported \(result.files_extracted) file(s) into sys_modules."
        } else {
            sysModulesImportMessage = message.isEmpty ? "Import failed." : "Failed: \(message)"
        }
    }
}
