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

// A compact boot signal that lives in the controller's own UIWindow. Keeping it here avoids
// reintroducing LoadingOverlayWindow's full-screen window and its cross-window hit-testing
// problems. shadPS4 does not expose a definitive "the title screen is ready" event, so the
// ring reports coarse, evidence-backed boot stages from the live log and completes after
// sustained successful presentation. The user can hide it immediately by tapping it.
@MainActor
private final class BootProgressState: ObservableObject {
    @Published var progress = 0.06
    @Published var isVisible = true
    @Published var stage = "Preparing"
    @Published var hasFailed = false

    private var timer: Timer?
    private var firstPresentedAt: Date?
    private var completionScheduled = false
    private var sessionLogStart: UInt64 = 0

    init() {
        // aether_crash.log is intentionally retained between launches for diagnostics. Only
        // inspect bytes written after this run's controller overlay appears; otherwise a
        // fatal marker from the previous game instantly turns every later launch red.
        sessionLogStart = currentLogLength()
        refresh()
        let timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    deinit {
        timer?.invalidate()
    }

    func handleTap() {
        if hasFailed {
            // The native render/event threads are still alive after a guest-thread fatal
            // signal (confirmed on-device at roughly two full CPU cores). Give the player a
            // clean route back to the library instead of merely hiding the only warning.
            shadps4_stop()
        }
        isVisible = false
        timer?.invalidate()
        timer = nil
    }

    private func refresh() {
        guard isVisible else { return }
        let text = readLogTail()

        // A fatal guest signal leaves the native SDL window and UIKit overlay alive, so the
        // player otherwise sees the last submitted (often black) frame forever and reasonably
        // assumes the game is still loading. Surface that distinction and keep the badge on
        // screen until the player dismisses it; a failure must never auto-hide as "Ready".
        let fatalMarkers = [
            "Unhandled access violation",
            "Unhandled illegal instruction",
            "Unhandled SIGTRAP",
        ]
        if fatalMarkers.contains(where: text.contains) {
            hasFailed = true
            stage = "Game stopped"
            timer?.invalidate()
            timer = nil
            return
        }

        var next = 0.06
        var nextStage = "Preparing"

        if text.contains("SDL video subsystem initialized") {
            next = 0.18
            nextStage = "Opening display"
        }
        if text.contains("InitHLELibs done") || text.contains("Initializing HLE libraries") {
            next = 0.38
            nextStage = "Loading system libraries"
        }
        if text.contains("guest entry reached") {
            next = 0.56
            nextStage = "Starting game"
        }

        let presentID = lastPresentID(in: text)
        if presentID != nil || shadps4_has_presented_frame() != 0 {
            if firstPresentedAt == nil { firstPresentedAt = Date() }
            next = max(next, 0.72)
            nextStage = "Drawing first frames"
        }
        if text.contains("/app0/") {
            next = max(next, 0.84)
            nextStage = "Loading game resources"
        }
        if let presentID, presentID >= 120 {
            next = max(next, 0.92)
            nextStage = "Finishing startup"
        }

        // Prefer an explicit title-screen marker when a game/emulator provides one. The
        // fallback requires both sustained presentation and real /app0 resource activity;
        // it is intentionally time-based rather than pretending an arbitrary log milestone
        // is a precise percentage.
        let explicitReady = text.contains("At the Title Screen") ||
            text.contains("AETHER_GAME_READY")
        let sustainedReady = firstPresentedAt.map { Date().timeIntervalSince($0) >= 18 } == true &&
            (presentID ?? 0) >= 600 && text.contains("/app0/")
        if explicitReady || sustainedReady {
            next = 1.0
            nextStage = "Ready"
        }

        progress = max(progress, next)
        stage = nextStage
        if progress >= 1.0 && !completionScheduled {
            completionScheduled = true
            timer?.invalidate()
            timer = nil
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .milliseconds(900))
                self?.isVisible = false
            }
        }
    }

    private func readLogTail() -> String {
        guard let url = logURL() else { return "" }
        guard let handle = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? handle.close() }
        let length = (try? handle.seekToEnd()) ?? 0
        if length < sessionLogStart {
            // The logger was truncated or replaced after the overlay appeared.
            sessionLogStart = 0
        }
        let maximum: UInt64 = 256 * 1024
        let tailStart = length > maximum ? length - maximum : 0
        try? handle.seek(toOffset: max(sessionLogStart, tailStart))
        guard let data = try? handle.readToEnd() else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    private func currentLogLength() -> UInt64 {
        guard let url = logURL(),
              let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attributes[.size] as? NSNumber
        else { return 0 }
        return size.uint64Value
    }

    private func logURL() -> URL? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)
            .first?.appendingPathComponent("aether_crash.log")
    }

    private func lastPresentID(in text: String) -> Int? {
        guard let regex = try? NSRegularExpression(
            pattern: #"FRAME_SLOT_ACQUIRE presentId=(\d+)"#
        ) else { return nil }
        let range = NSRange(text.startIndex..., in: text)
        guard let match = regex.matches(in: text, range: range).last,
              let idRange = Range(match.range(at: 1), in: text)
        else { return nil }
        return Int(text[idRange])
    }
}

