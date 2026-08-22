#!/usr/bin/env node

import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const headerPath = resolve(projectRoot, "src/core/emulator_settings.h");
const box64Path = resolve(projectRoot, "runtime/sources/box64/docs/USAGE.md");
const metadataPath = resolve(projectRoot, "runtime/settings/android-setting-metadata.json");
const shadOutput = resolve(projectRoot, "android/BachataS4/core/runtime/src/main/resources/runtime-settings/shadps4.json");
const boxOutput = resolve(projectRoot, "android/BachataS4/core/runtime/src/main/resources/runtime-settings/box64.json");

const launchOwnedBox64 = new Set([
  "BOX64_PATH",
  "BOX64_LD_LIBRARY_PATH",
  "BOX64_EMULATED_LIBS",
  "BOX64_LOAD_ADDR",
  "BOX64_PROFILE",
]);

const shadOverrides = {
  "General.trophy_notification_side": { kind: "ENUM", choices: ["right", "left"] },
  "Log.type": { kind: "ENUM", choices: ["wincolor", "sync", "async"] },
  "Input.cursor_state": { kind: "ENUM", choices: ["0", "1", "2"], nativeEnumOrdinal: true },
  "Audio.audio_backend": { kind: "ENUM", choices: ["SDL", "OpenAL"], nativeEnumOrdinal: true },
  "GPU.full_screen_mode": { kind: "ENUM", choices: ["Windowed", "Fullscreen", "Borderless"] },
  "GPU.present_mode": { kind: "ENUM", choices: ["Immediate", "Mailbox", "Fifo", "FifoRelaxed"] },
};

function humanize(value) {
  return value
    .replace(/^BOX64_/, "")
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .replace(/_/g, " ")
    .toLowerCase()
    .replace(/(^|\s)\S/g, (letter) => letter.toUpperCase());
}

function literalDefault(type, initializer) {
  const value = initializer?.trim();
  if (!value) {
    if (type === "bool") return false;
    if (/^(?:u?int|[ius]\d+|float|double|long)/.test(type)) return 0;
    if (type.includes("vector")) return [];
    return "";
  }
  if (value === "true") return true;
  if (value === "false") return false;
  if (/^-?\d+(?:\.\d+)?$/.test(value)) return Number(value);
  const quoted = value.match(/^"([\s\S]*)"$/);
  return quoted ? quoted[1] : null;
}

function kindForCpp(type) {
  if (type === "bool") return "BOOLEAN";
  if (type.includes("vector")) return "LIST";
  if (type.includes("filesystem::path")) return "PATH";
  if (type === "float" || type === "double") return "DECIMAL";
  if (/^(?:u?int|[ius]\d+|long)/.test(type)) return "INTEGER";
  return "STRING";
}

function discoverShadPs4() {
  const source = readFileSync(headerPath, "utf8");
  const specs = [];
  const structPattern = /struct\s+(\w+Settings)\s*\{([\s\S]*?)\n\};/g;
  for (const match of source.matchAll(structPattern)) {
    const section = match[1].replace(/Settings$/, "");
    const fieldPattern = /^\s*Setting<(.+?)>\s+(\w+)(?:\{([^;]*)\})?;/gm;
    for (const field of match[2].matchAll(fieldPattern)) {
      const [, type, name, initializer] = field;
      const nativeKey = `${section}.${name}`;
      const title = humanize(name);
      const base = {
        id: `${section.toLowerCase()}.${name}`,
        nativeKey,
        section,
        category: section,
        title,
        help: `Controls shadPS4 ${title.toLowerCase()}.`,
        kind: kindForCpp(type.trim()),
        defaultValue: literalDefault(type.trim(), initializer),
        minimum: null,
        maximum: null,
        choices: [],
        scope: name.endsWith("_dir") || name === "install_dirs" ? "GLOBAL_ONLY" : "GLOBAL_AND_GAME",
        restartRequired: true,
        risk: section === "Debug" || name.includes("validation") ? "ADVANCED" : "NORMAL",
        readOnlyReason: null,
      };
      specs.push({ ...base, ...(shadOverrides[nativeKey] ?? {}) });
    }
  }
  return specs.sort(compareSpecs);
}

