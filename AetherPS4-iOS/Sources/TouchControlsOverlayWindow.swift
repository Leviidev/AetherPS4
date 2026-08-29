import SwiftUI
import UIKit

// Full PS4 touch controller overlay, in SwiftUI, using the same proven pattern as
// LoadingOverlayWindow: a second UIWindow instead of a subview inside SDL's own window (see
// LoadingOverlayWindow's own header comment for why that alternative crashed on-device).
// Replaces touch_controls_layer.cpp's ImGui-based version (disabled, see vk_presenter.cpp) --
// the whole point of this session's threading work was getting real UI off ImGui and onto
// SwiftUI wherever the main thread being free actually makes that possible, and unlike the
// loading card, on-screen controls have no reason to ever need ImGui's render-thread access
// in the first place: touches are just forwarded to shadps4_apply_touch_input(), which
// works from any thread.
//
// Multi-touch (holding a stick with one thumb while pressing a face button with another)
// works for free here: each control below is its own SwiftUI view with its own gesture
// recognizer, and UIKit dispatches simultaneous touches across different views natively --
// no raw finger-event interception needed the way ImGui's single-emulated-pointer backend
// required.
//
// windowLevel is .normal + 1 -- above SDL's window, but below LoadingOverlayWindow's
// .alert + 1, so the loading card still sits on top of the controls while it's open rather
// than the two fighting over top position.
@MainActor
enum TouchControlsOverlayWindow {
    private static var window: UIWindow?

    static func show() {
        guard window == nil else { return }
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else {
            print("[AetherPS4] TouchControlsOverlayWindow: no active window scene, cannot show")
            return
        }

        let overlayWindow = UIWindow(windowScene: scene)
        overlayWindow.windowLevel = .normal + 1
        overlayWindow.backgroundColor = .clear
        overlayWindow.rootViewController = UIHostingController(rootView: TouchControlsView())
        overlayWindow.rootViewController?.view.backgroundColor = .clear
        overlayWindow.isHidden = false
        window = overlayWindow
        print("[AetherPS4] TouchControlsOverlayWindow: shown")
    }

    static func teardown() {
        window?.isHidden = true
        window = nil
        print("[AetherPS4] TouchControlsOverlayWindow: torn down")
    }
}

// Tracks every control's live state and is the single source of truth for what gets sent
// to shadps4_apply_touch_input() -- that call takes a full snapshot every time (not
// incremental deltas), so every control writes its own piece here and the combined state
// is resent on every change, from whichever control changed.
@MainActor
private final class TouchPadState: ObservableObject {
    var buttons: UInt32 = 0
    var leftX = 128
    var leftY = 128
    var rightX = 128
    var rightY = 128
    var l2 = 0
    var r2 = 0

    func setButton(_ bit: UInt32, pressed: Bool) {
        if pressed {
            buttons |= bit
        } else {
            buttons &= ~bit
        }
        send()
    }

    func send() {
        shadps4_apply_touch_input(buttons, Int32(leftX), Int32(leftY), Int32(rightX),
                                  Int32(rightY), Int32(l2), Int32(r2))
    }
}

private struct TouchControlsView: View {
    @StateObject private var state = TouchPadState()

    var body: some View {
        GeometryReader { geo in
            // 1% of the shorter dimension (landscape's height), times a 0.75 shrink --
            // same base-unit approach and shrink factor touch_controls_layer.cpp settled
            // on after "way too big" feedback, since this is the same physical control set
            // at the same physical screen sizes.
            let u = geo.size.height * 0.01 * 0.75
            let w = geo.size.width
            let h = geo.size.height

            ZStack {
                StickView(state: state, axisX: \.leftX, axisY: \.leftY)
                    .frame(width: u * 34, height: u * 34)
                    .position(x: u * 20, y: h - u * 22)

                DPadView(state: state, radius: u * 13)
                    .frame(width: u * 26, height: u * 26)
                    .position(x: u * 18, y: u * 36)

                StickView(state: state, axisX: \.rightX, axisY: \.rightY)
                    .frame(width: u * 34, height: u * 34)
                    .position(x: w - u * 36, y: h - u * 22)

                FaceButtonsView(state: state, radius: u * 8.5, spread: u * 12)
                    .frame(width: u * 40, height: u * 40)
                    .position(x: w - u * 16, y: u * 42)

                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_L1), label: "L1",
                              width: u * 16, height: u * 8)
                    .position(x: u * 10, y: u * 12)
                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_L2), label: "L2",
                              width: u * 16, height: u * 8, isTrigger: true, triggerAxis: \.l2)
                    .position(x: u * 10, y: u * 2)
                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_R1), label: "R1",
                              width: u * 16, height: u * 8)
                    .position(x: w - u * 24, y: u * 12)
                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_R2), label: "R2",
                              width: u * 16, height: u * 8, isTrigger: true, triggerAxis: \.r2)
                    .position(x: w - u * 24, y: u * 2)

                SmallButton(state: state, bit: UInt32(SHADPS4_PAD_OPTIONS), label: "OPT",
                           radius: u * 4.5)
                    .position(x: w * 0.58, y: u * 6)
                SmallButton(state: state, bit: UInt32(SHADPS4_PAD_TOUCHPAD), label: "TP",
                           radius: u * 4.5)
                    .position(x: w * 0.42, y: u * 6)
            }
        }
        .ignoresSafeArea()
    }
}

