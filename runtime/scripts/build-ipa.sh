#!/usr/bin/env bash
# Builds AetherPS4-iOS (unsigned, CODE_SIGNING_ALLOWED=NO — the sideloading tool
# does the real signing) and packages it into an .ipa on the Desktop.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT_DIR="$REPO_ROOT/AetherPS4-iOS"
SCHEME="AetherPS4-iOS"
CONFIGURATION="Release"
DEST_DIR="${1:-$HOME/Desktop}"

cd "$PROJECT_DIR"

echo "==> Building $SCHEME ($CONFIGURATION, iphoneos, unsigned)"
xcodebuild \
    -project "$SCHEME.xcodeproj" \
    -scheme "$SCHEME" \
    -configuration "$CONFIGURATION" \
    -sdk iphoneos \
    -destination "generic/platform=iOS" \
    CODE_SIGNING_ALLOWED=NO \
    build

echo "==> Locating build product"
BUILD_DIR=$(xcodebuild \
    -project "$SCHEME.xcodeproj" \
    -scheme "$SCHEME" \
    -configuration "$CONFIGURATION" \
    -sdk iphoneos \
    -showBuildSettings 2>/dev/null | awk -F'= ' '/ CONFIGURATION_BUILD_DIR =/ {print $2; exit}')
APP_PATH="$BUILD_DIR/$SCHEME.app"

if [[ ! -d "$APP_PATH" ]]; then
    echo "error: built app not found at $APP_PATH" >&2
    exit 1
fi

echo "==> Packaging IPA from $APP_PATH"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

mkdir -p "$WORK_DIR/Payload"
cp -R "$APP_PATH" "$WORK_DIR/Payload/"

mkdir -p "$DEST_DIR"
IPA_PATH="$DEST_DIR/$SCHEME.ipa"
rm -f "$IPA_PATH"

(cd "$WORK_DIR" && zip -qr "$IPA_PATH" Payload)

echo "==> Done: $IPA_PATH"
