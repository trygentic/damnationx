# tile_geometry_test fixture

`synthetic_level.bin` is a hand-built level tileset in the layout `tile_geometry::BuildPieceGeometryTable`
sees at run time, i.e. **after** `ReencodeDungeonCels`: triangles and trapezoids without padding bytes and
floor `TransparentSquare` frames stored as a triangle followed by a 16-row foliage `TransparentSquare`.
`test/tile_geometry_test.cpp` copies it into `DPieceMicros`/`SOLData` and builds the table from its CEL blob.

## Format (little-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `DXTG` |
| 4 | u32 | version, 1 |
| 8 | u8 | default `MicroTileLen` (12) |
| 9 | u8 | `dungeon_type` (1 = `DTYPE_CATHEDRAL`) |
| 10 | u16 | piece count |
| 12 | count × 34 | per piece: u8 `TileProperties`, u8 reserved (0), 16 × u16 `LevelCelBlock::data` (`type << 12 \| frame`) |
| … | u32 | CEL blob size |
| … | blob | re-encoded dungeon CEL: u32 frame count, u32 offsets of the 1-based frames, u32 blob size, frame data |

## Pieces

| Id | Name | SOL | Micros | Expected |
|---|---|---|---|---|
| 0 | Dirt | Solid+BlockLight+BlockMissile+Transparent | none | Dirt, h 0 |
| 1 | Floor | – | LTri, RTri | Floor, h 0 |
| 2 | WallSW | Solid+BlockLight+BlockMissile | LTrap, RTri, mt[2] Square | Wall, face SE (left half), h 64 |
| 3 | WallSE | Solid+BlockLight+BlockMissile | LTri, RTrap, mt[3] Square | Wall, face SW (right half), h 64 |
| 4 | Pillar | Solid+BlockLight | TSq 20 rows, TSq 10 rows, mt[2] TSq 5 rows | Pillar, faces SE+SW, h 37 |
| 5 | BackWall | Solid+BlockLight+BlockMissile+TransparentLeft | mt[0..4] Square | Wall, face SE (from SOL) + SW (square mt[1]), h 96 |
| 6 | FoliageFloor | – | re-encoded floor TSq ×2 | Floor, h 0 |
| 7 | Stairs | BlockMissile | TSq 16 rows ×2, mt[2] TSq 8 rows | Prop, h 40 |
| 8 | DirtWithTops | Solid+BlockLight+BlockMissile+Transparent | mt[2] LTrap, mt[3] RTrap, mt[4] TSq 32 rows | Dirt, no faces, h 96 |
| 9 | HellHighBlock | Solid+BlockLight | mt[0] Square, mt[12] Square | Pillar, face SE, h 32 (len 12) / 224 (len 16) |
| 10 | DoorwayHorizontal | – | LTri, RTri | Floor; cathedral arch piece (`dSpecial` 2) |
| 11 | DoorwayVertical | – | LTri, RTri | Floor; cathedral arch piece (`dSpecial` 1) |
| 12 | Fence | Solid | LTrap, RTri, mt[2] Square | Wall, face SE, h 64, BlockLight clear |
| 13 | BlockLightFloor | BlockLight | LTri, RTri | Floor, h 0, BlockLight set |
| 14 | WallSEfromSol | Solid+BlockLight+TransparentRight | LTri, TSq (every row partly opaque), mt[3] TSq 12 rows | Wall, face SW, h 44 |
| 15 | TallPillar | Solid+BlockLight+BlockMissile | mt[0..15] Square | Pillar, faces SE+SW, h 192 (len 12) / 255 (len 16, clamped) |
| 16 | TruncatedFrame | Solid | TSq with only 3 encoded rows | Pillar, face SE, h 3 |

Faces: a wall half stands on the cell's centre line, so the left half (LeftTrapezoid ground micro or SOL
`TransparentLeft`) is the SE face (+x, a wall running along y) and the right half (RightTrapezoid or `TransparentRight`) the
SW face (+y, a wall running along x). A Solid piece whose ground micro is a Square or TransparentSquare gets the face of
that half too, without being promoted from Pillar to Wall. The piece names above follow the drawn half.

The fixture floors report h 0 because their upper micros (`mt[2..]`) are empty. Only the ground diamond
(`mt[0]`, `mt[1]`) of a floor is ignored: a real floor that draws something in its upper micros reports that
height, e.g. L1 pieces 25/26 (124 px, BlockLight clear) and L2 pieces 266/558 (118 px, BlockLight set).

Frames (1-based): 1 LTri, 2 RTri, 3 LTrap, 4 RTrap, 5 Square, 6/7/8 TSq with 20/10/5 opaque rows, 9/10 floor
foliage, 11/12 TSq with 16/8 rows, 13 fully opaque TSq, 14 TSq with `-4, 24 px, -4` in every row, 15 TSq with
12 rows, 16 TSq truncated after 3 rows. Every opaque pixel is palette index 0x21.

## Arch facings (`ArchFacing`)

