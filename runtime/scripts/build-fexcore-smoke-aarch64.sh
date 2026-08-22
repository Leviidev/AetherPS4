#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
LOCK_PATH="${PROJECT_ROOT}/runtime/locks/components.lock.json"
FEX_SOURCE="${PROJECT_ROOT}/runtime/sources/fex"
BUILD_DIR="${PROJECT_ROOT}/runtime/build/fexcore-smoke-build"
STAGE_DIR="${PROJECT_ROOT}/runtime/build/fexcore-smoke-stage"
PATCH_PATH="${PROJECT_ROOT}/runtime/patches/fex-fexcore-only.patch"
SMOKE_SOURCE="${PROJECT_ROOT}/runtime/probes/fexcore-smoke.cpp"
VERIFIER="${PROJECT_ROOT}/runtime/tests/verify-fexcore-smoke-build.mjs"
GUEST_ENGINE_SOURCE="${PROJECT_ROOT}/src/core/fex/fex_guest_engine.cpp"
FEX_GUEST_CPU_SOURCE="${PROJECT_ROOT}/src/core/guest_cpu/fex_guest_cpu.cpp"
HLE_CALL_ADAPTER_SOURCE="${PROJECT_ROOT}/src/core/guest_cpu/hle_call_adapter.cpp"
FEX_HLE_BRIDGE_SOURCE="${PROJECT_ROOT}/src/core/guest_cpu/fex_hle_bridge.cpp"
GUEST_HARNESS_SOURCE="${PROJECT_ROOT}/runtime/probes/fexcore-guest-harness.cpp"
GUEST_HARNESS_VERIFIER="${PROJECT_ROOT}/runtime/tests/verify-fexcore-guest-harness-build.mjs"
PATCH_APPLIED=0

cleanup_patch() {
  local status=$?
  if [[ ${PATCH_APPLIED} -eq 1 ]] && ! git -C "${FEX_SOURCE}" apply -R "${PATCH_PATH}"; then
    echo "failed to reverse temporary FEXCore build patch" >&2
    if [[ ${status} -eq 0 ]]; then
      status=1
    fi
  fi
  trap - EXIT
  exit "${status}"
}
trap cleanup_patch EXIT

mapfile -t FEX_LOCK < <(node -e '
const { readFileSync } = require("node:fs");
const lock = JSON.parse(readFileSync(process.argv[1], "utf8"));
const component = lock.components?.find(({ name }) => name === "fex");
if (!component || !/^https:\/\//.test(component.url) || !/^[0-9a-f]{40}$/.test(component.revision)) process.exit(1);
console.log(component.url);
console.log(component.revision);
' "${LOCK_PATH}")

if [[ ${#FEX_LOCK[@]} -ne 2 ]]; then
  echo "invalid fex component lock" >&2
  exit 1
fi
FEX_URL=${FEX_LOCK[0]}
FEX_REVISION=${FEX_LOCK[1]}

bash "${PROJECT_ROOT}/runtime/scripts/checkout-component.sh" fex "${FEX_URL}" "${FEX_REVISION}"

FEX_SUBMODULES=(
  External/unordered_dense
  External/rpmalloc
  External/xxhash
  External/fmt
  External/range-v3
  Source/Common/cpp-optparse
)
git -C "${FEX_SOURCE}" submodule update --init --depth 1 --jobs 8 -- "${FEX_SUBMODULES[@]}"

if [[ $(git -C "${FEX_SOURCE}" rev-parse HEAD) != "${FEX_REVISION}" ]]; then
  echo "fex checkout does not match components lock" >&2
  exit 1
fi
git -C "${FEX_SOURCE}" submodule foreach --quiet --recursive '
  if [ -n "$(git status --porcelain)" ]; then
    echo "dirty fex submodule: $name" >&2
    exit 1
  fi
'

git -C "${FEX_SOURCE}" apply --check "${PATCH_PATH}"
git -C "${FEX_SOURCE}" apply "${PATCH_PATH}"
PATCH_APPLIED=1

cmake -S "${FEX_SOURCE}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu \
  -DCMAKE_BUILD_TYPE=Release \
  -DTUNE_CPU=none \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=${PROJECT_ROOT}=/usr/src/bachata-s4 -fmacro-prefix-map=${PROJECT_ROOT}=/usr/src/bachata-s4 -fdebug-prefix-map=${PROJECT_ROOT}=/usr/src/bachata-s4" \
  -DCMAKE_CXX_FLAGS="-ffile-prefix-map=${PROJECT_ROOT}=/usr/src/bachata-s4 -fmacro-prefix-map=${PROJECT_ROOT}=/usr/src/bachata-s4 -fdebug-prefix-map=${PROJECT_ROOT}=/usr/src/bachata-s4" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--build-id=sha1" \
  -DBUILD_FEXCORE_ONLY=ON \
  -DFEXCORE_PROJECT_SOURCE_DIR="${PROJECT_ROOT}/src" \
  -DFEXCORE_SMOKE_SOURCE="${SMOKE_SOURCE}" \
  -DFEXCORE_GUEST_HARNESS_SOURCES="${GUEST_ENGINE_SOURCE};${FEX_GUEST_CPU_SOURCE};${HLE_CALL_ADAPTER_SOURCE};${FEX_HLE_BRIDGE_SOURCE};${GUEST_HARNESS_SOURCE}" \
  -DBUILD_TESTING=OFF \
  -DBUILD_FEX_LINUX_TESTS=OFF \
  -DBUILD_THUNKS=OFF \
  -DBUILD_FEXCONFIG=OFF \
  -DENABLE_GDB_SYMBOLS=OFF \
  -DENABLE_LTO=OFF \
  -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
  -DENABLE_OFFLINE_TELEMETRY=OFF \
  -DENABLE_VIXL_DISASSEMBLER=OFF \
  -DENABLE_VIXL_SIMULATOR=OFF \
  -DENABLE_ZYDIS=OFF \
  -DENABLE_FEXCORE_PROFILER=OFF

# The guest harness sources live outside FEX's checkout. Force their target to
# rebuild after configuration so an older external-source timestamp cannot leave
# a stale ARM64 probe in the runtime package.
ninja -C "${BUILD_DIR}" -t clean fexcore-guest-harness
cmake --build "${BUILD_DIR}" --target fexcore-smoke fexcore-guest-harness --parallel
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"
cmake --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
aarch64-linux-gnu-strip "${STAGE_DIR}/bin/fexcore-smoke" "${STAGE_DIR}/bin/fexcore-guest-harness"
node "${VERIFIER}" "${STAGE_DIR}/bin/fexcore-smoke"
node "${GUEST_HARNESS_VERIFIER}" "${STAGE_DIR}/bin/fexcore-guest-harness"
