import SwiftUI
import UIKit

struct GameCardView: View {
    let game: Game
    let isSelected: Bool

    private let artCornerRadius: CGFloat = 16

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ZStack(alignment: .topTrailing) {
                artwork
                    .aspectRatio(1, contentMode: .fill)
                    .clipShape(RoundedRectangle(cornerRadius: artCornerRadius, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: artCornerRadius, style: .continuous)
                            .strokeBorder(.black.opacity(0.06), lineWidth: 1)
                    )
                    .shadow(color: .black.opacity(0.14), radius: 10, x: 0, y: 6)

                if !game.isAvailable {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.caption.bold())
                        .foregroundStyle(.white, .orange)
                        .padding(6)
                        .background(.black.opacity(0.35), in: Circle())
                        .padding(8)
                }
            }
            .frame(maxWidth: .infinity)

            VStack(alignment: .leading, spacing: 2) {
                Text(game.name)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                    .truncationMode(.tail)

                Text(PlayTimeStore.formatted(forTitleId: game.titleId) ?? "Not played yet")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: artCornerRadius + 4, style: .continuous)
                .fill(isSelected ? Color.accentColor.opacity(0.12) : Color.clear)
        )
        .contentShape(Rectangle())
        .animation(.easeOut(duration: 0.15), value: isSelected)
    }

    @ViewBuilder
    private var artwork: some View {
        if let iconPath = game.iconPath, let uiImage = UIImage(contentsOfFile: iconPath) {
            Image(uiImage: uiImage)
                .resizable()
        } else {
            RoundedRectangle(cornerRadius: artCornerRadius, style: .continuous)
                .fill(Color(.secondarySystemBackground))
                .overlay(
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 34))
                        .foregroundStyle(.tertiary)
                )
        }
    }
}

/// Adds a gentle press-down scale to any tappable card so the grid feels responsive
/// instead of static -- `NavigationLink` + `.buttonStyle(.plain)` alone gives no tap
/// feedback at all.
struct CardPressStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .animation(.easeOut(duration: 0.15), value: configuration.isPressed)
    }
}
