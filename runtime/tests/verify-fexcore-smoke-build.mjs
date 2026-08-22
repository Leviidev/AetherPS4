#!/usr/bin/env node

import { readdirSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const FEX_REVISION = "f2b679f6028ce1c38875233aecfcf5d3f8ebecec";
const EXPECTED_NEEDED = ["libc.so.6", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6"];

function fail(message) {
  console.error(`fexcore smoke build verification failed: ${message}`);
  process.exit(1);
}

if (process.argv.length !== 3) {
  fail("usage: verify-fexcore-smoke-build.mjs <artifact-path>");
}

const artifactPath = resolve(process.argv[2]);
const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "../..");
const stageDir = resolve(projectRoot, "runtime/build/fexcore-smoke-stage");

function run(command, args) {
  const result = spawnSync(command, args, { encoding: "utf8" });
  if (result.error) fail(`${command} could not inspect ${artifactPath}: ${result.error.message}`);
  if (result.status !== 0) {
    const detail = result.stderr.trim() || `exit ${result.status}`;
    fail(`${command} could not inspect ${artifactPath}: ${detail}`);
  }
  return result.stdout;
}

const elfHeader = run("readelf", ["-h", artifactPath]);
const programHeaders = run("readelf", ["-l", artifactPath]);
const dynamicSection = run("readelf", ["-d", artifactPath]);

for (const marker of [
  "Class:                             ELF64",
  "Data:                              2's complement, little endian",
  "Machine:                           AArch64",
]) {
  if (!elfHeader.includes(marker)) fail(`ELF header is missing ${marker.trim()}`);
}

if (!programHeaders.includes("[Requesting program interpreter: /lib/ld-linux-aarch64.so.1]")) {
  fail("ELF interpreter is not /lib/ld-linux-aarch64.so.1");
}

const needed = [...dynamicSection.matchAll(/\(NEEDED\).*Shared library: \[([^\]]+)\]/g)]
  .map((match) => match[1])
  .sort();
if (JSON.stringify(needed) !== JSON.stringify(EXPECTED_NEEDED)) {
  fail(`ELF dependencies differ: expected ${EXPECTED_NEEDED.join(",")}; got ${needed.join(",")}`);
}

const stringTable = run("strings", [artifactPath]);
for (const marker of [FEX_REVISION, "gpr=ok", "stack=ok", "fp=ok", "threads=ok", "tls=ok", "callback=ok", "invalidation=ok"]) {
  if (!stringTable.includes(marker)) fail(`artifact strings are missing ${marker}`);
}

function findForbiddenExecutables(directory) {
  const forbidden = [];
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = resolve(directory, entry.name);
    if (entry.isDirectory()) {
      forbidden.push(...findForbiddenExecutables(path));
    } else if ((entry.isFile() || entry.isSymbolicLink()) && (statSync(path).mode & 0o111) !== 0 &&
               /FEXInterpreter|LinuxEmulation|RootFS|thunk/i.test(path)) {
      forbidden.push(path);
    }
  }
  return forbidden;
}

const forbiddenExecutables = findForbiddenExecutables(stageDir);
if (forbiddenExecutables.length !== 0) {
  fail(`stage contains forbidden executable: ${forbiddenExecutables[0]}`);
}

console.log(
  `fexcore smoke build verified: machine=AArch64 interpreter=/lib/ld-linux-aarch64.so.1 needed=${needed.join(",")} revision=${FEX_REVISION} gpr=present stack=present fp=present threads=present tls=present callback=present invalidation=present`,
);
