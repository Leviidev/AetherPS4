import SwiftUI
import AppKit

struct GameCardView: View {
    let game: Game
    let isSelected: Bool

    var body: some View {
        VStack(spacing: 8) {
            ZStack {
                if let iconPath = game.iconPath, let nsImage = NSImage(contentsOfFile: iconPath) {
                    Image(nsImage: nsImage)
                        .resizable()
                        .aspectRatio(1, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .shadow(color: .black.opacity(0.25), radius: 6, x: 0, y: 3)
                } else {
                    RoundedRectangle(cornerRadius: 12)
                        .fill(.background.secondary)
                        .aspectRatio(1, contentMode: .fit)
                        .overlay(
                            Image(systemName: "gamecontroller.fill")
                                .font(.system(size: 38))
                                .foregroundStyle(.secondary)
                        )
                }

                if !game.isAvailable {
                    VStack {
                        Spacer()
                        Label("Unavailable", systemImage: "exclamationmark.triangle.fill")
                            .font(.caption2.bold())
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(.orange.opacity(0.9), in: Capsule())
                            .foregroundStyle(.white)
                            .padding(.bottom, 8)
                    }
                }
            }
            .frame(maxWidth: .infinity)

            VStack(spacing: 2) {
                Text(game.name)
                    .font(.callout.weight(.semibold))
                    .lineLimit(2)
                    .multilineTextAlignment(.center)

                if let titleId = game.titleId {
                    Text(titleId)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(isSelected ? Color.accentColor.opacity(0.15) : Color.clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14)
                .strokeBorder(isSelected ? Color.accentColor : Color.clear, lineWidth: 2)
        )
        .contentShape(Rectangle())
        .animation(.easeOut(duration: 0.15), value: isSelected)
    }
}
