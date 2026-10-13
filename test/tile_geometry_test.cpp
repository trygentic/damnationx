#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/point.hpp"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung_defs.hpp"
#include "levels/tile_geometry.hpp"

namespace devilution {
namespace {

using tile_geometry::Face;
using tile_geometry::GetPieceGeometry;
using tile_geometry::OccluderVolume;
using tile_geometry::PieceGeometry;
using tile_geometry::PieceKind;
using tile_geometry::UpdateOccluderVolume;
namespace volume = tile_geometry::volume;

constexpr uint8_t SW = static_cast<uint8_t>(Face::SW);
constexpr uint8_t SE = static_cast<uint8_t>(Face::SE);

// Piece ids of test/fixtures/tile_geometry/synthetic_level.bin (see its README).
enum FixturePiece : uint16_t {
	Dirt = 0,
	Floor = 1,
	WallSW = 2,
	WallSE = 3,
	Pillar = 4,
	BackWall = 5,
	FoliageFloor = 6,
	Stairs = 7,
	DirtWithTops = 8,
	HellHighBlock = 9,
	DoorwayHorizontal = 10,
	DoorwayVertical = 11,
	Fence = 12,
	BlockLightFloor = 13,
	WallSEfromSol = 14,
	TallPillar = 15,
	TruncatedFrame = 16,
	FixturePieceCount = 17,
};

struct SyntheticLevel {
	uint8_t microTileLen = 0;
	dungeon_type level = DTYPE_NONE;
	std::vector<MICROS> micros;
	std::vector<TileProperties> sol;
	std::vector<std::byte> cels;
};

uint16_t ReadU16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadU32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

std::vector<uint8_t> ReadFile(const char *path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool ParseSyntheticLevel(const std::vector<uint8_t> &bytes, SyntheticLevel &out)
{
	if (bytes.size() < 12 || std::memcmp(bytes.data(), "DXTG", 4) != 0) return false;
	if (ReadU32(&bytes[4]) != 1) return false;
	out.microTileLen = bytes[8];
	out.level = static_cast<dungeon_type>(bytes[9]);
	const uint16_t pieceCount = ReadU16(&bytes[10]);
	size_t pos = 12;
	const size_t pieceBytes = 2 + 16 * 2;
	if (bytes.size() < pos + pieceCount * pieceBytes + 4) return false;
	out.micros.resize(pieceCount);
	out.sol.resize(pieceCount);
	for (uint16_t piece = 0; piece < pieceCount; ++piece) {
		out.sol[piece] = static_cast<TileProperties>(bytes[pos]);
		pos += 2;
		for (int block = 0; block < 16; ++block, pos += 2) {
			out.micros[piece].mt[block] = LevelCelBlock { ReadU16(&bytes[pos]) };
		}
	}
	const uint32_t celSize = ReadU32(&bytes[pos]);
	pos += 4;
	if (bytes.size() < pos + celSize) return false;
	out.cels.resize(celSize);
	std::memcpy(out.cels.data(), &bytes[pos], celSize);
	return true;
}

const SyntheticLevel &Fixture()
{
	static const SyntheticLevel level = [] {
		SyntheticLevel result;
		for (const char *path : { "test/fixtures/tile_geometry/synthetic_level.bin", "../test/fixtures/tile_geometry/synthetic_level.bin" }) {
			const std::vector<uint8_t> bytes = ReadFile(path);
			if (bytes.empty()) continue;
			if (ParseSyntheticLevel(bytes, result)) return result;
			ADD_FAILURE() << "Malformed fixture " << path;
			return SyntheticLevel {};
		}
		ADD_FAILURE() << "Fixture test/fixtures/tile_geometry/synthetic_level.bin not found (run from the build directory)";
		return SyntheticLevel {};
	}();
	return level;
}

/** Copies the fixture into DPieceMicros/SOLData and builds the piece table. */
void BuildTable(uint_fast8_t microTileLen, dungeon_type level)
{
	const SyntheticLevel &fixture = Fixture();
	ASSERT_EQ(fixture.micros.size(), static_cast<size_t>(FixturePieceCount));
	std::memset(DPieceMicros, 0, sizeof(DPieceMicros));
	std::memset(SOLData, 0, sizeof(SOLData));
	std::copy(fixture.micros.begin(), fixture.micros.end(), DPieceMicros);
	std::copy(fixture.sol.begin(), fixture.sol.end(), SOLData);
	tile_geometry::BuildPieceGeometryTable(fixture.cels.data(), DPieceMicros, SOLData, microTileLen, level);
}

void BuildTable()
{
	BuildTable(Fixture().microTileLen, Fixture().level);
}

constexpr Point ArchCellVertical { 5, 5 };
constexpr Point ArchCellHorizontal { 6, 5 };

/** Resets every dungeon global the volume reads and lays out the fixture pieces over the whole grid. */
void SetupLevel()
{
	BuildTable();
	leveltype = DTYPE_CATHEDRAL;
	std::memset(dPiece, 0, sizeof(dPiece));
	std::memset(dSpecial, 0, sizeof(dSpecial));
	std::memset(dFlags, 0, sizeof(dFlags));
	std::memset(dTransVal, 0, sizeof(dTransVal));
	std::memset(dLight, 0, sizeof(dLight));
	std::memset(dObject, 0, sizeof(dObject));
	TransList.fill(false);
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			dPiece[x][y] = static_cast<uint16_t>((x * 7 + y * 3) % FixturePieceCount);
			dLight[x][y] = static_cast<uint8_t>((x + y) % 16);
		}
	}
	dPiece[ArchCellVertical.x][ArchCellVertical.y] = DoorwayVertical;
	dSpecial[ArchCellVertical.x][ArchCellVertical.y] = 1;
	dPiece[ArchCellHorizontal.x][ArchCellHorizontal.y] = DoorwayHorizontal;
	dSpecial[ArchCellHorizontal.x][ArchCellHorizontal.y] = 2;
}

uint32_t CellAt(const OccluderVolume &vol, Point tile)
{
	return vol.cells[static_cast<size_t>(tile.y) * MAXDUNX + tile.x];
}

TEST(TileGeometry, Classification)
{
	BuildTable();
	EXPECT_EQ(GetPieceGeometry(Dirt).kind, PieceKind::Dirt);
	EXPECT_EQ(GetPieceGeometry(Floor).kind, PieceKind::Floor);
	EXPECT_EQ(GetPieceGeometry(WallSW).kind, PieceKind::Wall);
	EXPECT_EQ(GetPieceGeometry(WallSE).kind, PieceKind::Wall);
	EXPECT_EQ(GetPieceGeometry(Pillar).kind, PieceKind::Pillar);
	EXPECT_EQ(GetPieceGeometry(BackWall).kind, PieceKind::Wall);
	EXPECT_EQ(GetPieceGeometry(FoliageFloor).kind, PieceKind::Floor);
	EXPECT_EQ(GetPieceGeometry(Stairs).kind, PieceKind::Prop);
	EXPECT_EQ(GetPieceGeometry(DirtWithTops).kind, PieceKind::Dirt);
	EXPECT_EQ(GetPieceGeometry(HellHighBlock).kind, PieceKind::Pillar);
	EXPECT_EQ(GetPieceGeometry(DoorwayHorizontal).kind, PieceKind::Floor);
	EXPECT_EQ(GetPieceGeometry(DoorwayVertical).kind, PieceKind::Floor);
	EXPECT_EQ(GetPieceGeometry(Fence).kind, PieceKind::Wall);
	EXPECT_EQ(GetPieceGeometry(BlockLightFloor).kind, PieceKind::Floor);
	EXPECT_EQ(GetPieceGeometry(WallSEfromSol).kind, PieceKind::Wall);
	EXPECT_EQ(GetPieceGeometry(TallPillar).kind, PieceKind::Pillar);
	EXPECT_EQ(GetPieceGeometry(TruncatedFrame).kind, PieceKind::Pillar);

	// Pieces past the fixture and past the table are empty dirt.
	EXPECT_EQ(GetPieceGeometry(FixturePieceCount).kind, PieceKind::Dirt);
	EXPECT_EQ(GetPieceGeometry(FixturePieceCount).heightPx, 0);
	EXPECT_EQ(GetPieceGeometry(MAXTILES).kind, PieceKind::Dirt);
	EXPECT_EQ(GetPieceGeometry(65535).heightPx, 0);
}

TEST(TileGeometry, Faces)
{
	BuildTable();
	EXPECT_EQ(GetPieceGeometry(Dirt).faces, 0);
	EXPECT_EQ(GetPieceGeometry(Floor).faces, 0);
	// A wall half stands on the cell's centre line: a LeftTrapezoid ground micro is a wall running along y, the SE
	// face (+x); a RightTrapezoid one a wall running along x, the SW face (+y). The fixture names follow the drawn half.
	EXPECT_EQ(GetPieceGeometry(WallSW).faces, SE);
	EXPECT_EQ(GetPieceGeometry(WallSE).faces, SW);
	EXPECT_EQ(GetPieceGeometry(BackWall).faces, SE | SW); // SE from SOL TransparentLeft, SW from its square right ground micro
	EXPECT_EQ(GetPieceGeometry(Stairs).faces, 0);         // BlockMissile prop, not solid
	EXPECT_EQ(GetPieceGeometry(DirtWithTops).faces, 0);   // trapezoids above ground level are not faces
	EXPECT_EQ(GetPieceGeometry(Fence).faces, SE);
	EXPECT_EQ(GetPieceGeometry(WallSEfromSol).faces, SW); // from SOL TransparentRight
	// A solid piece standing on a square ground micro gets the face of that half but stays a Pillar (Classification).
	EXPECT_EQ(GetPieceGeometry(Pillar).faces, SE | SW);
	EXPECT_EQ(GetPieceGeometry(TallPillar).faces, SE | SW);
	EXPECT_EQ(GetPieceGeometry(HellHighBlock).faces, SE);   // Square mt[0] only
	EXPECT_EQ(GetPieceGeometry(TruncatedFrame).faces, SE); // TransparentSquare mt[0] only
	EXPECT_EQ(GetPieceGeometry(BlockLightFloor).faces, 0); // triangles are floor
}

TEST(TileGeometry, HeightsFromFrames)
{
	BuildTable(12, DTYPE_CATHEDRAL);
	EXPECT_EQ(GetPieceGeometry(Dirt).heightPx, 0);
	EXPECT_EQ(GetPieceGeometry(Floor).heightPx, 0);           // the ground diamond is the base
	EXPECT_EQ(GetPieceGeometry(WallSW).heightPx, 64);         // trapezoid + Square at level 1
	EXPECT_EQ(GetPieceGeometry(WallSE).heightPx, 64);         // mt[3] Square at level 1
	EXPECT_EQ(GetPieceGeometry(Pillar).heightPx, 37);         // TransparentSquare: 5 opaque rows at level 1
	EXPECT_EQ(GetPieceGeometry(BackWall).heightPx, 96);       // Squares up to level 2
	EXPECT_EQ(GetPieceGeometry(FoliageFloor).heightPx, 0);    // foliage is ground decoration
	EXPECT_EQ(GetPieceGeometry(Stairs).heightPx, 40);         // 8 opaque rows at level 1
	EXPECT_EQ(GetPieceGeometry(DirtWithTops).heightPx, 96);   // fully opaque TransparentSquare at level 2
	EXPECT_EQ(GetPieceGeometry(HellHighBlock).heightPx, 32);
	EXPECT_EQ(GetPieceGeometry(Fence).heightPx, 64);
	EXPECT_EQ(GetPieceGeometry(BlockLightFloor).heightPx, 0);
	EXPECT_EQ(GetPieceGeometry(WallSEfromSol).heightPx, 44);  // 12 opaque rows at level 1 beat the 32-row mt[1]
	EXPECT_EQ(GetPieceGeometry(TallPillar).heightPx, 192);    // mt[10..11] at level 5
	EXPECT_EQ(GetPieceGeometry(TruncatedFrame).heightPx, 3);  // reads stop at the frame end
}

TEST(TileGeometry, HellHighBlocksIgnored)
{
	BuildTable(12, DTYPE_HELL);
	EXPECT_EQ(GetPieceGeometry(HellHighBlock).heightPx, 32);
	EXPECT_EQ(GetPieceGeometry(TallPillar).heightPx, 192);

	// With the full 16 blocks the same pieces reach mt[12..15] (and clamp to 255).
	BuildTable(16, DTYPE_HELL);
	EXPECT_EQ(GetPieceGeometry(HellHighBlock).heightPx, 224);
	EXPECT_EQ(GetPieceGeometry(TallPillar).heightPx, 255);
}

TEST(TileGeometry, CathedralArchPiecesCarryTheDefaultLintel)
{
	BuildTable(10, DTYPE_CATHEDRAL);
	// No pSpecialCels in tests: the default 64 px lintel.
	EXPECT_EQ(GetPieceGeometry(DoorwayVertical).lintelPx, 64);
	EXPECT_EQ(GetPieceGeometry(DoorwayHorizontal).lintelPx, 64);
	EXPECT_EQ(GetPieceGeometry(WallSW).lintelPx, 0);
	EXPECT_EQ(GetPieceGeometry(Floor).lintelPx, 0);

	BuildTable(12, DTYPE_HELL);
	EXPECT_EQ(GetPieceGeometry(DoorwayVertical).lintelPx, 0);
	EXPECT_EQ(GetPieceGeometry(DoorwayHorizontal).lintelPx, 0);
}

TEST(TileGeometry, ArchFacingPerTileset)
{
	using tile_geometry::ArchFacing;
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 1), SE);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 2), SW);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 3), SW);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 4), SE);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 5), SE);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 6), SE);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 7), 0); // door leaves
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 8), 0);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 0), 0);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, -1), 0);
	EXPECT_EQ(ArchFacing(DTYPE_CATHEDRAL, 127), 0);

	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 1), SE);
	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 2), SE);
	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 3), SW);
	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 4), SW);
	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 5), 0); // door leaves
	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 6), 0);
	EXPECT_EQ(ArchFacing(DTYPE_CATACOMBS, 0), 0);

	for (const dungeon_type level : { DTYPE_TOWN, DTYPE_CAVES, DTYPE_HELL, DTYPE_NEST, DTYPE_CRYPT, DTYPE_NONE }) {
		for (int8_t special = 0; special < 10; ++special) {
			EXPECT_EQ(ArchFacing(level, special), 0) << "level " << static_cast<int>(level) << " special " << static_cast<int>(special);
		}
	}
}

