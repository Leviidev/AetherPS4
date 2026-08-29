#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assembles FEXCore's own ASM+JSON correctness-test corpus into raw binaries
plus matching JSON config files, for fexcore-asm-tests (runtime/probes) to
run directly against this fork's FEXCore build. Bypasses upstream's
TestHarnessRunner (which needs full Linux syscall emulation we don't have on
iOS/macOS) entirely -- see fexcore-asm-tests.cpp's own header comment for why.

Usage: prepare-fex-asm-tests.py <asm_source_dir> <output_dir> [name_filter]
"""
import json
import re
import subprocess
import sys
from pathlib import Path

CONFIG_RE = re.compile(r"%ifdef\s+CONFIG\s*\n(.*?)\n%endif", re.DOTALL)


def extract_config(asm_text: str):
    match = CONFIG_RE.search(asm_text)
    if not match:
        return None
    # Config blocks may contain ';' comments same as the rest of the .asm file;
    # strip line comments before parsing since this is plain json.loads, not a
    # NASM-aware parser.
    lines = []
    for line in match.group(1).splitlines():
        stripped = line.split(";", 1)[0]
        lines.append(stripped)
    text = "\n".join(lines)
    return json.loads(text)


def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <asm_source_dir> <output_dir> [name_filter]", file=sys.stderr)
        return 2
    src_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    name_filter = sys.argv[3] if len(sys.argv) > 3 else None
    out_dir.mkdir(parents=True, exist_ok=True)

    asm_files = sorted(src_dir.rglob("*.asm"))
    if name_filter:
        asm_files = [f for f in asm_files if name_filter in str(f)]

    prepared = 0
    skipped_no_config = 0
    skipped_assemble_fail = 0
    manifest = []

    for asm_path in asm_files:
        rel = asm_path.relative_to(src_dir)
        text = asm_path.read_text(errors="replace")
        try:
            config = extract_config(text)
        except json.JSONDecodeError as exc:
            print(f"SKIP (bad json) {rel}: {exc}", file=sys.stderr)
            skipped_no_config += 1
            continue
        if config is None:
            skipped_no_config += 1
            continue

        out_stem = str(rel).replace("/", "__").rsplit(".", 1)[0]
        bin_path = out_dir / f"{out_stem}.bin"
        json_path = out_dir / f"{out_stem}.json"

        # Matches unittests/ASM/CMakeLists.txt's own pipeline exactly: these test files
        # have no BITS directive of their own (the real harness always assembles them as
        # 64-bit), and a trailing `ret` is appended as a safety net for any test whose own
        # code doesn't already end in hlt/ret.
        tmp_asm = bin_path.with_suffix(".tmp.asm")
        tmp_asm.write_text("BITS 64\n" + text.rstrip("\n") + "\nret\n")
        includes_dir = src_dir.parent / "Includes"
        nasm_args = ["nasm"]
        if includes_dir.is_dir():
            nasm_args += ["-i", str(includes_dir) + "/"]
        nasm_args += ["-f", "bin", str(tmp_asm), "-o", str(bin_path)]
        result = subprocess.run(nasm_args, capture_output=True, text=True)
        tmp_asm.unlink(missing_ok=True)
        if result.returncode != 0:
            print(f"SKIP (assemble failed) {rel}: {result.stderr.strip()}", file=sys.stderr)
            skipped_assemble_fail += 1
            continue

        config["_source"] = str(rel)
        json_path.write_text(json.dumps(config))
        manifest.append(out_stem)
        prepared += 1

    (out_dir / "manifest.txt").write_text("\n".join(manifest) + "\n")
    print(f"prepared={prepared} skipped_no_config={skipped_no_config} "
          f"skipped_assemble_fail={skipped_assemble_fail} total_asm_files={len(asm_files)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
