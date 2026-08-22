import Foundation

/// Finds the native `aetherps4-pkg-extract` tool (source: `tools/pkg-extract/`
/// at the repo root, built by `tools/pkg-extract/build.sh`). That tool turns
/// a raw, encrypted `.pkg` into a plain game directory (sce_sys/, eboot.bin)
/// that shadPS4's existing `-g <path>` flag can load directly -- the same
/// PKG-extraction logic already used and tested by the Bachata Android app's
/// native module, just built here as a small standalone desktop CLI instead
/// of a JNI library, since the desktop `shadps4` build has no PKG-handling
/// code of its own. Mirrors `EmulatorLocator`'s search strategy exactly.
enum PkgExtractorLocator {
    static let overrideEnvironmentKey = "AETHERPS4_PKG_EXTRACT_PATH"

    private static let knownRelativePath = "tools/pkg-extract/aetherps4-pkg-extract"

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
