# Bachata S4 Android

The Android app is scaffolded with the `create-android@0.1.0` multi-module
template in `android/BachataS4`.

This directory is the APK source for Bachata S4. Build it from the repository
root with the runtime build scripts (`runtime/scripts`) and the Gradle wrapper
here. The Gradle build packages existing runtime assets; it does not generate
them.

## Runtime settings

- Global defaults and sparse per-game overrides cover all exported shadPS4 JSON settings and all supported `BOX64_*` variables.
- Box64 offers the official `safest`, `safe`, `default`, `fast`, and `fastest` profiles. `custom` exposes every individual Box64 flag for fine-tuning.
- Profiles support typed editing, search, explicit raw shadPS4 JSON/Box64 validation, and JSON-only import/export.
- Four controller slots support persistent device identity, remapping, dead zones, inversion, triggers, vibration, and motion preferences.
- Touch controls support saved global/per-game layouts, movement, resizing, visibility, z-order, opacity, scale, vibration, and stick centering.

## Turnip drivers

Turnip is not bundled. The driver manager lists trusted emulator ZIP releases from [`JICA98/bachata-s4-drivers`](https://github.com/JICA98/bachata-s4-drivers), downloads them with size/hash/package validation, and supports local ZIP import. Installed versions can be selected globally or per game and remain usable offline.

## Legal content

Use only game content, firmware, keys, and other assets that you are legally
authorized to use. Do not commit or distribute copyrighted third-party content
with this project.
