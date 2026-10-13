# scrollrt golden fixtures

Consumed by `test/scrollrt_golden_test.cpp`. Copied next to the test binary as
`test/fixtures/scrollrt/` by `test/Fixtures.cmake`.

## synthetic_level.bin

A hand-built 24x24 Cathedral level in the layout the world traversal sees at run time: the piece table
(`SOLData` + `DPieceMicros`), the per-cell map layers, a re-encoded dungeon CEL blob (`pDungeonCels`, frames
after `ReencodeDungeonCels`: triangles/trapezoids without padding, floor `TransparentSquare` frames stored as
a triangle followed by a 16-row foliage `TransparentSquare`) and a CLX sprite list for `pSpecialCels` (arches).
The test copies it into the level globals, sets `TransList[1] = true`, builds the light tables with
`MakeLightTable()` and a synthetic blending lookup, and drives `DrawWorldForTesting` for a 640x480 screen with
the 128-row main panel (viewport 640x352).

### Format (little-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `DXSL` |
| 4 | u32 | version, 1 |
| 8 | u8 | `MicroTileLen` (10) |
| 9 | u8 | `dungeon_type` (1 = `DTYPE_CATHEDRAL`) |
| 10 | u8 | map width (24) |
| 11 | u8 | map height (24) |
| 12 | u8 | origin x in `dPiece` coordinates (0) |
| 13 | u8 | origin y (0) |
| 14 | u16 | piece count |
| 16 | count x 34 | per piece: u8 `TileProperties`, u8 reserved (0), 16 x u16 `LevelCelBlock::data` (`type << 12 \| frame`) |
| ... | width x height x 6 | per cell, x-major: u16 `dPiece`, u8 `dLight`, u8 `dFlags`, i8 `dSpecial`, i8 `dTransVal` |
| ... | u32 + blob | dungeon CEL: u32 frame count, u32 offsets of the 1-based frames, u32 end offset, frame data |
| ... | u32 + blob | special CLX list: u32 sprite count, u32 offsets, u32 end offset, CLX frames |

### Pieces

| Id | Name | SOL | Micros |
|---|---|---|---|
| 0 | Dirt | Solid+BlockLight+BlockMissile+Transparent | none (every cell outside the block) |
| 1 | Floor | - | LTri 1, RTri 2 |
| 2 | WallSW | Solid+BlockLight+BlockMissile | LTrap 3, RTri 2, mt[2] Square 5, mt[4] Square 6 |
| 3 | WallSE | Solid+BlockLight+BlockMissile | LTri 1, RTrap 4, mt[3] Square 5, mt[5] Square 6 |
| 4 | Pillar | Solid+BlockLight | mt[0..3] TSq 7 (4 px hole on the left of two rows in three) |
| 5 | WallSWTransparent | Solid+BlockLight+BlockMissile+Transparent+TransparentLeft | as WallSW (blended when `TransList[dTransVal]`) |
| 6 | FoliageFloor | - | TSq 8, TSq 9 (floor triangle + 11-row foliage) |
| 7 | DirtEdge | Solid+BlockLight+BlockMissile+Transparent | LTri 11, RTri 12, mt[2] TSq 10 (20 rows); also the `DrawOOB` sample at (0..1, 0..1) |
| 8 | Doorway | - | LTri 11, RTri 12; the cell carries `dSpecial` |
| 9 | BackWall | Solid+BlockLight+BlockMissile+TransparentLeft | Square 5, RTri 2, mt[2] Square 6, mt[4] Square 5 (SW face from SOL) |

Frames (1-based): 1/2 floor triangles, 3/4 wall trapezoids, 5/6 squares, 7 pillar `TransparentSquare`,
8/9 floor + foliage, 10 dirt top (20 opaque rows, 8 px hole), 11/12 second floor style. Opaque pixels are
`1..255` from a fixed xorshift32 seed (never 0, which the sprite blitter treats as a shadow).

### Map

* The outer ring is `DirtEdge`; the interior is `Floor`, with `FoliageFloor` where `(x + y) % 7 == 0`.
* `WallSE` along `x = 6`, `y = 3..12`; `WallSW` along `y = 6`, `x = 3..12`; `Pillar` at their corner (6, 6)
  and at (10, 10), (14, 14), (18, 8), (20, 19).
* `WallSWTransparent` at `y = 14`, `x = 8..12` with `dTransVal = 1` on those cells and the cells above and
  below (`y = 13`, `y = 15`).
* `BackWall` along `x = 16`, `y = 3..8`.
* `Doorway` cells with an arch: (9, 6) `dSpecial = 2` (SW-facing, in the x wall), (6, 9) `dSpecial = 1`
  (SE-facing, in the y wall) and (19, 4) `dSpecial = 1` (free-standing).
* `dLight` = `min(15, 3 * max(|x - 11|, |y - 11|) / 2, max(|x - 18|, |y - 5|) + 2)`: two light centres,
  fully dark (15) at the edges. `dFlags` = Explored everywhere, plus Visible|Lit where the cell is not fully
  dark.

### Special sprites

