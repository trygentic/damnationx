# dun_render golden fixtures

Consumed by `test/dun_render_golden_test.cpp`. Copied next to the
test binary as `test/fixtures/dun_render/` by the `dun_render_golden_test_resources`
target (`CMake/Tests.cmake`).

## frames.bin

Synthetic dungeon frames in the `ReencodeDungeonCels` layout (bottom row first,
triangle padding bytes removed), so `RenderTileFrame` consumes them exactly like
frames from a level CEL. Little-endian container:

| Offset | Type | Meaning |
|---|---|---|
| 0 | `char[4]` | magic `DUNF` |
| 4 | `u32` | version, 1 |
| 8 | `u32` | record count |
| 12 | records | see below |

Each record: `u8 tileType` (`TileType` from `Source/levels/dun_tile.hpp`),
`u8 height` (rows), `u16 reserved` (0), `u32 size`, then `size` bytes of frame
data.

Records, in order: `Square` (1024 B), `TransparentSquare` (RLE, 32 rows),
`LeftTriangle` (512 B), `RightTriangle` (512 B), `LeftTrapezoid` (784 B),
`RightTrapezoid` (784 B) and a 16-row `TransparentSquare` standing in for a
floor foliage frame (`RenderTileFoliage` renders those at height 16). The test
names the last one `TransparentSquare16`.

Pixel values are `1..255` (never 0, so the black-tile tests can identify written
pixels) drawn from a fixed xorshift32 seed. The RLE frames mix opaque and
transparent runs of 1..12 pixels per row, with every 7th row fully opaque and
rows `r % 11 == 5` fully transparent.

Regenerate (byte-identical) with the script below:

```python
#!/usr/bin/env python3
import struct, sys

SQUARE, TRANSPARENT_SQUARE, LEFT_TRIANGLE, RIGHT_TRIANGLE, LEFT_TRAPEZOID, RIGHT_TRAPEZOID = range(6)

class Rng:
    def __init__(self, seed): self.s = seed & 0xFFFFFFFF
    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        self.s = x & 0xFFFFFFFF
        return self.s
    def pixel(self): return 1 + self.next() % 255

lower = [2 * (i + 1) for i in range(16)]
upper = [32 - 2 * i for i in range(1, 16)]
def square(rng): return bytes(rng.pixel() for _ in range(1024))
def triangle(rng): return bytes(rng.pixel() for w in lower + upper for _ in range(w))
def trapezoid(rng): return bytes(rng.pixel() for w in lower + [32] * 16 for _ in range(w))
def transparent_square(rng, height):
    out = bytearray()
    for r in range(height):
        if r % 7 == 0:
            out.append(32); out.extend(rng.pixel() for _ in range(32)); continue
        if r % 11 == 5:
            out.append((-32) & 0xFF); continue
        remaining, opaque = 32, (r % 2) == 0
        while remaining > 0:
            n = 1 + rng.next() % min(remaining, 12)
            if opaque:
                out.append(n); out.extend(rng.pixel() for _ in range(n))
            else:
                out.append((-n) & 0xFF)
            remaining -= n; opaque = not opaque
    return bytes(out)

rng = Rng(0x2D5A5EED)
records = [(SQUARE, 32, square(rng)), (TRANSPARENT_SQUARE, 32, transparent_square(rng, 32)),
           (LEFT_TRIANGLE, 31, triangle(rng)), (RIGHT_TRIANGLE, 31, triangle(rng)),
           (LEFT_TRAPEZOID, 32, trapezoid(rng)), (RIGHT_TRAPEZOID, 32, trapezoid(rng)),
           (TRANSPARENT_SQUARE, 16, transparent_square(rng, 16))]
with open(sys.argv[1], "wb") as f:
    f.write(b"DUNF" + struct.pack("<II", 1, len(records)))
    for tile, height, data in records:
        f.write(struct.pack("<BBHI", tile, height, 0, len(data)) + data)
```

## goldens.tsv

One line per case: `tile`, `mask`, `light`, `clip` and the FNV-1a 64-bit hash
(16 hex digits) of the 96x96 output surface, tab-separated; `#` lines are
comments. Cases are every frame record x every valid `MaskType` (`Left` only
with `TransparentSquare`/`LeftTrapezoid`, `Right` only with
`TransparentSquare`/`RightTrapezoid`) x `{FullyDark, FullyLit, PartiallyLit,
HellLit}` x nine clip positions (full, top, deep top, bottom, deep bottom, left,
right, left+top, right+bottom). `HellLit` renders `LightTables[0]`-style data
through a lightmap without a fully-lit table, the way Hell does.

The surface is pre-filled with `((7x + 13y) << 1) | 1`, the light tables are
synthetic bijections and `paletteTransparencyLookup[a][b] = 5a + 3b + 1`, so
the hashes cover blended output and argument order without game data.

Recorded before any hybrid-lighting change to
`Source/engine/render/dun_render.cpp`; classic output must stay byte-identical
across the hybrid lighting work. Re-record only for an intentional classic
rendering change:

```sh
DUN_RENDER_GOLDEN_RECORD=$PWD/../test/fixtures/dun_render/goldens.tsv ./dun_render_golden_test --gtest_filter='*ClassicByteIdentity'
```

## Known pre-existing behaviour preserved by the goldens

The `Left`/`Right` prefix masks do not reproduce the unclipped blend cut-out on
some clipped frames (measured before the hybrid lighting work):

* `RenderTrapezoidUpperHalfClip*` seed the prefix from
  `InitPrefix<Mask>(clip.bottom)` while the upper half starts at frame row 16,
  so a trapezoid clipped by fewer than 16 bottom rows (including any top- or
  side-only clip) renders its upper half with the prefix of row
  `clip.bottom + (row - 16)` instead of `row`.
* `RenderTransparentSquareClipped` adjusts the prefix by `Width - drawWidth`,
  which is the absolute column only when `clip.right == 0`, and by
  `Width - remainingLeftClip` for a run overshooting the left clip; horizontally
  clipped `TransparentSquare` frames therefore blend a shifted column set.
  Vertical-only clips are exact.

`ClassicMatchesReference` therefore pins the blended pixel set for the prefix
masks only on the unclipped case and accepts either the opaque or the blended
value elsewhere. Changing this would change classic output and needs new
goldens; it is outside the hybrid-lighting work.