// MARK: - Sticks

private struct StickView: View {
    @ObservedObject var state: TouchPadState
    let axisX: ReferenceWritableKeyPath<TouchPadState, Int>
    let axisY: ReferenceWritableKeyPath<TouchPadState, Int>

    @State private var thumbOffset: CGSize = .zero
    @State private var isDragging = false

    var body: some View {
        GeometryReader { geo in
            let radius = min(geo.size.width, geo.size.height) / 2
            ZStack {
                Circle()
                    .fill(Color.white.opacity(0.15))
                Circle()
                    .stroke(Color.white.opacity(0.4), lineWidth: 2)
                Circle()
                    .fill(Color.white.opacity(isDragging ? 0.55 : 0.3))
                    .frame(width: radius * 0.9, height: radius * 0.9)
                    .offset(thumbOffset)
            }
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        isDragging = true
                        let dx = value.translation.width
                        let dy = value.translation.height
                        let dist = sqrt(dx * dx + dy * dy)
                        let clamped: CGSize
                        if dist > radius && dist > 0 {
                            let scale = radius / dist
                            clamped = CGSize(width: dx * scale, height: dy * scale)
                        } else {
                            clamped = CGSize(width: dx, height: dy)
                        }
                        thumbOffset = clamped
                        state[keyPath: axisX] = axisValue(clamped.width, radius: radius)
                        state[keyPath: axisY] = axisValue(clamped.height, radius: radius)
                        state.send()
                    }
                    .onEnded { _ in
                        isDragging = false
                        thumbOffset = .zero
                        state[keyPath: axisX] = 128
                        state[keyPath: axisY] = 128
                        state.send()
                    }
            )
        }
    }

    // Maps a clamped [-radius, radius] offset to a 0-255 axis value, 128 = center --
    // matches Input::GetAxis's own mapping (see touch_controls_layer.cpp's prior usage of
    // it), just computed directly here since that helper isn't exposed across the C API.
    private func axisValue(_ offset: CGFloat, radius: CGFloat) -> Int {
        guard radius > 0 else { return 128 }
        let normalized = max(-1.0, min(1.0, offset / radius))
        return Int((normalized * 127.0).rounded()) + 128
    }
}

// MARK: - D-Pad

private struct DPadView: View {
    @ObservedObject var state: TouchPadState
    let radius: CGFloat

    @State private var pressedBits: UInt32 = 0

    var body: some View {
        GeometryReader { geo in
            let center = CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)
            ZStack {
                Circle().fill(Color.white.opacity(0.15))
                Circle().stroke(Color.white.opacity(0.4), lineWidth: 2)
                dpadArrow(rotation: 0, active: pressedBits & UInt32(SHADPS4_PAD_UP) != 0)
                dpadArrow(rotation: 180, active: pressedBits & UInt32(SHADPS4_PAD_DOWN) != 0)
                dpadArrow(rotation: 270, active: pressedBits & UInt32(SHADPS4_PAD_LEFT) != 0)
                dpadArrow(rotation: 90, active: pressedBits & UInt32(SHADPS4_PAD_RIGHT) != 0)
            }
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        update(point: value.location, center: center)
                    }
                    .onEnded { _ in
                        apply(0)
                    }
            )
        }
    }

    private func dpadArrow(rotation: Double, active: Bool) -> some View {
        Image(systemName: "arrowtriangle.up.fill")
            .font(.system(size: 14))
            .foregroundColor(.white.opacity(active ? 0.9 : 0.5))
            .offset(y: -radius * 0.55)
            .rotationEffect(.degrees(rotation))
    }

    // Angle-based 8-way direction, same approach touch_controls_layer.cpp used: lets a
    // touch near a corner register as a diagonal (two bits at once) like a real d-pad's
    // corner does, rather than 4 separate quadrant rectangles.
    private func update(point: CGPoint, center: CGPoint) {
        let dx = point.x - center.x
        let dy = point.y - center.y
        let degrees = atan2(-dy, dx) * 180 / .pi
        let normalized = (degrees + 360).truncatingRemainder(dividingBy: 360)
        var bits: UInt32 = 0
        switch normalized {
        case 337.5..., ..<22.5: bits = UInt32(SHADPS4_PAD_RIGHT)
        case 22.5..<67.5: bits = UInt32(SHADPS4_PAD_UP) | UInt32(SHADPS4_PAD_RIGHT)
        case 67.5..<112.5: bits = UInt32(SHADPS4_PAD_UP)
        case 112.5..<157.5: bits = UInt32(SHADPS4_PAD_UP) | UInt32(SHADPS4_PAD_LEFT)
        case 157.5..<202.5: bits = UInt32(SHADPS4_PAD_LEFT)
        case 202.5..<247.5: bits = UInt32(SHADPS4_PAD_DOWN) | UInt32(SHADPS4_PAD_LEFT)
        case 247.5..<292.5: bits = UInt32(SHADPS4_PAD_DOWN)
        default: bits = UInt32(SHADPS4_PAD_DOWN) | UInt32(SHADPS4_PAD_RIGHT)
        }
        apply(bits)
    }

    private func apply(_ bits: UInt32) {
        let dpadMask = UInt32(SHADPS4_PAD_UP) | UInt32(SHADPS4_PAD_DOWN) | UInt32(SHADPS4_PAD_LEFT)
            | UInt32(SHADPS4_PAD_RIGHT)
        state.buttons = (state.buttons & ~dpadMask) | bits
        pressedBits = bits
        state.send()
    }
}