TEST(OccluderVolume, PackingRoundTrip)
{
	SetupLevel();
	const Point wall { 7, 9 };
	dPiece[wall.x][wall.y] = WallSW;
	dFlags[wall.x][wall.y] = DungeonFlag::Visible | DungeonFlag::Explored;
	dTransVal[wall.x][wall.y] = 3;
	TransList[3] = true;
	dLight[wall.x][wall.y] = 11;
	const Point dark { 8, 9 };
	dPiece[dark.x][dark.y] = WallSE;
	dLight[dark.x][dark.y] = 200; // out of range: clamped to the 4-bit field
	dTransVal[dark.x][dark.y] = 4; // TransList[4] is false

	auto vol = std::make_unique<OccluderVolume>();
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 1U);
	EXPECT_NE(vol->staticHash, 0U);

	const uint32_t expectedWall = 64u | volume::FaceSE | volume::BlockLight | volume::Solid | volume::Visible | volume::Explored | volume::Blended | (11u << volume::DLightShift);
	EXPECT_EQ(CellAt(*vol, wall), expectedWall);
	const uint32_t expectedDark = 64u | volume::FaceSW | volume::BlockLight | volume::Solid | (15u << volume::DLightShift);
	EXPECT_EQ(CellAt(*vol, dark), expectedDark);

	// Arch cells: walkable doorway with the arch sprite's shape (default 64 px lintel + 32 px band without pSpecialCels).
	const uint32_t archWord = CellAt(*vol, ArchCellVertical);
	EXPECT_NE(archWord & volume::Arch, 0U);
	EXPECT_EQ(archWord & volume::BlockLight, 0U);
	EXPECT_EQ(archWord & volume::Solid, 0U);
	EXPECT_EQ(archWord & volume::HeightMask, 96U);
	EXPECT_EQ((archWord >> volume::LintelShift) & 0x7Fu, 32U);
	EXPECT_EQ((archWord >> volume::DLightShift) & 0xFu, static_cast<uint32_t>((ArchCellVertical.x + ArchCellVertical.y) % 16));
	EXPECT_NE(CellAt(*vol, ArchCellHorizontal) & volume::Arch, 0U);
	EXPECT_EQ(CellAt(*vol, ArchCellHorizontal) & volume::BlockLight, 0U);

	// A door leaf is not an arch, and the packed word never uses the top nibble.
	const Point doorLeaf { 9, 9 };
	EXPECT_EQ(CellAt(*vol, doorLeaf) & volume::Arch, 0U);
	for (const uint32_t cell : vol->cells) {
		EXPECT_EQ(cell & 0xF0000000u, 0U);
	}
}

