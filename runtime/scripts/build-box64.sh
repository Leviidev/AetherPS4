#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source_dir="$project_root/runtime/sources/box64"
build_dir="$project_root/runtime/build/box64"
output="$project_root/android/BachataS4/core/runtime/src/main/jniLibs/arm64-v8a/libbox64.so"
entrypoint_patch="$project_root/runtime/patches/box64-winlator-glibc-entrypoint.patch"
vulkan_qcom_patch="$project_root/runtime/patches/box64-vulkan-dispatch-tile-qcom.patch"
adrenotools_vulkan_patch="$project_root/runtime/patches/box64-adrenotools-vulkan.patch"
vex_write_opcode_patch="$project_root/runtime/patches/box64-vex-write-opcode.patch"
native_write_opcode_patch="$project_root/runtime/patches/box64-native-write-opcode.patch"
expected_revision=50c8b90b09b433ab0767de44af2d0731cb0748b7
: "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT must point to a pinned Android NDK}"

test -f "$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
test "$(git -C "$source_dir" rev-parse HEAD)" = "$expected_revision"
if ! git -C "$source_dir" apply --reverse --check "$entrypoint_patch" 2>/dev/null; then
  git -C "$source_dir" diff --quiet
  git -C "$source_dir" apply --check "$entrypoint_patch"
  git -C "$source_dir" apply "$entrypoint_patch"
fi
if ! git -C "$source_dir" apply --reverse --check "$vulkan_qcom_patch" 2>/dev/null; then
  git -C "$source_dir" apply --check "$vulkan_qcom_patch"
  git -C "$source_dir" apply "$vulkan_qcom_patch"
fi
if ! git -C "$source_dir" apply --reverse --check "$adrenotools_vulkan_patch" 2>/dev/null; then
  git -C "$source_dir" apply --check "$adrenotools_vulkan_patch"
  git -C "$source_dir" apply "$adrenotools_vulkan_patch"
fi
if ! git -C "$source_dir" apply --reverse --check "$vex_write_opcode_patch" 2>/dev/null; then
  git -C "$source_dir" apply --check "$vex_write_opcode_patch"
  git -C "$source_dir" apply "$vex_write_opcode_patch"
fi
if ! git -C "$source_dir" apply --reverse --check "$native_write_opcode_patch" 2>/dev/null; then
  git -C "$source_dir" apply --check "$native_write_opcode_patch"
  git -C "$source_dir" apply "$native_write_opcode_patch"
fi
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=31 \
  -DANDROID=ON \
  -DWINLATOR_GLIBC=ON \
  -DARM64=ON \
  -DARM_DYNAREC=ON \
  -DBAD_SIGNAL=ON \
  -DNO_LIB_INSTALL=ON \
  -DNO_CONF_INSTALL=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target box64

binary="$build_dir/box64"
test -f "$binary"
header=$(readelf -h "$binary")
grep -Eq 'Class:[[:space:]]+ELF64' <<<"$header"
grep -Eq 'Type:[[:space:]]+DYN' <<<"$header"
grep -Eq 'Machine:[[:space:]]+AArch64' <<<"$header"
if readelf -d "$binary" | grep -Eq 'Shared library: \[(libc\.so\.6|libpthread\.so\.0|libstdc\+\+\.so\.6|libgcc_s\.so\.1|ld-linux[^]]*)\]'; then
  echo "Box64 depends on an unavailable desktop library" >&2
  exit 1
fi

install -Dm755 "$binary" "$output"
printf 'box64=%s\nrevision=%s\n' "$output" "$expected_revision"
