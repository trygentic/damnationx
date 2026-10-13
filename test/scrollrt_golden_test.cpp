/**
 * @file scrollrt_golden_test.cpp
 *
 * Classic byte-identity goldens for the world traversal and the
 * G-buffer plane invariants of the hybrid world pass.
 *
 * The synthetic 24x24 Cathedral level in `test/fixtures/scrollrt/synthetic_level.bin`
 * (format in the README next to it) is copied into the level globals and drawn
 * through `DrawWorldForTesting` into a 640x352 surface at 6 view positions x
 * zoom on/off x per-pixel lighting on/off with the hybrid mode latched off. The
 * FNV-1a hashes are compared with `goldens.tsv`, recorded after the DrawCell flush fix
 * and before any G-buffer code touched the traversal.
 *
 * Re-record the goldens with:
 *   SCROLLRT_GOLDEN_RECORD=/path/to/goldens.tsv ./scrollrt_golden_test --gtest_filter='ScrollrtGolden.*'
 *
 * `odd_zoom_goldens.tsv` pins the one sanctioned classic change of the zoomed frame (Zoom()'s odd-height tail)
 * against master: see OddHeightZoomDiffersFromMasterOnlyInRowZero and the README.
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "control/control.hpp"
#include "cursor.h"
#include "engine/clx_sprite.hpp"
#include "engine/displacement.hpp"
#include "engine/light_tables.hpp"
#include "engine/lighting_defs.hpp"
#include "engine/palette.h"
#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_view_transform.hpp"
#include "engine/render/scrollrt.h"
#include "engine/surface.hpp"
#include "inv.h"
#include "items.h"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung_defs.hpp"
#include "levels/tile_geometry.hpp"
#include "options.h"
#include "player.h"
#include "utils/endian_write.hpp"
#include "utils/palette_blending.hpp"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

namespace devilution {
namespace {

constexpr const char *FixturePath = "test/fixtures/scrollrt/synthetic_level.bin";
constexpr const char *GoldensPath = "test/fixtures/scrollrt/goldens.tsv";

constexpr int ScreenWidth = 640;
constexpr int ScreenHeight = 480;
constexpr int ViewportHeight = ScreenHeight - 128; // 352: the main panel covers the bottom 128 rows

// Fixture piece ids (see test/fixtures/scrollrt/README.md). WallSW / WallSE are named after the half that draws the wall
// (left / right); a left-half wall is lit as the SE face (WallRight) and a right-half one as the SW face (WallLeft).
enum FixturePiece : uint16_t {
	PieceDirt = 0,
	PieceFloor = 1,
	PieceWallSW = 2,
	PieceWallSE = 3,
	PiecePillar = 4,
	PieceWallSWTransparent = 5,
	PieceFoliageFloor = 6,
	PieceDirtEdge = 7,
	PieceDoorway = 8,
	PieceBackWall = 9,
};

struct SyntheticLevel {
	uint8_t microTileLen = 0;
	dungeon_type level = DTYPE_NONE;
	int width = 0;
	int height = 0;
	int originX = 0;
	int originY = 0;
	std::vector<MICROS> micros;
	std::vector<TileProperties> sol;
	struct Cell {
		uint16_t piece;
		uint8_t light;
		uint8_t flags;
		int8_t special;
		int8_t transVal;
	};
	std::vector<Cell> cells; // x-major
	std::vector<std::byte> cels;
	std::vector<uint8_t> specialCels;

	[[nodiscard]] const Cell &at(int x, int y) const { return cells[static_cast<size_t>(x) * height + y]; }
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
	if (bytes.size() < 16 || std::memcmp(bytes.data(), "DXSL", 4) != 0) return false;
	if (ReadU32(&bytes[4]) != 1) return false;
	out.microTileLen = bytes[8];
	out.level = static_cast<dungeon_type>(bytes[9]);
	out.width = bytes[10];
	out.height = bytes[11];
	out.originX = bytes[12];
	out.originY = bytes[13];
	const uint16_t pieceCount = ReadU16(&bytes[14]);
	size_t pos = 16;
	constexpr size_t PieceBytes = 2 + 16 * 2;
	constexpr size_t CellBytes = 6;
	const size_t cellCount = static_cast<size_t>(out.width) * out.height;
	if (bytes.size() < pos + pieceCount * PieceBytes + cellCount * CellBytes + 8) return false;
	out.micros.resize(pieceCount);
	out.sol.resize(pieceCount);
	for (uint16_t piece = 0; piece < pieceCount; ++piece) {
		out.sol[piece] = static_cast<TileProperties>(bytes[pos]);
		pos += 2;
		for (int block = 0; block < 16; ++block, pos += 2) {
			out.micros[piece].mt[block] = LevelCelBlock { ReadU16(&bytes[pos]) };
		}
	}
	out.cells.resize(cellCount);
	for (SyntheticLevel::Cell &cell : out.cells) {
		cell.piece = ReadU16(&bytes[pos]);
		cell.light = bytes[pos + 2];
		cell.flags = bytes[pos + 3];
		cell.special = static_cast<int8_t>(bytes[pos + 4]);
		cell.transVal = static_cast<int8_t>(bytes[pos + 5]);
		pos += CellBytes;
	}
	const uint32_t celSize = ReadU32(&bytes[pos]);
	pos += 4;
	if (bytes.size() < pos + celSize + 4) return false;
	out.cels.resize(celSize);
	std::memcpy(out.cels.data(), &bytes[pos], celSize);
	pos += celSize;
	const uint32_t specialSize = ReadU32(&bytes[pos]);
	pos += 4;
	if (bytes.size() < pos + specialSize) return false;
	out.specialCels.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(pos + specialSize));
	return true;
}

const SyntheticLevel &Fixture()
{
	static const SyntheticLevel level = [] {
		SyntheticLevel result;
		const std::vector<uint8_t> bytes = ReadFile(FixturePath);
		if (bytes.empty()) {
			ADD_FAILURE() << "Fixture " << FixturePath << " not found (run from the build directory)";
			return SyntheticLevel {};
		}
		if (!ParseSyntheticLevel(bytes, result)) {
			ADD_FAILURE() << "Malformed fixture " << FixturePath;
			return SyntheticLevel {};
		}
		return result;
	}();
	return level;
}

/**
 * @brief Installs the fixture into the level globals and the viewport geometry for a 640x480 screen.
 *
 * Everything the traversal reads is set explicitly: the tile data, the lights, the flags, the special (arch)
 * sprites, the transparency list, the light tables, the blending lookup, the single non-walking player and
 * the panel geometry. The blending lookup is synthetic (no palette is loaded in this test).
 */
