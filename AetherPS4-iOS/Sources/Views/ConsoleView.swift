import UIKit
import SwiftUI

struct ConsoleView: View {
    @Environment(EmulatorProcess.self) private var emulator

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(emulator.consoleLines) { line in
                            ConsoleLineView(line: line)
                        }
                    }
                    .padding(12)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .background(Color(uiColor: .systemBackground))
                .onChange(of: emulator.consoleLines.count) {
                    guard let lastID = emulator.consoleLines.last?.id else { return }
                    withAnimation(.easeOut(duration: 0.15)) {
                        proxy.scrollTo(lastID, anchor: .bottom)
                    }
                }
            }
        }
    }

    private var header: some View {
        HStack {
            Text("Console")
                .font(.title2.bold())

            statusBadge

            Spacer()

            Button {
                copyAllToPasteboard()
            } label: {
                Label("Copy Console", systemImage: "doc.on.doc")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .disabled(emulator.consoleLines.isEmpty)

            Button {
                emulator.clearConsole()
            } label: {
                Label("Clear Console", systemImage: "trash")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
        }
        .padding(16)
    }

    private func copyAllToPasteboard() {
        let text = emulator.consoleLines.map(\.text).joined(separator: "\n")
        let pasteboard = UIPasteboard.general
        pasteboard.string = text
    }

    @ViewBuilder
    private var statusBadge: some View {
        switch emulator.state {
        case .idle:
            EmptyView()
        case .extracting:
            Label("Extracting" + (emulator.runningGameName.map { " – \($0)" } ?? ""),
                  systemImage: "arrow.down.circle")
                .font(.caption.bold())
                .foregroundStyle(.blue)
        case .running:
            Label("Running" + (emulator.runningGameName.map { " – \($0)" } ?? ""),
                  systemImage: "circle.fill")
                .font(.caption.bold())
                .foregroundStyle(.green)
        case .exited(let status):
            Label("Exited (status \(status))", systemImage: status == 0 ? "checkmark.circle.fill" : "xmark.circle.fill")
                .font(.caption.bold())
                .foregroundStyle(status == 0 ? Color.secondary : Color.red)
        }
    }
}

private struct ConsoleLineView: View {
    let line: ConsoleLine

    var body: some View {
        Text(line.text)
            .font(.system(.body, design: .monospaced))
            .foregroundStyle(color)
            .textSelection(.enabled)
            .id(line.id)
            .contextMenu {
                Button("Copy Line") {
                    let pasteboard = UIPasteboard.general
                    pasteboard.string = line.text
                }
            }
    }

    private var color: Color {
        line.stream == .stderr ? .red : .primary
    }
}
