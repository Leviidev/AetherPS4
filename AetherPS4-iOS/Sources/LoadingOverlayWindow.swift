import SwiftUI
import UIKit

// Puts a live SwiftUI loading card on top of SDL's own window using the simplest tool UIKit
// has for exactly this -- a second UIWindow with a higher windowLevel -- instead of
// GameOverlayHost's approach (attaching as a subview inside SDL's own window, requiring
// child-view-controller containment and constraints anchored to a window this app doesn't
// own). That approach was never tested on-device before it shipped and crashed on the very
// first real run with an uncaught exception right after being attached; this one is a much
// more standard, lower-risk pattern with no shared view hierarchy at all. Only viable now
// that Emulator::Run() is split (see emulator.h): the main thread stays free for the whole
// game session instead of freezing the instant shadps4_run() used to take it over.
//
// Visual/interaction design ported from AetherX's LoadingConsoleOverlay (see
// ~/Documents/Coding/AetherX/AetherX/EmulatorView.swift) -- a floating card, not a
// full-screen cover, with an X (exit game) and a terminal (reopen the card) button both
// top-center. User-controlled open/close, not auto-dismissed: shadps4_has_presented_frame()
// (the only engine-level signal available) flips true on the presenter's very first
// swapchain present, which is confirmed to still be empty/loading/splash content, not the
// real game -- there's no reliable "loading is actually done" signal to time an automatic
// dismiss against, so like AetherX's own card, this one just stays open until the user
// closes it.
// SwiftUI's .allowsHitTesting(false) only stops SwiftUI's *own* gesture recognizers from
// firing inside this window's view tree -- it does not make the underlying UIWindow
// transparent to UIKit's own hit-testing. UIKit routes each touch to exactly one window (the
// topmost one whose hitTest(_:with:) returns non-nil at that point) and never falls through to
// a lower window on its own; a plain UIWindow's default hitTest returns *some* view for any
// point inside its bounds, even where nothing SwiftUI-interactive is rendered, which is enough
// for UIKit to award it the touch and never offer it to TouchControlsOverlayWindow underneath.
// Confirmed on-device: disabling hit-testing at the SwiftUI level alone did not fix on-screen
// controls being unresponsive.
//
// A first attempt compared the hit-tested view against rootViewController?.view, on the
// (wrong) assumption that a tap landing on real SwiftUI content -- a button, the card --
// would hit-test to something more specific nested underneath the root. Also confirmed
// on-device: it doesn't. SwiftUI's hosting view is typically one flat UIView handling all of
// its own internal hit-testing/gesture dispatch, not a deep native UIKit view per SwiftUI
// element, so hitTest returned that same single hosting view for *every* point regardless of
// what was actually there -- making the check always pass and this window pass through
// everything unconditionally, including taps on its own X/terminal/chevron buttons.
//
// This version tracks the actual interactive frames as SwiftUI reports them (via
// InteractiveFrameReporter below, one per real control) instead of trying to infer them from
// UIKit's hit-test result. A touch only gets awarded to this window if it falls inside one of
// those frames; everything else passes through to the window below.
private final class PassthroughWindow: UIWindow {
    var interactiveFrames: [CGRect] = []

    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        guard interactiveFrames.contains(where: { $0.contains(point) }) else { return nil }
        return super.hitTest(point, with: event)
    }
}

// Attach to any view that should count as "real, tappable content" for PassthroughWindow's
// hitTest above -- reports this view's frame (in window coordinates) up through the
// .interactiveFrame(id:) view modifier below every time it changes (position, or appearing/
// disappearing, e.g. the card's chevron-collapse toggling visibility).
private struct InteractiveFrameKey: PreferenceKey {
    static let defaultValue: [String: CGRect] = [:]
    static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

private extension View {
    func interactiveFrame(id: String) -> some View {
        background(
            GeometryReader { geo in
                Color.clear.preference(key: InteractiveFrameKey.self,
                                       value: [id: geo.frame(in: .global)])
            }
        )
    }
}

@MainActor
enum LoadingOverlayWindow {
    private static var window: PassthroughWindow?
    private static var uiState: LoadingOverlayUIState?

    /// Call once shadps4_prepare_window() has returned successfully -- SDL's window
    /// unconditionally exists by then (see EmulatorProcess.launch()), though this window
    /// doesn't actually need that to be true; it's fully independent.
    static func show(gameName: String) {
        guard window == nil else { return }
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })
        else {
            print("[AetherPS4] LoadingOverlayWindow: no active window scene, cannot show")
            return
        }

