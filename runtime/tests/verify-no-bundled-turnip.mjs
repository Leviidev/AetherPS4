#!/usr/bin/env node

import { lstatSync, readdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { basename, join, resolve } from "node:path";

const target = resolve(process.argv[2] ?? "runtime/build/rootfs");

function entries(path) {
  if (!lstatSync(path).isDirectory()) {
    return execFileSync("unzip", ["-Z1", path], { encoding: "utf8" })
      .split("\n")
      .filter(Boolean);
  }
  const result = [];
  const walk = (directory, prefix = "") => {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const relative = join(prefix, entry.name).replaceAll("\\", "/");
      result.push(relative);
      if (entry.isDirectory()) walk(join(directory, entry.name), relative);
    }
  };
  walk(path);
  return result;
}

const forbidden = entries(target).filter((entry) => {
  const path = entry.toLowerCase();
  const name = basename(path);
  return path.includes("turnip") ||
    name === "vulkan.ad07xx.so" ||
    name === "libvulkan_freedreno.so" ||
    /(^|\/)freedreno[^/]*\.json$/.test(path);
});

if (forbidden.length > 0) {
  console.error(`Bundled Turnip payload found:\n${forbidden.join("\n")}`);
  process.exit(1);
}

console.log(`No bundled Turnip payloads in ${target}`);
