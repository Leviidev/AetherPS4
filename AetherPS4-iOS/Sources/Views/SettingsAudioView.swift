import SwiftUI

struct SettingsAudioView: View {
    private let store = ConfigStore.shared

    @State private var audioBackend = 0 // AudioBackend: SDL/OpenAL
    @State private var audioOutputDisabled = false

    var body: some View {
        Form {
            Section {
                Picker("Audio Backend", selection: $audioBackend) {
                    Text("SDL").tag(0)
                    Text("OpenAL").tag(1)
                }
                .onChange(of: audioBackend) { _, v in store.setInt("Audio", "audio_backend", v) }
            }
            Section {
                Toggle("Disable Audio Output", isOn: $audioOutputDisabled)
                    .onChange(of: audioOutputDisabled) { _, v in
                        store.setBool("Audio", "disable_audio_output", v)
                    }
            } footer: {
                Text("Diagnostic only: skips opening a real audio device entirely, so the game runs silently. Used to test whether the game's own audio is interfering with StikDebug's background JIT connection.")
            }
        }
        .navigationTitle("Audio")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            store.reload()
            audioBackend = store.int("Audio", "audio_backend", default: 0)
            audioOutputDisabled = store.bool("Audio", "disable_audio_output", default: false)
        }
    }
}