TEST(OccluderVolume, VisibleCellsCountAsExplored)
{
	// A cell in the player's current vision is not hidden information: Visible alone must set the Explored bit the
	// GPU gates on, or every freshly seen cell goes black until the automap marks it.
	SetupLevel();
	const Point seen { 6, 6 };
	dFlags[seen.x][seen.y] = DungeonFlag::Visible;
	const Point unseen { 6, 7 };
	dFlags[unseen.x][unseen.y] = DungeonFlag::None;
	const Point lit { 7, 7 }; // no flags at all, but classic lighting reaches it
	dFlags[lit.x][lit.y] = DungeonFlag::None;
	dLight[lit.x][lit.y] = 9;
	dLight[unseen.x][unseen.y] = 15;
	auto vol = std::make_unique<OccluderVolume>();
	UpdateOccluderVolume(*vol, nullptr);
	EXPECT_NE(CellAt(*vol, seen) & volume::Explored, 0U);
	EXPECT_NE(CellAt(*vol, seen) & volume::Visible, 0U);
	EXPECT_NE(CellAt(*vol, lit) & volume::Explored, 0U) << "a cell classic draws lit is not hidden information";
	EXPECT_EQ(CellAt(*vol, unseen) & volume::Explored, 0U);
}

TEST(OccluderVolume, BlockLightIsNeverDerivedFromHeight)
{
	SetupLevel();
	auto vol = std::make_unique<OccluderVolume>();
	UpdateOccluderVolume(*vol, nullptr);
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			const uint32_t word = CellAt(*vol, { x, y });
			const bool expected = HasAnyOf(SOLData[dPiece[x][y]], TileProperties::BlockLight);
			EXPECT_EQ((word & volume::BlockLight) != 0, expected) << "cell " << x << "," << y << " piece " << dPiece[x][y];
			EXPECT_EQ((word & volume::Solid) != 0, HasAnyOf(SOLData[dPiece[x][y]], TileProperties::Solid)) << "cell " << x << "," << y;
			if (dSpecial[x][y] == 0) // arch cells take the arch sprite's height instead
				EXPECT_EQ(word & volume::HeightMask, GetPieceGeometry(dPiece[x][y]).heightPx) << "cell " << x << "," << y;
		}
	}
	// Tall solid fence without BlockLight; flat floor with BlockLight.
	const Point fence { 12, 0 }; // (12 * 7 + 0) % 17 == 16 -> override explicitly below
	dPiece[fence.x][fence.y] = Fence;
	const Point litFloor { 13, 0 };
	dPiece[litFloor.x][litFloor.y] = BlockLightFloor;
	UpdateOccluderVolume(*vol, nullptr);
	EXPECT_EQ(CellAt(*vol, fence) & volume::HeightMask, 64U);
	EXPECT_EQ(CellAt(*vol, fence) & volume::BlockLight, 0U);
	EXPECT_EQ(CellAt(*vol, litFloor) & volume::HeightMask, 0U);
	EXPECT_NE(CellAt(*vol, litFloor) & volume::BlockLight, 0U);
}