`dSpecial` is a 1-based index into the list: sprite 0 is a 64x128 arch, sprite 1 a 64x96 arch. Each is an
opaque frame around a 32x72 doorway hole at the bottom (columns 16..47); the top half of the lintel carries
runs of 8 identical pixels so both literal and fill CLX commands are exercised.

## goldens.tsv

`case<TAB>FNV-1a 64 hex` of the 640x352 frame for the 24 classic cases (6 view positions x zoom on/off x
per-pixel lighting on/off) with the hybrid mode latched off. Recorded after the DrawCell flush fix and before any
G-buffer code touched the traversal. Re-record with

```sh
SCROLLRT_GOLDEN_RECORD=/abs/path/goldens.tsv ./scrollrt_golden_test --gtest_filter='ScrollrtGolden.*'
```

from the build directory (the fixture is read from `test/fixtures/scrollrt/` relative to it).

## odd_zoom_goldens.tsv

The odd-height zoom cases of `ScrollrtGolden.OddHeightZoomDiffersFromMasterOnlyInRowZero`: a 640x479 screen (viewport
351 rows, so `Zoom()` takes its odd-height tail) at view 11,11 with zoom on and the light tables, with no side panel,
the character panel (left) and the inventory (right) open. The frame is drawn below one guard row. Columns:
`case`, FNV-1a 64 of master's rows 1..350, of master's row 0, whether master left the guard row `intact` or
`overwritten`, and of the current row 0.

Master's tail copied output row 1 over row 0 (no panel), the first 320 bytes of row 1 over row 0 (left panel), or
wrote 320 bytes before row 0 and left row 0 unzoomed (right panel; before `PalSurface->pixels` in the game). The tail
now doubles the top source row into row 0, like `gbuffer::ZoomInPlace`. The test asserts rows 1..350 are master's
bytes, row 0 is the one row that differs, row 0 is pixel-doubled across the viewport and the guard row is intact:
this is the one intended classic frame change, made together with the DrawCell flush fix.

The three master columns were recorded with the upstream `Zoom()` by building this test against it with the
current fixtures and running

```sh
SCROLLRT_ODD_ZOOM_RECORD=/abs/path/odd.tsv ./scrollrt_golden_test --gtest_filter='ScrollrtGolden.OddHeight*'
```

