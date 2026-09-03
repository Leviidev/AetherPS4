import SwiftUI

struct SettingsInputView: View {
    @AppStorage("touchControlsDisabled") private var touchControlsDisabled: Bool = false

    private let store = ConfigStore.shared

    @State private var motionControlsEnabled = true
    @State private var backgroundControllerInput = false
    @State private var useMiceAsMice = false

    var body: some View {
        Form {
            Section("Input") {
                Toggle("Motion Controls", isOn: $motionControlsEnabled)
                    .onChange(of: motionControlsEnabled) { _, v in store.setBool("Input", "motion_controls_enabled", v) }
                Toggle("Background Controller Input", isOn: $backgroundControllerInput)
                    .onChange(of: backgroundControllerInput) { _, v in store.setBool("Input", "background_controller_input", v) }
                Toggle("Treat Mice as Mice", isOn: $useMiceAsMice)
                    .onChange(of: useMiceAsMice) { _, v in store.setBool("Input", "use_mice_as_mice", v) }
                Text("Reports a connected trackpad/mouse as a real mouse instead of a second gamepad.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Touch Controls") {
                Toggle("Show Touch Controls", isOn: Binding(
                    get: { !touchControlsDisabled },
                    set: { touchControlsDisabled = !$0 }
                ))
                NavigationLink("Customize Touch Control Layout") {
                    TouchControlsLayoutEditorView()
                }
                Button("Reset Touch Control Layout", role: .destructive) {
                    TouchLayoutStore.shared.resetAll()
                }
            }
        }
        .navigationTitle("Input & Touch Controls")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: loadFromStore)
    }

    private func loadFromStore() {
        store.reload()
        motionControlsEnabled = store.bool("Input", "motion_controls_enabled", default: true)
        backgroundControllerInput = store.bool("Input", "background_controller_input", default: false)
        useMiceAsMice = store.bool("Input", "use_mice_as_mice", default: false)
    }
}