void InstallLevel(const SyntheticLevel &fixture)
{
	leveltype = fixture.level;
	MicroTileLen = fixture.microTileLen;
	std::memset(DPieceMicros, 0, sizeof(DPieceMicros));
	std::memset(SOLData, 0, sizeof(SOLData));
	std::copy(fixture.micros.begin(), fixture.micros.end(), DPieceMicros);
	std::copy(fixture.sol.begin(), fixture.sol.end(), SOLData);

	std::memset(dPiece, 0, sizeof(dPiece));
	std::memset(dLight, 0, sizeof(dLight));
	std::memset(dPreLight, 0, sizeof(dPreLight));
	std::memset(dFlags, 0, sizeof(dFlags));
	std::memset(dSpecial, 0, sizeof(dSpecial));
	std::memset(dTransVal, 0, sizeof(dTransVal));
	std::memset(dPlayer, 0, sizeof(dPlayer));
	std::memset(dMonster, 0, sizeof(dMonster));
	std::memset(dObject, 0, sizeof(dObject));
	std::memset(dItem, 0, sizeof(dItem));
	std::memset(dCorpse, 0, sizeof(dCorpse));
	for (int x = 0; x < fixture.width; ++x) {
		for (int y = 0; y < fixture.height; ++y) {
			const SyntheticLevel::Cell &cell = fixture.at(x, y);
			const int dx = fixture.originX + x;
			const int dy = fixture.originY + y;
			dPiece[dx][dy] = cell.piece;
			dLight[dx][dy] = cell.light;
			dPreLight[dx][dy] = cell.light;
			dFlags[dx][dy] = static_cast<DungeonFlag>(cell.flags);
			dSpecial[dx][dy] = cell.special;
			dTransVal[dx][dy] = cell.transVal;
		}
	}
	TransList = {};
	TransList[1] = true;

	{
		auto cels = std::make_unique<std::byte[]>(fixture.cels.size());
		std::memcpy(cels.get(), fixture.cels.data(), fixture.cels.size());
		pDungeonCels = std::move(cels);
		tile_geometry::BuildPieceGeometryTable(pDungeonCels.get(), DPieceMicros, SOLData, MicroTileLen, leveltype);
	}
	{
		auto special = std::make_unique<uint8_t[]>(fixture.specialCels.size());
		std::memcpy(special.get(), fixture.specialCels.data(), fixture.specialCels.size());
		pSpecialCels = OwnedClxSpriteList(std::move(special));
	}

	MakeLightTable();
	for (int a = 0; a < 256; ++a)
		for (int b = 0; b < 256; ++b)
			paletteTransparencyLookup[a][b] = static_cast<uint8_t>((a * 3 + b * 5 + 1) & 0xFF);

	Players.resize(1);
	MyPlayer = &Players[0];
	*MyPlayer = {};
	MyPlayer->_pmode = PM_STAND;

	gnScreenWidth = ScreenWidth;
	gnScreenHeight = ScreenHeight;
	gnViewportHeight = ViewportHeight;
	CalculatePanelAreas();
}

/** The (firstTile, offset) pair CalcFirstTilePosition derives for `view` (no walking). */
struct ViewGeometry {
	Point firstTile;
	Displacement offset;
};

ViewGeometry ComputeViewGeometry(Point view, bool zoom)
{
	// Mirrors CalcViewportGeometry (scrollrt.cpp) so the test does not depend on its private globals; the
	// GetScreenPosition self-check below proves the replication.
	const int zoomFactor = zoom ? 2 : 1;
	const int screenWidth = gnScreenWidth / zoomFactor;
	const int screenHeight = gnScreenHeight / zoomFactor;
	const int panelHeight = GetMainPanel().size.height / zoomFactor;
	const int pixelsToPanel = screenHeight - panelHeight;
	Point playerPosition { screenWidth / 2, pixelsToPanel / 2 };
	if (zoom)
		playerPosition.y += TILE_HEIGHT / 4;
	const int tilesToTop = (playerPosition.y + TILE_HEIGHT - 1) / TILE_HEIGHT;
	const int tilesToLeft = (playerPosition.x + TILE_WIDTH - 1) / TILE_WIDTH;
	Point startPosition = playerPosition - Displacement { tilesToLeft * TILE_WIDTH, tilesToTop * TILE_HEIGHT };
	Displacement tileShift = Displacement(Direction::North) * tilesToTop + Displacement(Direction::West) * tilesToLeft;
	if (tilesToLeft * TILE_WIDTH >= playerPosition.x) {
		startPosition += Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
		tileShift += Displacement(Direction::NorthEast);
	} else if (tilesToTop * TILE_HEIGHT < playerPosition.y) {
		startPosition += Displacement { 0, -TILE_HEIGHT };
		tileShift += Displacement(Direction::North);
	}
	ViewGeometry geometry {
		.firstTile = view + tileShift,
		.offset = Displacement { startPosition.x - (TILE_WIDTH / 2), startPosition.y + (TILE_HEIGHT / 2) - 1 },
	};
	// CalcFirstTilePosition skips the columns an open side panel covers.
	if (CanPanelsCoverView() && (IsLeftPanelOpen() || IsRightPanelOpen())) {
		const int multiplier = zoom ? 1 : 2;
		geometry.firstTile += Displacement(Direction::East) * multiplier;
		geometry.offset.deltaX += -TILE_WIDTH * multiplier / 2 / 2;
		if (IsLeftPanelOpen() && !zoom)
			geometry.offset.deltaX += SidePanelSize.width;
	}
	return geometry;
}

/** Sets the options, ViewPosition and viewport globals for one case and returns what DrawWorldForTesting needs. */
ViewGeometry PrepareView(Point view, bool zoom, bool perPixel)
{
	GetOptions().Graphics.zoomLevel.SetValue(zoom ? 8 : 4);
	GetOptions().Graphics.perPixelLighting.SetValue(perPixel);
	ViewPosition = view;
	MyPlayer->position.tile = view;
	CalcViewportGeometry();
	const ViewGeometry geometry = ComputeViewGeometry(view, zoom);
	// GetScreenPosition(view) = tileShift.worldToScreen() + tileOffset: proves the replicated geometry.
	const Displacement tileShift = geometry.firstTile - view;
	EXPECT_EQ(GetScreenPosition(view), Point {} + tileShift.worldToScreen() + geometry.offset) << "view " << view.x << "," << view.y << " zoom " << zoom;
	return geometry;
}

gbuffer::LatchInput LatchInputFor(bool enabled)
{
	return gbuffer::LatchInput {
		.optionEnabled = enabled,
		.presenterReady = enabled,
		.headless = false,
		.gameRunning = true,
		.level = DTYPE_CATHEDRAL,
		.logicalPalette = logical_palette.data(),
	};
}

uint64_t Fnv1a64(const Surface &out)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (int y = 0; y < out.h(); ++y) {
		const uint8_t *row = out.at(0, y);
		for (int x = 0; x < out.w(); ++x) {
			hash ^= row[x];
			hash *= 0x100000001b3ULL;
		}
	}
	return hash;
}

std::string HexU64(uint64_t value)
{
	static constexpr char Digits[] = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 15; i >= 0; --i, value >>= 4)
		out[i] = Digits[value & 0xF];
	return out;
}

std::map<std::string, std::string> ReadGoldens(const char *path)
{
	std::map<std::string, std::string> goldens;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		const size_t tab = line.find('\t');
		if (tab == std::string::npos)
			continue;
		goldens[line.substr(0, tab)] = line.substr(tab + 1);
	}
	return goldens;
}

constexpr std::array<Point, 6> ViewPositions { {
	{ 11, 11 }, // centre, both lights, the pillar and the walls
	{ 3, 3 },   // north-west corner: DrawOOB dirt tiles above and to the left
	{ 20, 20 }, // south-east corner: dPiece 0 (nothing drawn) and dark tiles below
	{ 5, 18 },  // west edge, mostly dark, transparent wall segment at the top
	{ 18, 5 },  // second light, back wall, free-standing arch
	{ 11, 2 },  // north edge: dirt row and both arches
} };

std::string CaseName(Point view, bool zoom, bool perPixel)
{
	return StrCat("view_", view.x, "_", view.y, zoom ? "_zoom" : "_nozoom", perPixel ? "_perpixel" : "_tables");
}

