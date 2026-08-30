import CoreGraphics
import Foundation

/// Per-control position offsets for the touch controller overlay (TouchControlsOverlayWindow),
/// persisted in UserDefaults under a stable key per control (see the `key:` arguments passed
/// to each control in TouchControlsView). Purely a Swift/UI-side concern -- unlike
/// ConfigStore, this has no C++-side counterpart, since the engine never sees where a touch
/// control is drawn, only the button/axis state it produces.
@MainActor
final class TouchLayoutStore: ObservableObject {
    static let shared = TouchLayoutStore()

    @Published private(set) var offsets: [String: CGSize] = [:]

    private let defaultsKey = "touchControlLayoutOffsets"

    private init() {
        load()
    }

    func offset(for key: String) -> CGSize {
        offsets[key] ?? .zero
    }

    func addOffset(_ delta: CGSize, for key: String) {
        let current = offsets[key] ?? .zero
        offsets[key] = CGSize(width: current.width + delta.width, height: current.height + delta.height)
    }

    func commit() {
        save()
    }

    func resetAll() {
        offsets = [:]
        save()
    }

    private func load() {
        guard let data = UserDefaults.standard.data(forKey: defaultsKey),
              let decoded = try? JSONDecoder().decode([String: CGSizeCodable].self, from: data) else {
            return
        }
        offsets = decoded.mapValues { CGSize(width: $0.width, height: $0.height) }
    }

    private func save() {
        let encoded = offsets.mapValues { CGSizeCodable(width: $0.width, height: $0.height) }
        guard let data = try? JSONEncoder().encode(encoded) else { return }
        UserDefaults.standard.set(data, forKey: defaultsKey)
    }
}

private struct CGSizeCodable: Codable {
    let width: CGFloat
    let height: CGFloat
}
