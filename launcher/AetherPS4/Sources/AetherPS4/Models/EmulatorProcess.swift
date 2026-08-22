import CryptoKit
import Foundation
import Observation

enum EmulatorRunState: Equatable {
    case idle
    /// Running the native `aetherps4-pkg-extract` tool (see
    /// `tools/pkg-extract/`) to turn a raw `.pkg` into a game directory
    /// shadPS4 can actually load, before shadps4 itself has started.
    case extracting
    case running
    case exited(status: Int32)
}

/// One line of console output, tagged by which stream it came from so the
/// console view can style stderr differently if it wants to.
struct ConsoleLine: Identifiable {
    enum Stream {
        case stdout
        case stderr
    }

    let id = UUID()
    let stream: Stream
    let text: String
}

/// Drives the real `shadps4` executable as a child process and streams its
/// output back live. Nothing about PS4 emulation happens in this class or
/// anywhere else in the launcher — it only starts/stops a process and reads
/// bytes from two pipes.
@MainActor
@Observable
final class EmulatorProcess {
    private(set) var state: EmulatorRunState = .idle
    private(set) var consoleLines: [ConsoleLine] = []
    private(set) var runningGameName: String?

    private var process: Process?
    private var stdoutPipe: Pipe?
    private var stderrPipe: Pipe?
    private var extractionPipe: Pipe?
    private var lastProgressPercent: Int = -1
    private var extractionWasCancelled = false

    var isRunning: Bool {
        if case .running = state { return true }
        return false
    }

    /// True while either the pkg extractor or shadPS4 itself is active —
    /// what the UI should use to disable "Launch" on other games / show a
    /// Stop-or-Cancel button, as opposed to `isRunning`, which callers that
    /// specifically care about shadPS4's own process (like the tests) use.
    var isBusy: Bool {
        switch state {
        case .running, .extracting: return true
        case .idle, .exited: return false
        }
    }

    /// Launches the given game. If `pkgPath` is a raw `.pkg`, this first
    /// extracts it (via the native `aetherps4-pkg-extract` tool built from
    /// `tools/pkg-extract/` — see that directory's own comments for why:
    /// shadPS4's desktop CLI has no PKG install step of its own, only
    /// `-g,--game TEXT  Game path or ID` for an already-decrypted eboot/game
    /// directory) into a cache directory, reusing a previous extraction if
    /// one already exists for this exact `.pkg` path. Non-`.pkg` paths (an
    /// already-extracted folder, a raw eboot.bin) are passed straight to
    /// shadPS4 untouched, exactly as before.
    func launch(executable: URL, pkgPath: String, gameName: String) {
        guard !isBusy else { return }

        guard pkgPath.lowercased().hasSuffix(".pkg") else {
            performLaunch(executable: executable, gamePath: pkgPath, gameName: gameName)
            return
        }

        let cachedEboot = Self.extractedEbootURL(forPkgPath: pkgPath)
        if FileManager.default.fileExists(atPath: cachedEboot.path) {
            appendLine(.stdout, "[AetherPS4] Using previously extracted game data.")
            performLaunch(executable: executable, gamePath: cachedEboot.path, gameName: gameName)
            return
        }

        guard let extractorTool = PkgExtractorLocator.locate() else {
            appendLine(
                .stderr,
                "[AetherPS4] Couldn't find aetherps4-pkg-extract (build it via tools/pkg-extract/build.sh). Passing the raw .pkg to shadPS4 instead."
            )
            performLaunch(executable: executable, gamePath: pkgPath, gameName: gameName)
            return
        }

        beginExtraction(
            tool: extractorTool, pkgPath: pkgPath, outputDirectory: cachedEboot.deletingLastPathComponent(),
            fallbackExecutable: executable, cachedEboot: cachedEboot, gameName: gameName)
    }

    func stop() {
        guard let process, process.isRunning else { return }
        if case .extracting = state {
            extractionWasCancelled = true
        }
        process.terminate()
    }

    func clearConsole() {
        consoleLines.removeAll()
    }

    // MARK: - Private