void ClearSurface(const Surface &out)
{
	for (int y = 0; y < out.h(); ++y)
		std::memset(out.at(0, y), 0, static_cast<size_t>(out.w()));
}

class ScrollrtGolden : public ::testing::Test {
protected:
	void SetUp() override
	{
		savedZoom = *GetOptions().Graphics.zoomLevel;
		savedPerPixel = *GetOptions().Graphics.perPixelLighting;
		ASSERT_FALSE(Fixture().cells.empty());
		InstallLevel(Fixture());
		// Classic mode for the goldens: the frame latch is explicitly off (a previous test may have enabled it).
		gbuffer::LatchForFrame(LatchInputFor(false));
		ASSERT_FALSE(gbuffer::Enabled());
		ASSERT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off);
	}

	void TearDown() override
	{
		gbuffer::EndFrame();
		GetOptions().Graphics.zoomLevel.SetValue(savedZoom);
		GetOptions().Graphics.perPixelLighting.SetValue(savedPerPixel);
	}

	int savedZoom = 4;
	bool savedPerPixel = false;
};

TEST_F(ScrollrtGolden, FixtureIsWellFormed)
{
	const SyntheticLevel &fixture = Fixture();
	EXPECT_EQ(fixture.level, DTYPE_CATHEDRAL);
	EXPECT_EQ(fixture.microTileLen, 10);
	EXPECT_EQ(fixture.width, 24);
	EXPECT_EQ(fixture.height, 24);
	EXPECT_EQ(fixture.micros.size(), 10U);
	EXPECT_EQ(fixture.at(6, 5).piece, PieceWallSE);
	EXPECT_EQ(fixture.at(5, 6).piece, PieceWallSW);
	EXPECT_EQ(fixture.at(9, 6).piece, PieceDoorway);
	EXPECT_EQ(fixture.at(9, 6).special, 2);
	EXPECT_EQ(fixture.at(6, 9).special, 1);
	EXPECT_EQ(fixture.at(10, 14).piece, PieceWallSWTransparent);
	EXPECT_EQ(fixture.at(10, 14).transVal, 1);
	EXPECT_EQ(fixture.at(11, 11).light, 0);
	EXPECT_EQ(fixture.at(0, 0).light, 15);
	ASSERT_TRUE(pSpecialCels);
	EXPECT_EQ(pSpecialCels->numSprites(), 2U);
	EXPECT_EQ((*pSpecialCels)[0].width(), 64);
	EXPECT_EQ((*pSpecialCels)[0].height(), 128);
	EXPECT_EQ((*pSpecialCels)[1].width(), 64);
	EXPECT_EQ((*pSpecialCels)[1].height(), 96);
}

/**
 * Mouse picking (cursor.cpp) is a separate model of the view; at every zoom level and screen size the centre of each
 * drawn tile diamond (GetScreenPosition, scaled to the screen like Zoom()) must pick that tile.
 */
TEST_F(ScrollrtGolden, MousePickingFindsTheDrawnTileAtEveryZoomLevel)
{
	SetFractionalZoomForTesting(true);
	const Point view { 40, 40 };
	struct Case {
		Size screen;
		bool leftPanel;
		bool rightPanel;
	};
	// Side panels only cover the view at 640 wide (CanPanelsCoverView).
	for (const Case testCase : { Case { { 640, 480 }, false, false }, Case { { 640, 480 }, true, false }, Case { { 640, 480 }, false, true },
	         Case { { 854, 480 }, false, false }, Case { { 1280, 720 }, false, false }, Case { { 1728, 1117 }, false, false },
	         Case { { 2560, 1440 }, false, false } }) {
		gnScreenWidth = testCase.screen.width;
		gnScreenHeight = testCase.screen.height;
		CalculatePanelAreas();
		CharFlag = testCase.leftPanel;
		invflag = testCase.rightPanel;
		const int viewportWidth = (testCase.leftPanel || testCase.rightPanel) ? gnScreenWidth - SidePanelSize.width : gnScreenWidth;
		const int viewportOffsetX = testCase.leftPanel ? SidePanelSize.width : 0;
		// Every level in quarters, the fractional ones included (with the HD art).
		for (int zoom = OptionEntryZoomLevel::MinQuarters; zoom <= OptionEntryZoomLevel::MaxQuarters; ++zoom) {
			GetOptions().Graphics.zoomLevel.SetValue(zoom);
			ASSERT_EQ(GetZoomQuarters(), zoom);
			ViewPosition = view;
			MyPlayer->position.tile = view;
			CalcViewportGeometry();
			const HdWorldTransform transform = ComputeHdWorldTransform(zoom, gnScreenWidth, gnViewportHeight, viewportWidth, viewportOffsetX);
			int checked = 0;
			int wrong = 0;
			std::string wrongTiles;
			for (int dy = -12; dy <= 12; ++dy) {
				for (int dx = -12; dx <= 12; ++dx) {
					const Point tile = view + Displacement { dx, dy };
					const Point bottomLeft = GetScreenPosition(tile);
					// The first screen pixel of world pixels well inside the diamond: its centre and four inner points.
					for (const Displacement inside : { Displacement { 0, 0 }, Displacement { -16, 0 }, Displacement { 15, 0 }, Displacement { 0, -7 }, Displacement { 0, 6 } }) {
						const Point world = Point { bottomLeft.x + TILE_WIDTH / 2, bottomLeft.y - TILE_HEIGHT / 2 } + inside;
						const Point centre { transform.ToScreenX(world.x), transform.ToScreenY(world.y) };
						if (!transform.viewport.contains(centre.x, centre.y) || centre.y >= GetMainPanel().position.y)
							continue;
						bool flipflag = false;
						const Point picked = GetTileUnderScreenPosition(centre, flipflag);
						++checked;
						if (picked != tile) {
							++wrong;
							wrongTiles += StrCat(" [tile ", tile.x, ",", tile.y, " at ", centre.x, ",", centre.y, " picked ", picked.x, ",", picked.y, "]");
						}
					}
				}
			}
			EXPECT_GT(checked, 5);
			EXPECT_EQ(wrong, 0) << testCase.screen.width << "x" << testCase.screen.height << " left " << testCase.leftPanel << " right " << testCase.rightPanel
			                    << " zoom " << zoom << ": " << wrong << " of " << checked << ":" << wrongTiles;
		}
	}
	CharFlag = false;
	invflag = false;
	gnScreenWidth = ScreenWidth;
	gnScreenHeight = ScreenHeight;
	CalculatePanelAreas();
	GetOptions().Graphics.zoomLevel.SetValue(4);
	SetFractionalZoomForTesting(std::nullopt);
}

/** Without the HD art a fractional level falls back to the nearest whole level, and the wheel steps whole levels. */
TEST_F(ScrollrtGolden, FractionalZoomNeedsTheHdArt)
{
	SetFractionalZoomForTesting(false);
	GetOptions().Graphics.zoomLevel.SetValue(5); // 1.25x
	EXPECT_EQ(GetZoomQuarters(), 4);
	GetOptions().Graphics.zoomLevel.SetValue(6); // 1.5x
	EXPECT_EQ(GetZoomQuarters(), 8);
	StepZoomLevel(1);
	EXPECT_EQ(*GetOptions().Graphics.zoomLevel, 12);
	SetFractionalZoomForTesting(true);
	StepZoomLevel(1);
	EXPECT_EQ(*GetOptions().Graphics.zoomLevel, 13);
	EXPECT_EQ(GetZoomQuarters(), 13);
	StepZoomLevel(10);
	EXPECT_EQ(*GetOptions().Graphics.zoomLevel, 16);
	ToggleZoom();
	EXPECT_EQ(GetZoomQuarters(), 4);
	EXPECT_EQ(OptionEntryZoomLevel::FormatQuarters(5), "1.25");
	EXPECT_EQ(OptionEntryZoomLevel::FormatQuarters(14), "3.5");
	EXPECT_EQ(OptionEntryZoomLevel::FormatQuarters(8), "2");
	SetFractionalZoomForTesting(std::nullopt);
	GetOptions().Graphics.zoomLevel.SetValue(4);
}