(in record mode the last column repeats that build's row 0); the last column was then recorded the same way on the
fixed build. The same upstream build reproduced all 24 `goldens.tsv` hashes.

## Regenerating synthetic_level.bin

The generator below produces the file byte-for-byte. Run it as
`python3 gen.py test/fixtures/scrollrt/synthetic_level.bin` from the repository root.

```python
#!/usr/bin/env python3
import struct
import sys

SQUARE, TSQUARE, LTRI, RTRI, LTRAP, RTRAP = range(6)
SOLID, BLOCK_LIGHT, BLOCK_MISSILE, TRANSPARENT, TRANSPARENT_LEFT, TRANSPARENT_RIGHT = 1, 2, 4, 8, 0x10, 0x20
FLAG_VISIBLE, FLAG_LIT, FLAG_EXPLORED = 1 << 1, 1 << 6, 1 << 7
WIDTH, HEIGHT, MICRO_TILE_LEN, DTYPE_CATHEDRAL = 24, 24, 10, 1


class Rng:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x & 0xFFFFFFFF
        return self.s

    def pixel(self):
        return 1 + self.next() % 255


LOWER = [2 * (i + 1) for i in range(16)]
UPPER = [32 - 2 * i for i in range(1, 16)]


def square(rng):
    return bytes(rng.pixel() for _ in range(1024))


def triangle(rng):
    return bytes(rng.pixel() for w in LOWER + UPPER for _ in range(w))


def trapezoid(rng):
    return bytes(rng.pixel() for w in LOWER + [32] * 16 for _ in range(w))


def tsquare(rng, rows, opaque_rows, gap):
    out = bytearray()
    for r in range(rows):
        if r >= opaque_rows:
            out.append(0xE0)
            continue
        g = gap if (r % 3) != 2 else 0
        if g > 0:
            out.append((-g) & 0xFF)
        out.append(32 - g)
        out += bytes(rng.pixel() for _ in range(32 - g))
    return bytes(out)


def foliage_floor(rng, right):
    return triangle(rng) + tsquare(rng, 16, 11, 6 if right else 3)


def clx_sprite(width, height, pixels):
    frame = bytearray(struct.pack("<HHH", 6, width, height))
    for y in range(height - 1, -1, -1):
        row = pixels[y * width:(y + 1) * width]
        x = 0
        while x < width:
            if row[x] < 0:
                n = 0
                while x < width and row[x] < 0 and n < 0x7F:
                    n += 1
                    x += 1
                frame.append(n)
                continue
            same = 1
            while x + same < width and row[x + same] == row[x] and same < 63:
                same += 1
            if same >= 3:
                frame.append(0xBF - same)
                frame.append(row[x])
                x += same
                continue
            n = 0
            control = len(frame)
            frame.append(0)
            while x < width and row[x] >= 0 and n < 63 and not (x + 2 < width and row[x] == row[x + 1] and row[x] == row[x + 2]):
                frame.append(row[x])
                n += 1
                x += 1
            frame[control] = (256 - n) & 0xFF
    return bytes(frame)


def clx_list(frames):
    header = struct.pack("<I", len(frames))
    offset = 4 * (len(frames) + 2)
    for f in frames:
        header += struct.pack("<I", offset)
        offset += len(f)
    header += struct.pack("<I", offset)
    return header + b"".join(frames)


def arch_sprite(rng, h):
    w = 64
    lintel = h - 72
    px = [-1] * (w * h)
    for y in range(h):
        for x in range(w):
            hole = 16 <= x < 48 and y >= lintel
            if hole:
                continue
            if y < lintel // 2 and (y // 4) % 2 == 0:
                px[y * w + x] = 40 + (x // 8)
            else:
                px[y * w + x] = rng.pixel()
    return clx_sprite(w, h, px)


def block(t, f):
    return (t << 12) | f


def piece(sol, blocks):
    mt = [0] * 16
    for i, v in blocks.items():
        mt[i] = v
    return sol, mt


def main(path):
    rng = Rng(0x5C30117)
    frames = [
        triangle(rng), triangle(rng), trapezoid(rng), trapezoid(rng), square(rng), square(rng),
        tsquare(rng, 32, 32, 4), foliage_floor(rng, right=False), foliage_floor(rng, right=True),
        tsquare(rng, 32, 20, 8), triangle(rng), triangle(rng),
    ]
    pieces = [
        piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT, {}),
        piece(0, {0: block(LTRI, 1), 1: block(RTRI, 2)}),
        piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE, {0: block(LTRAP, 3), 1: block(RTRI, 2), 2: block(SQUARE, 5), 4: block(SQUARE, 6)}),
        piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE, {0: block(LTRI, 1), 1: block(RTRAP, 4), 3: block(SQUARE, 5), 5: block(SQUARE, 6)}),
        piece(SOLID | BLOCK_LIGHT, {0: block(TSQUARE, 7), 1: block(TSQUARE, 7), 2: block(TSQUARE, 7), 3: block(TSQUARE, 7)}),
        piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT | TRANSPARENT_LEFT, {0: block(LTRAP, 3), 1: block(RTRI, 2), 2: block(SQUARE, 5), 4: block(SQUARE, 6)}),
        piece(0, {0: block(TSQUARE, 8), 1: block(TSQUARE, 9)}),
        piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT, {0: block(LTRI, 11), 1: block(RTRI, 12), 2: block(TSQUARE, 10)}),
        piece(0, {0: block(LTRI, 11), 1: block(RTRI, 12)}),
        piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT_LEFT, {0: block(SQUARE, 5), 1: block(RTRI, 2), 2: block(SQUARE, 6), 4: block(SQUARE, 5)}),
    ]
    d_piece = [[1] * HEIGHT for _ in range(WIDTH)]
    d_special = [[0] * HEIGHT for _ in range(WIDTH)]
    d_trans = [[0] * HEIGHT for _ in range(WIDTH)]
    for x in range(WIDTH):
        for y in range(HEIGHT):
            if x == 0 or y == 0 or x == WIDTH - 1 or y == HEIGHT - 1:
                d_piece[x][y] = 7
            elif (x + y) % 7 == 0:
                d_piece[x][y] = 6
    for y in range(3, 13):
        d_piece[6][y] = 3
    for x in range(3, 13):
        d_piece[x][6] = 2
    d_piece[6][6] = 4
    for x in range(8, 13):
        d_piece[x][14] = 5
        d_trans[x][14] = 1
        d_trans[x][15] = 1
        d_trans[x][13] = 1
    for y in range(3, 9):
        d_piece[16][y] = 9
    for (x, y) in [(10, 10), (14, 14), (18, 8), (20, 19)]:
        d_piece[x][y] = 4
    d_piece[9][6] = 8
    d_special[9][6] = 2
    d_piece[6][9] = 8
    d_special[6][9] = 1
    d_piece[19][4] = 8
    d_special[19][4] = 1

    def light_at(x, y):
        d1 = (3 * max(abs(x - 11), abs(y - 11))) // 2
        d2 = max(abs(x - 18), abs(y - 5)) + 2
        return min(15, d1, d2)

    out = b"DXSL" + struct.pack("<IBBBBBBH", 1, MICRO_TILE_LEN, DTYPE_CATHEDRAL, WIDTH, HEIGHT, 0, 0, len(pieces))
    for sol, mt in pieces:
        out += struct.pack("<BB16H", sol, 0, *mt)
    for x in range(WIDTH):
        for y in range(HEIGHT):
            light = light_at(x, y)
            flags = FLAG_EXPLORED
            if light < 15:
                flags |= FLAG_VISIBLE | FLAG_LIT
            out += struct.pack("<HBBbb", d_piece[x][y], light, flags, d_special[x][y], d_trans[x][y])
    cel = clx_list(frames)
    out += struct.pack("<I", len(cel)) + cel
    special = clx_list([arch_sprite(rng, 128), arch_sprite(rng, 96)])
    out += struct.pack("<I", len(special)) + special
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main(sys.argv[1])
```
