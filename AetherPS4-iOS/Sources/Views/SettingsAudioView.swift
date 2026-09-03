import SwiftUI

struct SettingsAudioView: View {
    private let store = ConfigStore.shared

    @State private var audioBackend = 0 // AudioBackend: SDL/OpenAL

    var body: some View {
        Form {
            Section {
                Picker("Audio Backend", selection: $audioBackend) {
                    Text("SDL").tag(0)
                    Text("OpenAL").tag(1)
                }
                .onChange(of: audioBackend) { _, v in store.setInt("Audio", "audio_backend", v) }
            }
        }
        .navigationTitle("Audio")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            store.reload()
            audioBackend = store.int("Audio", "audio_backend", default: 0)
        }
    }
}