TEST_F(ScrollrtGolden, ClassicTraversalMatchesGoldens)
{
	const char *recordPath = std::getenv("SCROLLRT_GOLDEN_RECORD");
	std::map<std::string, std::string> goldens;
	if (recordPath == nullptr) {
		goldens = ReadGoldens(GoldensPath);
		ASSERT_FALSE(goldens.empty()) << "no goldens in " << GoldensPath;
	}
	std::vector<std::pair<std::string, std::string>> recorded;

	OwnedSurface out(ScreenWidth, ViewportHeight);
	std::optional<uint64_t> previous;
	for (const Point view : ViewPositions) {
		for (const bool zoom : { false, true }) {
			for (const bool perPixel : { false, true }) {
				const std::string name = CaseName(view, zoom, perPixel);
				SCOPED_TRACE(name);
				const ViewGeometry geometry = PrepareView(view, zoom, perPixel);
				ClearSurface(out);
				DrawWorldForTesting(out, geometry.firstTile, geometry.offset);
				EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off) << "classic mode never leaves Off";
				const uint64_t hash = Fnv1a64(out);
				// A frame identical to the previous case would mean a switch had no effect.
				if (previous.has_value())
					EXPECT_NE(hash, *previous) << "case renders identically to the previous one";
				previous = hash;
				const std::string hex = HexU64(hash);
				if (recordPath != nullptr) {
					recorded.emplace_back(name, hex);
					continue;
				}
				const auto it = goldens.find(name);
				ASSERT_NE(it, goldens.end()) << "missing golden";
				EXPECT_EQ(hex, it->second);
			}
		}
	}

	if (recordPath != nullptr) {
		std::ofstream file(recordPath);
		ASSERT_TRUE(file.is_open()) << recordPath;
		file << "# scrollrt classic traversal goldens: FNV-1a 64 of the 640x352 frame (see README.md)\n";
		for (const auto &[name, hex] : recorded)
			file << name << '\t' << hex << '\n';
		SUCCEED() << "recorded " << recorded.size() << " goldens to " << recordPath;
	} else {
		EXPECT_EQ(goldens.size(), ViewPositions.size() * 4) << "stale rows in " << GoldensPath;
	}
}

// ---- Odd viewport height under zoom: a sanctioned classic frame change (item 9) ----

constexpr const char *OddZoomGoldensPath = "test/fixtures/scrollrt/odd_zoom_goldens.tsv";

/** FNV-1a 64 over rows [firstRow, firstRow + rows) of `out`. */
uint64_t RowsHash(const Surface &out, int firstRow, int rows)
{
	return Fnv1a64(out.subregionY(firstRow, rows));
}

/**
 * Zoom() on an odd output height. The viewport is the screen minus the 128-row main panel at 640x479 (351 rows) and
 * the whole screen at widths > 640 (control_panel.cpp CalculatePanelAreas), so `Resolution=1280x721` or 640x479 with
 * zoom on reach its odd-height tail. Master's tail was `memcpy(dst - out.pitch() + 1, dst + 1, viewportWidth)` after
 * the loop left `dst` on row 0: it copied output row 1 over row 0 (no panel), the first 320 bytes of row 1 over the
 * first 320 of row 0 (left panel), or wrote 320 bytes *before* row 0 (right panel: out of bounds, before
 * PalSurface->pixels at the top of the back buffer) and left row 0 holding the unzoomed source row. The tail now
 * doubles the top source row into row 0, like gbuffer::ZoomInPlace does for plane A.
 *
 * `odd_zoom_goldens.tsv` holds, per case, master's hash of rows 1..350, master's row 0 and whether master kept the
 * guard row above the surface intact (all recorded with the upstream Zoom()), and the new row 0. The
 * test proves rows 1..350 are master's bytes, row 0 is the only row that changed, and nothing is written outside.
 */
TEST_F(ScrollrtGolden, OddHeightZoomDiffersFromMasterOnlyInRowZero)
{
	struct Case {
		const char *name;
		bool leftPanel;
		bool rightPanel;
	};
	static constexpr std::array<Case, 3> Cases { {
	    { "odd_zoom_no_panel", false, false },
	    { "odd_zoom_left_panel", true, false },
	    { "odd_zoom_right_panel", false, true },
	} };
	constexpr int OddScreenHeight = ScreenHeight - 1; // 479
	constexpr uint8_t GuardByte = 0x5A;

	const char *recordPath = std::getenv("SCROLLRT_ODD_ZOOM_RECORD");
	std::map<std::string, std::string> goldens;
	if (recordPath == nullptr) {
		goldens = ReadGoldens(OddZoomGoldensPath);
		ASSERT_EQ(goldens.size(), Cases.size()) << "goldens in " << OddZoomGoldensPath;
	}
	std::vector<std::string> recorded;

	struct RestoreScreen {
		bool charFlag = CharFlag;
		bool invFlag = invflag;
		~RestoreScreen()
		{
			CharFlag = charFlag;
			invflag = invFlag;
			gnScreenHeight = ScreenHeight;
			CalculatePanelAreas();
			CalcViewportGeometry();
		}
	} restoreScreen;
	gnScreenHeight = OddScreenHeight;
	CalculatePanelAreas(); // gnViewportHeight = screen height - main panel at 640 wide
	ASSERT_EQ(gnViewportHeight, 351);
	ASSERT_TRUE(CanPanelsCoverView());

	for (const Case &testCase : Cases) {
		SCOPED_TRACE(testCase.name);
		CharFlag = testCase.leftPanel;
		invflag = testCase.rightPanel;
		const ViewGeometry geometry = PrepareView({ 11, 11 }, /*zoom=*/true, /*perPixel=*/false);

		// One guard row above the frame: master's right-panel tail wrote into it.
		OwnedSurface buffer(ScreenWidth, 1 + gnViewportHeight);
		std::memset(buffer.at(0, 0), GuardByte, static_cast<size_t>(buffer.w()));
		const Surface out = buffer.subregionY(1, gnViewportHeight);
		ClearSurface(out);
		DrawWorldForTesting(out, geometry.firstTile, geometry.offset);

		const uint8_t *guard = buffer.at(0, 0);
		const bool guardIntact = std::all_of(guard, guard + buffer.w(), [](uint8_t b) { return b == GuardByte; });
		const std::string rest = HexU64(RowsHash(out, 1, out.h() - 1));
		const std::string row0 = HexU64(RowsHash(out, 0, 1));
		if (recordPath != nullptr) {
			// Master's columns; the new row 0 column is filled in from a run of this build.
			recorded.push_back(StrCat(testCase.name, "\t", rest, "\t", row0, "\t", guardIntact ? "intact" : "overwritten", "\t", row0));
			continue;
		}

		EXPECT_TRUE(guardIntact) << "Zoom() wrote outside the surface";
		// Row 0 is the doubled top source row: pixel pairs across the viewport columns (both viewport widths are even).
		const int viewportOffsetX = testCase.leftPanel ? SidePanelSize.width : 0;
		const int viewportWidth = (testCase.leftPanel || testCase.rightPanel) ? ScreenWidth - SidePanelSize.width : ScreenWidth;
		const uint8_t *row0Pixels = out.at(0, 0);
		for (int x = viewportOffsetX; x < viewportOffsetX + viewportWidth; x += 2)
			ASSERT_EQ(row0Pixels[x], row0Pixels[x + 1]) << "row 0, x " << x;

		const auto it = goldens.find(testCase.name);
		ASSERT_NE(it, goldens.end()) << "missing golden";
		const std::vector<std::string> fields = [&]() {
			std::vector<std::string> result;
			std::string_view line = it->second;
			while (true) {
				const size_t tab = line.find('\t');
				result.emplace_back(line.substr(0, tab));
				if (tab == std::string_view::npos)
					break;
				line.remove_prefix(tab + 1);
			}
			return result;
		}();
		ASSERT_EQ(fields.size(), 4U) << it->second;
		const std::string &masterRest = fields[0];
		const std::string &masterRow0 = fields[1];
		const std::string &masterGuard = fields[2];
		const std::string &newRow0 = fields[3];
		EXPECT_EQ(rest, masterRest) << "rows 1..350 must stay master's bytes";
		EXPECT_EQ(row0, newRow0);
		EXPECT_NE(row0, masterRow0) << "row 0 is the one row that changed";
		EXPECT_EQ(masterGuard, testCase.rightPanel ? "overwritten" : "intact") << "master's out-of-bounds write (right panel only)";
	}

	if (recordPath != nullptr) {
		std::ofstream file(recordPath);
		ASSERT_TRUE(file.is_open()) << recordPath;
		file << "# case\tmaster rows 1..350\tmaster row 0\tmaster guard row\trow 0 (see README.md)\n";
		for (const std::string &line : recorded)
			file << line << '\n';
		SUCCEED() << "recorded " << recorded.size() << " cases to " << recordPath;
	}
}

