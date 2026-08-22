import XCTest
@testable import AetherPS4

/// Exercises the real `aetherps4-pkg-extract` binary (built from
/// `tools/pkg-extract/` via that directory's `build.sh`) directly as a
/// process, the same way `EmulatorProcess` invokes it. No real PS4 PKG is
/// available as a committed test fixture (real ones are large, copyrighted
/// game data), so this covers the mechanism -- the tool is found, runs, and
/// reports a real parse failure for a non-PKG file -- rather than real
/// decryption, which was verified manually against an actual game PKG.
final class PkgExtractCLITests: XCTestCase {
    func testLocatorFindsRealExecutable() throws {
        guard let tool = PkgExtractorLocator.locate() else {
            throw XCTSkip("aetherps4-pkg-extract not found; build it via tools/pkg-extract/build.sh first")
        }
        XCTAssertTrue(FileManager.default.isExecutableFile(atPath: tool.path))
    }

    func testProbeRejectsNonPkgFileWithoutHanging() throws {
        guard let tool = PkgExtractorLocator.locate() else {
            throw XCTSkip("aetherps4-pkg-extract not found; build it via tools/pkg-extract/build.sh first")
        }

        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("AetherPS4Tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }
        let notAPkg = tempDir.appendingPathComponent("NotARealGame.pkg")
        try Data("placeholder".utf8).write(to: notAPkg)

        let process = Process()
        process.executableURL = tool
        process.arguments = ["probe", notAPkg.path]
        let outPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = outPipe

        try process.run()
        process.waitUntilExit()

        XCTAssertNotEqual(process.terminationStatus, 0, "a garbage file should fail PKG header validation")
        let output = String(data: outPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        XCTAssertTrue(output.contains("Invalid PKG header"), "expected the real header-validation error, got: \(output)")
    }
}
