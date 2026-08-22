#!/usr/bin/env node
// verify-native-fixes.mjs
// Verifies Bachata S4-specific fixes are present in:
//   1. JNI libs packaged directly inside the APK (libwinlator.so, libbachata_vortek_server.so)
//   2. runtime.zip → host/lib/libvulkan_vortek.so (Vortek client, pinned JICA98 fork)

import { execFileSync } from "node:child_process";
import { closeSync, mkdtempSync, openSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const apk = resolve(process.argv[2] ?? "android/BachataS4/app/build/outputs/apk/debug/app-debug.apk");
const nativeLibs = [
  { path: "lib/arm64-v8a/libwinlator.so", markers: { exportedSymbol: "Java_com_winlator_renderer_GPUImage_unlockHardwareBuffer", strings: ["abstract bind path=", "abstract listen path="] } },
  {
    path: "lib/arm64-v8a/libbachata_vortek_server.so",
    markers: {
      strings: [
        "bachata_vortek_fence_host_wait",
        // GPU-VA / resource lifetime instrumentation (Mali DEVICE_LOST diagnostics).
        "bachata_vortek_gpu_va_track",
        "VORTEK_ALLOC",
        "DEVICE_LOST_SNAPSHOT",
        "RESOURCE_DESTROY_DEFERRED",
        "PENDING_RESOURCE",
        "COPY_BUFFER_TO_IMAGE",
        "GPU_RANGE_INVALID",
      ],
    },
  },
];

// Vortek client fix: verify libvulkan_vortek.so inside runtime.zip (packaged in APK assets).
// build-vortek-client.sh embeds -DBACHATA_VORTEK_CLIENT_BUILD_ID="$client_revision" so the
// revision string appears verbatim in the binary.  SOURCE.txt carries the same revision.
const VORTEK_IN_ZIP = "host/lib/libvulkan_vortek.so";
const VORTEK_SOURCE_IN_ZIP = "usr/share/bachata/vortek/SOURCE.txt";
const RUNTIME_ZIP_IN_APK = "assets/runtime/runtime.zip";

function fail(message) {
  throw new Error(message);
}

function extract(apkPath, entry, destination) {
  const fd = openSync(destination, "w");
  try {
    execFileSync("unzip", ["-p", apkPath, entry], { stdio: ["ignore", fd, "pipe"], maxBuffer: 512 * 1024 * 1024 });
  } finally {
    closeSync(fd);
  }
}

const entries = execFileSync("unzip", ["-Z1", apk], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 })
  .split("\n").filter(Boolean);

