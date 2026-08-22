#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const MAX_INPUT_BYTES = 128 * 1024 * 1024;

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "../..");
const lock = JSON.parse(readFileSync(resolve(projectRoot, "runtime/locks/runtime-inputs.lock.json"), "utf8"));
const inputDir = resolve(process.argv[2] ?? resolve(projectRoot, "runtime/build/inputs"));
mkdirSync(inputDir, { recursive: true });

for (const input of lock.inputs) {
  if (basename(input.name) !== input.name || !/^https:\/\//.test(input.url) || !/^[0-9a-f]{64}$/.test(input.sha256)) {
    throw new Error(`Invalid locked input: ${input.name}`);
  }
  const output = join(inputDir, input.name);
  let existing;
  try { existing = readFileSync(output); } catch (error) { if (error.code !== "ENOENT") throw error; }
  if (existing) {
    if (sha256(existing) !== input.sha256) throw new Error(`Locked input hash mismatch: ${input.name}`);
    console.log(`verified ${input.name}`);
    continue;
  }

  const response = await fetch(input.url, { redirect: "follow" });
  if (!response.ok) throw new Error(`Download failed (${response.status}): ${input.name}`);
  const declaredSize = Number(response.headers.get("content-length") ?? 0);
  if (declaredSize > MAX_INPUT_BYTES) throw new Error(`Locked input too large: ${input.name}`);
  const bytes = Buffer.from(await response.arrayBuffer());
  if (bytes.length > MAX_INPUT_BYTES || sha256(bytes) !== input.sha256) throw new Error(`Downloaded input hash mismatch: ${input.name}`);
  const temporary = `${output}.tmp`;
  rmSync(temporary, { force: true });
  writeFileSync(temporary, bytes, { mode: 0o600 });
  renameSync(temporary, output);
  console.log(`downloaded ${input.name}`);
}
