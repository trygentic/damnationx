// Source/levels/tile_geometry.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include "engine/point.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung_defs.hpp"
#include "levels/tile_properties.hpp"

namespace devilution::tile_geometry {

enum class PieceKind : uint8_t { Dirt = 0, Floor = 1, Wall = 2, Pillar = 3, Prop = 4 };
enum class Face : uint8_t { SW = 1, SE = 2 };
struct PieceGeometry {
	uint8_t heightPx = 0; // topmost opaque row above the tile base across mt[0..MicroTileLen-1], clamped to 255
	// Face bits. A wall half stands on the cell's centre line: SE (plane x = i + 0.5, facing +x) when mt[0] is LeftTrapezoid or SOL
	// TransparentLeft, SW (plane y = j + 0.5, facing +y) when mt[1] is RightTrapezoid or TransparentRight; a Solid piece standing on a
	// Square / TransparentSquare ground micro gets that half's face too (it stays a Pillar).
	uint8_t faces = 0;
	PieceKind kind = PieceKind::Dirt;
	uint8_t lintelPx = 0; // arches only: bottom of the lintel above the base (light passes below), else 0
};

/** Builds the table for the loaded level. One call appended at the end of SetDungeonMicros (Source/levels/dun_tile_data.cpp:216). */
void BuildPieceGeometryTable(const std::byte *dungeonCels, std::span<const MICROS> micros, std::span<const TileProperties> sol, uint_fast8_t microTileLen, dungeon_type level);
[[nodiscard]] const PieceGeometry &GetPieceGeometry(uint16_t piece);
[[nodiscard]] uint8_t ArchFacing(dungeon_type level, int8_t dSpecialValue); // 1 = SW (WallLeft), 2 = SE (WallRight), 0 = not an arch

// Packed per-cell u32 for the GPU (112x112, uploaded as an r32uint texture; the legacy occluder buffer is a separate 0/1 array)
namespace volume {
constexpr uint32_t HeightMask = 0xFFu; // bits 0-7   heightPx (0..255)
constexpr uint32_t FaceSW = 1u << 8, FaceSE = 1u << 9; // PieceGeometry::faces: the DDA occludes only behind these face planes
constexpr uint32_t BlockLight = 1u << 10; // authoritative TileProperties::BlockLight (never derived from height)
constexpr uint32_t Solid = 1u << 11;
constexpr uint32_t Visible = 1u << 12;  // DungeonFlag::Visible this frame
constexpr uint32_t Explored = 1u << 13; // DungeonFlag::Explored
constexpr uint32_t Arch = 1u << 14;     // explored arch cells occlude by lintel only, independent of BlockLight
constexpr uint32_t DoorOpen = 1u << 15; // informational (debug view); occlusion follows BlockLight of the current dPiece
constexpr uint32_t Blended = 1u << 16;  // TransList[dTransVal] this frame
constexpr uint32_t DLightShift = 17;    // bits 17-20 authoritative dLight 0..15 (ambient + gate)
constexpr uint32_t LintelShift = 21;    // bits 21-27 lintelPx / 2 (0..254 px)
} // namespace volume

struct OccluderVolume {
	std::array<uint32_t, MAXDUNX * MAXDUNY> cells {};
	uint64_t staticHash = 0; // word-hash of dPiece + dSpecial (u64 words, multiply-xorshift mix; ~5 us)
	uint32_t generation = 0; // incremented whenever any cell changed (compare-and-write in the pack loop); the presenter uploads only when it changes
};
using DoorOpenFn = bool (*)(Point tile); // supplied by dx.cpp (FindObjectAtPosition(tile)->isDoor() && _oVar4 == DOOR_OPEN); nullptr in tests; only called for cells with dObject != 0
/** Rebuilds static bits when the dPiece/dSpecial hash changes, refreshes dynamic bits (Visible/Explored/Blended/DoorOpen/dLight) every call
 *  with compare-and-write, bumps `generation` when anything changed. Returns true when generation changed. */
bool UpdateOccluderVolume(OccluderVolume &volume, DoorOpenFn doorOpen);

namespace detail {
// Arch occluder band helpers, defined in dun_tile_data.cpp next to the piece table and used by UpdateOccluderVolume.
[[nodiscard]] uint8_t ArchLintelPx(int8_t special);
[[nodiscard]] int ArchOccluderTopPx(int8_t special);
// Incremented by every BuildPieceGeometryTable call; mixed into OccluderVolume::staticHash.
[[nodiscard]] uint32_t PieceGeometryTableBuilds();
} // namespace detail

} // namespace devilution::tile_geometry
