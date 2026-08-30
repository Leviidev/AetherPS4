import SwiftUI

struct SettingsView: View {
    @AppStorage("showFpsCounter") private var showFpsCounter: Bool = true
    @AppStorage("networkEnabled") private var networkEnabled: Bool = true
    @AppStorage("consoleLoggingEnabled") private var consoleLoggingEnabled: Bool = false
    @AppStorage("touchControlsDisabled") private var touchControlsDisabled: Bool = false
    @AppStorage("performanceOverlayEnabled") private var performanceOverlayEnabled: Bool = false

    private let store = ConfigStore.shared

    // GPU / Rendering
    @State private var nullGpu = false
    @State private var dumpShaders = false
    @State private var readbacksMode = 0 // GpuReadbacksMode: Disabled/Relaxed/Precise
    @State private var directMemoryAccess = false
    @State private var vblankFrequency = 60
    @State private var fsrEnabled = false
    @State private var rcasEnabled = true
    @State private var rcasAttenuation = 250
    @State private var hdrAllowed = false

    // Debug
    @State private var debugDump = false
    @State private var shaderCollect = false

    // Vulkan
    @State private var vkValidationEnabled = false
    @State private var vkCrashDiagnosticEnabled = false
    @State private var renderdocEnabled = false
    @State private var pipelineCacheEnabled = false

    // Input
    @State private var motionControlsEnabled = true
    @State private var backgroundControllerInput = false
    @State private var useMiceAsMice = false

    // General
    @State private var neoMode = false
    @State private var devKitMode = false
    @State private var extraDmemMBytes = 0

    // Audio
    @State private var audioBackend = 0 // AudioBackend: SDL/OpenAL

