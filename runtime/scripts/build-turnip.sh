#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
component_lock="$project_root/runtime/locks/components.lock.json"
mesa_revision=$(
  node -e '
    const lock = require(process.argv[1]);
    const mesa = lock.components.find((component) => component.name === "mesa");
    if (!mesa) process.exit(1);
    process.stdout.write(mesa.revision);
  ' "$component_lock"
)
mesa_source_url=${MESA_SOURCE_URL:-https://gitlab.freedesktop.org/mesa/mesa.git}

"$project_root/runtime/scripts/checkout-component.sh" \
  mesa \
  "$mesa_source_url" \
  "$mesa_revision"

ndk_root=${ANDROID_NDK_ROOT:-${ANDROID_HOME:-/home/jica/Android/Sdk}/ndk/29.0.13599879}
if [[ ! -d "$ndk_root" ]]; then
  echo "Android NDK 29 required at $ndk_root; set ANDROID_NDK_ROOT" >&2
  exit 1
fi
command -v meson >/dev/null || { echo "meson is required to build Turnip" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja is required to build Turnip" >&2; exit 1; }

build_dir="$project_root/runtime/build/turnip-arm64"
install_dir="$project_root/runtime/build/turnip-install"
cross_file="$project_root/runtime/build/turnip-android-aarch64.ini"
rm -rf "$build_dir" "$install_dir"
mkdir -p "$(dirname "$cross_file")" "$install_dir"

cat >"$cross_file" <<EOF
[binaries]
c = '$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang'
cpp = '$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang++'
ar = '$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
strip = '$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
pkgconfig = 'false'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

meson setup "$build_dir" "$project_root/runtime/sources/mesa" \
  --cross-file "$cross_file" \
  --prefix "$install_dir" \
  --buildtype release \
  -Dplatforms=android \
  -Dandroid-stub=true \
  -Dandroid-libbacktrace=disabled \
  -Dplatform-sdk-version=31 \
  -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=kgsl \
  -Dgallium-drivers= \
  -Degl=disabled \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dopengl=false \
  -Dllvm=disabled \
  -Dshared-llvm=disabled \
  -Dbuild-tests=false

ninja -C "$build_dir" install
find "$install_dir" -type f \( -name 'libvulkan_freedreno.so' -o -name '*.json' \) -print
