import SwiftUI
import UIKit

struct GameDetailView: View {
    @Environment(GameLibrary.self) private var library
    @Environment(EmulatorProcess.self) private var emulator
    @Environment(\.dismiss) private var dismiss

    let game: Game

    @State private var showRemoveConfirmation = false
    @State private var launchError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            ZStack(alignment: .bottomLeading) {
                if let iconPath = game.iconPath, let uiImage = UIImage(contentsOfFile: iconPath) {
                    Image(uiImage: uiImage)
                        .resizable()
                        .aspectRatio(1, contentMode: .fit)
                        .frame(maxWidth: 180)
                        .clipShape(RoundedRectangle(cornerRadius: 14))
                        .shadow(color: .black.opacity(0.35), radius: 8, x: 0, y: 4)
                } else {
                    RoundedRectangle(cornerRadius: 14)
                        .fill(.background.secondary)
                        .aspectRatio(1, contentMode: .fit)
                        .frame(maxWidth: 180)
                        .overlay(
                            Image(systemName: "gamecontroller.fill")
                                .font(.system(size: 44))
                                .foregroundStyle(.secondary)
                        )
                }
            }
            .frame(maxWidth: .infinity)

            VStack(alignment: .leading, spacing: 4) {
                Text(game.name)
                    .font(.title3.bold())

                HStack(spacing: 8) {
                    if let titleId = game.titleId {
                        Text(titleId)
                            .font(.caption.monospaced().bold())
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.accentColor.opacity(0.2), in: RoundedRectangle(cornerRadius: 4))
                            .foregroundStyle(Color.accentColor)
                    }

                    if let appVersion = game.appVersion {
                        Text("v\(appVersion)")
                            .font(.caption.monospaced())
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.secondary.opacity(0.15), in: RoundedRectangle(cornerRadius: 4))
                            .foregroundStyle(.secondary)
                    }
                }

                if let playTime = PlayTimeStore.formatted(forTitleId: game.titleId) {
                    Label(playTime, systemImage: "clock")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                Label {
                    Text(game.absolutePkgPath)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                        .lineLimit(3)
                } icon: {
                    Image(systemName: "doc.fill")
                        .foregroundStyle(.secondary)
                }

                if !game.isAvailable {
                    Label("File not found on disk", systemImage: "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }

            if let launchError {
                Text(launchError)
                    .font(.caption)
                    .foregroundStyle(.red)
            }

            Spacer()

            VStack(spacing: 10) {
                if emulator.isBusy && emulator.runningGameName == game.name {
                    Button(role: .destructive) {
                        emulator.stop()
                    } label: {
                        Label(
                            emulator.state == .extracting ? "Cancel Extraction" : "Stop Game",
                            systemImage: "stop.fill"
                        )
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                } else {
                    Button {
                        launch()
                    } label: {
                        Label("Launch Game", systemImage: "play.fill")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!game.isAvailable || emulator.isBusy)
                }

                Button(role: .destructive) {
                    showRemoveConfirmation = true
                } label: {
                    Label("Remove from Library", systemImage: "trash")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }
        }
        .padding(20)
        .background(.background.secondary)
        .confirmationDialog(
            "Remove \"\(game.name)\" from your library?",
            isPresented: $showRemoveConfirmation,
            titleVisibility: .visible
        ) {
            Button("Remove", role: .destructive) {
                library.removeGame(game)
                dismiss()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This deletes the imported copy of the .pkg file from the app.")
        }
    }

    private func launch() {
        launchError = nil
        emulator.launch(pkgPath: game.absolutePkgPath, gameName: game.name)
    }
}
