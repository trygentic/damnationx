#!/usr/bin/env python3
"""Run the actual world reset on a guarded 64 KiB WASM stack (requires em++/node)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "Source/render3d/world_gpu.cpp").read_text()
start = source.index("void WorldGpuRelease()")
brace = source.index("{", start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
reset = source[start:end]

# Keep the production reset intact; only GPU release/invalidation are stubbed.
# The old Level = LevelGeometry{} needed a 75 KiB temporary and overflowed here.
probe = r'''
#include "render3d/world_level.hpp"
#include <memory>
#include <cstdio>
namespace devilution::render3d {
LevelGeometry Level;
void ReleaseAll() {}
void InvalidateLevel() {}
''' + reset + r'''
}
int main() {
    using namespace devilution::render3d;
    for (int pass = 0; pass != 2; ++pass) {
        Level.vertices.resize(2);
        Level.indices.resize(3);
        Level.atlasRgba.resize(16);
        Level.atlasMips.resize(2);
        Level.stairs.fill(3);
        Level.stairsDir.fill(2);
        Level.occluders.fill(42);
        Level.atlasWidth = Level.atlasHeight = 4;
        Level.isTown = true;
        Level.wallHeight = 7;
        Level.generation = 9;
        WorldGpuRelease();
        if (!Level.vertices.empty() || !Level.indices.empty()
            || !Level.atlasRgba.empty() || !Level.atlasMips.empty()
            || Level.atlasWidth || Level.atlasHeight || Level.isTown
            || Level.wallHeight != 1 || Level.generation)
            return 1;
        for (size_t i = 0; i != Level.stairs.size(); ++i)
            if (Level.stairs[i] || Level.stairsDir[i] || Level.occluders[i])
                return 2;
    }
    std::printf("PASS: world reset fits a guarded 64 KiB WASM stack (%zu-byte geometry)\n", sizeof(Level));
}
'''

with tempfile.TemporaryDirectory(prefix="world-reset-") as directory:
    directory = Path(directory)
    cpp, js = directory / "probe.cpp", directory / "probe.cjs"
    cpp.write_text(probe)
    subprocess.run(shlex.split(os.environ.get("EMXX", "em++")) + [
        "-O3", "-std=c++23", "-sSTACK_SIZE=64KB", "-sSTACK_OVERFLOW_CHECK=2",
        "-I" + str(root / "Source"), str(cpp), "-o", str(js),
    ], check=True)
    subprocess.run(shlex.split(os.environ.get("NODE", "node")) + [str(js)], check=True)
