import SwiftUI

// Shown the instant a game launch starts (ContentView, tied to EmulatorProcess.isRunning),
// in the app's own existing window -- not a separate UIWindow, and not attached to SDL's.
//
// This can't live-update (no progress text, no scrolling console): SwiftUI processing is
// completely frozen for the entire duration shadps4_run() runs on the main thread, an
// already-established fact of this architecture (see EmulatorProcess.launch()'s own
// comment: "SwiftUI won't be able to process taps... again until shadps4_run() returns").
// Multiple attempts at a live-updating version (a repeating Timer on RunLoop.main, a
// background poll that dispatches new work to the main thread once SDL's window is found)
// each confirmed a different way that no *new* work can be scheduled onto the main thread
// once shadPS4 owns it -- one crashed the app outright, the other's dispatched work
// silently never ran, even across an 11,000-line, clearly-long session.
//
// What actually works: present this before calling shadps4_run(), using ordinary,
// already-reliable SwiftUI presentation, AFTER EmulatorProcess.launch() has already
// requested the landscape rotation and waited for it to finish animating (see launch()'s
// own comment -- presenting this concurrently with an in-flight forced rotation is what
// was making it disappear once the rotation settled, confirmed on-device across multiple
// attempts at timing the two against each other). SDL creates its own new UIWindow once it
// starts up and that window naturally ends up on top by normal z-order once it's shown,
// covering this one automatically -- no explicit dismiss needed. The visible result is a
// frozen "Loading..." screen for as long as boot takes, then the game's own video
// underneath it becomes visible once SDL's window appears in front.
struct GameLoadingCoverView: View {
    let gameName: String?

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 20) {
                ProgressView()
                    .controlSize(.large)
                    .tint(.white)
                Text(gameName.map { "Loading \($0)…" } ?? "Loading game…")
                    .font(.title3.bold())
                    .foregroundStyle(.white)
                Text("Do not tap or leave the app")
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.7))
            }
        }
    }
}
