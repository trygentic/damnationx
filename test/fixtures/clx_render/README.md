# clx_render goldens

Classic byte-identity goldens for the CLX sprite blitter (`Source/engine/render/clx_render.cpp`,
`Source/engine/render/blit_impl.hpp`), consumed by `test/clx_render_golden_test.cpp`.

`goldens.tsv` was recorded before any hybrid-lighting change to the sprite blitter and must not change while the classic dispatch is bit-exact.

## Format

Tab-separated, `#` comments ignored:

```
sprite	frame	op	position	x	y	fnv64
```

* `sprite`: `resistance` (`data/resistance.clx`, 6 frames of 20x22) or `dvl_lrpopup`
  (`ui_art/dvl_lrpopup.clx`, 1 frame of 385x280); both are copied by `clx_render_benchmark_resources`.
* `op`: `Draw`, `TRN`, `Blended`, `BlendedTRN`, `Outline`, `OutlineSkipColorZero` (the outline ops only
  run for sprites narrower than `RenderClxOutline`'s 253 px contract, so not for the popup).
* `position`: `Full`, `ClipLeft`, `ClipRight`, `ClipTop`, `ClipBottom`, `ClipLeftTop`,
  `ClipRightBottom`, `ClipLeftBottom`, `ClipRightTop`; `x`/`y` is the bottom-left position passed to
  `ClxDraw*` on a 400x300 8-bit surface pre-filled with `(x * 31 + y * 17) & 0xFF`.
* `fnv64`: FNV-1a 64 over the 400x300 surface bytes (row-major, logical width, hex).

The TRN is `t[0] = 0, t[i] = 1 + (i * 7 + 3) % 255`; the blend table is
`paletteTransparencyLookup[a][b] = (a * 3 + b * 5 + 11) & 0xFF`; the outline colour is 0xFE.

## Re-recording

Only when the classic output is intentionally changed:

```
cd build-native-<task> && DEVILUTIONX_CLX_RENDER_GOLDEN_RECORD=../test/fixtures/clx_render/goldens.tsv ./clx_render_golden_test
```

## Capture / stamp parity

The same test binary also holds the hybrid-lighting parity suites (`ClxRenderCapture.*`, `ClxRenderStamp.*`).
They need no fixture: a synthetic CL2 frame (fills, pixel runs, colour-0 shadows, transparent runs crossing
rows) is drawn through every entry point at unclipped / ClipY / ClipXY / offscreen positions, on the back
buffer and on a subregion of it, and an oracle derived from the bitmap checks that the colour matches the
classic blit and that exactly the written pixels carry the expected plane A / plane B bytes (row-span plane
via `clx_normal_map::SetNormalPlaneProviderForTesting`, flat card, emissive card, arch tile context, Unlit
stamp, outline stamp) while everything else stays untouched. A second, 96 px wide synthetic frame with
whole-row fills (63 px) and pixel runs (65 px) drives the same oracle through every writer so the
fixed-size store cascade (runs of 1 / 2..3 / 4..7 / 8..15 / 16..32 bytes) and the libc fallback for
longer runs are all covered (`ClxRenderCapture.LongRunsThroughEveryWriter`).

`ClxRenderGolden.HybridColourByteIdentity` replays every classic golden case on the real assets with the
hybrid path active (plane / flat / emissive / arch capture and Unlit stamping): the colour plane must hash
to the same `goldens.tsv` value, so the fused `BlitDirect` writers in `blit_impl.hpp` (colour, plane A and
plane B written by one run-length dispatch) are proven bit-exact against the classic `memcpy` / `memset`
blit on runs up to 65 px and under every clip path.
