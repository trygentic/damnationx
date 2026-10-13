/**
 * @file tile_geometry.cpp
 *
 * Packed occluder volume for the 2.5D hybrid lighting.
 *
 * Reads only the dun_tile_data globals (dPiece, dFlags, dTransVal, TransList, dLight, dSpecial, dObject,
 * SOLData, leveltype) and never anything from gendung/drlg, so that tile_geometry_test links without
 * libdevilutionx.
 *
 * The per-piece table (BuildPieceGeometryTable, GetPieceGeometry) is defined in dun_tile_data.cpp:
 * SetDungeonMicros calls BuildPieceGeometryTable, and every target that links libdevilutionx_dun_tile_data
 * (the dun_render benchmark and golden test among them) must resolve it without linking this library.
 */
#include "levels/tile_geometry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "engine/point.hpp"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung_defs.hpp"
#include "utils/enum_traits.h"

namespace devilution::tile_geometry {

namespace {

constexpr uint8_t FaceSWBit = static_cast<uint8_t>(Face::SW);
constexpr uint8_t FaceSEBit = static_cast<uint8_t>(Face::SE);

/**
 * @brief Multiply-xorshift hash over `dPiece` and `dSpecial` read as u64 words, plus the piece
 * table build count so that a table rebuilt for another tileset (or another `pSpecialCels`) re-packs the
 * static bits even when `dPiece` and `dSpecial` hash the same.
 */
uint64_t HashStaticState()
{
	constexpr uint64_t Mul = 0x9E3779B97F4A7C15ULL;
	uint64_t hash = 0x2545F4914F6CDD1DULL;
	const auto mixWords = [&hash](const void *data, size_t bytes) {
		const auto *bytesPtr = static_cast<const std::byte *>(data);
		size_t offset = 0;
		for (; offset + sizeof(uint64_t) <= bytes; offset += sizeof(uint64_t)) {
			uint64_t word;
			std::memcpy(&word, bytesPtr + offset, sizeof(word));
			hash = (hash ^ word) * Mul;
			hash ^= hash >> 29;
		}
		if (offset < bytes) {
			uint64_t word = 0;
			std::memcpy(&word, bytesPtr + offset, bytes - offset);
			hash = (hash ^ word) * Mul;
			hash ^= hash >> 29;
		}
	};
	mixWords(dPiece, sizeof(dPiece));
	mixWords(dSpecial, sizeof(dSpecial));
	const uint32_t tableBuilds = detail::PieceGeometryTableBuilds();
	mixWords(&tableBuilds, sizeof(tableBuilds));
	hash *= Mul;
	hash ^= hash >> 32;
	return hash == 0 ? 1 : hash; // 0 is the "never hashed" state of OccluderVolume::staticHash
}

constexpr uint32_t StaticMask = volume::HeightMask | volume::FaceSW | volume::FaceSE | volume::BlockLight | volume::Solid | volume::Arch | (0x7Fu << volume::LintelShift);
constexpr uint32_t DynamicMask = volume::Visible | volume::Explored | volume::DoorOpen | volume::Blended | (0xFu << volume::DLightShift);
static_assert((StaticMask & DynamicMask) == 0);
static_assert((StaticMask | DynamicMask) == 0x0FFFFFFFu);

uint32_t PackStatic(uint16_t piece, int8_t special)
{
	const PieceGeometry &geometry = GetPieceGeometry(piece);
	const TileProperties sol = piece < MAXTILES ? SOLData[piece] : TileProperties::None;
	uint32_t word = geometry.heightPx;
	if ((geometry.faces & FaceSWBit) != 0) word |= volume::FaceSW;
	if ((geometry.faces & FaceSEBit) != 0) word |= volume::FaceSE;
	// Authoritative gameplay bits, copied verbatim (never derived from height).
	if (HasAnyOf(sol, TileProperties::BlockLight)) word |= volume::BlockLight;
	if (HasAnyOf(sol, TileProperties::Solid)) word |= volume::Solid;
	if (ArchFacing(leveltype, special) != 0) {
		// The doorway piece is a walkable floor; the arch sprite is the occluding shape (lintel..top band).
		const int archHeight = std::max(detail::ArchOccluderTopPx(special), static_cast<int>(geometry.heightPx));
		word = (word & ~volume::HeightMask) | static_cast<uint32_t>(std::min(archHeight, 255));
		word |= volume::Arch;
		word |= static_cast<uint32_t>(detail::ArchLintelPx(special) / 2) << volume::LintelShift;
	}
	return word;
}

} // namespace

uint8_t ArchFacing(dungeon_type level, int8_t dSpecialValue)
{
	// Facings checked against the l1s/l2s sprites and the megatiles that place the doorways: an SE-facing
	// arch's lintel rises to the right on screen (parallel to the SE tile edge), an SW-facing one falls to
	// the right. See test/fixtures/tile_geometry/README.md.
	switch (level) {
	case DTYPE_CATHEDRAL:
		switch (dSpecialValue) {
		case 1: // pieces 11, 70, 320, 210, 340, 417: doorway in a wall running along y
		case 4: // piece 254
		case 5: // piece 258
		case 6: // piece 266
			return FaceSEBit;
		case 2: // pieces 10, 248, 324, 343, 330, 420: doorway in a wall running along x
		case 3: // piece 252
			return FaceSWBit;
		default: // 7, 8: door leaves (objects.cpp)
			return 0;
		}
	case DTYPE_CATACOMBS:
		switch (dSpecialValue) {
		case 1: // cells (i, j+2) and (i, j+1) past post piece 131
		case 2:
			return FaceSEBit;
		case 3: // cells (i+1, j) and (i+2, j) past post pieces 134/138
		case 4:
			return FaceSWBit;
		default: // 5, 6: door leaves (objects.cpp)
			return 0;
		}
	default: // town trees, crypt door leaves (1/2); caves, hell and nest have no specials
		return 0;
	}
}

bool UpdateOccluderVolume(OccluderVolume &volume, DoorOpenFn doorOpen)
{
	const uint64_t hash = HashStaticState();
	const bool rebuildStatic = hash != volume.staticHash;
	volume.staticHash = hash;

	bool changed = false;
	// Texture layout: texel (x, y) lives at cells[y * MAXDUNX + x], read as `textureLoad(volumeTex, vec2i(x, y))`.
	for (int y = 0; y < MAXDUNY; ++y) {
		uint32_t *row = &volume.cells[static_cast<size_t>(y) * MAXDUNX];
		for (int x = 0; x < MAXDUNX; ++x) {
			uint32_t word = rebuildStatic ? PackStatic(dPiece[x][y], dSpecial[x][y]) : (row[x] & StaticMask);
			const DungeonFlag flags = dFlags[x][y];
			if (HasAnyOf(flags, DungeonFlag::Visible)) word |= volume::Visible;
			// Hidden information is what classic never draws lit. A cell is not hidden when the automap explored it,
			// when it is in the player's current vision, or when classic lighting reaches it (dLight below 15):
			// keying the no-reveal bit on the vision flags alone left cells classic drew lit black in hybrid mode.
			const uint8_t light = std::min<uint8_t>(dLight[x][y], 15);
			if (HasAnyOf(flags, DungeonFlag::Explored) || HasAnyOf(flags, DungeonFlag::Visible) || light < 15) word |= volume::Explored;
			if (TransList[static_cast<uint8_t>(dTransVal[x][y])]) word |= volume::Blended;
			word |= static_cast<uint32_t>(light) << volume::DLightShift;
			if (dObject[x][y] != 0 && doorOpen != nullptr && doorOpen(Point { x, y })) word |= volume::DoorOpen;
			if (word != row[x]) {
				row[x] = word;
				changed = true;
			}
		}
	}
	if (changed) ++volume.generation;
	return changed;
}

} // namespace devilution::tile_geometry