        let state = LoadingOverlayUIState()
        uiState = state

        let overlayWindow = PassthroughWindow(windowScene: scene)
        // .alert + 1: above everything UIKit itself puts up (alerts, action sheets), and
        // above SDL's own window regardless of which one is "key" -- windowLevel ordering
        // is independent of key-window status, unlike relying on presentation z-order.
        overlayWindow.windowLevel = .alert + 1
        overlayWindow.backgroundColor = .clear
        overlayWindow.rootViewController = UIHostingController(
            rootView: LoadingOverlayRootView(gameName: gameName, state: state) { frames in
                overlayWindow.interactiveFrames = frames
            })
        overlayWindow.rootViewController?.view.backgroundColor = .clear
        overlayWindow.isHidden = false
        window = overlayWindow
        print("[AetherPS4] LoadingOverlayWindow: shown")
    }

    /// Call once the game session actually ends -- tears the whole overlay window down
    /// (not just the card content), regardless of whether the user had left it open or
    /// closed at the time.
    static func teardown() {
        uiState?.stopTailing()
        uiState = nil
        window?.isHidden = true
        window = nil
        print("[AetherPS4] LoadingOverlayWindow: torn down")
    }
}

@MainActor
private final class LoadingOverlayUIState: ObservableObject {
    @Published var isCardVisible = true
    @Published var liveText: String = ""
    // Only meaningful when consoleLoggingEnabled is false -- see syntheticTick() below.
    @Published var syntheticProgress: Double = 0

    // Read once at launch, not observed live: changing this setting mid-boot would need to
    // start/stop the tailing timer out from under whatever's currently running, and the
    // setting only exists to control *this* loading session's behavior anyway.
    let consoleLoggingEnabled = UserDefaults.standard.bool(forKey: "consoleLoggingEnabled")

    private var pollTimer: Timer?

    init() {
        // Tailing the log file and re-rendering ~32KB of monospaced text several times a
        // second was confirmed to noticeably lag the app -- reported directly, and no
        // surprise given SwiftUI's diffing cost on a large Text view redrawing every
        // 0.5s. Off by default (see SettingsView's own toggle): when disabled, this skips
        // touching the filesystem or liveText entirely and just drives a synthetic
        // progress estimate instead, so there's no lag source to begin with rather than
        // just hiding the console UI while still paying its cost underneath.
        let timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            if self.consoleLoggingEnabled {
                self.refreshLog()
            } else {
                self.syntheticTick()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
        if consoleLoggingEnabled {
            refreshLog()
        }
    }

    func stopTailing() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    // A "hopeful" loading bar for when there's no console text to derive real milestone
    // progress from: creeps toward 90% and never claims to finish, same reasoning as the
    // real milestone-based progress below (there's no reliable "now it's actually running"
    // signal either way) -- just without anything to key off of.
    private func syntheticTick() {
        syntheticProgress += (0.9 - syntheticProgress) * 0.03
    }

    // Last 32KB of the crash log, tailed live -- same window/cadence GameOverlay.swift's
    // now-unused refresh() used. AetherX's ConsoleLogger tees stdout/stderr through a Pipe
    // for a true live stream instead of polling a file; CrashLogger.swift here already
    // redirects stdout/stderr straight to disk via freopen (see its own header comment on
    // why: unbuffered writes survive a hard crash), so polling the same file is simpler and
    // doesn't need a second redirection layer competing with that one.
    private func refreshLog() {
        guard let documentsURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        else { return }
        let logURL = documentsURL.appendingPathComponent("aether_crash.log")
        guard let handle = try? FileHandle(forReadingFrom: logURL) else { return }
        defer { try? handle.close() }
        let fileSize = (try? handle.seekToEnd()) ?? 0
        let maxBytes: UInt64 = 32 * 1024
        let start = fileSize > maxBytes ? fileSize - maxBytes : 0
        try? handle.seek(toOffset: start)
        if let data = try? handle.readToEnd() {
            liveText = String(decoding: data, as: UTF8.self)
        }
    }
}

private struct LoadingOverlayRootView: View {
    let gameName: String
    @ObservedObject var state: LoadingOverlayUIState
    // Reported to PassthroughWindow every time the collected interactive frames change (see
    // LoadingOverlayWindow.show(), which wires this to the window's own interactiveFrames) --
    // this view has no UIWindow reference of its own, so this is the only path back.
    let onInteractiveFramesChanged: ([CGRect]) -> Void

