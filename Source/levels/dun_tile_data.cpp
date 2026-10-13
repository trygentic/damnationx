#include "levels/dun_tile_data.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "engine/clx_sprite.hpp"
#include "engine/load_file.hpp"
#include "engine/point.hpp"
#include "engine/world_tile.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung_defs.hpp"
#include "levels/reencode_dun_cels.hpp"
#include "levels/tile_geometry.hpp"
#include "levels/tile_properties.hpp"
#include "utils/algorithm/container.hpp"
#include "utils/bitset2d.hpp"
#include "utils/endian_swap.hpp"
#include "utils/enum_traits.h"
#include "utils/status_macros.hpp"

namespace devilution {

uint8_t currlevel;
bool setlevel;

Bitset2d<DMAXX, DMAXY> DungeonMask;
uint8_t dungeon[DMAXX][DMAXY];
uint8_t pdungeon[DMAXX][DMAXY];
Bitset2d<DMAXX, DMAXY> Protected;
WorldTileRectangle SetPieceRoom;
WorldTileRectangle SetPiece;
OptionalOwnedClxSpriteList pSpecialCels;
std::unique_ptr<MegaTile[]> pMegaTiles;
std::unique_ptr<std::byte[]> pDungeonCels;
TileProperties SOLData[MAXTILES];
WorldTilePosition dminPosition;
WorldTilePosition dmaxPosition;
dungeon_type leveltype;
_setlevels setlvlnum;
dungeon_type setlvltype;
Point ViewPosition;
uint_fast8_t MicroTileLen;
int8_t TransVal;
std::array<bool, 256> TransList;
uint16_t dPiece[MAXDUNX][MAXDUNY];
MICROS DPieceMicros[MAXTILES];
int8_t dTransVal[MAXDUNX][MAXDUNY];
uint8_t dLight[MAXDUNX][MAXDUNY];
uint8_t dPreLight[MAXDUNX][MAXDUNY];
DungeonFlag dFlags[MAXDUNX][MAXDUNY];
int8_t dPlayer[MAXDUNX][MAXDUNY];
int16_t dMonster[MAXDUNX][MAXDUNY];
int8_t dCorpse[MAXDUNX][MAXDUNY];
int8_t dObject[MAXDUNX][MAXDUNY];
int8_t dSpecial[MAXDUNX][MAXDUNY];
int themeCount;
THEME_LOC themeLoc[MAXTHEMES];
uint32_t DungeonSeeds[NUMLEVELS];
std::optional<uint32_t> LevelSeeds[NUMLEVELS];

namespace {

std::unique_ptr<uint16_t[]> LoadMinData(size_t &tileCount)
{
	switch (leveltype) {
	case DTYPE_TOWN: {
		auto min = LoadFileInMemWithStatus<uint16_t>("nlevels\\towndata\\town.min", &tileCount);
		if (!min.has_value()) {
			return LoadFileInMem<uint16_t>("levels\\towndata\\town.min", &tileCount);
		} else {
			return std::move(*min);
		}
	}
	case DTYPE_CATHEDRAL:
		return LoadFileInMem<uint16_t>("levels\\l1data\\l1.min", &tileCount);
	case DTYPE_CATACOMBS:
		return LoadFileInMem<uint16_t>("levels\\l2data\\l2.min", &tileCount);
	case DTYPE_CAVES:
		return LoadFileInMem<uint16_t>("levels\\l3data\\l3.min", &tileCount);
	case DTYPE_HELL:
		return LoadFileInMem<uint16_t>("levels\\l4data\\l4.min", &tileCount);
	case DTYPE_NEST:
		return LoadFileInMem<uint16_t>("nlevels\\l6data\\l6.min", &tileCount);
	case DTYPE_CRYPT:
		return LoadFileInMem<uint16_t>("nlevels\\l5data\\l5.min", &tileCount);
	default:
		app_fatal("LoadMinData");
	}
}

} // namespace

std::expected<void, std::string> LoadLevelSOLData()
{
	switch (leveltype) {
	case DTYPE_TOWN:
		if (!LoadIntegralFileInMemWithStatus("nlevels\\towndata\\town.sol", SOLData).has_value()) {
			RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("levels\\towndata\\town.sol", SOLData));
		}
		break;
	case DTYPE_CATHEDRAL:
		RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("levels\\l1data\\l1.sol", SOLData));
		// Fix incorrectly marked arched tiles
		SOLData[9] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[15] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[16] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[20] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[21] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[27] |= TileProperties::BlockMissile;
		SOLData[28] |= TileProperties::BlockMissile;
		SOLData[51] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[56] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[58] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[61] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[63] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[65] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[72] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[208] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[247] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[253] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[257] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[323] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		SOLData[403] |= TileProperties::BlockLight;
		// Fix incorrectly marked pillar tile
		SOLData[24] |= TileProperties::BlockLight;
		// Fix incorrectly marked wall tile
		SOLData[450] |= TileProperties::BlockLight | TileProperties::BlockMissile;
		break;
	case DTYPE_CATACOMBS:
		RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("levels\\l2data\\l2.sol", SOLData));
		break;
	case DTYPE_CAVES:
		RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("levels\\l3data\\l3.sol", SOLData));
		// The graphics for tile 48 sub-tile 171 frame 461 are partly incorrect, as they
		// have a few pixels that should belong to the solid tile 49 instead.
		// Marks the sub-tile as "BlockMissile" to avoid treating it as a floor during rendering.
		SOLData[170] |= TileProperties::BlockMissile;
		// Fence sub-tiles 481 and 487 are substitutes for solid sub-tiles 473 and 479
		// but are not marked as solid.
		SOLData[481] |= TileProperties::Solid;
		SOLData[487] |= TileProperties::Solid;
		break;
	case DTYPE_HELL:
		RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("levels\\l4data\\l4.sol", SOLData));
		SOLData[210] = TileProperties::None; // Tile is incorrectly marked as being solid
		break;
	case DTYPE_NEST:
		RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("nlevels\\l6data\\l6.sol", SOLData));
		break;
	case DTYPE_CRYPT:
		RETURN_IF_ERROR(LoadIntegralFileInMemWithStatus("nlevels\\l5data\\l5.sol", SOLData));
		SOLData[142] = TileProperties::None; // Tile is incorrectly marked as being solid
		break;
	default:
		return std::unexpected("LoadLevelSOLData");
	}
	return {};
}

