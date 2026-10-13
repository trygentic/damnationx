# normal_map fixtures

Synthetic CLX sprites for `test/clx_normal_map_test.cpp` and `test/clx_normal_map_benchmark.cpp`.

## Provenance

Both files are **synthetic**: generated once from procedurally drawn 8-bit surfaces through the real encoder
(`SurfaceToClx`, `Source/utils/surface_to_clx.cpp`, transparent index 255) and committed. They are not extracted,
traced or derived from Diablo, Hellfire or any proprietary archive, and no game data is needed to regenerate them.
The generator is a 60-line program that draws the two shapes below and calls `SurfaceToClx(surface, 1, 255)`; the
test file's `EncodeClxBytes` mirrors the same encoder byte for byte, so the fixtures can be re-derived from the
descriptions here.

## disc.clx — 128 x 128, 1 frame (447 bytes)

A uniform disc of radius 44 px centred at (64, 64) in palette index 40; everything else transparent.

* Exercises the silhouette dome: rim normals point outward along the radial direction, the centre
  is flat (LUT index 3), and the luminance detail term is zero (uniform colour).
* Row-span packing: Σ chord widths ≈ π·44² = 6082 bytes of 16384 dense (37 %). This is the monster-like packed
  ratio the wasm cache budget is derived from; `clx_normal_map_benchmark` reports the exact value.

## ramp.clx — 64 x 128, 1 frame (368 bytes)

* Body: columns [16, 48), feet rows [2, 114) (112 rows), palette index `16 + 2 · (row − 2)` increasing upward
  (16 at the bottom, 238 at the top).
* Shadow: feet rows 0 and 1, columns [8, 56), palette index 0 (`ShadowByte` in the plane).

With the test's linear-luminance palette the body is a uniform vertical luminance gradient: without de-lighting
the detail normal tilts toward the feet (negative card y); with de-lighting the gradient is subtracted
and the interior is flat. Packed bytes: 32·112 + 2·48 = 3680 of 8192 dense (45 %).

## Palette

The fixtures carry indices only. The tests pair them with a synthetic palette whose *linear* luminance is
proportional to the index (`LinearLuminancePalette()` in the test), so index ramps are uniform luminance ramps.