    var body: some View {
        ZStack {
            if state.isCardVisible {
                LoadingCard(gameName: gameName, state: state)
                    .interactiveFrame(id: "card")
            }

            // X (exit game) and terminal (reopen the card) buttons, both top-center, same
            // placement AetherX uses. X is always available, even mid-load, so a stuck-
            // looking boot isn't a dead end. The reopen button only shows once there's
            // actually a hidden card to reopen.
            VStack {
                HStack(spacing: 20) {
                    Button {
                        shadps4_stop()
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 24))
                            .foregroundColor(.white.opacity(0.7))
                    }
                    if !state.isCardVisible {
                        Button {
                            state.isCardVisible = true
                        } label: {
                            Image(systemName: "terminal.fill")
                                .font(.system(size: 20))
                                .foregroundColor(.white.opacity(0.7))
                        }
                    }
                }
                .padding(.top, 8)
                .interactiveFrame(id: "buttons")
                Spacer()
            }
        }
        .onPreferenceChange(InteractiveFrameKey.self) { frames in
            onInteractiveFramesChanged(Array(frames.values))
        }
    }
}

private struct LoadingCard: View {
    let gameName: String
    @ObservedObject var state: LoadingOverlayUIState
    @State private var copied = false

    // Coarse, heuristic progress -- shadPS4 doesn't report a real percentage, so this
    // estimates how far boot has gotten by checking which known log lines (from actual
    // boot sequences observed on-device this session) have appeared. Deliberately caps
    // below 100%: there's no reliable "now the real game is running" log line to key off
    // of, so this never claims to be finished -- the user closes the card themselves once
    // they can see the game is actually running underneath it.
    private static let milestones: [(String, Double)] = [
        ("SDL video subsystem initialized", 0.10),
        ("Creating SDL Vulkan window", 0.20),
        ("SDL Vulkan window creation returned", 0.30),
        ("TryOpenSDLControllers", 0.35),
        ("InitHLELibs: Initializing HLE libraries", 0.50),
        ("GET_RENDER_FRAME_WAIT frameGetId=0 ", 0.65),
        ("FRAME_SLOT_ACQUIRE presentId=0 ", 0.75),
        ("At the Title Screen", 0.90),
    ]

    private var progress: Double {
        guard state.consoleLoggingEnabled else { return state.syntheticProgress }
        var best = 0.0
        for (marker, value) in Self.milestones where state.liveText.contains(marker) {
            best = max(best, value)
        }
        return best
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Loading \(gameName)")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(.white)
                    Text("\(Int(progress * 100))%")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(.white.opacity(0.6))
                }
                Spacer()
                Button {
                    state.isCardVisible = false
                } label: {
                    Image(systemName: "chevron.down.circle.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.white.opacity(0.5))
                }
            }

            ProgressView(value: progress)
                .tint(.white)

            // Console logging is off by default (see SettingsView) -- tailing the log file
            // and re-rendering it several times a second was confirmed to noticeably lag
            // the app. When it's off, LoadingOverlayUIState never touches the filesystem or
            // populates liveText at all, so this just doesn't render the section rather
            // than showing an empty one.
            if state.consoleLoggingEnabled {
                Divider().background(Color.white.opacity(0.2))

                HStack {
                    Text("Console")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(.white.opacity(0.5))
                    Spacer()
                    Button(action: copyLog) {
                        HStack(spacing: 4) {
                            Image(systemName: copied ? "checkmark" : "doc.on.doc")
                            Text(copied ? "Copied" : "Copy")
                        }
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(.white.opacity(0.8))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(Color.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
                    }
                }

                ScrollViewReader { proxy in
                    ScrollView {
                        Text(state.liveText.isEmpty ? "Waiting for output..." : state.liveText)
                            .font(.system(size: 10, weight: .regular, design: .monospaced))
                            .foregroundColor(.white.opacity(0.85))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                            .id("logEnd")
                    }
                    .frame(height: 220)
                    .onChange(of: state.liveText) { _ in
                        proxy.scrollTo("logEnd", anchor: .bottom)
                    }
                }
            }
        }
        .padding(16)
        .frame(maxWidth: 480)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 16, style: .continuous).stroke(Color.white.opacity(0.15), lineWidth: 1))
        .padding(.horizontal, 24)
    }

    private func copyLog() {
        UIPasteboard.general.string = state.liveText
        copied = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { copied = false }
    }
}