int DoorCallbackCalls = 0;
bool DoorCallbackSawCellWithoutObject = false;
bool DoorOpenForTesting(Point tile)
{
	++DoorCallbackCalls;
	if (dObject[tile.x][tile.y] == 0) DoorCallbackSawCellWithoutObject = true;
	return tile == Point { 3, 4 };
}

TEST(OccluderVolume, DoorOpenOnlyQueriedForObjects)
{
	SetupLevel();
	dObject[3][4] = 1;
	dObject[20][30] = -2;
	DoorCallbackCalls = 0;
	DoorCallbackSawCellWithoutObject = false;

	auto vol = std::make_unique<OccluderVolume>();
	UpdateOccluderVolume(*vol, DoorOpenForTesting);
	EXPECT_EQ(DoorCallbackCalls, 2);
	EXPECT_FALSE(DoorCallbackSawCellWithoutObject);
	EXPECT_NE(CellAt(*vol, { 3, 4 }) & volume::DoorOpen, 0U);
	EXPECT_EQ(CellAt(*vol, { 20, 30 }) & volume::DoorOpen, 0U);
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			if (x == 3 && y == 4) continue;
			EXPECT_EQ(CellAt(*vol, { x, y }) & volume::DoorOpen, 0U) << x << "," << y;
		}
	}

	// A null callback never sets the bit and leaves objects alone.
	auto other = std::make_unique<OccluderVolume>();
	UpdateOccluderVolume(*other, nullptr);
	EXPECT_EQ(CellAt(*other, { 3, 4 }) & volume::DoorOpen, 0U);
}

