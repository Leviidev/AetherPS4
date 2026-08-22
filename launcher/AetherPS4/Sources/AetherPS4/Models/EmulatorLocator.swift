import Foundation

/// Finds the native ARM64 AetherPS4/shadPS4 executable this launcher drives.
///
/// The launcher is intentionally a separate project from the emulator core
/// (its own Swift package, its own build system) but lives inside the same
/// repository checkout, so it locates the emulator by walking up from
/// wherever it's currently running/built looking for the known relative
/// build output path, rather than assuming a fixed absolute location.
///
/// Also note: the desktop `shadps4` CLI has no PKG install/extract/metadata
/// support of its own — it only ever takes an already-usable game path via
/// `-g,--game TEXT  Game path or ID` (confirmed via `--help`). For a raw
/// `.pkg`, `EmulatorProcess` extracts it first using the native
/// `aetherps4-pkg-extract` tool (see `PkgExtractorLocator` and
/// `tools/pkg-extract/`) before ever invoking shadps4 -- that extraction
/// logic is the same one already used and tested by the Bachata Android
/// app's native module, built here as a standalone desktop CLI instead of a
/// JNI library. This locator only ever finds and runs the shadps4 binary
/// itself.
enum EmulatorLocator {
    /// Explicit override, e.g. for development/testing or if the build output
    /// moves. Checked before any auto-detection.
    static let overrideEnvironmentKey = "AETHERPS4_EXECUTABLE_PATH"

    /// Relative to a repository checkout root, where `runtime/scripts/build-*`
    /// and this session's own CMake configure both place the built binary.
    private static let knownRelativePath = "runtime/build/shadps4-macos-arm64/shadps4"

    static func locate() -> URL? {
        if let overridden = ProcessInfo.processInfo.environment[overrideEnvironmentKey],
           FileManager.default.isExecutableFile(atPath: overridden) {
            return URL(fileURLWithPath: overridden)
        }
        if let stored = UserDefaults.standard.string(forKey: overrideEnvironmentKey),
           FileManager.default.isExecutableFile(atPath: stored) {
            return URL(fileURLWithPath: stored)
        }

        var searchRoots: [URL] = []
        searchRoots.append(URL(fileURLWithPath: FileManager.default.currentDirectoryPath))
        searchRoots.append(Bundle.main.bundleURL)
        if let executablePath = Bundle.main.executablePath {
            searchRoots.append(URL(fileURLWithPath: executablePath))
        }

        for root in searchRoots {
            if let found = searchUpward(from: root) {
                return found
            }
        }
        return nil
    }

    private static func searchUpward(from start: URL, maxLevels: Int = 12) -> URL? {
        var directory = start.hasDirectoryPath ? start : start.deletingLastPathComponent()
        for _ in 0..<maxLevels {
            let candidate = directory.appendingPathComponent(knownRelativePath)
            if FileManager.default.isExecutableFile(atPath: candidate.path) {
                return candidate
            }
            let parent = directory.deletingLastPathComponent()
            if parent == directory { break }
            directory = parent
        }
        return nil
    }

    static func setOverride(_ path: String?) {
        if let path {
            UserDefaults.standard.set(path, forKey: overrideEnvironmentKey)
        } else {
            UserDefaults.standard.removeObject(forKey: overrideEnvironmentKey)
        }
    }
}