// ---- Hybrid world pass: plane invariants ----

/** [start, end) of the opaque columns of frame row `row` (0 = bottom) for the fixed-shape tile types. */
std::pair<int, int> ShapeSpan(TileType tile, int row)
{
	const int lowerWidth = 2 * (row + 1);
	const int upperWidth = 32 - (2 * (row - 15));
	switch (tile) {
	case TileType::Square:
		return { 0, 32 };
	case TileType::LeftTriangle:
		return row < 16 ? std::pair { 32 - lowerWidth, 32 } : std::pair { 32 - upperWidth, 32 };
	case TileType::RightTriangle:
		return row < 16 ? std::pair { 0, lowerWidth } : std::pair { 0, upperWidth };
	case TileType::LeftTrapezoid:
		return row < 16 ? std::pair { 32 - lowerWidth, 32 } : std::pair { 0, 32 };
	case TileType::RightTrapezoid:
		return row < 16 ? std::pair { 0, lowerWidth } : std::pair { 0, 32 };
	case TileType::TransparentSquare:
		break;
	}
	return { 0, 0 };
}

int FrameRows(TileType tile)
{
	return (tile == TileType::LeftTriangle || tile == TileType::RightTriangle) ? DunFrameTriangleHeight : DunFrameHeight;
}

class ScrollrtPlanes : public ::testing::Test {
protected:
	void SetUp() override
	{
		savedZoom = *GetOptions().Graphics.zoomLevel;
		savedPerPixel = *GetOptions().Graphics.perPixelLighting;
		ASSERT_FALSE(Fixture().cells.empty());
		InstallLevel(Fixture());
		out = std::make_unique<OwnedSurface>(ScreenWidth, ScreenHeight);
		ClearSurface(*out);
		gbuffer::Allocate(out->begin(), static_cast<uint16_t>(out->pitch()), ScreenWidth, ScreenHeight);
		ASSERT_NE(gbuffer::CurrentPlanes.aux, nullptr);
		ASSERT_TRUE(gbuffer::CurrentPlanes.covers(*out));
	}

	void TearDown() override
	{
		gbuffer::EndFrame();
		gbuffer::Free();
		out.reset();
		GetOptions().Graphics.zoomLevel.SetValue(savedZoom);
		GetOptions().Graphics.perPixelLighting.SetValue(savedPerPixel);
	}

	/** Latches the mode on (forced: presenter "ready"), runs the world pass and leaves the frame in StampUnlit. */
	ViewGeometry DrawHybrid(Point view, bool zoom)
	{
		const ViewGeometry geometry = PrepareView(view, zoom, /*perPixel=*/false);
		gbuffer::LatchForFrame(LatchInputFor(true));
		EXPECT_TRUE(gbuffer::Enabled());
		DrawWorldForTesting(*out, geometry.firstTile, geometry.offset);
		EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::StampUnlit) << "EndWorldPass arms StampUnlit";
		EXPECT_TRUE(gbuffer::BackBufferIsHybrid());
		return geometry;
	}

	[[nodiscard]] uint8_t aux(int x, int y) const { return gbuffer::CurrentPlanes.aux[static_cast<size_t>(y) * out->pitch() + x]; }
	[[nodiscard]] uint8_t base(int x, int y) const { return gbuffer::CurrentPlanes.base[static_cast<size_t>(y) * out->pitch() + x]; }
	[[nodiscard]] gbuffer::Kind kind(int x, int y) const { return gbuffer::AuxKind(aux(x, y)); }

	/** Checks every opaque pixel of one micro frame of a cell against the expected kind/normal/B. */
	void ExpectMicro(Point tile, int block, gbuffer::Kind expectedKind, uint8_t expectedNormal, std::optional<gbuffer::FootKind> foot, const char *what)
	{
		const Point cellBase = GetScreenPosition(tile);
		const LevelCelBlock levelCelBlock { DPieceMicros[dPiece[tile.x][tile.y]].mt[block] };
		ASSERT_TRUE(levelCelBlock.hasValue()) << what;
		const TileType tileType = levelCelBlock.type();
		const bool rightHalf = (block & 1) != 0;
		const int course = block / 2;
		const int leftX = cellBase.x + (rightHalf ? DunFrameWidth : 0);
		const int bottomY = cellBase.y - course * TILE_HEIGHT;
		int checked = 0;
		for (int row = 0; row < FrameRows(tileType); ++row) {
			const auto [start, end] = ShapeSpan(tileType, row);
			const int y = bottomY - row;
			if (y < 0 || y >= ViewportHeight) continue;
			for (int col = start; col < end; ++col) {
				const int x = leftX + col;
				if (x < 0 || x >= ScreenWidth) continue;
				const uint8_t a = aux(x, y);
				ASSERT_EQ(gbuffer::AuxKind(a), expectedKind) << what << " at " << x << "," << y << " (row " << row << ", col " << col << ")";
				ASSERT_EQ(gbuffer::AuxNormal(a), expectedNormal) << what << " normal at " << x << "," << y;
				if (foot.has_value()) {
					const uint8_t expectedBase = static_cast<uint8_t>((cellBase.y - gbuffer::FootRows(*foot, col)) & 0xFF);
					ASSERT_EQ(base(x, y), expectedBase) << what << " B at " << x << "," << y << " (row " << row << ", col " << col << ")";
					// H = 32k + r - foot(c) is what the presenter recovers from B.
					ASSERT_EQ(static_cast<uint8_t>((base(x, y) - y) & 0xFF), static_cast<uint8_t>((32 * course + row - gbuffer::FootRows(*foot, col)) & 0xFF)) << what << " h at " << x << "," << y;
				}
				++checked;
			}
		}
		EXPECT_GT(checked, 0) << what << ": no pixel of the micro frame is on screen";
	}

	std::unique_ptr<OwnedSurface> out;
	int savedZoom = 4;
	bool savedPerPixel = false;
};