TEST(OccluderVolume, StaticBitsRebuildOnlyWhenPieceOrSpecialChanges)
{
	SetupLevel();
	auto vol = std::make_unique<OccluderVolume>();
	ASSERT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	const uint64_t hash = vol->staticHash;
	const Point floorCell { 0, 0 }; // piece (0 * 7 + 0 * 3) % 17 == 0 -> Dirt; use a known floor cell instead
	dPiece[floorCell.x][floorCell.y] = Floor;
	ASSERT_TRUE(UpdateOccluderVolume(*vol, nullptr)); // dPiece changed: rebuilt
	EXPECT_NE(vol->staticHash, hash);
	const uint64_t hashAfterPiece = vol->staticHash;
	EXPECT_EQ(CellAt(*vol, floorCell) & volume::BlockLight, 0U);

	// Mutating SOLData alone is not part of the static hash: the stored static bits stay.
	SOLData[Floor] |= TileProperties::BlockLight;
	EXPECT_FALSE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->staticHash, hashAfterPiece);
	EXPECT_EQ(vol->generation, 2U);
	EXPECT_EQ(CellAt(*vol, floorCell) & volume::BlockLight, 0U);

	// A dSpecial change re-hashes and rebuilds every static word.
	dSpecial[40][40] = 7;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_NE(vol->staticHash, hashAfterPiece);
	EXPECT_EQ(vol->generation, 3U);
	EXPECT_NE(CellAt(*vol, floorCell) & volume::BlockLight, 0U);
	SOLData[Floor] &= ~TileProperties::BlockLight;

	// Restoring the exact previous state produces the previous hash.
	dSpecial[40][40] = 0;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->staticHash, hashAfterPiece);
}