void SetDungeonMicros(std::unique_ptr<std::byte[]> &dungeonCels, uint_fast8_t &microTileLen)
{
	microTileLen = 10;
	size_t blocks = 10;

	if (leveltype == DTYPE_TOWN) {
		microTileLen = 16;
		blocks = 16;
	} else if (leveltype == DTYPE_HELL) {
		microTileLen = 12;
		blocks = 16;
	}

	size_t tileCount;
	const std::unique_ptr<uint16_t[]> levelPieces = LoadMinData(tileCount);

	ankerl::unordered_dense::map<uint16_t, DunFrameInfo> frameToTypeMap;
	frameToTypeMap.reserve(4096);
	for (size_t levelPieceId = 0; levelPieceId < tileCount / blocks; levelPieceId++) {
		uint16_t *pieces = &levelPieces[blocks * levelPieceId];
		for (uint32_t block = 0; block < blocks; block++) {
			const LevelCelBlock levelCelBlock { Swap16LE(pieces[blocks - 2 + (block & 1) - (block & 0xE)]) };
			DPieceMicros[levelPieceId].mt[block] = levelCelBlock;
			if (levelCelBlock.hasValue()) {
				if (const auto it = frameToTypeMap.find(levelCelBlock.frame()); it == frameToTypeMap.end()) {
					frameToTypeMap.emplace_hint(it, levelCelBlock.frame(),
					    DunFrameInfo { static_cast<uint8_t>(block), levelCelBlock.type(), SOLData[levelPieceId] });
				}
			}
		}
	}
	std::vector<std::pair<uint16_t, DunFrameInfo>> frameToTypeList = std::move(frameToTypeMap).extract();
	c_sort(frameToTypeList, [](const std::pair<uint16_t, DunFrameInfo> &a, const std::pair<uint16_t, DunFrameInfo> &b) {
		return a.first < b.first;
	});
	ReencodeDungeonCels(dungeonCels, frameToTypeList);

	std::vector<std::pair<uint16_t, uint16_t>> celBlockAdjustments = ComputeCelBlockAdjustments(frameToTypeList);
	if (celBlockAdjustments.size() == 0) {
		tile_geometry::BuildPieceGeometryTable(dungeonCels.get(), DPieceMicros, SOLData, microTileLen, leveltype);
		return;
	}
	for (size_t levelPieceId = 0; levelPieceId < tileCount / blocks; levelPieceId++) {
		for (uint32_t block = 0; block < blocks; block++) {
			LevelCelBlock &levelCelBlock = DPieceMicros[levelPieceId].mt[block];
			const uint16_t frame = levelCelBlock.frame();
			const auto pair = std::make_pair(frame, frame);
			const auto it = std::upper_bound(celBlockAdjustments.begin(), celBlockAdjustments.end(), pair,
			    [](std::pair<uint16_t, uint16_t> p1, std::pair<uint16_t, uint16_t> p2) { return p1.first < p2.first; });
			if (it != celBlockAdjustments.end()) {
				levelCelBlock.data -= it->second;
			}
		}
	}
	tile_geometry::BuildPieceGeometryTable(dungeonCels.get(), DPieceMicros, SOLData, microTileLen, leveltype);
}

} // namespace devilution