TEST_F(ScrollrtPlanes, UnlitOutsideViewportRowsAndFloorPixelsAreFloor)
{
	DrawHybrid({ 11, 11 }, /*zoom=*/false);

	// Rows at and below the viewport are never touched by the world pass.
	for (int y = ViewportHeight; y < ScreenHeight; ++y)
		for (int x = 0; x < ScreenWidth; ++x)
			ASSERT_EQ(aux(x, y), gbuffer::UnlitByte) << "row " << y << " col " << x;

	// Plain floor cells: both triangles are Floor with the up normal, wherever they are on screen.
	for (const Point tile : { Point { 11, 11 }, Point { 12, 10 }, Point { 14, 10 } }) {
		ASSERT_EQ(dPiece[tile.x][tile.y], PieceFloor);
		ExpectMicro(tile, 0, gbuffer::Kind::Floor, gbuffer::NormalUp, std::nullopt, "floor left triangle");
		ExpectMicro(tile, 1, gbuffer::Kind::Floor, gbuffer::NormalUp, std::nullopt, "floor right triangle");
	}
}

TEST_F(ScrollrtPlanes, FullyDarkCellsAreDrawnFullBrightAsFloor)
{
	// A fully dark cell keeps LightsMax for culling, but its tiles are drawn full-bright and
	// tagged Floor like every other tile. The GPU's bilinear ambient is 0 at the cell centre and rises toward lit
	// neighbours, reproducing the classic per-pixel bleed instead of a hard cell-shaped black edge.
	DrawHybrid({ 5, 18 }, /*zoom=*/false);
	const Point dark { 1, 19 };
	ASSERT_EQ(dLight[dark.x][dark.y], LightsMax);
	ASSERT_EQ(dPiece[dark.x][dark.y], PieceFloor);
	ExpectMicro(dark, 0, gbuffer::Kind::Floor, gbuffer::NormalUp, std::nullopt, "dark floor left triangle");
	ExpectMicro(dark, 1, gbuffer::Kind::Floor, gbuffer::NormalUp, std::nullopt, "dark floor right triangle");
	const Point cellBase = GetScreenPosition(dark);
	int nonBlack = 0;
	for (int row = 0; row < DunFrameTriangleHeight; ++row) {
		const auto [start, end] = ShapeSpan(TileType::LeftTriangle, row);
		const int y = cellBase.y - row;
		if (y < 0 || y >= ViewportHeight) continue;
		for (int col = start; col < end; ++col) {
			const int x = cellBase.x + col;
			if (x < 0 || x >= ScreenWidth) continue;
			ASSERT_NE(aux(x, y), gbuffer::UnlitByte) << x << "," << y;
			if (out->at(x, y)[0] != 0) ++nonBlack;
		}
	}
	EXPECT_GT(nonBlack, 0) << "the dark cell's texture is drawn, not blackened";
}

TEST_F(ScrollrtPlanes, WallColumnsRecoverHeightFromPlaneB)
{
	DrawHybrid({ 9, 9 }, /*zoom=*/false);

	// A wall half stands on the cell's centre line. The fixture's "WallSW" piece draws its wall in the left half
	// (LeftTrapezoid foot + square courses): a wall running along y, i.e. the SE face, WallRight with the c / 2 foot table,
	// whose foot lines join from one wall piece to the next. The RightTriangle half is a floor-shaped micro frame and
	// therefore Floor (triangle -> Floor). The last wall of each run is checked because a wall drawn later (east / south)
	// overlaps its neighbour's halves.
	const Point sw { 12, 6 };
	ASSERT_EQ(dPiece[sw.x][sw.y], PieceWallSW);
	ExpectMicro(sw, 0, gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "left-half wall foot");
	ExpectMicro(sw, 2, gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "left-half wall course 1");
	ExpectMicro(sw, 4, gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "left-half wall course 2");
	ExpectMicro(sw, 1, gbuffer::Kind::Floor, gbuffer::NormalUp, std::nullopt, "left-half wall right triangle");

	// Mirrored: the "WallSE" piece's right half is a wall running along x, the SW face (WallLeft, (31 - c) / 2 foot). Its
	// left triangle is overdrawn by the next wall of the column (and the last wall of the column by the transparent
	// segment two rows south), so only the right half is checked here.
	const Point se { 6, 10 };
	ASSERT_EQ(dPiece[se.x][se.y], PieceWallSE);
	ExpectMicro(se, 1, gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, gbuffer::FootKind::SW, "right-half wall foot");
	ExpectMicro(se, 3, gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, gbuffer::FootKind::SW, "right-half wall course 1");
	ExpectMicro(se, 5, gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, gbuffer::FootKind::SW, "right-half wall course 2");

	// A wall whose left-half face comes from the SOL TransparentLeft bit: square courses lit as WallRight with the SE foot.
	const Point back { 16, 8 };
	ASSERT_EQ(dPiece[back.x][back.y], PieceBackWall);
	ExpectMicro(back, 0, gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "back wall course 0");
	ExpectMicro(back, 2, gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "back wall course 1");

	// The pillar stands on square micros in both halves: it is lit as the two wall lines through its cell's centre (	// lighting it like the floor at the pixel sampled the ground a tile or more behind it and drew it as a dark panel among
	// lit walls), with one camera-facing normal so its two halves show no seam. Never WallTop, never Floor.
	{
		const Point pillar { 10, 10 };
		ASSERT_EQ(dPiece[pillar.x][pillar.y], PiecePillar);
		const Point cellBase = GetScreenPosition(pillar);
		int checked = 0;
		for (int y = cellBase.y - 31; y <= cellBase.y; ++y) {
			for (int x = cellBase.x + 4; x < cellBase.x + TILE_WIDTH; ++x) { // the fixture's holes are the leftmost 4 px of a half
				if (x < 0 || x >= ScreenWidth || y < 0 || y >= ViewportHeight) continue;
				const bool rightHalf = x >= cellBase.x + DunFrameWidth;
				const gbuffer::Kind k = kind(x, y);
				ASSERT_NE(k, gbuffer::Kind::WallTop) << x << "," << y;
				if (k != (rightHalf ? gbuffer::Kind::WallLeft : gbuffer::Kind::WallRight)) continue;
				ASSERT_EQ(gbuffer::AuxNormal(aux(x, y)), gbuffer::NormalCardFlat) << x << "," << y;
				const int c = x - cellBase.x - (rightHalf ? DunFrameWidth : 0);
				const uint8_t expectedBase = static_cast<uint8_t>((cellBase.y - gbuffer::FootRows(rightHalf ? gbuffer::FootKind::SW : gbuffer::FootKind::SE, c)) & 0xFF);
				ASSERT_EQ(base(x, y), expectedBase) << x << "," << y;
				++checked;
			}
		}
		EXPECT_GT(checked, 32 * 32) << "the pillar's opaque pixels are its two wall lines";
	}
}