TEST(OccluderVolume, StaticBitsRebuildWhenThePieceTableIsRebuilt)
{
	SetupLevel();
	const Point floorCell { 0, 0 };
	dPiece[floorCell.x][floorCell.y] = Floor;
	auto vol = std::make_unique<OccluderVolume>();
	ASSERT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	const uint64_t hash = vol->staticHash;
	const uint32_t generation = vol->generation;
	EXPECT_EQ(CellAt(*vol, floorCell) & volume::BlockLight, 0U);

	// Rebuilding the table from identical data re-hashes, but every word packs the same: no new generation.
	tile_geometry::BuildPieceGeometryTable(Fixture().cels.data(), DPieceMicros, SOLData, Fixture().microTileLen, Fixture().level);
	EXPECT_FALSE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_NE(vol->staticHash, hash);
	EXPECT_EQ(vol->generation, generation);

	// A table rebuilt for different tile data re-packs the static bits although dPiece and dSpecial are unchanged.
	SOLData[Floor] |= TileProperties::BlockLight;
	tile_geometry::BuildPieceGeometryTable(Fixture().cels.data(), DPieceMicros, SOLData, Fixture().microTileLen, Fixture().level);
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, generation + 1);
	EXPECT_NE(CellAt(*vol, floorCell) & volume::BlockLight, 0U);
	SOLData[Floor] &= ~TileProperties::BlockLight;
}

