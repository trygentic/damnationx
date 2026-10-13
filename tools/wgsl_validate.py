#!/usr/bin/env python3
"""Validate the hybrid WGSL sources embedded in Source/engine/render/webgpu_shaders.hpp.

Extracts every ``R"WGSL(...)WGSL"`` string from the header. The first string is the shared
prelude (``HybridPreludeWgsl``); it is concatenated in front of every other string exactly
as the presenter does before ``wgpuDeviceCreateShaderModule``. A later string without an entry
point is a library (``HdFloorWgsl``): it is validated on its own after the prelude and also
concatenated (after the prelude) in front of every entry-point source that follows it. Each concatenated source is
written to a temporary ``.wgsl`` file and validated with ``naga`` (``cargo install naga-cli``)
and ``tint`` (the compiler browsers use) when they are found.

Exit codes:
  0   every source validated with at least one tool that was found, and no found tool failed
  1   extraction failed, or any found tool rejected any source
  77  neither naga nor tint is available (ctest maps this to SKIPPED via SKIP_RETURN_CODE 77)

Besides compiling, the script checks two structural rules with a plain text
scan of the entry-point sources: neither stage declares a ``var<storage`` binding, and the
compute stage binds nothing but ``var<uniform>`` and textures.

Usage: wgsl_validate.py <path/to/webgpu_shaders.hpp> [--keep DIR]
       --keep DIR writes the concatenated sources to DIR instead of a temporary directory.
       WGSL_NAGA / WGSL_TINT override the executables looked up on PATH (a cargo install
       that is not on PATH, ~/.cargo/bin/naga, is tried as a fallback).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_SKIP = 77

RAW_STRING = re.compile(r'R"WGSL\((.*?)\)WGSL"', re.DOTALL)
RAW_STRING_JOIN = re.compile(r'\)WGSL"[ \t]*(//[^\n]*)?\s*R"WGSL\(')
NAME_BEFORE = re.compile(r"constexpr\s+char\s+(\w+)\s*\[\]\s*=\s*$")


def extract_sources(header_text: str) -> list[tuple[str, str]]:
    """Returns [(constexpr name, raw string body)] in file order."""
    sources: list[tuple[str, str]] = []
    # Long sources are split into adjacent raw-string pieces (MSVC's 16 KB literal limit); rejoin them.
    header_text = RAW_STRING_JOIN.sub("", header_text)
    for match in RAW_STRING.finditer(header_text):
        prefix = header_text[: match.start()].rstrip()
        # The declaration sits on the same line, right before the raw string. A mention of the
        # delimiter inside a comment (the file header documents it) has no declaration and is skipped.
        line = prefix.splitlines()[-1] if prefix else ""
        name_match = NAME_BEFORE.search(line)
        if name_match is None:
            continue
        sources.append((name_match.group(1), match.group(1)))
    return sources


def entry_point_of(body: str) -> str | None:
    if re.search(r"@compute\b", body):
        return "compute"
    if re.search(r"@fragment\b", body):
        return "fragment"
    return None


def structural_checks(name: str, body: str, stage: str | None) -> list[str]:
    """Rules that are cheaper to check textually than through a compiler."""
    problems: list[str] = []
    if re.search(r"var\s*<\s*storage", body):
        problems.append(f"{name}: declares a storage buffer binding (var<storage>), forbidden in both hybrid stages")
    if stage == "compute":
        for decl in re.finditer(r"@group\(\d+\)\s*@binding\(\d+\)\s*var\s*(<[^>]*>)?\s*\w+\s*:\s*([\w<>, ]+?)\s*;", body):
            address_space = (decl.group(1) or "").strip()
            type_name = decl.group(2).strip()
            if address_space == "<uniform>":
                continue
            if address_space == "" and type_name.startswith("texture_"):
                continue
            problems.append(f"{name}: compute binding '{decl.group(0).strip()}' is neither var<uniform> nor a texture")
    return problems


def find_tool(env_var: str, exe: str, fallbacks: list[Path]) -> str | None:
    override = os.environ.get(env_var)
    if override:
        return override if Path(override).exists() else None
    found = shutil.which(exe)
    if found:
        return found
    for candidate in fallbacks:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def run_tool(label: str, argv: list[str]) -> bool:
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"  {label}: could not run ({error})")
        return False
    output = (result.stdout + result.stderr).strip()
    if result.returncode == 0:
        print(f"  {label}: ok")
        return True
    print(f"  {label}: FAILED (exit {result.returncode})")
    if output:
        print("    " + output.replace("\n", "\n    "))
    return False


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: wgsl_validate.py <webgpu_shaders.hpp> [--keep DIR]", file=sys.stderr)
        return EXIT_FAIL
    header = Path(argv[1])
    keep_dir: Path | None = None
    if "--keep" in argv:
        keep_dir = Path(argv[argv.index("--keep") + 1])
        keep_dir.mkdir(parents=True, exist_ok=True)
    try:
        text = header.read_text(encoding="utf-8")
    except OSError as error:
        print(f"wgsl_validate: cannot read {header}: {error}", file=sys.stderr)
        return EXIT_FAIL

    sources = extract_sources(text)
    if len(sources) < 2:
        print(f"wgsl_validate: expected a prelude plus at least one entry-point source in {header}, found {len(sources)}")
        return EXIT_FAIL
    prelude_name, prelude = sources[0]
    if "HybridPrelude" not in prelude_name:
        print(f"wgsl_validate: first raw string is {prelude_name}, expected the HybridPreludeWgsl prelude")
        return EXIT_FAIL

    combined: list[tuple[str, str, str | None]] = []
    problems: list[str] = []
    libraries = ""
    for name, body in sources[1:]:
        stage = entry_point_of(body)
        problems.extend(structural_checks(name, body, stage))
        if stage is None:
            # A library (functions and bindings without an entry point) for the sources after it.
            combined.append((name, prelude + body, stage))
            libraries += body
            continue
        combined.append((name, prelude + libraries + body, stage))
    if all(stage is None for _, _, stage in combined):
        problems.append("no @compute / @fragment entry point found in any source")
    if problems:
        for problem in problems:
            print("wgsl_validate: " + problem)
        return EXIT_FAIL

    home = Path.home()
    naga = find_tool("WGSL_NAGA", "naga", [home / ".cargo" / "bin" / "naga"])
    tint = find_tool("WGSL_TINT", "tint", [])
    if naga is None and tint is None:
        print("wgsl_validate: neither naga nor tint found on PATH (cargo install naga-cli); skipped")
        return EXIT_SKIP
    print(f"wgsl_validate: naga={naga or '-'} tint={tint or '-'}; {len(combined)} entry-point sources, prelude {prelude_name}")

    all_ok = True
    with tempfile.TemporaryDirectory(prefix="wgsl_validate_") as tmp:
        out_dir = keep_dir if keep_dir is not None else Path(tmp)
        for name, source, stage in combined:
            path = out_dir / f"{name}.wgsl"
            path.write_text(source, encoding="utf-8")
            print(f"{name} ({stage}, {len(source)} bytes) -> {path}")
            if naga is not None:
                # naga validates on parse; a second positional argument would translate. Validation only here.
                all_ok &= run_tool("naga", [naga, str(path)])
            if tint is not None:
                # tint: parse + resolve (WGSL validation happens there); the WGSL re-emission is discarded.
                # (`--validate` would validate the generated backend output instead, not the WGSL.)
                all_ok &= run_tool("tint", [tint, "--format", "wgsl", "-o", os.devnull, str(path)])
    if not all_ok:
        print("wgsl_validate: FAILED")
        return EXIT_FAIL
    print("wgsl_validate: all sources validated")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv))