TEST_F(ScrollrtPlanes, BlendedWallPixelsCarryTheirOwnB)
{
	DrawHybrid({ 10, 16 }, /*zoom=*/false);

	// Transparent wall with TransList[dTransVal] active: the upper courses use MaskType::Transparent, so every
	// pixel is a 50 % blend tagged WallBlended and carries the left-half (SE face) wall's B (blended policy).
	const Point tile { 10, 14 };
	ASSERT_EQ(dPiece[tile.x][tile.y], PieceWallSWTransparent);
	ASSERT_TRUE(TransList[dTransVal[tile.x][tile.y]]);
	ExpectMicro(tile, 2, gbuffer::Kind::WallBlended, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "blended course 1");
	ExpectMicro(tile, 4, gbuffer::Kind::WallBlended, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "blended course 2");

	// The same piece next to it is a plain (non-blended) wall when its dTransVal is not active.
	const Point solid { 9, 14 };
	ASSERT_EQ(dPiece[solid.x][solid.y], PieceWallSWTransparent);
	TransList[1] = false;
	gbuffer::EndFrame();
	DrawHybrid({ 10, 16 }, /*zoom=*/false);
	ExpectMicro(solid, 2, gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE, "solid course 1 with transparency off");
	TransList[1] = true;
}

TEST_F(ScrollrtPlanes, ArchesAreWallFacesAnchoredOnTheirWallLine)
{
	DrawHybrid({ 9, 9 }, /*zoom=*/false);

	ASSERT_TRUE(pSpecialCels);
	struct ArchCase {
		Point tile;
		int holeColumn; // a doorway-hole column not covered by the wall drawn one row later
	};
	for (const ArchCase &archCase : { ArchCase { { 9, 6 }, 20 }, ArchCase { { 6, 9 }, 44 } }) {
		const Point tile = archCase.tile;
		SCOPED_TRACE(StrCat("arch at ", tile.x, ",", tile.y));
		ASSERT_EQ(dPiece[tile.x][tile.y], PieceDoorway);
		// dSpecial is a 1-based index into the special sprite list.
		ASSERT_LE(static_cast<uint32_t>(dSpecial[tile.x][tile.y]), pSpecialCels->numSprites());
		const ClxSprite arch = (*pSpecialCels)[dSpecial[tile.x][tile.y] - 1];
		const int lintelRows = arch.height() - 72; // the fixture's doorway hole is 72 rows tall
		const uint8_t facing = tile_geometry::ArchFacing(leveltype, dSpecial[tile.x][tile.y]);
		ASSERT_NE(facing, 0) << "dSpecial " << static_cast<int>(dSpecial[tile.x][tile.y]) << " is an arch";
		const gbuffer::Kind expectedKind = facing == 1 ? gbuffer::Kind::WallLeft : gbuffer::Kind::WallRight;
		const uint8_t expectedNormal = facing == 1 ? gbuffer::NormalWallLeft : gbuffer::NormalWallRight;
		const Point feet = GetScreenPosition(tile);
		// The lintel (the top rows above the hole, full width) is opaque in the fixture sprite. The wall drawn
		// one row later (south-east / south-west of the doorway) rises 96 px from a base 16 px lower and covers
		// the lintel's lower rows, so the rows above that wall are checked: every pixel is the arch's wall kind
		// with B on the wall line through the doorway cell's centre (gbuffer::AnchorArchOnWallLine), so
		// h = rows above that line, like the wall pieces next to the arch.
		const gbuffer::FootKind foot = facing == 1 ? gbuffer::FootKind::SW : gbuffer::FootKind::SE;
		constexpr int RowsCoveredByNeighbourWalls = 3 * TILE_HEIGHT - TILE_HEIGHT / 2;
		const int firstRow = std::max(arch.height() - lintelRows, RowsCoveredByNeighbourWalls);
		int checked = 0;
		for (int row = firstRow; row < arch.height(); ++row) {
			const int y = feet.y - row;
			if (y < 0 || y >= ViewportHeight) continue;
			for (int x = feet.x; x < feet.x + arch.width(); ++x) {
				if (x < 0 || x >= ScreenWidth) continue;
				ASSERT_EQ(kind(x, y), expectedKind) << x << "," << y;
				ASSERT_EQ(gbuffer::AuxNormal(aux(x, y)), expectedNormal) << x << "," << y;
				const int footRows = gbuffer::ArchFootRows(foot, x - feet.x);
				ASSERT_EQ(base(x, y), static_cast<uint8_t>((feet.y - footRows) & 0xFF)) << x << "," << y;
				ASSERT_EQ(static_cast<uint8_t>((base(x, y) - y) & 0xFF), static_cast<uint8_t>((row - footRows) & 0xFF)) << "h at " << x << "," << y;
				++checked;
			}
		}
		EXPECT_EQ(checked, (arch.height() - firstRow) * arch.width());
		// The doorway hole keeps what was underneath: the floor triangles of the doorway cell itself.
		const int holeY = feet.y - 8;
		ASSERT_EQ(kind(feet.x + archCase.holeColumn, holeY), gbuffer::Kind::Floor) << "under the arch";
	}
}