TEST(OccluderVolume, GenerationBumpsOnVisibleFlipAndNotWhenIdle)
{
	SetupLevel();
	auto vol = std::make_unique<OccluderVolume>();
	EXPECT_EQ(vol->generation, 0U);
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 1U);

	for (int i = 0; i < 3; ++i) {
		EXPECT_FALSE(UpdateOccluderVolume(*vol, nullptr));
		EXPECT_EQ(vol->generation, 1U);
	}

	const Point cell { 30, 31 };
	dFlags[cell.x][cell.y] |= DungeonFlag::Visible;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 2U);
	EXPECT_NE(CellAt(*vol, cell) & volume::Visible, 0U);
	EXPECT_FALSE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 2U);

	dFlags[cell.x][cell.y] &= ~DungeonFlag::Visible;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 3U);
	EXPECT_EQ(CellAt(*vol, cell) & volume::Visible, 0U);

	// Explored, blending and light changes are dynamic too. A cell classic lights already carries the Explored bit
	// (not hidden information), so darken it first to make the Explored flip observable.
	dLight[cell.x][cell.y] = 15;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 4U);
	dFlags[cell.x][cell.y] |= DungeonFlag::Explored;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	dTransVal[cell.x][cell.y] = 9;
	EXPECT_FALSE(UpdateOccluderVolume(*vol, nullptr)); // TransList[9] is still false
	TransList[9] = true;
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_NE(CellAt(*vol, cell) & volume::Blended, 0U);
	dLight[cell.x][cell.y] = static_cast<uint8_t>((dLight[cell.x][cell.y] + 1) % 16);
	EXPECT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	EXPECT_EQ(vol->generation, 7U);
}

TEST(OccluderVolume, UnchangedPathTiming)
{
	SetupLevel();
	auto vol = std::make_unique<OccluderVolume>();
	ASSERT_TRUE(UpdateOccluderVolume(*vol, nullptr));
	ASSERT_FALSE(UpdateOccluderVolume(*vol, nullptr)); // warm

	constexpr int Iterations = 100;
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < Iterations; ++i) {
		if (UpdateOccluderVolume(*vol, nullptr)) FAIL() << "unexpected change on iteration " << i;
	}
	const auto end = std::chrono::steady_clock::now();
	const double meanUs = std::chrono::duration<double, std::micro>(end - start).count() / Iterations;
	RecordProperty("update_occluder_volume_unchanged_us", static_cast<int>(meanUs));
	std::printf("UpdateOccluderVolume unchanged path: %.1f us per call (mean of %d)\n", meanUs, Iterations);
	// A wall-clock bound inside a ctest-registered binary fails on slow runners (-O0 coverage CI, sanitizers, QEMU), so
	// by default the time is only recorded; the evidence run sets TILE_GEOMETRY_TIMING_ASSERT to assert the budget.
	if (std::getenv("TILE_GEOMETRY_TIMING_ASSERT") != nullptr) {
		EXPECT_LE(meanUs, 120.0);
	} else {
		std::printf("TILE_GEOMETRY_TIMING_ASSERT is not set: the 120 us bound is recorded, not asserted\n");
	}
	EXPECT_EQ(vol->generation, 1U);
}

} // namespace
} // namespace devilution