The facings hard-coded in `tile_geometry.cpp` were checked against `levels/l1data/l1s.cel`, `l1.til` and
`levels/l2data/l2s.cel`, `l2.til` from `diabdat.mpq`: the top edge of an SE-facing arch rises to the right on
screen (parallel to the SE tile edge, `foot(c) = c/2`), an SW-facing one falls to the right, and the megatiles
place cathedral doorways 11/254/258/266 (`dSpecial` 1/4/5/6) at +y of their wall piece (a wall running along
y shows its SE face) and 10/252 (`dSpecial` 2/3) at +x (SW face). Catacombs `dSpecial` 1/2 are drawn on the
cells at +y of post piece 131 (SE), 3/4 on the cells at +x of 134/138 (SW). Frames 7/8 (L1) and 5/6 (L2)
are door leaves, not arches. All L1/L2 special frames are 160 px tall, so the lintel is 128 px in the game
and the 64 px default in tests (no `pSpecialCels`).

## Regenerating

```python
#!/usr/bin/env python3
import struct, sys
SQUARE, TSQUARE, LTRI, RTRI, LTRAP, RTRAP = range(6)
SOLID, BLOCK_LIGHT, BLOCK_MISSILE, TRANSPARENT, TRANSPARENT_LEFT, TRANSPARENT_RIGHT = 1, 2, 4, 8, 0x10, 0x20
PX = 0x21
def triangle_rows(): return list(range(2, 33, 2)) + list(range(30, 0, -2))
def frame_square(): return bytes([PX]) * (32 * 32)
def frame_triangle(): return b"".join(bytes([PX]) * w for w in triangle_rows())
def frame_trapezoid(): return b"".join(bytes([PX]) * w for w in range(2, 33, 2)) + bytes([PX]) * (32 * 16)
def frame_tsquare(opaque_rows, total_rows=32):
    return b"".join((bytes([32]) + bytes([PX]) * 32) if r < opaque_rows else bytes([0xE0]) for r in range(total_rows))
def frame_tsquare_mixed(): return (bytes([0xFC, 24]) + bytes([PX]) * 24 + bytes([0xFC])) * 32
def frame_foliage_floor(): return frame_triangle() + frame_tsquare(2, total_rows=16)
frames = [frame_triangle(), frame_triangle(), frame_trapezoid(), frame_trapezoid(), frame_square(),
          frame_tsquare(20), frame_tsquare(10), frame_tsquare(5), frame_foliage_floor(), frame_foliage_floor(),
          frame_tsquare(16), frame_tsquare(8), frame_tsquare(32), frame_tsquare_mixed(), frame_tsquare(12),
          frame_tsquare(3, 3)]
def block(t, f): return (t << 12) | f
def piece(sol, blocks):
    mt = [0] * 16
    for i, v in blocks.items(): mt[i] = v
    return sol, mt
pieces = [
    piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT, {}),
    piece(0, {0: block(LTRI, 1), 1: block(RTRI, 2)}),
    piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE, {0: block(LTRAP, 3), 1: block(RTRI, 2), 2: block(SQUARE, 5)}),
    piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE, {0: block(LTRI, 1), 1: block(RTRAP, 4), 3: block(SQUARE, 5)}),
    piece(SOLID | BLOCK_LIGHT, {0: block(TSQUARE, 6), 1: block(TSQUARE, 7), 2: block(TSQUARE, 8)}),
    piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT_LEFT, {k: block(SQUARE, 5) for k in range(5)}),
    piece(0, {0: block(TSQUARE, 9), 1: block(TSQUARE, 10)}),
    piece(BLOCK_MISSILE, {0: block(TSQUARE, 11), 1: block(TSQUARE, 11), 2: block(TSQUARE, 12)}),
    piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE | TRANSPARENT, {2: block(LTRAP, 3), 3: block(RTRAP, 4), 4: block(TSQUARE, 13)}),
    piece(SOLID | BLOCK_LIGHT, {0: block(SQUARE, 5), 12: block(SQUARE, 5)}),
    piece(0, {0: block(LTRI, 1), 1: block(RTRI, 2)}),
    piece(0, {0: block(LTRI, 1), 1: block(RTRI, 2)}),
    piece(SOLID, {0: block(LTRAP, 3), 1: block(RTRI, 2), 2: block(SQUARE, 5)}),
    piece(BLOCK_LIGHT, {0: block(LTRI, 1), 1: block(RTRI, 2)}),
    piece(SOLID | BLOCK_LIGHT | TRANSPARENT_RIGHT, {0: block(LTRI, 1), 1: block(TSQUARE, 14), 3: block(TSQUARE, 15)}),
    piece(SOLID | BLOCK_LIGHT | BLOCK_MISSILE, {k: block(SQUARE, 5) for k in range(16)}),
    piece(SOLID, {0: block(TSQUARE, 16)}),
]
out = b"DXTG" + struct.pack("<IBBH", 1, 12, 1, len(pieces))
for sol, mt in pieces: out += struct.pack("<BB16H", sol, 0, *mt)
cel = struct.pack("<I", len(frames)); offset = 4 * (len(frames) + 2)
for f in frames: cel += struct.pack("<I", offset); offset += len(f)
cel += struct.pack("<I", offset) + b"".join(frames)
out += struct.pack("<I", len(cel)) + cel
open(sys.argv[1], "wb").write(out)
```

Run it as `python3 gen.py test/fixtures/tile_geometry/synthetic_level.bin` from the repository root.
