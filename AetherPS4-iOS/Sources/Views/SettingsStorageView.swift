import SwiftUI

/// Lets the user point new game installs at an external drive instead of this app's own
/// internal container -- a PS4 game's installed (decompressed) size commonly runs 2-3x its
/// package size, easily 80GB+ for a modern title, which internal storage alone often can't
/// spare. See ExternalStorageStore's own comment for why this needs a persisted
/// security-scoped bookmark rather than just a remembered path.
struct SettingsStorageView: View {
    @Bindable private var externalStorage = ExternalStorageStore.shared
    @State private var isFolderPickerPresented = false
    @State private var showForgetConfirmation = false

    var body: some View {
        Form {
            Section {
                if externalStorage.isConnected {
                    Label {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(externalStorage.displayName ?? "External Drive")
                            Text("Connected")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    } icon: {
                        Image(systemName: "externaldrive.fill")
                            .foregroundStyle(.green)
                    }
                } else if externalStorage.displayName != nil {
                    Label {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(externalStorage.displayName ?? "External Drive")
                            Text("Not currently reachable -- check it's plugged in")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    } icon: {
                        Image(systemName: "externaldrive.trianglebadge.exclamationmark")
                            .foregroundStyle(.orange)
                    }
                } else {
                    Label("No external drive configured", systemImage: "externaldrive")
                        .foregroundStyle(.secondary)
                }

                Button(externalStorage.displayName == nil ? "Choose External Drive…" : "Change External Drive…") {
                    isFolderPickerPresented = true
                }

                if externalStorage.displayName != nil {
                    Button("Check Connection") {
                        externalStorage.refresh()
                    }
                    Button("Forget This Drive", role: .destructive) {
                        showForgetConfirmation = true
                    }
                }
            } footer: {
                Text(externalStorage.isConnected
                     ? "New games install here automatically instead of internal storage. Games already installed internally are unaffected."
                     : "Pick a folder on a drive connected to this device (over USB-C, or anything else that shows up in the file picker). New installs will go there automatically once connected.")
            }
        }
        .navigationTitle("Storage")
        .navigationBarTitleDisplayMode(.inline)
        .fileImporter(
            isPresented: $isFolderPickerPresented,
            allowedContentTypes: [.folder]
        ) { result in
            guard case .success(let url) = result else { return }
            externalStorage.chooseFolder(url)
        }
        .confirmationDialog(
            "Forget this drive?",
            isPresented: $showForgetConfirmation,
            titleVisibility: .visible
        ) {
            Button("Forget", role: .destructive) {
                externalStorage.forget()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("New games will go back to internal storage. Games already installed on the drive are not deleted -- they'll just show as unavailable until you choose it again.")
        }
    }
}
