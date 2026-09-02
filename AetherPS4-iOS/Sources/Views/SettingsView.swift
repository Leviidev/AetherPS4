import SwiftUI
import UniformTypeIdentifiers

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
    @State private var showSplashScreen = false

    // Audio
    @State private var audioBackend = 0 // AudioBackend: SDL/OpenAL

    // Debug export: installed games live under Application Support, which Files app doesn't
    // expose (UIFileSharingEnabled only surfaces Documents) -- this copies a chosen game's
    // eboot.bin into Documents so it becomes reachable for troubleshooting. Temporary debug
    // tool, not meant to stay long-term.
    @State private var installedGameDirs: [String] = []
    @State private var ebootExportMessage: String?

    // System module import: lets the user supply real, dumped PS4 system modules (.sprx
    // files -- Sony's own code, which shadPS4 can't ship and falls back to its own,
    // incomplete HLE reimplementations of without them) as a ZIP, extracted directly into
    // the directory the engine already scans before falling back to HLE.
    @State private var isSysModulesImporterPresented = false
    @State private var sysModulesImportMessage: String?

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
                Toggle("Show Splash Screen", isOn: $showSplashScreen)
                    .onChange(of: showSplashScreen) { _, v in store.setBool("General", "show_splash", v) }
            } header: {
                Text("Console")
            } footer: {
                Text("PS4 Pro Mode reports the console as a Neo unit to games that support enhanced modes. Extra Flexible Memory raises the game's flexible-memory budget for titles that need more than the default allotment. Show Splash Screen displays the game's own branding image while it loads, matching real PS4 hardware, before the game hides it itself.")
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
            } header: {
                Text("Debug: Export Game Executable")
            } footer: {
                Text("Copies the selected installed game's eboot.bin into the Files app (Documents) for troubleshooting a crash report.")
            }

            Section {
                Button("Import System Modules…") {
                    isSysModulesImporterPresented = true
                }
                if let sysModulesImportMessage {
                    Text(sysModulesImportMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } header: {
                Text("System Modules")
            } footer: {
                Text("Some games depend on real PS4 system modules (.sprx files) that AetherPS4 can't include -- they must be dumped from your own, legally owned PlayStation 4 console. Import a ZIP of those files here to place them where the emulator looks for them.")
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
        .onAppear {
            loadFromStore()
            loadInstalledGameDirs()
        }
        .fileImporter(
            isPresented: $isSysModulesImporterPresented,
            allowedContentTypes: [.zip],
            allowsMultipleSelection: false
        ) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            importSysModulesZip(from: url)
        }
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
        showSplashScreen = store.bool("General", "show_splash", default: false)

        audioBackend = store.int("Audio", "audio_backend", default: 0)
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
