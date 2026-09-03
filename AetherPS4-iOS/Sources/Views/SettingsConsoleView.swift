import SwiftUI

struct SettingsConsoleView: View {
    private let store = ConfigStore.shared

    @State private var neoMode = false
    @State private var devKitMode = false
    @State private var extraDmemMBytes = 0
    @State private var showSplashScreen = false

    var body: some View {
        Form {
            Section {
                Toggle("PS4 Pro Mode (Neo)", isOn: $neoMode)
                    .onChange(of: neoMode) { _, v in store.setBool("General", "neo_mode", v) }
                Toggle("Dev Kit Mode", isOn: $devKitMode)
                    .onChange(of: devKitMode) { _, v in store.setBool("General", "dev_kit_mode", v) }
                Stepper("Extra Flexible Memory: \(extraDmemMBytes) MB", value: $extraDmemMBytes, in: 0...2048, step: 128)
                    .onChange(of: extraDmemMBytes) { _, v in store.setInt("General", "extra_dmem_in_mbytes", v) }
                Toggle("Show Splash Screen", isOn: $showSplashScreen)
                    .onChange(of: showSplashScreen) { _, v in store.setBool("General", "show_splash", v) }
            }
        }
        .navigationTitle("Console")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: loadFromStore)
    }

    private func loadFromStore() {
        store.reload()
        neoMode = store.bool("General", "neo_mode", default: false)
        devKitMode = store.bool("General", "dev_kit_mode", default: false)
        extraDmemMBytes = store.int("General", "extra_dmem_in_mbytes", default: 0)
        showSplashScreen = store.bool("General", "show_splash", default: false)
    }
}
