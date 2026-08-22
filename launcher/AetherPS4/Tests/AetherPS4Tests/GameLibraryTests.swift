import XCTest
@testable import AetherPS4

@MainActor
final class GameLibraryTests: XCTestCase {
    private var tempDir: URL!

    override func setUpWithError() throws {
        tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("AetherPS4Tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: tempDir)
    }

    private func makeDummyPkg(named name: String = "TestGame.pkg") throws -> URL {
        let url = tempDir.appendingPathComponent(name)
        try Data("not a real pkg, just enough bytes to exist".utf8).write(to: url)
        return url
    }

    func testAddGameAppearsInLibrary() throws {
        let store = tempDir.appendingPathComponent("library.json")
        let library = GameLibrary(storeURL: store)
        let pkg = try makeDummyPkg()

        XCTAssertTrue(library.games.isEmpty)
        let added = library.addGame(pkgPath: pkg.path)
        XCTAssertNotNil(added)
        XCTAssertEqual(library.games.count, 1)
        XCTAssertEqual(library.games.first?.pkgPath, pkg.path)
        XCTAssertEqual(library.games.first?.name, "TestGame")
        XCTAssertTrue(library.games.first?.isAvailable ?? false)
    }

    func testAddingSameGameTwiceDoesNotDuplicate() throws {
        let store = tempDir.appendingPathComponent("library.json")
        let library = GameLibrary(storeURL: store)
        let pkg = try makeDummyPkg()

        XCTAssertNotNil(library.addGame(pkgPath: pkg.path))
        XCTAssertNil(library.addGame(pkgPath: pkg.path))
        XCTAssertEqual(library.games.count, 1)
    }

    func testLibraryPersistsAcrossInstances() throws {
        let store = tempDir.appendingPathComponent("library.json")
        let pkg = try makeDummyPkg()

        let first = GameLibrary(storeURL: store)
        first.addGame(pkgPath: pkg.path)

        let second = GameLibrary(storeURL: store)
        XCTAssertEqual(second.games.count, 1)
        XCTAssertEqual(second.games.first?.pkgPath, pkg.path)
    }

    func testRemoveGame() throws {
        let store = tempDir.appendingPathComponent("library.json")
        let library = GameLibrary(storeURL: store)
        let pkg = try makeDummyPkg()

        guard let added = library.addGame(pkgPath: pkg.path) else {
            return XCTFail("expected game to be added")
        }
        library.removeGame(added)
        XCTAssertTrue(library.games.isEmpty)
    }

    func testMissingFileIsMarkedUnavailableNotDeleted() throws {
        let store = tempDir.appendingPathComponent("library.json")
        let library = GameLibrary(storeURL: store)
        let pkg = try makeDummyPkg()

        library.addGame(pkgPath: pkg.path)
        try FileManager.default.removeItem(at: pkg)

        // Still present in the library -- not silently deleted.
        XCTAssertEqual(library.games.count, 1)
        XCTAssertFalse(library.games.first?.isAvailable ?? true)
    }
}
