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
    private enum CheckPhase: Equatable {
        case pending
        case checking
        case ok
        case failed
    }

    let onPassed: () -> Void

    @State private var memoryPhase: CheckPhase = .pending
    @State private var jitPhase: CheckPhase = .pending
    @State private var everythingPassed = false
    @Environment(\.scenePhase) private var scenePhase

    private var isChecking: Bool {
        memoryPhase == .checking || jitPhase == .checking || memoryPhase == .pending
    }

    private var hasFailure: Bool {
        memoryPhase == .failed || jitPhase == .failed
    }

    var body: some View {
        ZStack {
            Color(.systemBackground).ignoresSafeArea()
            VStack(spacing: 28) {
                header

                VStack(alignment: .leading, spacing: 0) {
                    checkRow(title: "Memory Entitlement", detail: "com.apple.developer.kernel.increased-memory-limit", phase: memoryPhase)
                    Divider().padding(.leading, 52)
                    checkRow(title: "JIT Script (StikDebug)", detail: "Required for guest code execution", phase: jitPhase)
                }
                .background(Color(.secondarySystemBackground))
                .clipShape(RoundedRectangle(cornerRadius: 14))

                if hasFailure {
                    failureHelp
                }

                Spacer()

                if everythingPassed {
                    Button {
                        onPassed()
                    } label: {
                        Text("Continue")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                } else if hasFailure {
                    Button {
                        runChecks()
                    } label: {
                        Text("Retry")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)
                }
            }
            .padding(24)
        }
        .onAppear { runChecks() }
        .onChange(of: scenePhase) { _, newPhase in
            if newPhase == .active, hasFailure {
                runChecks()
            }
        }
    }

    private var header: some View {
        VStack(spacing: 12) {
            Image(systemName: everythingPassed ? "checkmark.circle.fill" : (hasFailure ? "exclamationmark.triangle.fill" : "gearshape.2"))
                .font(.system(size: 44))
                .foregroundStyle(everythingPassed ? .green : (hasFailure ? .yellow : .secondary))
                .symbolEffect(.pulse, isActive: isChecking)
                .padding(.top, 24)
            Text(everythingPassed ? "Setup Verified" : (hasFailure ? "Setup Check Failed" : "Verifying Setup…"))
                .font(.title2.bold())
        }
    }

    private var failureHelp: some View {
        VStack(spacing: 12) {
            if jitPhase == .failed {
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
            if memoryPhase == .failed {
                Text("This build is missing the increased-memory-limit entitlement. Reinstall a build signed with it.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .padding(.horizontal, 8)
    }

    private func checkRow(title: String, detail: String, phase: CheckPhase) -> some View {
        HStack(spacing: 12) {
            statusIcon(for: phase)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                Text(detail)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer()
        }
        .padding(14)
    }

    @ViewBuilder
    private func statusIcon(for phase: CheckPhase) -> some View {
        switch phase {
        case .pending:
            Image(systemName: "circle.dotted")
                .foregroundStyle(.tertiary)
        case .checking:
            ProgressView()
        case .ok:
            Image(systemName: "checkmark.circle.fill")
                .foregroundStyle(.green)
        case .failed:
            Image(systemName: "xmark.circle.fill")
                .foregroundStyle(.red)
        }
    }

    private func runChecks() {
        everythingPassed = false
        memoryPhase = .checking
        jitPhase = .pending

        DispatchQueue.global(qos: .userInitiated).async {
            let memoryOk = checkAppEntitlement("com.apple.developer.kernel.increased-memory-limit")
            DispatchQueue.main.async {
                memoryPhase = memoryOk ? .ok : .failed
                jitPhase = .checking
            }

            // Gate the real BRK-based probe behind checkDebugged() -- see this view's
            // header comment for why calling shadps4_probe_jit() without it is unsafe.
            let debuggerAttached = checkDebugged()
            let jitOk = debuggerAttached && (shadps4_probe_jit() != 0)

            DispatchQueue.main.async {
                jitPhase = jitOk ? .ok : .failed
                if memoryOk && jitOk {
                    everythingPassed = true
                }
            }
        }
    }
}
