#!/usr/bin/env bash
# Builds AetherPS4.app (the launcher) for Apple Silicon macOS and assembles a
# proper double-clickable .app bundle around the SwiftPM executable.
set -euo pipefail

launcher_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$launcher_root"

config="${1:-release}"

swift build -c "$config" --arch arm64

bin_path=$(swift build -c "$config" --arch arm64 --show-bin-path)
binary="$bin_path/AetherPS4"

app_bundle="$launcher_root/AetherPS4.app"
rm -rf "$app_bundle"
mkdir -p "$app_bundle/Contents/MacOS"
mkdir -p "$app_bundle/Contents/Resources"

cp "$binary" "$app_bundle/Contents/MacOS/AetherPS4"
cp "$launcher_root/Resources/Info.plist" "$app_bundle/Contents/Info.plist"

echo "Built: $app_bundle"
file "$app_bundle/Contents/MacOS/AetherPS4"
lipo -archs "$app_bundle/Contents/MacOS/AetherPS4"
