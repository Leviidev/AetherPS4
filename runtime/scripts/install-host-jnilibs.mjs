#!/usr/bin/env node
// Extract host glibc loader + Box64 into Android jniLibs so ProcessBuilder can
// exec them. Android denies execve (EACCES) for binaries under app filesDir
// even when mode +x is set; APK-extracted nativeLibraryDir is executable.
//
// Usage:
//   node runtime/scripts/install-host-jnilibs.mjs [runtime.zip]
// Default: assets runtime.zip, else download from Bachata-S4-Runtimes.

import { writeFileSync, existsSync, mkdirSync, chmodSync, unlinkSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(__dirname, "../..");
const jniDir = join(
  projectRoot,
  "android/BachataS4/core/runtime/src/main/jniLibs/arm64-v8a",
);

const DEFAULT_URL =
  process.env.BACHATA_RUNTIME_ZIP_URL ||
  "https://github.com/JICA98/Bachata-S4-Runtimes/releases/download/v0.1.0/runtime.zip";

const zipArg = process.argv[2];
const assetZip = join(
  projectRoot,
  "android/BachataS4/app/src/main/assets/runtime/runtime.zip",
);

function fail(msg) {
  console.error(msg);
  process.exit(1);
}

async function main() {
  mkdirSync(jniDir, { recursive: true });

  let zipPath = zipArg || (existsSync(assetZip) ? assetZip : null);
  let tmpZip = null;
  if (!zipPath) {
    tmpZip = join(projectRoot, "runtime/out/host-jnilibs-runtime.zip");
    mkdirSync(dirname(tmpZip), { recursive: true });
    console.log(`Downloading ${DEFAULT_URL}`);
    const res = await fetch(DEFAULT_URL);
    if (!res.ok) fail(`Download failed: ${res.status} ${res.statusText}`);
    const buf = Buffer.from(await res.arrayBuffer());
    writeFileSync(tmpZip, buf);
    zipPath = tmpZip;
    console.log(`Saved ${tmpZip} (${buf.length} bytes)`);
  } else {
    console.log(`Using ${zipPath}`);
  }

  const mapping = [
    ["host/ld-linux-aarch64.so.1", "libbachata_host_loader.so"],
    ["host/box64", "libbachata_host_box64.so"],
  ];

  for (const [entry, name] of mapping) {
    const out = join(jniDir, name);
    const result = spawnSync("unzip", ["-p", zipPath, entry], {
      encoding: "buffer",
      maxBuffer: 64 * 1024 * 1024,
    });
    if (result.status !== 0) {
      fail(
        `Failed to extract ${entry}: ${result.stderr?.toString() || result.status}`,
      );
    }
    writeFileSync(out, result.stdout);
    chmodSync(out, 0o755);
    console.log(`Wrote ${out} (${result.stdout.length} bytes)`);
  }

  if (tmpZip && existsSync(tmpZip)) {
    try {
      unlinkSync(tmpZip);
    } catch {
      /* ignore */
    }
  }
  console.log("Host jniLibs installed for executable ProcessBuilder launch.");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
