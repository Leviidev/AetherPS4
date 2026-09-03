import SwiftUI

struct SettingsGraphicsView: View {
    private let store = ConfigStore.shared

    // GPU / Rendering
    @State private var nullGpu = false
    @State private var dumpShaders = false
    @State private var readbacksMode = 0 // GpuReadbacksMode: Disabled/Relaxed/Precise
    @State private var directMemoryAccess = false
    @State private var vblankFrequency = 60
    @State private var hdrAllowed = false

    // Upscaling
    @State private var fsrEnabled = false
    @State private var rcasEnabled = true
    @State private var rcasAttenuation = 250

    // Vulkan
    @State private var vkValidationEnabled = false
    @State private var vkCrashDiagnosticEnabled = false
    @State private var renderdocEnabled = false
    @State private var pipelineCacheEnabled = false

    var body: some View {
        Form {
            Section("GPU & Rendering") {
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
            }

            Section("Upscaling") {
                Toggle("FSR Upscaling", isOn: $fsrEnabled)
                    .onChange(of: fsrEnabled) { _, v in store.setBool("GPU", "fsr_enabled", v) }
                Toggle("Sharpening (RCAS)", isOn: $rcasEnabled)
                    .onChange(of: rcasEnabled) { _, v in store.setBool("GPU", "rcas_enabled", v) }
                if rcasEnabled {
                    Stepper("Sharpening Strength: \(rcasAttenuation)", value: $rcasAttenuation, in: 0...1000, step: 25)
                        .onChange(of: rcasAttenuation) { _, v in store.setInt("GPU", "rcas_attenuation", v) }
                    Text("Lower values sharpen the image more.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Section("Vulkan") {
                Toggle("Vulkan Validation Layers", isOn: $vkValidationEnabled)
                    .onChange(of: vkValidationEnabled) { _, v in store.setBool("Vulkan", "vkvalidation_enabled", v) }
                Toggle("Vulkan Crash Diagnostics", isOn: $vkCrashDiagnosticEnabled)
                    .onChange(of: vkCrashDiagnosticEnabled) { _, v in store.setBool("Vulkan", "vkcrash_diagnostic_enabled", v) }
                Toggle("RenderDoc Capture Support", isOn: $renderdocEnabled)
                    .onChange(of: renderdocEnabled) { _, v in store.setBool("Vulkan", "renderdoc_enabled", v) }
                Toggle("Pipeline Cache", isOn: $pipelineCacheEnabled)
                    .onChange(of: pipelineCacheEnabled) { _, v in store.setBool("Vulkan", "pipeline_cache_enabled", v) }
            }
        }
        .navigationTitle("Graphics")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: loadFromStore)
    }

    private func loadFromStore() {
        store.reload()
        nullGpu = store.bool("GPU", "null_gpu", default: false)
        dumpShaders = store.bool("GPU", "dump_shaders", default: false)
        readbacksMode = store.int("GPU", "readbacks_mode", default: 0)
        directMemoryAccess = store.bool("GPU", "direct_memory_access_enabled", default: false)
        vblankFrequency = store.int("GPU", "vblank_frequency", default: 60)
        hdrAllowed = store.bool("GPU", "hdr_allowed", default: false)

        fsrEnabled = store.bool("GPU", "fsr_enabled", default: false)
        rcasEnabled = store.bool("GPU", "rcas_enabled", default: true)
        rcasAttenuation = store.int("GPU", "rcas_attenuation", default: 250)

        vkValidationEnabled = store.bool("Vulkan", "vkvalidation_enabled", default: false)
        vkCrashDiagnosticEnabled = store.bool("Vulkan", "vkcrash_diagnostic_enabled", default: false)
        renderdocEnabled = store.bool("Vulkan", "renderdoc_enabled", default: false)
        pipelineCacheEnabled = store.bool("Vulkan", "pipeline_cache_enabled", default: false)
    }
}
