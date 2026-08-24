import SwiftUI

// Blocking launch-time check: confirms the two preconditions the emulator actually
// depends on before letting the user into the library --
//   1. The increased-memory-limit entitlement is present in this build's signature.
//   2. StikDebug is attached AND its JIT script is actually servicing BRK requests.
//
// (2) is checked in two steps, not one straight call into shadps4_probe_jit(): that
// probe triggers a real BRK trap, which raises a raw, unhandled SIGTRAP and kills the
// process outright if no debugger happens to be attached to service it (confirmed
// on-device -- see shadps4_probe_jit's own doc comment in shadps4_ios_api.h). checkDebugged()
// only inspects this process's own codesign/csops state and never touches the BRK path,
// so it's always safe to call first as a gate.
struct SetupCheckView: View {
    private enum CheckState {
        case checking
        case passed
        case failed(memoryOk: Bool, jitOk: Bool)
    }

    let onPassed: () -> Void

    @State private var state: CheckState = .checking
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        ZStack {
            Color(.systemBackground).ignoresSafeArea()
            content
                .padding()
        }
        .onAppear { runChecks() }
        .onChange(of: scenePhase) { _, newPhase in
            if newPhase == .active, case .failed = state {
                runChecks()
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        switch state {
        case .checking:
            VStack(spacing: 16) {
                ProgressView()
                Text("Verifying setup…")
                    .foregroundStyle(.secondary)
            }
        case .passed:
            VStack(spacing: 16) {
                Image(systemName: "checkmark.circle.fill")
                    .font(.system(size: 48))
                    .foregroundStyle(.green)
                Text("Setup verified")
                    .font(.headline)
            }
        case .failed(let memoryOk, let jitOk):
            VStack(spacing: 20) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .font(.system(size: 44))
                    .foregroundStyle(.yellow)

                Text("Setup Check Failed")
                    .font(.title2.bold())

                VStack(alignment: .leading, spacing: 12) {
                    checkRow(title: "Memory Entitlement", ok: memoryOk)
                    checkRow(title: "JIT Script (StikDebug)", ok: jitOk)
                }
                .padding()
                .background(Color(.secondarySystemBackground))
                .clipShape(RoundedRectangle(cornerRadius: 12))

                if !jitOk {
                    VStack(spacing: 8) {
                        Text("StikDebug isn't attached, or its JIT script isn't running. Open StikDebug to attach, then return here.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                        Button {
                            JITEnabler.requestStikDebugJIT()
                        } label: {
                            Label("Open StikDebug", systemImage: "arrow.up.forward.app")
                        }
                        .buttonStyle(.borderedProminent)
                    }
                }

                if !memoryOk {
                    Text("This build is missing the increased-memory-limit entitlement. Reinstall a build signed with it.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }

                Button("Retry") { runChecks() }
                    .buttonStyle(.bordered)
            }
        }
    }

    private func checkRow(title: String, ok: Bool) -> some View {
        HStack {
            Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(ok ? .green : .red)
            Text(title)
            Spacer()
        }
    }

    private func runChecks() {
        state = .checking
        DispatchQueue.global(qos: .userInitiated).async {
            let memoryOk = checkAppEntitlement("com.apple.developer.kernel.increased-memory-limit")
            // Gate the real BRK-based probe behind checkDebugged() -- see this view's
            // header comment for why calling shadps4_probe_jit() without it is unsafe.
            let debuggerAttached = checkDebugged()
            let jitOk = debuggerAttached && (shadps4_probe_jit() != 0)

            DispatchQueue.main.async {
                if memoryOk && jitOk {
                    state = .passed
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        onPassed()
                    }
                } else {
                    state = .failed(memoryOk: memoryOk, jitOk: jitOk)
                }
            }
        }
    }
}
