#!/usr/bin/env python3
"""Exercise the real imposter installer with a recycled WebGPU view handle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'Source/engine/render/hd_sprites_webgpu.cpp').read_text()
start = source.index('void InstallImposters(')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1

probe = r'''
#include <cassert>
#include <cstdint>
#include <array>
enum class ClipState { Unrequested, Resident };
struct ClipSlot {
  int view = 0, bindGroup = 0, hybridBindGroup = 0, divisor = 0;
  bool external = false;
  uint64_t externalGeneration = 0;
  ClipState state = ClipState::Unrequested;
};
struct { std::array<ClipSlot, 2> clips; } Gpu;
namespace render3d {
struct SkinnedImposterOutput { int clip; int view; uint64_t generation; };
}
bool ValidClip(int clip) { return clip >= 0 && clip < 2; }
void ReleaseClipTexture(ClipSlot &slot) {
  slot.view = slot.bindGroup = slot.hybridBindGroup = 0;
}
''' + source[start:end].replace('output.view == nullptr', 'output.view == 0') + r'''
int main() {
  auto &slot = Gpu.clips[0];
  render3d::SkinnedImposterOutput output {0, 42, 1};
  InstallImposters(&output, 1);
  slot.bindGroup = 100; slot.hybridBindGroup = 101;
  InstallImposters(&output, 1);
  assert(slot.bindGroup == 100 && slot.hybridBindGroup == 101);
  // The browser reused the released view's integer ID for a new texture.
  // Equality of handles must not preserve bindings to the destroyed texture.
  output.generation = 2;
  InstallImposters(&output, 1);
  assert(slot.bindGroup == 0 && slot.hybridBindGroup == 0);
  assert(slot.externalGeneration == 2);
  Gpu.clips[1].state = ClipState::Resident;
  Gpu.clips[1].view = 99; // An authored sprite must never be replaced.
  output.clip = 1;
  InstallImposters(&output, 1);
  assert(Gpu.clips[1].view == 99);
}
'''
with tempfile.TemporaryDirectory() as d:
    cpp, exe = Path(d) / 'probe.cpp', Path(d) / 'probe'
    cpp.write_text(probe)
    subprocess.run(['c++', '-std=c++20', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Imposter bindings follow texture generations, including recycled view handles.')
