#!/usr/bin/env bash
set -euo pipefail

# Complete Reproducibility Verification for Bachata-S4
# Builds tree A and tree B under different absolute paths and wall-clock times.
# Compares hashes for:
#   bin/shadps4
#   host/shadps4-arm64-fex
#   host/fexcore-guest-harness
#   host/fexcore-smoke
#   host/box64
#   runtime.zip
#   manifest.json
#   unsigned APK

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
expected_revision=$(git -C "$project_root" rev-parse HEAD 2>/dev/null || true)
source_date_epoch=${SOURCE_DATE_EPOCH:-$(git -C "$project_root" show -s --format=%ct HEAD 2>/dev/null || echo 1785935989)}

a_root=${REPRO_A_ROOT:-/tmp/bachata-repro-a}
b_root=${REPRO_B_ROOT:-/var/tmp/bachata-repro-b}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

echo "=== Compiler and Linker Versions ==="
echo "Clang version:"
clang --version | head -n2 || true
echo "AArch64 GCC version:"
aarch64-linux-gnu-gcc --version | head -n1 || true
echo "Linker version:"
ld --version | head -n1 || true
echo "SOURCE_DATE_EPOCH=$source_date_epoch ($(date -u -d "@$source_date_epoch" '+%Y-%m-%d %H:%M:%S UTC'))"
echo "===================================="

setup_tree() {
  local tree=$1
  echo "==> Preparing tree at $tree"
  rm -rf "$tree"
  mkdir -p "$tree"
  cp -a "$project_root/." "$tree/"
  rm -rf "$tree/runtime/build" "$tree/android/BachataS4/app/build" "$tree/android/BachataS4/.gradle" "$tree/.git"
  find "$tree/android/BachataS4" -type d \( -name ".cxx" -o -name ".kotlin" -o -name "build" \) -prune -exec rm -rf {} + 2>/dev/null || true
  find "$tree/runtime/sources" "$tree/externals" -name ".git" -type f -exec rm -f {} + 2>/dev/null || true
  mkdir -p "$tree/runtime/sources/winlator-app/.git" "$tree/runtime/sources/vortek-client/.git" "$tree/runtime/sources/box64/.git" "$tree/runtime/sources/fex/.git"
}

build_tree() {
  local tree=$1
  echo "==> Building runtime and APK in $tree"
  (
    cd "$tree"
    export SOURCE_DATE_EPOCH="$source_date_epoch"
    export CCACHE_DISABLE=1
    export BOX64_ALLOW_NO_GIT=1
    export REPRO_CHECKOUT_PATH="$tree"
    runtime/scripts/build-runtime-debian.sh
    node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
    node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
    cd android/BachataS4
    export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
    export ANDROID_HOME=/opt/android-sdk
    ./gradlew --no-daemon clean assembleFdroidDebug
    cd ../..
    node runtime/tests/verify-apk-runtime.mjs android/BachataS4/app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk
  )
}

echo "=== Step 1: Building Tree A ($a_root) ==="
setup_tree "$a_root"
build_tree "$a_root"

echo "Sleeping 2 seconds between builds to vary wall-clock time..."
sleep 2

echo "=== Step 2: Building Tree B ($b_root) ==="
setup_tree "$b_root"
build_tree "$b_root"

echo "=== Step 3: Extracting and Comparing Hashes ==="

extract_hashes() {
  local tree=$1
  local apk="$tree/android/BachataS4/app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk"
  local zip="$tree/android/BachataS4/app/src/main/assets/runtime/runtime.zip"
  local manifest="$tree/android/BachataS4/app/src/main/assets/runtime/manifest.json"
  local tmp_dir
  tmp_dir=$(mktemp -d)

  unzip -q -p "$zip" "bin/shadps4" > "$tmp_dir/shadps4"
  unzip -q -p "$zip" "host/shadps4-arm64-fex" > "$tmp_dir/shadps4-arm64-fex"
  unzip -q -p "$zip" "host/fexcore-guest-harness" > "$tmp_dir/fexcore-guest-harness"
  unzip -q -p "$zip" "host/fexcore-smoke" > "$tmp_dir/fexcore-smoke"
  unzip -q -p "$zip" "host/box64" > "$tmp_dir/box64"

  echo "bin/shadps4:                $(sha256sum "$tmp_dir/shadps4" | cut -d' ' -f1)"
  echo "host/shadps4-arm64-fex:     $(sha256sum "$tmp_dir/shadps4-arm64-fex" | cut -d' ' -f1)"
  echo "host/fexcore-guest-harness: $(sha256sum "$tmp_dir/fexcore-guest-harness" | cut -d' ' -f1)"
  echo "host/fexcore-smoke:         $(sha256sum "$tmp_dir/fexcore-smoke" | cut -d' ' -f1)"
  echo "host/box64:                 $(sha256sum "$tmp_dir/box64" | cut -d' ' -f1)"
  echo "runtime.zip:                $(sha256sum "$zip" | cut -d' ' -f1)"
  echo "manifest.json:              $(sha256sum "$manifest" | cut -d' ' -f1)"
  echo "unsigned APK:               $(sha256sum "$apk" | cut -d' ' -f1)"

  rm -rf "$tmp_dir"
}

echo "--- Hashes for Build A ---"
a_hashes=$(extract_hashes "$a_root")
echo "$a_hashes"

echo "--- Hashes for Build B ---"
b_hashes=$(extract_hashes "$b_root")
echo "$b_hashes"

if [[ "$a_hashes" != "$b_hashes" ]]; then
  echo "ERROR: Hashes differ between Build A and Build B!" >&2
  diff -u <(echo "$a_hashes") <(echo "$b_hashes") || true
  fail "Build A and Build B produced non-identical outputs"
fi

echo "SUCCESS: All 8 target artifacts produce identical SHA-256 hashes across different paths and wall-clock times!"