// Per-piece geometry table for the 2.5D hybrid lighting, declared in levels/tile_geometry.hpp.
// It lives in this translation unit because SetDungeonMicros builds it: libdevilutionx_tile_geometry links
// libdevilutionx_dun_tile_data, not the other way round, so a definition in tile_geometry.cpp would leave
// targets that link only libdevilutionx_dun_tile_data with an unresolved symbol.
namespace devilution::tile_geometry {

namespace {

constexpr uint8_t FaceSWBit = static_cast<uint8_t>(Face::SW);
constexpr uint8_t FaceSEBit = static_cast<uint8_t>(Face::SE);

/** Lintel height used when the arch sprite is not available. */
constexpr int DefaultLintelPx = 64;

/** Rows of an arch sprite above its lintel (`lintelPx = height() - 32`). */
constexpr int ArchTopBandPx = 32;

std::array<PieceGeometry, MAXTILES> PieceTable {};
const PieceGeometry EmptyPiece {};
uint32_t PieceTableBuilds = 0;

/**
 * @brief Cathedral doorway pieces and the `dSpecial` value the level generator assigns to them
 * (`drlg_l1.cpp` InitDungeonPieces and `objects.cpp` ObjL1Special). Catacombs arches are keyed by the
 * neighbours of the post piece (`drlg_l2.cpp` InitDungeonPieces), not by piece, so they have no per-piece
 * entry and `GetPieceGeometry(piece).lintelPx` is 0 on L2; the packed volume word carries the lintel for
 * both tilesets (`PackStatic` reads it from the cell's `dSpecial`), so per-cell consumers read it there.
 */
struct CathedralArchPiece {
	uint16_t piece;
	int8_t special;
};
constexpr CathedralArchPiece CathedralArchPieces[] = {
	{ 11, 1 }, { 70, 1 }, { 320, 1 }, { 210, 1 }, { 340, 1 }, { 417, 1 },
	{ 10, 2 }, { 248, 2 }, { 324, 2 }, { 343, 2 }, { 330, 2 }, { 420, 2 },
	{ 252, 3 }, { 254, 4 }, { 258, 5 }, { 266, 6 }
};

/**
 * @brief Height in pixels of the arch sprite for `special` (1-based `dSpecial`), 0 when unavailable.
 */
int ArchSpriteHeight(int8_t special)
{
	if (special <= 0 || !pSpecialCels.has_value()) return 0;
	const auto index = static_cast<uint32_t>(special - 1);
	if (index >= pSpecialCels->numSprites()) return 0;
	return (*pSpecialCels)[index].height();
}

/**
 * @brief Bottom of the lintel above the tile base for the arch sprite `special`.
 */
uint8_t LintelPx(int8_t special)
{
	const int height = ArchSpriteHeight(special);
	if (height <= ArchTopBandPx) return DefaultLintelPx;
	return static_cast<uint8_t>(std::min(height - ArchTopBandPx, 254));
}


/**
 * @brief Number of frame rows (counted from the bottom) up to and including the topmost row that has
 * an opaque pixel, for a frame in the re-encoded dungeon CEL blob (`ReencodeDungeonCels`).
 *
 * Only `TransparentSquare` carries transparency; every other type is a fully opaque shape.
 */
uint_fast8_t FrameOpaqueRows(const std::byte *dungeonCels, LevelCelBlock block)
{
	switch (block.type()) {
	case TileType::Square:
	case TileType::LeftTrapezoid:
	case TileType::RightTrapezoid:
		return static_cast<uint_fast8_t>(DunFrameHeight);
	case TileType::LeftTriangle:
	case TileType::RightTriangle:
		return static_cast<uint_fast8_t>(DunFrameTriangleHeight);
	case TileType::TransparentSquare:
		break;
	}
	if (dungeonCels == nullptr) return 0;

	// Frame table: u32 frame count, u32 offsets of the 1-based frames, u32 blob size.
	const auto *frameTable = reinterpret_cast<const uint32_t *>(dungeonCels);
	const uint32_t frameCount = Swap32LE(frameTable[0]);
	const uint32_t frame = block.frame();
	if (frame == 0 || frame > frameCount) return 0;
	const auto *src = reinterpret_cast<const uint8_t *>(dungeonCels) + Swap32LE(frameTable[frame]);
	const auto *const end = reinterpret_cast<const uint8_t *>(dungeonCels) + Swap32LE(frameTable[frame + 1]);

	uint_fast8_t top = 0;
	for (uint_fast8_t row = 0; row < DunFrameHeight && src < end; ++row) {
		int_fast16_t remaining = DunFrameWidth;
		bool opaque = false;
		while (remaining > 0 && src < end) {
			const auto run = static_cast<int8_t>(*src++);
			if (run > 0) {
				if (end - src < run) return top; // truncated frame
				opaque = true;
				src += run;
				remaining -= run;
			} else if (run < 0) {
				remaining += run;
			} else {
				return top; // malformed run
			}
		}
		if (opaque) top = row + 1;
	}
	return top;
}

PieceGeometry ClassifyPiece(const std::byte *dungeonCels, const MICROS &micros, TileProperties sol, uint_fast8_t microTileLen)
{
	PieceGeometry result;
	const uint_fast8_t count = std::min<uint_fast8_t>(microTileLen, 16);

	// The border/dirt filler draws nothing at ground level (mt[0] and mt[1] are empty; its upper micros may
	// still carry the dark wall tops of the surrounding void). On caves and hell this also catches solid rock and
	// wall pieces whose ground micros are empty; their faces, height and SOL bits are still filled in below, only
	// the kind reads Dirt.
	if (count < 2 || (!micros.mt[0].hasValue() && !micros.mt[1].hasValue())) {
		result.kind = PieceKind::Dirt;
	} else if (!HasAnyOf(sol, TileProperties::Solid | TileProperties::BlockMissile)) {
		result.kind = PieceKind::Floor; // scrollrt.cpp IsFloor
	} else if (HasAnyOf(sol, TileProperties::Solid)) {
		result.kind = PieceKind::Pillar; // promoted to Wall below when it shows a face
	} else {
		result.kind = PieceKind::Prop; // BlockMissile floors: L3/L4 stairs (reencode_dun_cels.hpp)
	}

	// A wall half stands on the cell's centre line: a trapezoid frame is the half-diamond of floor in front of the wall plus
	// the wall above it (its lower diagonal is the floor's front edge, not the wall's foot). The left half therefore shows a
	// wall running along y (plane x = const, facing +x: the SE face) and the right half a wall running along x (plane
	// y = const, facing +y: the SW face), exactly like the arch facings in ArchFacing.
	const bool wallLeft = (micros.mt[0].hasValue() && micros.mt[0].type() == TileType::LeftTrapezoid) || HasAnyOf(sol, TileProperties::TransparentLeft);
	const bool wallRight = (micros.mt[1].hasValue() && micros.mt[1].type() == TileType::RightTrapezoid) || HasAnyOf(sol, TileProperties::TransparentRight);
	// A solid piece whose ground micro is a square (colonnades, see-through arcades, pillars) stands on the same line: it
	// gets the face of its half but stays a Pillar. Only a triangle there is plain floor (the Cathedral filler mass).
	const auto standsOnSquare = [&](int half) {
		if (result.kind != PieceKind::Pillar || !micros.mt[half].hasValue()) return false;
		const TileType type = micros.mt[half].type();
		return type == TileType::Square || type == TileType::TransparentSquare;
	};
	if (wallLeft || standsOnSquare(0))
		result.faces |= FaceSEBit;
	if (wallRight || standsOnSquare(1))
		result.faces |= FaceSWBit;
	if (result.kind == PieceKind::Pillar && (wallLeft || wallRight))
		result.kind = PieceKind::Wall;

	// The ground diamond of a floor lies on the tile base (h = 0); only what is drawn above it counts.
	// A floor whose upper micros are empty therefore reports heightPx 0 even when SOL marks it BlockLight: the
	// march blocks only while the ray is below the cell's height, so such a cell never occludes and
	// vision blocking is left to dLight. A floor that draws something in its upper micros (L1 pieces 25/26,
	// L2 pieces 266/558) reports that height like any other piece and occludes below it when BlockLight is set.
	const uint_fast8_t firstBlock = result.kind == PieceKind::Floor ? 2 : 0;
	int heightPx = 0;
	for (uint_fast8_t block = firstBlock; block < count; ++block) {
		const LevelCelBlock levelCelBlock = micros.mt[block];
		if (!levelCelBlock.hasValue()) continue;
		const uint_fast8_t rows = FrameOpaqueRows(dungeonCels, levelCelBlock);
		if (rows == 0) continue;
		heightPx = std::max(heightPx, static_cast<int>(DunFrameHeight) * (block >> 1) + static_cast<int>(rows));
	}
	result.heightPx = static_cast<uint8_t>(std::min(heightPx, 255));
	return result;
}

} // namespace

namespace detail {

/** @brief Bottom of the lintel above the tile base for the arch sprite `special`, used by the volume packer. */
uint8_t ArchLintelPx(int8_t special)
{
	return LintelPx(special);
}

/** @brief Top of the arch occluder band above the tile base for the arch sprite `special`. */
int ArchOccluderTopPx(int8_t special)
{
	return std::max(ArchSpriteHeight(special), LintelPx(special) + ArchTopBandPx);
}

/** @brief Number of BuildPieceGeometryTable calls so far; mixed into the occluder volume's static hash. */
uint32_t PieceGeometryTableBuilds()
{
	return PieceTableBuilds;
}

} // namespace detail

void BuildPieceGeometryTable(const std::byte *dungeonCels, std::span<const MICROS> micros, std::span<const TileProperties> sol, uint_fast8_t microTileLen, dungeon_type level)
{
	++PieceTableBuilds;
	PieceTable.fill(PieceGeometry {});
	const size_t count = std::min({ micros.size(), sol.size(), PieceTable.size() });
	for (size_t piece = 0; piece < count; ++piece) {
		PieceTable[piece] = ClassifyPiece(dungeonCels, micros[piece], sol[piece], microTileLen);
	}
	if (level == DTYPE_CATHEDRAL) {
		for (const CathedralArchPiece &arch : CathedralArchPieces) {
			if (arch.piece < count) PieceTable[arch.piece].lintelPx = LintelPx(arch.special);
		}
	}
}

const PieceGeometry &GetPieceGeometry(uint16_t piece)
{
	if (piece >= PieceTable.size()) return EmptyPiece;
	return PieceTable[piece];
}

} // namespace devilution::tile_geometry