private struct TouchControlsView: View {
    @StateObject private var state = TouchPadState()
    @StateObject private var bootProgress = BootProgressState()

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
                    .position(x: u * 22, y: u * 38)

                StickView(state: state, axisX: \.rightX, axisY: \.rightY)
                    .frame(width: u * 34, height: u * 34)
                    .position(x: w - u * 36, y: h - u * 22)

                // radius/spread were u*8.5/u*12: diagonal neighbors (Triangle-Square,
                // Triangle-Circle, Cross-Square, Cross-Circle) are spread*sqrt(2) apart
                // center-to-center, so their hit circles' actual gap was
                // 12*1.41 - 2*8.5 = 0 -- exactly touching, not just visually close. Reported
                // on-device as bad, too-close hitboxes. u*10/u*17 gives each button a bigger
                // hit target and a real ~4u gap between diagonal neighbors
                // (17*1.41 - 2*10 ≈ 4). Cluster center moved further left (w - u*34, from
                // w - u*30) and the frame widened to match, since the bigger spread pushes
                // the rightmost button (Circle) further right again -- its edge now lands at
                // w - u*34 + u*17 + u*10 = w - u*7, still safely on-screen.
                FaceButtonsView(state: state, radius: u * 10, spread: u * 17)
                    .frame(width: u * 54, height: u * 54)
                    .position(x: w - u * 34, y: u * 47)

                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_L1), label: "L1",
                              width: u * 16, height: u * 8)
                    .position(x: u * 10, y: u * 12)
                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_L2), label: "L2",
                              width: u * 16, height: u * 8, isTrigger: true, triggerAxis: \.l2)
                    .position(x: u * 10, y: u * 2)
                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_R1), label: "R1",
                              width: u * 16, height: u * 8)
                    .position(x: w - u * 40, y: u * 12)
                ShoulderButton(state: state, bit: UInt32(SHADPS4_PAD_R2), label: "R2",
                              width: u * 16, height: u * 8, isTrigger: true, triggerAxis: \.r2)
                    .position(x: w - u * 40, y: u * 2)

                Group {
                    SmallButton(state: state, bit: UInt32(SHADPS4_PAD_SHARE), label: "SH",
                               radius: u * 4.5)
                        .position(x: w * 0.40, y: u * 6)
                    SmallButton(state: state, bit: UInt32(SHADPS4_PAD_TOUCHPAD), label: "TP",
                               radius: u * 4.5)
                        .position(x: w * 0.50, y: u * 6)
                    SmallButton(state: state, bit: UInt32(SHADPS4_PAD_OPTIONS), label: "OPT",
                               radius: u * 4.5)
                        .position(x: w * 0.60, y: u * 6)
                }

                if bootProgress.isVisible {
                    Button {
                        bootProgress.handleTap()
                    } label: {
                        ZStack {
                            Circle()
                                .fill(.black.opacity(0.48))
                            Circle()
                                .stroke(bootProgress.hasFailed ? .red.opacity(0.35) :
                                            .white.opacity(0.28), lineWidth: 3)
                            Circle()
                                .trim(from: 0, to: bootProgress.hasFailed ? 1 :
                                    bootProgress.progress)
                                .stroke(bootProgress.hasFailed ? .red : .white,
                                        style: StrokeStyle(
                                    lineWidth: 3, lineCap: .round
                                ))
                                .rotationEffect(.degrees(-90))
                            Text(bootProgress.hasFailed ? "!" :
                                "\(Int((bootProgress.progress * 100).rounded()))")
                                .font(.system(size: max(10, u * 3.2), weight: .semibold,
                                              design: .rounded))
                                .foregroundStyle(.white)
                                .monospacedDigit()
                        }
                        .frame(width: u * 13, height: u * 13)
                    }
                    .buttonStyle(.plain)
                    .position(x: w * 0.50, y: u * 15)
                    .accessibilityLabel(bootProgress.hasFailed ? "Game stopped" :
                        "Game loading progress")
                    .accessibilityValue(bootProgress.hasFailed ? bootProgress.stage :
                        "\(Int((bootProgress.progress * 100).rounded())) percent, \(bootProgress.stage)")
                    .accessibilityHint(bootProgress.hasFailed ? "Tap to stop the game" :
                        "Tap to hide")
                    .transition(.scale.combined(with: .opacity))
                }
            }
            .animation(.easeInOut(duration: 0.2), value: bootProgress.isVisible)
            .animation(.linear(duration: 0.35), value: bootProgress.progress)
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
        // simultaneousGesture, not gesture: the four face buttons are close enough that their
        // *bounding frames* (not their circular contentShape/hit area, which do have a real
        // gap -- see FaceButtonsView's own comment) overlap at the corners. SwiftUI's plain
        // .gesture() sets each sibling's DragGesture up as UIKit-exclusive, which forces a
        // recognizer-negotiation pass to pick a winner whenever frames overlap like this --
        // and that negotiation was eating the first touch, only resolving (and registering the
        // press) once the finger moved enough to disambiguate. Reported on-device as buttons
        // only registering on "press then drag out of it". simultaneousGesture opts this
        // recognizer out of that exclusivity entirely, so it starts the instant its own
        // contentShape is hit, with no negotiation against its siblings.
        .simultaneousGesture(
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
