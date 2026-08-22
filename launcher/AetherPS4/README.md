# AetherPS4 Launcher

A small, native SwiftUI macOS launcher for the AetherPS4/shadPS4 ARM64 emulator built
elsewhere in this repository (`runtime/build/shadps4-macos-arm64/shadps4`). It is a
development/testing frontend -- add `.pkg` references, launch the emulator, and watch
its stdout/stderr live -- not a full commercial frontend, and it does not implement any
emulation itself.

This is a completely separate project from the emulator core: its own Swift package, its
own build system (Swift Package Manager), living in its own `launcher/` directory. It
never modifies, rebuilds, or links against FEXCore or the C++ shadPS4/AetherPS4 codebase
-- it only launches the existing built `shadps4` executable as a child process and reads
its output.

## Requirements

- Apple Silicon Mac, macOS 14+
- Swift 6 toolchain (ships with recent Xcode)
- The emulator itself already built: see the repository root `PORTING.md` for how
  `runtime/build/shadps4-macos-arm64/shadps4` gets built. The launcher looks for it
  there automatically (see "Locating the emulator" below).
- The PKG extractor built: `tools/pkg-extract/build.sh` (needs the emulator build above
  to already exist, since it reuses that build's LibreSSL). Without this, launching a raw
  `.pkg` still works but falls back to the old unextracted behavior -- see "Launch
  command" below.

## Build & run

```sh
# From the repo root, one-time (or after pulling changes to tools/pkg-extract/):
# build the PKG extractor
tools/pkg-extract/build.sh

cd launcher/AetherPS4

# Run directly during development (opens a normal window):
swift run

# Run the test suite (exercises the real shadps4 binary -- see "What's verified" below):
swift test

# Build a proper double-clickable .app bundle for Apple Silicon:
./scripts/build-app.sh
open AetherPS4.app
```

`scripts/build-app.sh` runs `swift build -c release --arch arm64` and assembles
`AetherPS4.app` (binary + `Resources/Info.plist`) next to it. It prints `file`/`lipo`
output at the end so you can confirm the result is genuinely `arm64`, not x86_64.

## What's in the app

- **Library**: a grid of added games. Drag a `.pkg` onto the window, or use **Add
  Game**. Selecting a game shows its name, its package path, a **Launch Game** button,
  and **Remove from Library**.
- **Console**: monospaced, auto-scrolling, live stdout/stderr from the emulator process,
  a **Clear Console** button, and a status badge (running / exited with status N).
- **Settings**: shows the auto-detected emulator executable path, with an optional
  manual override if auto-detection ever fails. Nothing else -- see "Firmware" below for
  why there's no firmware section.

## Locating the emulator

`EmulatorLocator` (`Sources/AetherPS4/Models/EmulatorLocator.swift`) walks upward from
wherever the launcher is currently running (or its current working directory, for `swift
run`) looking for `runtime/build/shadps4-macos-arm64/shadps4` -- the exact path this
session's own build produces. This works whether you run it via `swift run` from
`launcher/AetherPS4/` or as a built `.app` sitting anywhere inside the repo checkout. If
the emulator ever gets built somewhere else, set it explicitly in **Settings**, or export
`AETHERPS4_EXECUTABLE_PATH` before launching.

## Launch command

Before writing any launch code, this session ran `shadps4 --help` and read
`src/main.cpp`'s actual argument handling rather than guessing. The desktop `shadps4`
CLI has exactly one relevant flag: `-g,--game TEXT  Game path or ID`, and it expects an
already-decrypted game path (an extracted directory or eboot.bin) -- it has no PKG
install/extract/metadata step of its own.

That decrypt/extract step does exist in this repository, but originally only inside the
Android app's own native module (`android/BachataS4/core/runtime/src/main/cpp/pkg/`,
JNI-only, not linked into the desktop build). `tools/pkg-extract/` (see that directory's
own comments, and `build.sh`) ports the same crypto/PFS-extraction logic to a small
standalone desktop CLI, `aetherps4-pkg-extract`, swapping only the Android
JNI/javax.crypto RSA bridge for one built on LibreSSL's BIGNUM modexp (already vendored
for shadPS4's own use). It's a separate build from both the launcher and the emulator
core -- it doesn't link FEXCore or the C++ shadPS4 codebase, and building it doesn't
require reconfiguring the main CMake project (it reuses that project's already-built
LibreSSL static library).

When you launch a `.pkg`, `EmulatorProcess` now runs that tool first, extracting into a
per-pkg cache under `~/Library/Application Support/AetherPS4/extracted/` (reused on
later launches of the same game, visible in the Console as
"Using previously extracted game data"), then passes the extracted `eboot.bin`'s path to
shadPS4 -- still exactly `-g <path>`, nothing invented, just no longer pointed at the raw
encrypted `.pkg`. If extraction fails for any reason, it falls back to the old behavior
(passing the raw `.pkg` straight through) so a failure here never blocks launching
entirely.

**Verified against a real game** (Sonic Mania, `CUSA07023`): extraction completes in
under half a second, the extracted `eboot.bin` starts with the real PS4 SELF magic
(`4F 15 3D 1D`), and shadPS4 loads it, resolves its imports, allocates guest memory, and
starts executing the game's own code -- it even renders real frames through the
Vulkan/KosmicKrisp pipeline before hitting the current actual frontier: a crash from an
unimplemented libc function in FEX's guest-HLE layer (`ENOSYS`/78) that the game's own
code doesn't handle gracefully. That's a FEXCore/AetherPS4 core gap, not a PKG or
launcher problem -- see `PORTING.md` for that side of things.

## Firmware

There is deliberately no firmware UI. Confirmed before writing any UI, not assumed:
shadPS4 is fully HLE (high-level emulation) -- PS4 system libraries are reimplemented
directly in `src/core/libraries/*`, and the project's own README states plainly that it
"does not include games, firmware, keys, licenses, or copyrighted system components."
There is no `--firmware`/`--bios` flag in `--help`, and no code path in `src/` that loads
a firmware image. Nothing to manage here.

## What's verified

- `swift build -c release --arch arm64` succeeds; `scripts/build-app.sh` produces a
  genuine `arm64` `.app` (checked with `file`/`lipo -archs`).
- The built `.app` launches and stays running with no crash report.
- `swift test` (`Tests/AetherPS4Tests/`) exercises the real logic behind every button,
  against the real `shadps4` binary, not mocks:
  - `GameLibraryTests`: adding a `.pkg` reference makes it appear in the library
    immediately and persists it across a fresh `GameLibrary` instance (simulating an app
    relaunch); duplicate adds are rejected; removing a game removes only the reference;
    a game whose file has been deleted is marked unavailable, never silently dropped.
  - `EmulatorProcessTests`: `EmulatorLocator` finds the real built executable;
    `launch()` actually starts it, real stdout text from shadPS4's own startup banner
    appears live in the captured console within seconds, and `stop()` actually
    terminates the real running process.
- `fexcore-smoke` and `fexcore-guest-harness` (the FEXCore ARM64 JIT verification from
  earlier in this port) were re-run after building this launcher and still both pass
  fully -- this launcher never touches FEXCore or the C++ emulator core.
- `PkgExtractCLITests` exercises the real `aetherps4-pkg-extract` binary as a process
  (found via `PkgExtractorLocator`, run against a non-PKG file to confirm it reports a
  real header-validation failure rather than hanging). Real PKG decryption/extraction
  was verified manually against an actual game (see "Launch command" above) since a real
  PKG can't be a committed test fixture.
