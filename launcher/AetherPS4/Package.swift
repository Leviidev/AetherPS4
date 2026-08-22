// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "AetherPS4",
    platforms: [
        .macOS(.v14)
    ],
    targets: [
        .executableTarget(
            name: "AetherPS4",
            path: "Sources/AetherPS4"
        ),
        .testTarget(
            name: "AetherPS4Tests",
            dependencies: ["AetherPS4"]
        )
    ]
)
