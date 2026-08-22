import XCTest
@testable import AetherPS4

/// Exercises the real launch/console-capture/exit-status path against the
/// actual built AetherPS4/shadPS4 executable -- no PS4 game content is
/// available in this environment, so these tests launch it with a
/// placeholder `.pkg` path and verify the *mechanism* (process starts, real
/// output streams in live, a real exit status is observed), not that a game
/// boots.
@MainActor
final class EmulatorProcessTests: XCTestCase {
    func testLocatorFindsRealExecutable() throws {
        guard let executable = EmulatorLocator.locate() else {
            throw XCTSkip("shadps4 executable not found in this checkout; build it first")
        }
        XCTAssertTrue(FileManager.default.isExecutableFile(atPath: executable.path))
    }

    /// shadPS4 doesn't abort cleanly given an invalid game path: it logs a
    /// loader error, then still proceeds through full startup (memory
    /// manager, SDL/Vulkan window) and keeps running on other threads even
    /// after a critical error on its main thread -- verified manually before
    /// writing this test (real console capture showed
    /// "Unable to read self header!" then "Unreachable code! Failed to
    /// create window handle: ..." and the process was still alive seconds
    /// later). So this test verifies the real, observed behavior: the
    /// process starts, real output streams into the console live (including
    /// that real error text), and Stop() actually terminates it -- rather
    /// than assuming a clean, prompt exit that isn't what actually happens.
    ///
    /// A `.pkg`-suffixed path now goes through `aetherps4-pkg-extract`
    /// first (see `PkgExtractorLocator`/`tools/pkg-extract/`). This test's
    /// placeholder file isn't a real PKG, so that extraction step fails fast
    /// on the bad header -- `EmulatorProcess` then falls back to passing the
    /// raw path straight to shadPS4 exactly as it always did, which is what
    /// this test actually exercises (the pipe-capture/stop mechanism against
    /// the real binary, not real PKG decryption -- see
    /// `PkgExtractCLITests` in this target for that).
    func testLaunchStartsProcessCapturesLiveConsoleOutputAndStopTerminatesIt() throws {
        guard let executable = EmulatorLocator.locate() else {
            throw XCTSkip("shadps4 executable not found in this checkout; build it first")
        }

        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("AetherPS4Tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }
        let fakePkg = tempDir.appendingPathComponent("NotARealGame.pkg")
        try Data("placeholder".utf8).write(to: fakePkg)

        let emulator = EmulatorProcess()
        XCTAssertEqual(emulator.state, .idle)

        emulator.launch(executable: executable, pkgPath: fakePkg.path, gameName: "NotARealGame")
        XCTAssertTrue(emulator.isBusy, "should be extracting or running immediately after launch() returns")
        XCTAssertFalse(emulator.consoleLines.isEmpty, "the extraction attempt should already be logged")

        // Wait for real stdout/stderr to actually stream in live -- proves
        // the pipe-capture mechanism the Console view relies on works against
        // the real binary, not just that the process object exists. Give the
        // failed-extraction fallback room to run first.
        let gotRealOutput = wait(
            for: { emulator.consoleLines.contains { $0.text.contains("shadps4 emulator") } },
            timeoutSeconds: 20
        )
        XCTAssertTrue(gotRealOutput, "expected shadPS4's own real startup log line to appear live in the console")
        XCTAssertTrue(emulator.isRunning, "shadPS4 is expected to still be running at this point, not self-terminate")

        emulator.stop()
        let stopped = wait(
            for: { if case .exited = emulator.state { true } else { false } },
            timeoutSeconds: 15
        )
        XCTAssertTrue(stopped, "Stop Game should terminate the running process")
        XCTAssertFalse(emulator.isRunning)
    }

    func testStopIsSafeWhenNothingIsRunning() throws {
        let emulator = EmulatorProcess()
        emulator.stop()
        XCTAssertEqual(emulator.state, .idle)
    }

    /// Polls `condition` on the main run loop until it's true or `timeoutSeconds`
    /// elapses. `Process.terminationHandler` fires asynchronously, so tests need
    /// to pump the run loop rather than block it.
    private func wait(for condition: () -> Bool, timeoutSeconds: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeoutSeconds)
        while Date() < deadline {
            if condition() { return true }
            RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.05))
        }
        return condition()
    }
}