// MARK: - Face buttons

private struct FaceButtonsView: View {
    @ObservedObject var state: TouchPadState
    let radius: CGFloat
    let spread: CGFloat

    var body: some View {
        ZStack {
            FaceButton(state: state, bit: UInt32(SHADPS4_PAD_TRIANGLE), radius: radius) {
                Image(systemName: "triangle").font(.system(size: radius * 0.7))
            }
            .offset(y: -spread)
            FaceButton(state: state, bit: UInt32(SHADPS4_PAD_CROSS), radius: radius) {
                Image(systemName: "xmark").font(.system(size: radius * 0.6))
            }
            .offset(y: spread)
            FaceButton(state: state, bit: UInt32(SHADPS4_PAD_SQUARE), radius: radius) {
                Image(systemName: "square").font(.system(size: radius * 0.6))
            }
            .offset(x: -spread)
            FaceButton(state: state, bit: UInt32(SHADPS4_PAD_CIRCLE), radius: radius) {
                Image(systemName: "circle").font(.system(size: radius * 0.6))
            }
            .offset(x: spread)
        }
    }
}

private struct FaceButton<Glyph: View>: View {
    @ObservedObject var state: TouchPadState
    let bit: UInt32
    let radius: CGFloat
    @ViewBuilder let glyph: () -> Glyph

    @State private var isPressed = false

    var body: some View {
        ZStack {
            Circle().fill(Color.white.opacity(isPressed ? 0.5 : 0.15))
            Circle().stroke(Color.white.opacity(0.4), lineWidth: 1.5)
            glyph().foregroundColor(.white.opacity(0.85))
        }
        .frame(width: radius * 2, height: radius * 2)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !isPressed else { return }
                    isPressed = true
                    state.setButton(bit, pressed: true)
                }
                .onEnded { _ in
                    isPressed = false
                    state.setButton(bit, pressed: false)
                }
        )
    }
}

// MARK: - Shoulder buttons

private struct ShoulderButton: View {
    @ObservedObject var state: TouchPadState
    let bit: UInt32
    let label: String
    let width: CGFloat
    let height: CGFloat
    var isTrigger: Bool = false
    var triggerAxis: ReferenceWritableKeyPath<TouchPadState, Int>? = nil

    @State private var isPressed = false

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(Color.white.opacity(isPressed ? 0.5 : 0.15))
            RoundedRectangle(cornerRadius: 6)
                .stroke(Color.white.opacity(0.4), lineWidth: 1.5)
            Text(label)
                .font(.system(size: 12, weight: .semibold))
                .foregroundColor(.white.opacity(0.85))
        }
        .frame(width: width, height: height)
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !isPressed else { return }
                    isPressed = true
                    state.setButton(bit, pressed: true)
                    if let triggerAxis { state[keyPath: triggerAxis] = 255 }
                    state.send()
                }
                .onEnded { _ in
                    isPressed = false
                    state.setButton(bit, pressed: false)
                    if let triggerAxis { state[keyPath: triggerAxis] = 0 }
                    state.send()
                }
        )
    }
}

// MARK: - Small buttons (Options / TouchPad)

private struct SmallButton: View {
    @ObservedObject var state: TouchPadState
    let bit: UInt32
    let label: String
    let radius: CGFloat

    @State private var isPressed = false

    var body: some View {
        ZStack {
            Circle().fill(Color.white.opacity(isPressed ? 0.5 : 0.15))
            Circle().stroke(Color.white.opacity(0.4), lineWidth: 1.5)
            Text(label)
                .font(.system(size: 9, weight: .semibold))
                .foregroundColor(.white.opacity(0.85))
        }
        .frame(width: radius * 2, height: radius * 2)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !isPressed else { return }
                    isPressed = true
                    state.setButton(bit, pressed: true)
                }
                .onEnded { _ in
                    isPressed = false
                    state.setButton(bit, pressed: false)
                }
        )
    }
}