function paragraph(lines) {
  const parts = [];
  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) {
      if (parts.length) break;
      continue;
    }
    if (trimmed.startsWith("* ") || trimmed.startsWith("```")) continue;
    parts.push(trimmed.replace(/`/g, ""));
  }
  return parts.join(" ");
}

function box64ValueDetails(lines) {
  const choices = [];
  let defaultChoice = null;
  for (const line of lines) {
    const option = line.match(/^\s*\*\s+([^:]+):/);
    if (!option) continue;
    const value = option[1].trim();
    choices.push(value);
    if (line.includes("[Default]")) defaultChoice = value;
  }
  const unique = [...new Set(choices)];
  if (unique.length === 2 && unique.includes("0") && unique.includes("1")) {
    return { kind: "BOOLEAN", defaultValue: defaultChoice === null ? null : defaultChoice === "1", choices: [] };
  }
  if (unique.length > 1 && unique.length <= 12 && unique.every((value) => /^-?\d+$/.test(value))) {
    return { kind: "ENUM", defaultValue: defaultChoice, choices: unique };
  }
  if (defaultChoice !== null && /^-?\d+$/.test(defaultChoice)) {
    return { kind: "INTEGER", defaultValue: Number(defaultChoice), choices: [] };
  }
  return { kind: "STRING", defaultValue: defaultChoice, choices: [] };
}

function discoverBox64() {
  const lines = readFileSync(box64Path, "utf8").split(/\r?\n/);
  const specs = [];
  let category = "General";
  for (let index = 0; index < lines.length; index += 1) {
    const categoryMatch = lines[index].match(/^##\s+(.+)/);
    if (categoryMatch && !lines[index].startsWith("###")) {
      category = categoryMatch[1].trim();
      continue;
    }
    const settingMatch = lines[index].match(/^###\s+(BOX64_[A-Z0-9_\[\]-]+)/);
    if (!settingMatch) continue;
    const nativeKey = settingMatch[1];
    const body = [];
    while (index + 1 < lines.length && !lines[index + 1].startsWith("### ") && !lines[index + 1].startsWith("## ")) {
      body.push(lines[index + 1]);
      index += 1;
    }
    const value = box64ValueDetails(body);
    if (nativeKey === "BOX64_PROFILE") {
      value.kind = "ENUM";
      value.defaultValue = "default";
      value.choices = ["safest", "safe", "default", "fast", "fastest"];
    }
    const readOnly = launchOwnedBox64.has(nativeKey);
    specs.push({
      id: `box64.${nativeKey.toLowerCase().replace(/^box64_/, "").replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "")}`,
      nativeKey,
      section: "Box64",
      category,
      title: humanize(nativeKey),
      help: paragraph(body) || `Controls ${nativeKey}.`,
      kind: value.kind,
      defaultValue: value.defaultValue,
      minimum: null,
      maximum: null,
      choices: value.choices,
      scope: "GLOBAL_AND_GAME",
      restartRequired: true,
      risk: category === "Fragile or Legacy" ? "DANGEROUS" : category === "Debug" ? "ADVANCED" : "NORMAL",
      readOnlyReason: readOnly ? "Managed by BachataS4 runtime launch wiring." : null,
    });
  }
  return specs.sort(compareSpecs);
}

function compareSpecs(a, b) {
  return a.category.localeCompare(b.category) || a.nativeKey.localeCompare(b.nativeKey);
}

function metadataMap(specs) {
  return Object.fromEntries(specs.map((spec) => [spec.nativeKey, spec]));
}

function applyMetadata(discovered, metadata, label) {
  const discoveredKeys = new Set(discovered.map((spec) => spec.nativeKey));
  const metadataKeys = new Set(Object.keys(metadata));
  const missing = [...discoveredKeys].filter((key) => !metadataKeys.has(key));
  const extra = [...metadataKeys].filter((key) => !discoveredKeys.has(key));
  if (missing.length || extra.length) {
    throw new Error(`${label} metadata mismatch; missing=[${missing.join(",")}] extra=[${extra.join(",")}]`);
  }
  return Object.values(metadata).sort(compareSpecs);
}

function serialized(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

const discoveredShad = discoverShadPs4();
const discoveredBox = discoverBox64();
const bootstrap = process.argv.includes("--bootstrap-metadata");
const check = process.argv.includes("--check");

if (bootstrap) {
  writeFileSync(metadataPath, serialized({ shadps4: metadataMap(discoveredShad), box64: metadataMap(discoveredBox) }));
}

const metadata = JSON.parse(readFileSync(metadataPath, "utf8"));
const shadCatalog = applyMetadata(discoveredShad, metadata.shadps4, "shadPS4");
const boxCatalog = applyMetadata(discoveredBox, metadata.box64, "Box64");
const expectedShad = serialized(shadCatalog);
const expectedBox = serialized(boxCatalog);

if (check) {
  if (readFileSync(shadOutput, "utf8") !== expectedShad || readFileSync(boxOutput, "utf8") !== expectedBox) {
    throw new Error("Generated Android settings catalogs are stale");
  }
} else {
  writeFileSync(shadOutput, expectedShad);
  writeFileSync(boxOutput, expectedBox);
}

console.log(`shadPS4=${shadCatalog.length} box64=${boxCatalog.length}`);