    private var versionLabel: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
            as? String ?? "Unknown"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion")
            as? String ?? "Unknown"
        return "AetherPS4 \(version) (\(build))"
    }

    var body: some View {
        Form {
            Section("Display & Performance") {
                Toggle("Show In-Game FPS Counter", isOn: $showFpsCounter)
                Text("Displays the real-time frame rate, frame time, and resolution overlay inside the emulator window.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("Console Logging", isOn: $consoleLoggingEnabled)
                Text("Shows a live, scrolling console in the loading card. Off by default -- tailing the log file and re-rendering it several times a second noticeably lags the app, so it's only worth turning on when you actually need to see what's happening during boot.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("Performance Overlay", isOn: $performanceOverlayEnabled)
                Text("A small on-screen badge showing live FPS and CPU usage while a game is running. Takes effect the next time you start a game.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Network") {
                Toggle("Enable Network (ShadNet)", isOn: $networkEnabled)
                Text("Required for online games (Rocket League, etc.). Enables ShadNet P2P and system network access.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Toggle("PS4 Pro Mode (Neo)", isOn: $neoMode)
                    .onChange(of: neoMode) { _, v in store.setBool("General", "neo_mode", v) }
                Toggle("Dev Kit Mode", isOn: $devKitMode)
                    .onChange(of: devKitMode) { _, v in store.setBool("General", "dev_kit_mode", v) }
                Stepper("Extra Flexible Memory: \(extraDmemMBytes) MB", value: $extraDmemMBytes, in: 0...2048, step: 128)
                    .onChange(of: extraDmemMBytes) { _, v in store.setInt("General", "extra_dmem_in_mbytes", v) }
            } header: {
                Text("Console")
            } footer: {
                Text("PS4 Pro Mode reports the console as a Neo unit to games that support enhanced modes. Extra Flexible Memory raises the game's flexible-memory budget for titles that need more than the default allotment.")
            }

            Section {
                Toggle("Null GPU (Skip Rendering)", isOn: $nullGpu)
                    .onChange(of: nullGpu) { _, v in store.setBool("GPU", "null_gpu", v) }
                Toggle("Dump Shaders", isOn: $dumpShaders)
                    .onChange(of: dumpShaders) { _, v in store.setBool("GPU", "dump_shaders", v) }
                Picker("GPU Readbacks", selection: $readbacksMode) {
                    Text("Disabled").tag(0)
                    Text("Relaxed").tag(1)
                    Text("Precise").tag(2)
                }
                .onChange(of: readbacksMode) { _, v in store.setInt("GPU", "readbacks_mode", v) }
                Toggle("Direct Memory Access", isOn: $directMemoryAccess)
                    .onChange(of: directMemoryAccess) { _, v in store.setBool("GPU", "direct_memory_access_enabled", v) }
                Stepper("VBlank Frequency: \(vblankFrequency) Hz", value: $vblankFrequency, in: 30...240, step: 10)
                    .onChange(of: vblankFrequency) { _, v in store.setInt("GPU", "vblank_frequency", v) }
                Toggle("Allow HDR", isOn: $hdrAllowed)
                    .onChange(of: hdrAllowed) { _, v in store.setBool("GPU", "hdr_allowed", v) }
            } header: {
                Text("GPU & Rendering")
            } footer: {
                Text("Null GPU skips issuing real draw calls entirely -- useful for telling apart a CPU-side hang from a GPU-side one. Dump Shaders writes every compiled shader to disk for inspection. GPU Readbacks controls how eagerly the emulator copies GPU-written memory back for the CPU to read; Precise is the most correct but slowest.")
            }

            Section {
                Toggle("FSR Upscaling", isOn: $fsrEnabled)
                    .onChange(of: fsrEnabled) { _, v in store.setBool("GPU", "fsr_enabled", v) }
                Toggle("Sharpening (RCAS)", isOn: $rcasEnabled)
                    .onChange(of: rcasEnabled) { _, v in store.setBool("GPU", "rcas_enabled", v) }
                if rcasEnabled {
                    Stepper("Sharpening Strength: \(rcasAttenuation)", value: $rcasAttenuation, in: 0...1000, step: 25)
                        .onChange(of: rcasAttenuation) { _, v in store.setInt("GPU", "rcas_attenuation", v) }
                }
            } header: {
                Text("Upscaling")
            } footer: {
                Text("Lower sharpening strength values sharpen the image more.")
            }

            Section {
                Toggle("Vulkan Validation Layers", isOn: $vkValidationEnabled)
                    .onChange(of: vkValidationEnabled) { _, v in store.setBool("Vulkan", "vkvalidation_enabled", v) }
                Toggle("Vulkan Crash Diagnostics", isOn: $vkCrashDiagnosticEnabled)
                    .onChange(of: vkCrashDiagnosticEnabled) { _, v in store.setBool("Vulkan", "vkcrash_diagnostic_enabled", v) }
                Toggle("RenderDoc Capture Support", isOn: $renderdocEnabled)
                    .onChange(of: renderdocEnabled) { _, v in store.setBool("Vulkan", "renderdoc_enabled", v) }
                Toggle("Pipeline Cache", isOn: $pipelineCacheEnabled)
                    .onChange(of: pipelineCacheEnabled) { _, v in store.setBool("Vulkan", "pipeline_cache_enabled", v) }
            } header: {
                Text("Vulkan")
            } footer: {
                Text("Validation layers and crash diagnostics add real overhead -- only turn these on while investigating a specific problem, not for normal play.")
            }

            Section {
                Toggle("Debug Register Dump", isOn: $debugDump)
                    .onChange(of: debugDump) { _, v in store.setBool("Debug", "debug_dump", v) }
                Toggle("Collect Shaders", isOn: $shaderCollect)
                    .onChange(of: shaderCollect) { _, v in store.setBool("Debug", "shader_collect", v) }
            } header: {
                Text("Debug")
            } footer: {
                Text("Intended for reporting bugs, not everyday use -- these write extra diagnostic state to the crash log and slow the emulator down.")
            }

            Section("Audio") {
                Picker("Audio Backend", selection: $audioBackend) {
                    Text("SDL").tag(0)
                    Text("OpenAL").tag(1)
                }
                .onChange(of: audioBackend) { _, v in store.setInt("Audio", "audio_backend", v) }
            }

            Section {
                Toggle("Motion Controls", isOn: $motionControlsEnabled)
                    .onChange(of: motionControlsEnabled) { _, v in store.setBool("Input", "motion_controls_enabled", v) }
                Toggle("Background Controller Input", isOn: $backgroundControllerInput)
                    .onChange(of: backgroundControllerInput) { _, v in store.setBool("Input", "background_controller_input", v) }
                Toggle("Treat Mice as Mice", isOn: $useMiceAsMice)
                    .onChange(of: useMiceAsMice) { _, v in store.setBool("Input", "use_mice_as_mice", v) }
            } header: {
                Text("Input")
            } footer: {
                Text("\"Treat Mice as Mice\" reports a connected trackpad/mouse as a real mouse to the game instead of emulating a second gamepad with it.")
            }

            Section {
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
            } header: {
                Text("Touch Controls")
            } footer: {
                Text("Turning this off removes the on-screen stick/button overlay entirely -- only useful with an actual controller connected. Both settings take effect the next time you start a game.")
            }

            Section {
                EmptyView()
            } footer: {
                Text("Changes in the sections above are written to config.json immediately, but only take effect the next time the app is launched -- the engine only reads them once, at the first game you start each session.")
            }

            Section {
                Link(destination: URL(string: "https://discord.gg/xApMHWAzkh")!) {
                    Label("Join the Discord", systemImage: "bubble.left.and.bubble.right")
                }
            } header: {
                Text("Community")
            } footer: {
                Text(versionLabel)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("Version \(versionLabel)")
            }
        }
        .onAppear(perform: loadFromStore)
    }

    private func loadFromStore() {
        store.reload()

        nullGpu = store.bool("GPU", "null_gpu", default: false)
        dumpShaders = store.bool("GPU", "dump_shaders", default: false)
        readbacksMode = store.int("GPU", "readbacks_mode", default: 0)
        directMemoryAccess = store.bool("GPU", "direct_memory_access_enabled", default: false)
        vblankFrequency = store.int("GPU", "vblank_frequency", default: 60)
        fsrEnabled = store.bool("GPU", "fsr_enabled", default: false)
        rcasEnabled = store.bool("GPU", "rcas_enabled", default: true)
        rcasAttenuation = store.int("GPU", "rcas_attenuation", default: 250)
        hdrAllowed = store.bool("GPU", "hdr_allowed", default: false)

        debugDump = store.bool("Debug", "debug_dump", default: false)
        shaderCollect = store.bool("Debug", "shader_collect", default: false)

        vkValidationEnabled = store.bool("Vulkan", "vkvalidation_enabled", default: false)
        vkCrashDiagnosticEnabled = store.bool("Vulkan", "vkcrash_diagnostic_enabled", default: false)
        renderdocEnabled = store.bool("Vulkan", "renderdoc_enabled", default: false)
        pipelineCacheEnabled = store.bool("Vulkan", "pipeline_cache_enabled", default: false)

        motionControlsEnabled = store.bool("Input", "motion_controls_enabled", default: true)
        backgroundControllerInput = store.bool("Input", "background_controller_input", default: false)
        useMiceAsMice = store.bool("Input", "use_mice_as_mice", default: false)

        neoMode = store.bool("General", "neo_mode", default: false)
        devKitMode = store.bool("General", "dev_kit_mode", default: false)
        extraDmemMBytes = store.int("General", "extra_dmem_in_mbytes", default: 0)

        audioBackend = store.int("Audio", "audio_backend", default: 0)
    }
}
