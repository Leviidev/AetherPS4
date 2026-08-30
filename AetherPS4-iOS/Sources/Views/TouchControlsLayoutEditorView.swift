import SwiftUI

// Reachable from Settings ("Customize Touch Control Layout"), so a player can reposition
// controls without a game running -- moved here from an in-overlay "Edit Layout" mode
// (TouchControlsOverlayWindow.swift) after on-device feedback that editing mid-gameplay felt
// wrong. This renders a standalone preview of the same control layout using placeholders
// only (no real stick/button behavior needed here, no game to send input to), sharing the
// exact same position math and TouchLayoutStore persistence the real overlay reads from, so
// a drag here shows up in the next game launch's real controls.
struct TouchControlsLayoutEditorView: View {
    @Environment(EmulatorProcess.self) private var emulator
    @StateObject private var layout = TouchLayoutStore.shared

    var body: some View {
        GeometryReader { geo in
            // Same base-unit formula as TouchControlsOverlayWindow's TouchControlsView --
            // must stay in sync with that file so a position dragged here matches where the
            // real control actually ends up in-game.
            let u = geo.size.height * 0.01 * 0.75
            let w = geo.size.width
            let h = geo.size.height

            ZStack {
                Color.black.opacity(0.85)

                ForEach(TouchControlLayoutSpec.all(u: u, w: w, h: h)) { spec in
                    LayoutHandle(layout: layout, key: spec.key, label: spec.label,
                                width: spec.width, height: spec.height)
                        .position(x: spec.x + layout.offset(for: spec.key).width,
                                 y: spec.y + layout.offset(for: spec.key).height)
                }

                VStack {
                    Text("Drag any control to move it")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                        .background(.black.opacity(0.6), in: Capsule())
                        .padding(.top, 12)
                    Spacer()
                }
            }
            .ignoresSafeArea()
        }
        .navigationTitle("Touch Control Layout")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button("Reset", role: .destructive) {
                    layout.resetAll()
                }
            }
        }
        // Real gameplay is always landscape (see AetherPS4App's AppDelegate comment), so
        // this preview has to match or a control dragged here in portrait would land
        // somewhere different once the real overlay actually shows up. Uses the exact same
        // lock/unlock EmulatorProcess uses for a real game session.
        .onAppear { emulator.lockToLandscape() }
        .onDisappear { emulator.unlockOrientation() }
    }
}

// Mirrors the base (pre-offset) positions TouchControlsOverlayWindow's TouchControlsView
// lays real controls out at. Kept in its own type, rather than shared code the real overlay
// also calls, so this preview screen can never accidentally change real control behavior --
// only their positions, through TouchLayoutStore, which both read independently.
private struct TouchControlLayoutSpec: Identifiable {
    let key: String
    let label: String
    let width: CGFloat
    let height: CGFloat
    let x: CGFloat
    let y: CGFloat

    var id: String { key }

    static func all(u: CGFloat, w: CGFloat, h: CGFloat) -> [TouchControlLayoutSpec] {
        [
            TouchControlLayoutSpec(key: "leftStick", label: "L Stick", width: u * 34, height: u * 34,
                                   x: u * 20, y: h - u * 22),
            TouchControlLayoutSpec(key: "dpad", label: "D-Pad", width: u * 26, height: u * 26,
                                   x: u * 22, y: u * 38),
            TouchControlLayoutSpec(key: "rightStick", label: "R Stick", width: u * 34, height: u * 34,
                                   x: w - u * 36, y: h - u * 22),
            TouchControlLayoutSpec(key: "faceButtons", label: "Face", width: u * 54, height: u * 54,
                                   x: w - u * 34, y: u * 47),
            TouchControlLayoutSpec(key: "L1", label: "L1", width: u * 16, height: u * 8,
                                   x: u * 10, y: u * 12),
            TouchControlLayoutSpec(key: "L2", label: "L2", width: u * 16, height: u * 8,
                                   x: u * 10, y: u * 2),
            TouchControlLayoutSpec(key: "R1", label: "R1", width: u * 16, height: u * 8,
                                   x: w - u * 40, y: u * 12),
            TouchControlLayoutSpec(key: "R2", label: "R2", width: u * 16, height: u * 8,
                                   x: w - u * 40, y: u * 2),
            TouchControlLayoutSpec(key: "share", label: "SH", width: u * 9, height: u * 9,
                                   x: w * 0.40, y: u * 6),
            TouchControlLayoutSpec(key: "touchpad", label: "TP", width: u * 9, height: u * 9,
                                   x: w * 0.50, y: u * 6),
            TouchControlLayoutSpec(key: "options", label: "OPT", width: u * 9, height: u * 9,
                                   x: w * 0.60, y: u * 6),
        ]
    }
}

// A dashed, draggable placeholder for one control. Not the real control itself -- this
// screen has no game session to send input to, and even during real gameplay the real
// controls' own gesture recognizers (press/drag-to-move-stick) would fight a
// drag-to-reposition gesture on the same view, which is why this is a separate type instead.
struct LayoutHandle: View {
    @ObservedObject var layout: TouchLayoutStore
    let key: String
    let label: String
    var width: CGFloat
    var height: CGFloat

    // Tracks the last-seen cumulative translation from the current drag so each onChanged
    // call can compute just this step's delta -- DragGesture's translation is relative to
    // the drag's start, not the previous callback, and TouchLayoutStore.addOffset is additive.
    @State private var lastTranslation: CGSize = .zero

    init(layout: TouchLayoutStore, key: String, label: String, size: CGFloat) {
        self.layout = layout
        self.key = key
        self.label = label
        self.width = size
        self.height = size
    }

    init(layout: TouchLayoutStore, key: String, label: String, width: CGFloat, height: CGFloat) {
        self.layout = layout
        self.key = key
        self.label = label
        self.width = width
        self.height = height
    }

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.accentColor.opacity(0.35))
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.white, style: StrokeStyle(lineWidth: 2, dash: [4, 3]))
            Text(label)
                .font(.system(size: 11, weight: .bold))
                .foregroundStyle(.white)
        }
        .frame(width: max(width, 28), height: max(height, 28))
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    let delta = CGSize(width: value.translation.width - lastTranslation.width,
                                       height: value.translation.height - lastTranslation.height)
                    layout.addOffset(delta, for: key)
                    lastTranslation = value.translation
                }
                .onEnded { _ in
                    lastTranslation = .zero
                    layout.commit()
                }
        )
    }
}