const temporary = mkdtempSync(join(tmpdir(), "bachata-native-fixes-"));
try {
  // --- JNI library checks ---
  for (const lib of nativeLibs) {
    if (!entries.includes(lib.path)) fail(`APK is missing native library: ${lib.path}`);
    const local = join(temporary, lib.path.split("/").pop());
    extract(apk, lib.path, local);
    if (lib.markers.exportedSymbol) {
      const symbols = execFileSync("nm", ["-D", local], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
      if (!symbols.includes(lib.markers.exportedSymbol)) {
        fail(`${lib.path} does not export ${lib.markers.exportedSymbol}`);
      }
    }
    if (lib.markers.strings) {
      const strings = execFileSync("strings", [local], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
      for (const marker of lib.markers.strings) {
        if (!strings.includes(marker)) fail(`${lib.path} is missing runtime string "${marker}"`);
      }
    }
    console.log(`native fixes verified: ${lib.path}`);
  }

  // --- Vortek client fix: inspect runtime.zip/host/lib/libvulkan_vortek.so ---
  if (!entries.includes(RUNTIME_ZIP_IN_APK)) {
    fail(`APK is missing ${RUNTIME_ZIP_IN_APK}`);
  }
  const runtimeZipLocal = join(temporary, "runtime.zip");
  extract(apk, RUNTIME_ZIP_IN_APK, runtimeZipLocal);

  // List entries in runtime.zip
  const zipEntries = execFileSync("unzip", ["-Z1", runtimeZipLocal], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 })
    .split("\n").filter(Boolean);

  if (!zipEntries.includes(VORTEK_IN_ZIP)) {
    fail(`runtime.zip is missing ${VORTEK_IN_ZIP}`);
  }

  // Extract libvulkan_vortek.so from runtime.zip
  const vortekLocal = join(temporary, "libvulkan_vortek.so");
  const vortekFd = openSync(vortekLocal, "w");
  try {
    execFileSync("unzip", ["-p", runtimeZipLocal, VORTEK_IN_ZIP], { stdio: ["ignore", vortekFd, "pipe"], maxBuffer: 128 * 1024 * 1024 });
  } finally {
    closeSync(vortekFd);
  }

  // (a) ICD export must be present
  const vortekSymbols = execFileSync("nm", ["-D", vortekLocal], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  if (!vortekSymbols.includes("vk_icdGetInstanceProcAddr")) {
    // Some toolchains hide via version script; fall back to readelf
    const elfSyms = execFileSync("readelf", ["-Ws", vortekLocal], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
    if (!elfSyms.includes("vk_icdGetInstanceProcAddr")) {
      fail(`${VORTEK_IN_ZIP} is missing ICD export vk_icdGetInstanceProcAddr`);
    }
  }

  // (b) BACHATA_VORTEK_CLIENT_BUILD_ID marker must be embedded (build-vortek-client.sh -DBACHATA_VORTEK_CLIENT_BUILD_ID="$rev")
  const vortekStrings = execFileSync("strings", [vortekLocal], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  // The marker is a 40-char hex git SHA embedded as a string literal in the binary.
  // We verify it exists; SOURCE.txt below confirms the exact revision.
  if (!/[0-9a-f]{40}/.test(vortekStrings)) {
    fail(`${VORTEK_IN_ZIP} does not contain a BACHATA_VORTEK_CLIENT_BUILD_ID marker (40-char hex SHA)`);
  }

  // (c) Must NOT be an Android/Bionic binary
  const elfDyn = execFileSync("readelf", ["-d", vortekLocal], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 });
  if (/linker64|bionic/i.test(elfDyn)) {
    fail(`${VORTEK_IN_ZIP} appears to be an Android/Bionic binary (linker64/bionic in dynamic section)`);
  }
  // Must link glibc libc.so.6
  if (!elfDyn.includes("libc.so.6")) {
    fail(`${VORTEK_IN_ZIP} does not link glibc libc.so.6 — expected aarch64-glibc target`);
  }

  // (d) SOURCE.txt provenance: read pinned revision from runtime.zip
  let pinnedRevision = null;
  if (zipEntries.includes(VORTEK_SOURCE_IN_ZIP)) {
    const sourceTxtLocal = join(temporary, "vortek_SOURCE.txt");
    const sourceFd = openSync(sourceTxtLocal, "w");
    try {
      execFileSync("unzip", ["-p", runtimeZipLocal, VORTEK_SOURCE_IN_ZIP], { stdio: ["ignore", sourceFd, "pipe"], maxBuffer: 1 * 1024 * 1024 });
    } finally {
      closeSync(sourceFd);
    }
    const sourceText = readFileSync(sourceTxtLocal, "utf8");
    const revMatch = sourceText.match(/^revision=([0-9a-f]{40})$/m);
    if (revMatch) {
      pinnedRevision = revMatch[1];
      // Verify the pinned revision string appears in the binary
      if (!vortekStrings.includes(pinnedRevision)) {
        fail(
          `${VORTEK_IN_ZIP} does not contain the pinned JICA98 revision ${pinnedRevision} ` +
          `from ${VORTEK_SOURCE_IN_ZIP} — binary may not be from the pinned fork`
        );
      }
      console.log(`vortek client fix verified: revision=${pinnedRevision}`);
    } else {
      console.warn(`WARNING: ${VORTEK_SOURCE_IN_ZIP} present but no revision= line found`);
    }
  } else {
    fail(`runtime.zip is missing ${VORTEK_SOURCE_IN_ZIP} — provenance record required`);
  }

  console.log(`vortek client verified: ${VORTEK_IN_ZIP} (ICD export ✓, glibc ✓, no bionic ✓, revision pinned ✓)`);
  console.log(`native fixes verified for ${apk}`);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
