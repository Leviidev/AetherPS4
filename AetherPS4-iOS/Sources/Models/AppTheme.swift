import SwiftUI
import UIKit

/// The app's chosen accent/theme color, persisted in UserDefaults. Purely a Swift/UI-side
/// concern -- the engine has no notion of "theme color," so unlike ConfigStore this never
/// touches config.json. Applied at the root (ContentView) via `.tint(...)`, which SwiftUI
/// propagates to every `Color.accentColor` reference throughout the app (buttons, selected
/// states, toggles, etc.) without each of those call sites needing to know about this store.
@MainActor
final class AppTheme: ObservableObject {
    static let shared = AppTheme()

    struct Preset: Identifiable {
        let id: String
        let name: String
        let color: Color
    }

    // "System Blue" (nil hex) reproduces the app's original, un-themed look -- SwiftUI's own
    // default accent color -- rather than picking an arbitrary color as a fake "default".
    static let presets: [Preset] = [
        Preset(id: "system", name: "System Blue", color: .blue),
        Preset(id: "purple", name: "Purple", color: .purple),
        Preset(id: "indigo", name: "Indigo", color: .indigo),
        Preset(id: "pink", name: "Pink", color: .pink),
        Preset(id: "red", name: "Red", color: .red),
        Preset(id: "orange", name: "Orange", color: .orange),
        Preset(id: "yellow", name: "Yellow", color: .yellow),
        Preset(id: "green", name: "Green", color: .green),
        Preset(id: "teal", name: "Teal", color: .teal),
        Preset(id: "mint", name: "Mint", color: .mint),
    ]

    @Published var accentColor: Color {
        didSet { persist() }
    }

    // Background tint intentionally derives from accentColor rather than being a separate
    // stored color -- one picker changes "the app's theme," not two independently-drifting
    // settings the user has to keep in sync themselves.
    var backgroundTint: Color {
        accentColor.opacity(0.05)
    }

    private let defaultsKey = "appThemeAccentColorHex"

    private init() {
        if let hex = UserDefaults.standard.string(forKey: defaultsKey), let color = Color(themeHex: hex) {
            accentColor = color
        } else {
            accentColor = .blue
        }
    }

    private func persist() {
        UserDefaults.standard.set(accentColor.toHex(), forKey: defaultsKey)
    }
}

extension Color {
    init?(themeHex hex: String) {
        var s = hex.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let value = UInt32(s, radix: 16) else { return nil }
        let r = Double((value >> 16) & 0xFF) / 255
        let g = Double((value >> 8) & 0xFF) / 255
        let b = Double(value & 0xFF) / 255
        self = Color(red: r, green: g, blue: b)
    }

    func toHex() -> String {
        let ui = UIColor(self)
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        ui.getRed(&r, green: &g, blue: &b, alpha: &a)
        return String(format: "#%02X%02X%02X", Int(r * 255), Int(g * 255), Int(b * 255))
    }
}
