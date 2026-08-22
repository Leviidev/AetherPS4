#!/usr/bin/env node

import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { closeSync, mkdtempSync, openSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const apk = resolve(process.argv[2] ?? "android/BachataS4/app/build/outputs/apk/debug/app-debug.apk");
const required = [
  "assets/runtime/manifest.json",
  "assets/runtime/runtime.zip",
  "lib/arm64-v8a/libbachata_host_loader.so",
];

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function listing(archive) {
  return execFileSync("unzip", ["-Z1", archive], { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 })
    .split("\n").filter(Boolean);
}

function forbidden(entries) {
  return entries.filter((entry) => {
    const path = entry.toLowerCase();
    const name = path.split("/").at(-1);
    return path.includes("turnip") || name === "vulkan.ad07xx.so" ||
      name === "libvulkan_freedreno.so" || /(^|\/)freedreno[^/]*\.json$/.test(path);
  });
}

const apkEntries = listing(apk);
const missing = required.filter((entry) => !apkEntries.includes(entry));
if (missing.length) throw new Error(`APK is missing managed runtime assets: ${missing.join(", ")}`);

const temporary = mkdtempSync(join(tmpdir(), "bachata-apk-runtime-"));
try {
  const runtimeZip = join(temporary, "runtime.zip");
  const runtimeZipFd = openSync(runtimeZip, "w");
  try {
    execFileSync("unzip", ["-p", apk, "assets/runtime/runtime.zip"], {
      stdio: ["ignore", runtimeZipFd, "pipe"],
      maxBuffer: 16 * 1024 * 1024,
    });
  } finally {
    closeSync(runtimeZipFd);
  }
  const runtimeEntries = listing(runtimeZip);
  const manifest = JSON.parse(execFileSync("unzip", ["-p", apk, "assets/runtime/manifest.json"], {
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
  }));
  for (const runnerPath of [
    "host/fexcore-smoke",
    "host/fexcore-guest-harness",
    "host/shadps4-arm64-fex",
  ]) {
    if (!runtimeEntries.includes(runnerPath)) throw new Error(`Nested runtime ZIP is missing ${runnerPath}`);
    const declaredRunner = Array.isArray(manifest.files)
      ? manifest.files.find((file) => file.path === runnerPath)
      : undefined;
    if (!declaredRunner) throw new Error(`Runtime manifest is missing ${runnerPath}`);
    const runner = execFileSync("unzip", ["-p", runtimeZip, runnerPath], {
      encoding: "buffer",
      maxBuffer: 128 * 1024 * 1024,
    });
    if (runner.length < 20 || runner[0] !== 0x7f || runner.subarray(1, 4).toString() !== "ELF") {
      throw new Error(`Nested FEXCore runner is not ELF: ${runnerPath}`);
    }
    if (runner[4] !== 2) throw new Error(`Nested FEXCore runner is not ELF64: ${runnerPath}`);
    if (runner[5] !== 1) throw new Error(`Nested FEXCore runner is not little-endian ELF: ${runnerPath}`);
    if (runner.readUInt16LE(18) !== 183) throw new Error(`Nested FEXCore runner is not AArch64 ELF: ${runnerPath}`);
    if (declaredRunner.size !== runner.length) throw new Error(`Runtime manifest size mismatch: ${runnerPath}`);
    if (declaredRunner.sha256 !== sha256(runner)) throw new Error(`Runtime manifest SHA-256 mismatch: ${runnerPath}`);
  }
  const nativeFexEntries = apkEntries.filter((entry) => entry.startsWith("lib/") && entry.toLowerCase().includes("fex"));
  if (nativeFexEntries.length) throw new Error(`APK packages FEXCore through jniLibs: ${nativeFexEntries.join(", ")}`);
  const offenders = [...forbidden(apkEntries), ...forbidden(runtimeEntries).map((entry) => `runtime.zip:${entry}`)];
  if (offenders.length) throw new Error(`APK bundles forbidden Turnip payloads:\n${offenders.join("\n")}`);
  console.log(`APK runtime verified: ${apkEntries.length} APK entries, FEXCore smoke runner verified, no bundled Turnip`);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