    /// Where a `.pkg`'s extracted contents are cached, keyed by a hash of its
    /// absolute path so the same game re-launches instantly after the first
    /// extraction. Lives next to the game library's own JSON store.
    private static func extractedEbootURL(forPkgPath pkgPath: String) -> URL {
        let digest = SHA256.hash(data: Data(pkgPath.utf8))
        let key = digest.map { String(format: "%02x", $0) }.joined()
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("AetherPS4", isDirectory: true)
            .appendingPathComponent("extracted", isDirectory: true)
            .appendingPathComponent(key, isDirectory: true)
        return base.appendingPathComponent("eboot.bin")
    }

    private func beginExtraction(
        tool: URL, pkgPath: String, outputDirectory: URL, fallbackExecutable: URL, cachedEboot: URL,
        gameName: String
    ) {
        let process = Process()
        process.executableURL = tool
        process.arguments = ["extract", pkgPath, outputDirectory.path]

        let combinedPipe = Pipe()
        process.standardOutput = combinedPipe
        process.standardError = combinedPipe

        appendLine(.stdout, "[AetherPS4] Extracting \(gameName)…")
        lastProgressPercent = -1

        combinedPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            Task { @MainActor [weak self] in
                self?.appendExtractionOutput(data)
            }
        }

        process.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                self?.finishExtraction(
                    finished, executable: fallbackExecutable, pkgPath: pkgPath, cachedEboot: cachedEboot,
                    gameName: gameName)
            }
        }

        do {
            try process.run()
        } catch {
            appendLine(.stderr, "[AetherPS4] Failed to start pkg extractor: \(error.localizedDescription)")
            state = .exited(status: -1)
            return
        }

        self.process = process
        self.extractionPipe = combinedPipe
        self.runningGameName = gameName
        self.state = .extracting
    }

    private func appendExtractionOutput(_ data: Data) {
        guard let text = String(data: data, encoding: .utf8), !text.isEmpty else { return }
        for rawLine in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let line = String(rawLine)
            if line.hasPrefix("PROGRESS ") {
                let fields = line.dropFirst("PROGRESS ".count).split(separator: " ", maxSplits: 2)
                guard fields.count >= 2, let done = UInt64(fields[0]), let total = UInt64(fields[1]), total > 0
                else { continue }
                let percent = Int(min(done * 100 / total, 100))
                // One console line per 10% step, not one per block -- a real
                // extraction emits hundreds of PROGRESS lines per second.
                if percent / 10 != lastProgressPercent / 10 {
                    lastProgressPercent = percent
                    appendLine(.stdout, "[AetherPS4] Extracting… \(percent)%")
                }
            } else if line.hasPrefix("STATUS ") {
                continue
            } else {
                appendLine(.stdout, line)
            }
        }
    }

    private func finishExtraction(
        _ finished: Process, executable: URL, pkgPath: String, cachedEboot: URL, gameName: String
    ) {
        extractionPipe?.fileHandleForReading.readabilityHandler = nil
        extractionPipe = nil
        process = nil

        if extractionWasCancelled {
            extractionWasCancelled = false
            appendLine(.stdout, "[AetherPS4] Extraction cancelled.")
            state = .exited(status: finished.terminationStatus)
            return
        }

        let status = finished.terminationStatus
        guard status == 0, FileManager.default.fileExists(atPath: cachedEboot.path) else {
            appendLine(
                .stderr,
                "[AetherPS4] PKG extraction failed (exit \(status)). Passing the raw .pkg to shadPS4 instead — expect a loader error."
            )
            performLaunch(executable: executable, gamePath: pkgPath, gameName: gameName)
            return
        }

        appendLine(.stdout, "[AetherPS4] Extraction complete.")
        performLaunch(executable: executable, gamePath: cachedEboot.path, gameName: gameName)
    }

    /// Launches `shadps4 -g <gamePath>` (the real, existing CLI flag —
    /// `-g,--game TEXT  Game path or ID` per `shadps4 --help`; nothing here is
    /// an invented argument) against an already-usable game path: either a
    /// non-`.pkg` path passed straight through, or the eboot.bin produced by
    /// `beginExtraction` above.
    private func performLaunch(executable: URL, gamePath: String, gameName: String) {
        let process = Process()
        process.executableURL = executable
        
        var arguments = ["-g", gamePath]
        if UserDefaults.standard.object(forKey: "showFpsCounter") as? Bool ?? true {
            arguments.append("--show-fps")
        }
        process.arguments = arguments
        process.environment = launchEnvironment(besideExecutable: executable)

        let outPipe = Pipe()
        let errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe

        appendLine(.stdout, "$ \(executable.path) \(arguments.joined(separator: " "))")

        outPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            Task { @MainActor [weak self] in
                self?.appendOutput(data, stream: .stdout)
            }
        }
        errPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            Task { @MainActor [weak self] in
                self?.appendOutput(data, stream: .stderr)
            }
        }

        process.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                self?.handleTermination(finished)
            }
        }

        do {
            try process.run()
        } catch {
            appendLine(.stderr, "Failed to launch shadPS4: \(error.localizedDescription)")
            state = .exited(status: -1)
            return
        }

        self.process = process
        self.stdoutPipe = outPipe
        self.stderrPipe = errPipe
        self.runningGameName = gameName
        self.state = .running
    }

    /// shadPS4's own build bundles the KosmicKrisp Vulkan driver (and an ICD
    /// manifest naming it) right next to the executable, but the system
    /// Vulkan loader doesn't know to look there on its own -- absent an
    /// explicit `VK_ICD_FILENAMES`/`VK_DRIVER_FILES`, it falls back to
    /// whatever's registered system-wide, which on a dev machine with
    /// Homebrew's `molten-vk` installed is MoltenVK (self-described as a
    /// "Vulkan Portability" implementation -- matches the exact wording
    /// shadPS4 logs when this goes wrong: "Installed Vulkan Portability
    /// library doesn't implement the VK_KHR_surface extension"). Pointing
    /// both env vars at the bundled manifest, when it's actually present
    /// next to the executable, makes the loader use the driver this build
    /// was actually built and tested against instead.
    private func launchEnvironment(besideExecutable executable: URL) -> [String: String] {
        var environment = ProcessInfo.processInfo.environment
        let icdManifest = executable.deletingLastPathComponent().appendingPathComponent("kosmickrisp_mesa_icd.json")
        if FileManager.default.fileExists(atPath: icdManifest.path) {
            environment["VK_ICD_FILENAMES"] = icdManifest.path
            environment["VK_DRIVER_FILES"] = icdManifest.path
        }
        return environment
    }

    private func appendOutput(_ data: Data, stream: ConsoleLine.Stream) {
        guard let text = String(data: data, encoding: .utf8), !text.isEmpty else { return }
        // shadPS4 emits multiple newline-terminated log lines per read; split
        // so the console renders one ConsoleLine per line rather than one
        // giant blob per buffer flush.
        let lines = text.split(separator: "\n", omittingEmptySubsequences: true)
        for line in lines {
            appendLine(stream, String(line))
        }
    }

    private func appendLine(_ stream: ConsoleLine.Stream, _ text: String) {
        consoleLines.append(ConsoleLine(stream: stream, text: text))
    }

    private func handleTermination(_ finished: Process) {
        stdoutPipe?.fileHandleForReading.readabilityHandler = nil
        stderrPipe?.fileHandleForReading.readabilityHandler = nil
        // Drain anything left in the pipe buffers after the readability
        // handler stops firing but before the process object is released.
        if let remaining = try? stdoutPipe?.fileHandleForReading.readToEnd() {
            appendOutput(remaining, stream: .stdout)
        }
        if let remaining = try? stderrPipe?.fileHandleForReading.readToEnd() {
            appendOutput(remaining, stream: .stderr)
        }

        let status = finished.terminationStatus
        appendLine(.stdout, "[AetherPS4] shadPS4 exited with status \(status)")
        state = .exited(status: status)
        process = nil
        stdoutPipe = nil
        stderrPipe = nil
    }
}