TEST_F(ScrollrtPlanes, HybridFrameIsTheFullBrightClassicFrame)
{
	// In hybrid mode every tile is drawn through LightTables[0] (identity outside Hell),
	// fully dark ones included. The colour output must therefore equal the classic frame of the same level with
	// every cell at light level 0.
	const ViewGeometry geometry = DrawHybrid({ 11, 11 }, /*zoom=*/false);
	std::vector<uint8_t> hybrid;
	for (int y = 0; y < ViewportHeight; ++y)
		hybrid.insert(hybrid.end(), out->at(0, y), out->at(0, y) + ScreenWidth);
	gbuffer::EndFrame();

	for (int x = 0; x < MAXDUNX; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			dLight[x][y] = 0;
	gbuffer::LatchForFrame(LatchInputFor(false));
	ClearSurface(*out);
	DrawWorldForTesting(*out, geometry.firstTile, geometry.offset);
	std::vector<uint8_t> classicFullBright;
	for (int y = 0; y < ViewportHeight; ++y)
		classicFullBright.insert(classicFullBright.end(), out->at(0, y), out->at(0, y) + ScreenWidth);
	EXPECT_EQ(hybrid, classicFullBright);

	// And it differs from the classic frame with the real light levels (the check above is not vacuous).
	InstallLevel(Fixture());
	PrepareView({ 11, 11 }, /*zoom=*/false, /*perPixel=*/false);
	ClearSurface(*out);
	DrawWorldForTesting(*out, geometry.firstTile, geometry.offset);
	std::vector<uint8_t> classic;
	for (int y = 0; y < ViewportHeight; ++y)
		classic.insert(classic.end(), out->at(0, y), out->at(0, y) + ScreenWidth);
	EXPECT_NE(hybrid, classic);
}

TEST_F(ScrollrtPlanes, ZoomDoublesPlaneAInPlace)
{
	// Under zoom the blitters write the top-left quarter and ZoomInPlace pixel-doubles plane A (plane B keeps
	// unzoomed rows): every 2x2 block of A is uniform and the wall column is still classified.
	DrawHybrid({ 9, 9 }, /*zoom=*/true);
	int nonUnlit = 0;
	for (int y = 0; y + 1 < ViewportHeight; y += 2) {
		for (int x = 0; x + 1 < ScreenWidth; x += 2) {
			const uint8_t a = aux(x, y);
			ASSERT_EQ(aux(x + 1, y), a) << x << "," << y;
			ASSERT_EQ(aux(x, y + 1), a) << x << "," << y;
			ASSERT_EQ(aux(x + 1, y + 1), a) << x << "," << y;
			if (a != gbuffer::UnlitByte) ++nonUnlit;
		}
	}
	EXPECT_GT(nonUnlit, 1000);
	// The left-half wall at (8,6) is in view: its screen position under zoom is the unzoomed one doubled.
	const Point sw { 8, 6 };
	const Point cellBase = GetScreenPosition(sw);
	const int x = 2 * (cellBase.x + 8);
	const int y = 2 * (cellBase.y - 40); // course 1, frame row 8
	ASSERT_GE(x, 0);
	ASSERT_LT(y, ViewportHeight);
	EXPECT_EQ(kind(x, y), gbuffer::Kind::WallRight);
}

/** A 12x9 CLX sprite standing in for the software cursor (built like scrollrt_test's helper). */
std::vector<uint8_t> MakeCursorList()
{
	constexpr int Width = 12;
	constexpr int Height = 9;
	std::vector<uint8_t> frame(6);
	WriteLE16(&frame[0], 6);
	WriteLE16(&frame[2], Width);
	WriteLE16(&frame[4], Height);
	for (int y = Height - 1; y >= 0; --y) {
		// One transparent pixel, ten literal pixels, one transparent pixel per row.
		frame.push_back(1);
		frame.push_back(static_cast<uint8_t>(256 - (Width - 2)));
		for (int x = 1; x < Width - 1; ++x)
			frame.push_back(static_cast<uint8_t>(200 + x + y));
		frame.push_back(1);
	}
	std::vector<uint8_t> list(12);
	WriteLE32(&list[0], 1);
	WriteLE32(&list[4], 12);
	WriteLE32(&list[8], static_cast<uint32_t>(12 + frame.size()));
	list.insert(list.end(), frame.begin(), frame.end());
	return list;
}

TEST_F(ScrollrtPlanes, CursorStampsUnlitAndRestoresPlaneA)
{
	DrawHybrid({ 11, 11 }, /*zoom=*/false);
	ASSERT_TRUE(gbuffer::StampActive());

	const std::vector<uint8_t> list = MakeCursorList();
	const ClxSprite cursor = ClxSpriteList { list.data() }[0];
	const Point feet = GetScreenPosition({ 11, 11 }) + Displacement { 20, -4 }; // over the lit floor
	const Rectangle rect { { feet.x, feet.y - cursor.height() + 1 }, { cursor.width(), cursor.height() } };
	ASSERT_TRUE(rect.position.x >= 0 && rect.position.y >= 0 && rect.position.x + rect.size.width <= ScreenWidth && rect.position.y + rect.size.height <= ViewportHeight);

	std::vector<uint8_t> before(static_cast<size_t>(rect.size.width) * rect.size.height);
	for (int y = 0; y < rect.size.height; ++y)
		for (int x = 0; x < rect.size.width; ++x)
			before[static_cast<size_t>(y) * rect.size.width + x] = aux(rect.position.x + x, rect.position.y + y);
	ASSERT_NE(std::count(before.begin(), before.end(), gbuffer::UnlitByte), static_cast<std::ptrdiff_t>(before.size())) << "the cursor must land on world pixels";

	// DrawCursor: SaveRect, then the cursor's ClxDraw stamps Unlit on exactly the pixels it writes.
	std::vector<uint8_t> saved(before.size());
	gbuffer::SaveRect(rect, saved);
	EXPECT_EQ(saved, before);
	ClxDraw(*out, feet, cursor);
	for (int y = 0; y < rect.size.height; ++y) {
		for (int x = 0; x < rect.size.width; ++x) {
			const uint8_t a = aux(rect.position.x + x, rect.position.y + y);
			const uint8_t original = before[static_cast<size_t>(y) * rect.size.width + x];
			if (x == 0 || x == rect.size.width - 1)
				EXPECT_EQ(a, original) << "transparent cursor column " << x << " row " << y << " keeps the world byte";
			else
				EXPECT_EQ(a, gbuffer::UnlitByte) << "cursor pixel " << x << "," << y;
		}
	}

	// UndrawCursor: RestoreRect puts the world bytes back.
	gbuffer::RestoreRect(rect, saved);
	for (int y = 0; y < rect.size.height; ++y)
		for (int x = 0; x < rect.size.width; ++x)
			ASSERT_EQ(aux(rect.position.x + x, rect.position.y + y), before[static_cast<size_t>(y) * rect.size.width + x]) << x << "," << y;
}

TEST_F(ScrollrtPlanes, WorldPassOverheadTiming)
{
	// Budget to validate: world-pass CPU overhead <= +35 % at 640x480 native. Both configurations draw
	// the 640x352 viewport at view {11,11} into the fixture's 640x480 surface with the planes allocated over it
	// (classic mode keeps them allocated and untouched); the mode is latched off for the classic sample and on
	// for the hybrid sample. Classic and hybrid calls alternate so both see the same machine state, and the
	// medians of the per-call times are compared. This is a wall-clock measurement inside the ctest-registered binary, so by
	// default it only records and prints; the 3x sanity bound is asserted when SCROLLRT_TIMING_ASSERT is set
	// (the integration evidence run sets it), so a loaded CI runner cannot turn the measurement into a failure.
	constexpr int Rounds = 61; // odd, so the median is a sample
	const ViewGeometry geometry = PrepareView({ 11, 11 }, /*zoom=*/false, /*perPixel=*/false);

	const auto timeDraw = [&](bool hybrid) {
		gbuffer::EndFrame();
		gbuffer::LatchForFrame(LatchInputFor(hybrid));
		EXPECT_EQ(gbuffer::Enabled(), hybrid);
		ClearSurface(*out);
		const auto start = std::chrono::steady_clock::now();
		DrawWorldForTesting(*out, geometry.firstTile, geometry.offset);
		const auto end = std::chrono::steady_clock::now();
		EXPECT_EQ(gbuffer::CurrentMode, hybrid ? gbuffer::Mode::StampUnlit : gbuffer::Mode::Off);
		return std::chrono::duration<double, std::micro>(end - start).count();
	};

	timeDraw(false); // warm-up
	timeDraw(true);
	std::vector<double> classicUs;
	std::vector<double> hybridUs;
	for (int round = 0; round < Rounds; ++round) {
		classicUs.push_back(timeDraw(false));
		hybridUs.push_back(timeDraw(true));
	}
	std::sort(classicUs.begin(), classicUs.end());
	std::sort(hybridUs.begin(), hybridUs.end());
	const double classicMedianUs = classicUs[Rounds / 2];
	const double hybridMedianUs = hybridUs[Rounds / 2];
	const double ratio = hybridMedianUs / classicMedianUs;
	RecordProperty("world_pass_classic_us", static_cast<int>(classicMedianUs));
	RecordProperty("world_pass_hybrid_us", static_cast<int>(hybridMedianUs));
	RecordProperty("world_pass_hybrid_over_classic_percent", static_cast<int>(ratio * 100.0));
	std::printf("World pass 640x352 at view 11,11: classic %.1f us, hybrid %.1f us, ratio %.3fx (medians of %d interleaved rounds; SPEC section 11 budget <= 1.35x)\n",
	    classicMedianUs, hybridMedianUs, ratio, Rounds);
	EXPECT_GT(classicMedianUs, 0.0);
	if (std::getenv("SCROLLRT_TIMING_ASSERT") != nullptr) {
		EXPECT_LE(ratio, 3.0);
	} else {
		std::printf("SCROLLRT_TIMING_ASSERT is not set: the 3x sanity bound is recorded, not asserted\n");
	}
}

} // namespace
} // namespace devilution
