#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "control/control.hpp"
#include "diablo.h"
#include "engine/clx_sprite.hpp"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/webgpu_presenter.hpp"
#include "engine/surface.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "monster.h"
#include "objects.h"
#include "options.h"
#include "tables/misdat.h"
#include "utils/endian_write.hpp"
#include "utils/palette_blending.hpp"
#include "utils/surface_to_clx.hpp"
#include "utils/ui_fwd.h"

using namespace devilution;

// TilesInView

TEST(Scroll_rt, calc_tiles_in_view_original)
{
	gnScreenWidth = 640;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight - 128;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	int columns = 0;
	int rows = 0;
	TilesInView(&columns, &rows);
	EXPECT_EQ(columns, 10);
	EXPECT_EQ(rows, 11);
}

TEST(Scroll_rt, calc_tiles_in_view_original_zoom)
{
	gnScreenWidth = 640;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight - 128;
	GetOptions().Graphics.zoomLevel.SetValue(8);
	int columns = 0;
	int rows = 0;
	TilesInView(&columns, &rows);
	EXPECT_EQ(columns, 5);
	EXPECT_EQ(rows, 6);
}

TEST(Scroll_rt, calc_tiles_in_view_960_540)
{
	gnScreenWidth = 960;
	gnScreenHeight = 540;
	gnViewportHeight = gnScreenHeight;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	int columns = 0;
	int rows = 0;
	TilesInView(&columns, &rows);
	EXPECT_EQ(columns, 15);
	EXPECT_EQ(rows, 17);
}

TEST(Scroll_rt, calc_tiles_in_view_640_512)
{
	gnScreenWidth = 640;
	gnScreenHeight = 512;
	gnViewportHeight = gnScreenHeight - 128;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	int columns = 0;
	int rows = 0;
	TilesInView(&columns, &rows);
	EXPECT_EQ(columns, 10);
	EXPECT_EQ(rows, 12);
}

TEST(Scroll_rt, calc_tiles_in_view_768_480_zoom)
{
	gnScreenWidth = 768;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight;
	GetOptions().Graphics.zoomLevel.SetValue(8);
	int columns = 0;
	int rows = 0;
	TilesInView(&columns, &rows);
	EXPECT_EQ(columns, 6);
	EXPECT_EQ(rows, 8);
}

// CalcTileOffset

TEST(Scroll_rt, calc_tile_offset_original)
{
	gnScreenWidth = 640;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight - 128;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	int x = 0;
	int y = 0;
	CalcTileOffset(&x, &y);
	EXPECT_EQ(x, 0);
	EXPECT_EQ(y, 0);
}

TEST(Scroll_rt, calc_tile_offset_original_zoom)
{
	gnScreenWidth = 640;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight - 128;
	GetOptions().Graphics.zoomLevel.SetValue(8);
	int x = 0;
	int y = 0;
	CalcTileOffset(&x, &y);
	EXPECT_EQ(x, 0);
	EXPECT_EQ(y, 8);
}

TEST(Scroll_rt, calc_tile_offset_960_540)
{
	gnScreenWidth = 960;
	gnScreenHeight = 540;
	gnViewportHeight = gnScreenHeight;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	int x = 0;
	int y = 0;
	CalcTileOffset(&x, &y);
	EXPECT_EQ(x, 0);
	EXPECT_EQ(y, 2);
}

TEST(Scroll_rt, calc_tile_offset_853_480)
{
	gnScreenWidth = 853;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	int x = 0;
	int y = 0;
	CalcTileOffset(&x, &y);
	EXPECT_EQ(x, 21);
	EXPECT_EQ(y, 0);
}

TEST(Scroll_rt, calc_tile_offset_768_480_zoom)
{
	gnScreenWidth = 768;
	gnScreenHeight = 480;
	gnViewportHeight = gnScreenHeight;
	GetOptions().Graphics.zoomLevel.SetValue(8);
	int x = 0;
	int y = 0;
	CalcTileOffset(&x, &y);
	EXPECT_EQ(x, 0);
	EXPECT_EQ(y, 8);
}

// RowsCoveredByPanel

TEST(Scroll_rt, calc_tiles_covered_by_panel_original)
{
	gnScreenWidth = 640;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	CalculatePanelAreas();
	EXPECT_EQ(RowsCoveredByPanel(), 0);
}

TEST(Scroll_rt, calc_tiles_covered_by_panel_960)
{
	gnScreenWidth = 960;
	GetOptions().Graphics.zoomLevel.SetValue(4);
	CalculatePanelAreas();
	EXPECT_EQ(RowsCoveredByPanel(), 4);
}

TEST(Scroll_rt, calc_tiles_covered_by_panel_960_zoom)
{
	gnScreenWidth = 960;
	GetOptions().Graphics.zoomLevel.SetValue(8);
	CalculatePanelAreas();
	EXPECT_EQ(RowsCoveredByPanel(), 2);
}

// ---- 2.5D hybrid lighting: world pass integration ----

namespace {

/**
 * @brief Builds a single-frame CLX sprite list from a top-down pixel grid.
 *
 * `pixels` holds one int per pixel, row-major, top row first; -1 is transparent, anything else is the colour index.
 * Opaque pixels are written as literal runs except for the pixels flagged in `fillRuns`, which are written as
 * fill-N commands so both blitter functor paths (pixels and fill) are exercised.
 */
std::vector<uint8_t> MakeClxList(int width, int height, const std::vector<int> &pixels)
{
	std::vector<uint8_t> frame(6);
	WriteLE16(&frame[0], 6);
	WriteLE16(&frame[2], static_cast<uint16_t>(width));
	WriteLE16(&frame[4], static_cast<uint16_t>(height));
	for (int y = height - 1; y >= 0; --y) { // CL2 rows are stored bottom-up
		const int *row = &pixels[static_cast<size_t>(y) * width];
		int x = 0;
		while (x < width) {
			if (row[x] < 0) {
				int n = 0;
				while (x < width && row[x] < 0 && n < 0x7F) {
					++n;
					++x;
				}
				frame.push_back(static_cast<uint8_t>(n));
				continue;
			}
			// A run of the same colour of length >= 3 becomes a fill command.
			int same = 1;
			while (x + same < width && row[x + same] == row[x] && same < 63)
				++same;
			if (same >= 3) {
				frame.push_back(static_cast<uint8_t>(0xBF - same));
				frame.push_back(static_cast<uint8_t>(row[x]));
				x += same;
				continue;
			}
			int n = 0;
			const size_t control = frame.size();
			frame.push_back(0);
			while (x < width && row[x] >= 0 && n < 63 && !(x + 2 < width && row[x] == row[x + 1] && row[x] == row[x + 2])) {
				frame.push_back(static_cast<uint8_t>(row[x]));
				++n;
				++x;
			}
			frame[control] = static_cast<uint8_t>(256 - n);
		}
	}
	std::vector<uint8_t> list(12);
	WriteLE32(&list[0], 1);
	WriteLE32(&list[4], 12);
	WriteLE32(&list[8], static_cast<uint32_t>(12 + frame.size()));
	list.insert(list.end(), frame.begin(), frame.end());
	return list;
}

constexpr int SpriteWidth = 12;
constexpr int SpriteHeight = 9;

/** A 12x9 sprite with transparent margins, a colour-0 (shadow) pixel, literal runs and a fill run. */
std::vector<int> TestSpritePixels()
{
	std::vector<int> pixels(static_cast<size_t>(SpriteWidth) * SpriteHeight, -1);
	for (int y = 1; y < SpriteHeight - 1; ++y) {
		for (int x = 1; x < SpriteWidth - 1; ++x) {
			pixels[static_cast<size_t>(y) * SpriteWidth + x] = 1 + ((x * 7 + y * 13) % 200);
		}
	}
	// A fill run of 5 pixels of colour 42 and a shadow pixel (colour 0).
	for (int x = 3; x < 8; ++x)
		pixels[static_cast<size_t>(4) * SpriteWidth + x] = 42;
	pixels[static_cast<size_t>(2) * SpriteWidth + 2] = 0;
	return pixels;
}

void FillSurfaceWithPattern(const Surface &out)
{
	for (int y = 0; y < out.h(); ++y)
		for (int x = 0; x < out.w(); ++x)
			out.SetPixel({ x, y }, static_cast<uint8_t>((x * 5 + y * 3) & 0xFF));
}

std::vector<uint8_t> SurfaceBytes(const Surface &out)
{
	std::vector<uint8_t> bytes;
	bytes.reserve(static_cast<size_t>(out.w()) * out.h());
	for (int y = 0; y < out.h(); ++y)
		for (int x = 0; x < out.w(); ++x)
			bytes.push_back(out[Point { x, y }]);
	return bytes;
}

/** Distinct, non-identity light tables, TRN and blending lookup so every primitive produces a different image. */
void InstallDistinctiveTables(std::array<uint8_t, 256> &trn)
{
	for (size_t k = 0; k < LightTables.size(); ++k)
		for (int c = 0; c < 256; ++c)
			LightTables[k][c] = static_cast<uint8_t>(k == 0 ? c : (c ^ (k * 37)) & 0xFF);
	LightTables[LightsMax] = {}; // pitch black, as MakeLightTable does
	for (int c = 0; c < 256; ++c)
		trn[c] = static_cast<uint8_t>((c + 100) & 0xFF);
	for (int a = 0; a < 256; ++a)
		for (int b = 0; b < 256; ++b)
			paletteTransparencyLookup[a][b] = static_cast<uint8_t>((a * 3 + b * 5 + 1) & 0xFF);
}

enum class Primitive : uint8_t { Draw, Blended, Trn, BlendedTrn };

void DrawReference(const Surface &out, Point position, ClxSprite sprite, Primitive primitive, const uint8_t *table)
{
	switch (primitive) {
	case Primitive::Draw:
		ClxDraw(out, position, sprite);
		break;
	case Primitive::Blended:
		ClxDrawBlended(out, position, sprite);
		break;
	case Primitive::Trn:
		ClxDrawTRN(out, position, sprite, table);
		break;
	case Primitive::BlendedTrn:
		ClxDrawBlendedTRN(out, position, sprite, table);
		break;
	}
}

class HybridSpriteFunnelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		// The distinctive tables replace process-wide globals; keep the originals so later tests see the real ones.
		savedLightTables = LightTables;
		savedTransparency.assign(&paletteTransparencyLookup[0][0], &paletteTransparencyLookup[0][0] + TransparencyLookupSize);
		InstallDistinctiveTables(trn);
		list = MakeClxList(SpriteWidth, SpriteHeight, TestSpritePixels());
		savedMode = gbuffer::CurrentMode;
		savedKind = gbuffer::CurrentCardKind;
	}

	void TearDown() override
	{
		gbuffer::CurrentMode = savedMode;
		gbuffer::CurrentCardKind = savedKind;
		LightTables = savedLightTables;
		std::copy(savedTransparency.begin(), savedTransparency.end(), &paletteTransparencyLookup[0][0]);
	}

	[[nodiscard]] ClxSprite sprite() const { return ClxSpriteList { list.data() }[0]; }

	std::vector<uint8_t> DrawWith(const WorldSpriteDraw &draw, bool hybrid)
	{
		OwnedSurface out(32, 24);
		FillSurfaceWithPattern(out);
		DrawWorldSprite(out, draw, hybrid);
		return SurfaceBytes(out);
	}

	std::vector<uint8_t> DrawReferenceBytes(Primitive primitive, const uint8_t *table)
	{
		OwnedSurface out(32, 24);
		FillSurfaceWithPattern(out);
		DrawReference(out, Feet, sprite(), primitive, table);
		return SurfaceBytes(out);
	}

	static constexpr Point Feet { 9, 17 };
	static constexpr size_t TransparencyLookupSize = 256 * 256;
	std::array<uint8_t, 256> trn {};
	std::vector<uint8_t> list;
	gbuffer::Mode savedMode {};
	gbuffer::Kind savedKind {};
	std::array<std::array<uint8_t, LightTableSize>, NumLightingLevels> savedLightTables {};
	std::vector<uint8_t> savedTransparency;
};

} // namespace

TEST(HybridShadeIndex, ClassicIsIdentity)
{
	for (int idx = 0; idx <= LightsMax; ++idx)
		EXPECT_EQ(HybridShadeIndex(idx, /*hybrid=*/false), idx);
}

TEST(HybridTileShadeIndex, TilesAreAlwaysFullBrightInHybrid)
{
	for (int idx = 0; idx <= LightsMax + 3; ++idx) {
		EXPECT_EQ(HybridTileShadeIndex(idx, /*hybrid=*/false), idx);
		EXPECT_EQ(HybridTileShadeIndex(idx, /*hybrid=*/true), 0) << "index " << idx;
	}
}

TEST(HybridShadeIndex, HybridIsFullBrightExceptFullyDark)
{
	for (int idx = 0; idx < LightsMax; ++idx)
		EXPECT_EQ(HybridShadeIndex(idx, /*hybrid=*/true), 0) << "index " << idx;
	EXPECT_EQ(HybridShadeIndex(LightsMax, /*hybrid=*/true), LightsMax);
	EXPECT_EQ(HybridShadeIndex(LightsMax + 3, /*hybrid=*/true), LightsMax);
}

TEST_F(HybridSpriteFunnelTest, ReferencePrimitivesAreDistinguishable)
{
	// The dispatch table test below is only meaningful if every primitive it can select produces a different image.
	const std::vector<std::vector<uint8_t>> images {
		DrawReferenceBytes(Primitive::Draw, nullptr),
		DrawReferenceBytes(Primitive::Blended, nullptr),
		DrawReferenceBytes(Primitive::Trn, trn.data()),
		DrawReferenceBytes(Primitive::BlendedTrn, trn.data()),
		DrawReferenceBytes(Primitive::Trn, LightTables[3].data()),
		DrawReferenceBytes(Primitive::BlendedTrn, LightTables[3].data()),
		DrawReferenceBytes(Primitive::Trn, LightTables[LightsMax].data()),
	};
	OwnedSurface untouched(32, 24);
	FillSurfaceWithPattern(untouched);
	const std::vector<uint8_t> background = SurfaceBytes(untouched);
	for (size_t i = 0; i < images.size(); ++i) {
		EXPECT_NE(images[i], background) << "primitive " << i << " drew nothing";
		for (size_t j = i + 1; j < images.size(); ++j)
			EXPECT_NE(images[i], images[j]) << "primitives " << i << " and " << j << " are indistinguishable";
	}
}

TEST_F(HybridSpriteFunnelTest, ClassicDispatchIsBitExact)
{
	struct Case {
		const char *name;
		bool useTrn;
		int shadeIndex;
		bool blended;
		bool selfLit;
		gbuffer::Kind archKind;
		Primitive expected;
		enum class Table : uint8_t { None, Trn, Light } table;
	};
	using Table = Case::Table;
	const Case cases[] = {
		{ "plain", false, 0, false, false, gbuffer::Kind::Unlit, Primitive::Draw, Table::None },
		{ "blended", false, 0, true, false, gbuffer::Kind::Unlit, Primitive::Blended, Table::None },
		{ "lit", false, 3, false, false, gbuffer::Kind::Unlit, Primitive::Trn, Table::Light },
		{ "lit blended", false, 3, true, false, gbuffer::Kind::Unlit, Primitive::BlendedTrn, Table::Light },
		{ "fully dark", false, LightsMax, false, false, gbuffer::Kind::Unlit, Primitive::Trn, Table::Light },
		{ "trn", true, 0, false, false, gbuffer::Kind::Unlit, Primitive::Trn, Table::Trn },
		{ "trn blended", true, 0, true, false, gbuffer::Kind::Unlit, Primitive::BlendedTrn, Table::Trn },
		{ "trn wins over light", true, 7, false, false, gbuffer::Kind::Unlit, Primitive::Trn, Table::Trn },
		{ "trn blended wins over light", true, 7, true, false, gbuffer::Kind::Unlit, Primitive::BlendedTrn, Table::Trn },
		{ "self-lit does not change classic output", false, 3, false, true, gbuffer::Kind::Unlit, Primitive::Trn, Table::Light },
		{ "arch kind does not change classic output", false, 3, true, false, gbuffer::Kind::WallLeft, Primitive::BlendedTrn, Table::Light },
	};
	for (const Case &c : cases) {
		const WorldSpriteDraw draw {
			.feet = Feet,
			.sprite = sprite(),
			.shadeIndex = c.shadeIndex,
			.trn = c.useTrn ? trn.data() : nullptr,
			.blended = c.blended,
			.selfLit = c.selfLit,
			.archKind = c.archKind,
		};
		const uint8_t *table = c.table == Table::Trn ? trn.data() : c.table == Table::Light ? LightTables[c.shadeIndex].data() : nullptr;
		EXPECT_EQ(DrawWith(draw, /*hybrid=*/false), DrawReferenceBytes(c.expected, table)) << c.name;
		EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off) << c.name << ": classic dispatch must not touch the G-buffer mode";
	}
}

TEST_F(HybridSpriteFunnelTest, HybridSetsCardKind)
{
	gbuffer::CurrentCardKind = gbuffer::Kind::Unlit;
	EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite() }, /*hybrid=*/true), DrawReferenceBytes(Primitive::Draw, nullptr));
	EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::Card);

	DrawWith({ .feet = Feet, .sprite = sprite(), .selfLit = true }, /*hybrid=*/true);
	EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::CardEmissive) << "self-lit sprites are emissive cards";

	gbuffer::CurrentCardKind = gbuffer::Kind::Unlit;
	EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .trn = trn.data() }, /*hybrid=*/true), DrawReferenceBytes(Primitive::Trn, trn.data()));
	EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::CardEmissive) << "TRN'd sprites keep the TRN and are emissive cards";

	DrawWith({ .feet = Feet, .sprite = sprite(), .blended = true }, /*hybrid=*/true);
	EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::Card) << "blending alone does not make a card emissive";
}

TEST_F(HybridSpriteFunnelTest, HybridRunsPrimitivesFullBright)
{
	// Hybrid runs the same primitives with shadeIndex = 0; the presenter lights the card. A lit
	// authoritative index must therefore not select a light table (fully dark is the next test).
	for (const int shadeIndex : { 0, 3, static_cast<int>(LightsMax) - 1 }) {
		EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = shadeIndex }, /*hybrid=*/true),
		    DrawReferenceBytes(Primitive::Draw, nullptr))
		    << "shadeIndex " << shadeIndex;
		EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = shadeIndex, .blended = true }, /*hybrid=*/true),
		    DrawReferenceBytes(Primitive::Blended, nullptr))
		    << "blended, shadeIndex " << shadeIndex;
		EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = shadeIndex, .trn = trn.data() }, /*hybrid=*/true),
		    DrawReferenceBytes(Primitive::Trn, trn.data()))
		    << "the semantic TRN is kept, shadeIndex " << shadeIndex;
	}
	// The same requests in classic mode do apply the light table, so the hybrid images above are not vacuous.
	EXPECT_NE(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = 3 }, /*hybrid=*/false), DrawReferenceBytes(Primitive::Draw, nullptr));
	EXPECT_NE(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = LightsMax }, /*hybrid=*/false), DrawReferenceBytes(Primitive::Draw, nullptr));
}

TEST_F(HybridSpriteFunnelTest, FullyDarkSpritesStayBlackAndPassThrough)
{
	// No-reveal: items, light-flagged missiles and arches on a dLight == 15 cell reach the funnel with
	// LightsMax and no cull. Hybrid keeps the classic dark table and tags them pass-through, so the presenter cannot
	// light them; a semantic TRN still wins, as in classic.
	for (const bool blended : { false, true }) {
		const Primitive primitive = blended ? Primitive::BlendedTrn : Primitive::Trn;
		gbuffer::CurrentCardKind = gbuffer::Kind::Card;
		EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = LightsMax, .blended = blended }, /*hybrid=*/true),
		    DrawReferenceBytes(primitive, LightTables[LightsMax].data()))
		    << "blended " << blended;
		EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::CardEmissive);
		EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = LightsMax, .blended = blended }, /*hybrid=*/true),
		    DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = LightsMax, .blended = blended }, /*hybrid=*/false))
		    << "the classic image, blended " << blended;

		// Arches too: no wall context, pass-through.
		gbuffer::CurrentCardKind = gbuffer::Kind::Card;
		EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = LightsMax, .blended = blended, .archKind = gbuffer::Kind::WallLeft }, /*hybrid=*/true),
		    DrawReferenceBytes(primitive, LightTables[LightsMax].data()));
		EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::CardEmissive);
	}
	EXPECT_EQ(DrawWith({ .feet = Feet, .sprite = sprite(), .shadeIndex = LightsMax, .trn = trn.data() }, /*hybrid=*/true),
	    DrawReferenceBytes(Primitive::Trn, trn.data()))
	    << "infravision / stone / unique TRNs keep their colours";
	EXPECT_EQ(gbuffer::CurrentCardKind, gbuffer::Kind::CardEmissive);
}

TEST_F(HybridSpriteFunnelTest, HybridArchFunnelSetsWallContext)
{
	constexpr uint8_t Sentinel = 0xFF;
	for (const gbuffer::Kind archKind : { gbuffer::Kind::WallLeft, gbuffer::Kind::WallRight }) {
		gbuffer::CurrentCardKind = gbuffer::Kind::Card;
		gbuffer::CurrentTileAux = Sentinel;
		const std::vector<uint8_t> image = DrawWith({ .feet = Feet, .sprite = sprite(), .blended = true, .archKind = archKind }, /*hybrid=*/true);
		EXPECT_EQ(image, DrawReferenceBytes(Primitive::Blended, nullptr)) << "arch pixels are drawn with the same primitive";
		EXPECT_EQ(gbuffer::CurrentCardKind, archKind) << "arches route the ClxDraw through the WallLeft/WallRight constant-aux path";
		// SetTileContext fills CurrentTileAux when the real gbuffer.cpp is linked; a stub leaves the sentinel.
		const uint8_t expectedNormal = archKind == gbuffer::Kind::WallLeft ? gbuffer::NormalWallLeft : gbuffer::NormalWallRight;
		if (gbuffer::CurrentTileAux != Sentinel) {
			EXPECT_EQ(gbuffer::CurrentTileAux, gbuffer::PackAux(archKind, expectedNormal));
			EXPECT_EQ(gbuffer::CurrentTileLeftX, Feet.x);
			for (int c = 0; c < 32; ++c)
				EXPECT_EQ(gbuffer::CurrentTileBaseRow[c], static_cast<uint8_t>(Feet.y & 0xFF)) << "foot None: B = tile base for column " << c;
		}
	}
}

namespace {

gbuffer::LatchInput EnablingLatchInput(dungeon_type level)
{
	return gbuffer::LatchInput {
		.optionEnabled = true,
		.presenterReady = true,
		.headless = false,
		.gameRunning = true,
		.level = level,
		.logicalPalette = logical_palette.data(),
	};
}

} // namespace

TEST(HybridLatch, TownDisablesTheMode)
{
	gbuffer::LatchForFrame(EnablingLatchInput(DTYPE_TOWN));
	EXPECT_FALSE(gbuffer::Enabled());
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off);
	LatchHybridRepresentFrame(EnablingLatchInput(DTYPE_TOWN));
	EXPECT_FALSE(gbuffer::Enabled());
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off) << "a disabled re-present never arms StampUnlit";
	gbuffer::EndFrame();
}

TEST(HybridLatch, HeadlessAndMenuDisableTheMode)
{
	gbuffer::LatchInput in = EnablingLatchInput(DTYPE_CATHEDRAL);
	in.headless = true;
	gbuffer::LatchForFrame(in);
	EXPECT_FALSE(gbuffer::Enabled());
	in = EnablingLatchInput(DTYPE_CATHEDRAL);
	in.gameRunning = false;
	gbuffer::LatchForFrame(in);
	EXPECT_FALSE(gbuffer::Enabled());
	in = EnablingLatchInput(DTYPE_CATHEDRAL);
	in.presenterReady = false;
	gbuffer::LatchForFrame(in);
	EXPECT_FALSE(gbuffer::Enabled());
	gbuffer::EndFrame();
}

TEST(HybridLatch, LatchResetsModeAndLatchesPalette)
{
	gbuffer::CurrentMode = gbuffer::Mode::StampUnlit; // a frame whose RenderPresent returned early
	const uint32_t before = gbuffer::FrameCounter();
	gbuffer::LatchForFrame(EnablingLatchInput(DTYPE_CATHEDRAL));
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off) << "the latch never carries StampUnlit into the next world pass";
	EXPECT_EQ(gbuffer::FrameCounter(), before + 1);
	EXPECT_EQ(gbuffer::CurrentPalette(), logical_palette.data());
	gbuffer::EndFrame();
}

TEST(HybridLatch, RepresentPathStampsUnlitWhenEnabled)
{
	LatchHybridRepresentFrame(EnablingLatchInput(DTYPE_CATHEDRAL));
	// A gbuffer stub never enables the mode; with the real implementation the dungeon input enables it.
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Enabled() ? gbuffer::Mode::StampUnlit : gbuffer::Mode::Off);
	gbuffer::EndFrame();
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off);
}

TEST(HybridLatch, WorldPathLatchesWithoutArmingStamp)
{
	LatchHybridWorldFrame(EnablingLatchInput(DTYPE_CATHEDRAL));
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off) << "DrawAndBlit's latch leaves the mode to BeginWorldPass";
	LatchHybridWorldFrame(EnablingLatchInput(DTYPE_TOWN));
	EXPECT_FALSE(gbuffer::Enabled());
	gbuffer::EndFrame();
}

TEST(HybridLatch, WorldPathStartsANewNormalMapFrameBudget)
{
	// The on-demand generation budget and the hit / miss counters are per game frame. Without the reset in
	// LatchHybridWorldFrame the first generations of a session spent the budget for good and every later miss was flat.
	const bool savedNormalMaps = *GetOptions().Graphics.hybridNormalMaps;
	GetOptions().Graphics.hybridNormalMaps.SetValue(true);
	OwnedSurface backBuffer(64, 32);
	gbuffer::Allocate(backBuffer.begin(), static_cast<uint16_t>(backBuffer.pitch()), 64, 32);
	clx_normal_map::SetOptions(clx_normal_map::Options { .generate = true });

	// Three distinct discs, stacked as three frames.
	constexpr int FrameSize = 16;
	OwnedSurface art(FrameSize, FrameSize * 3);
	for (int y = 0; y < art.h(); ++y) {
		for (int x = 0; x < art.w(); ++x) {
			const int dx = x - FrameSize / 2;
			const int dy = (y % FrameSize) - FrameSize / 2;
			art[{ x, y }] = dx * dx + dy * dy <= 36 ? static_cast<uint8_t>(40 + (y / FrameSize) * 20 + ((x ^ y) & 3)) : 0;
		}
	}
	const OwnedClxSpriteList frames = SurfaceToClx(art, 3, 0);
	clx_normal_map::SetBudget(1, 1000000000);
	clx_normal_map::ResetFrameStats();
	EXPECT_NE(clx_normal_map::GetOrBuildNormalPlane(frames[0], false).data, nullptr);
	EXPECT_EQ(clx_normal_map::GetOrBuildNormalPlane(frames[1], false).data, nullptr) << "budget spent: flat and queued";
	EXPECT_EQ(clx_normal_map::GetStats().misses, 2U);

	LatchHybridWorldFrame(EnablingLatchInput(DTYPE_CATHEDRAL));
	ASSERT_TRUE(gbuffer::Enabled());
	clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_EQ(s.generatedThisFrame, 0U);
	EXPECT_EQ(s.misses, 0U);
	EXPECT_EQ(s.hits, 0U);
	EXPECT_EQ(s.queued, 0U) << "Pump drained the queued frame";
	EXPECT_NE(clx_normal_map::GetOrBuildNormalPlane(frames[2], false).data, nullptr) << "the new frame's budget generates now";
	s = clx_normal_map::GetStats();
	EXPECT_EQ(s.generatedThisFrame, 1U);
	EXPECT_EQ(s.misses, 1U);

	gbuffer::EndFrame();
	clx_normal_map::ClearNormalPlaneCache();
	clx_normal_map::SetBudget(8, 1500);
	clx_normal_map::ResetFrameStats();
	gbuffer::Free();
	GetOptions().Graphics.hybridNormalMaps.SetValue(savedNormalMaps);
}

TEST(HybridLatch, RepurposedBackBufferIsNeverComposited)
{
	// Loading screens (ShowProgress -> ClearScreenBuffer) and in-game movies (play_movie) reuse the back buffer
	// while gbRunGame stays true; their presents must not go through the composite over the last world frame's planes.
	OwnedSurface backBuffer(64, 32);
	gbuffer::Allocate(backBuffer.begin(), static_cast<uint16_t>(backBuffer.pitch()), 64, 32);
	gbuffer::LatchForFrame(EnablingLatchInput(DTYPE_CATHEDRAL));
	gbuffer::BeginWorldPass(32);
	gbuffer::EndWorldPass();
	gbuffer::EndFrame();
	ASSERT_TRUE(gbuffer::BackBufferIsHybrid());

	// A re-present right after the option was switched off still composites the hybrid frame: the cursor must stamp.
	gbuffer::LatchInput off = EnablingLatchInput(DTYPE_CATHEDRAL);
	off.optionEnabled = false;
	LatchHybridRepresentFrame(off);
	EXPECT_FALSE(gbuffer::Enabled());
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::StampUnlit);
	gbuffer::EndFrame();

	const bool savedHeadless = HeadlessMode;
	HeadlessMode = true; // ClearScreenBuffer's fill needs PalSurface; the invalidation comes first
	ClearScreenBuffer();
	HeadlessMode = savedHeadless;
	EXPECT_FALSE(gbuffer::BackBufferIsHybrid());
	LatchHybridRepresentFrame(off);
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off) << "the cleared buffer is presented classic";
	gbuffer::EndFrame();
	gbuffer::Free();
}

TEST(HybridLatch, CurrentLatchInputReflectsGameState)
{
	const bool savedOption = *GetOptions().Graphics.hybridLighting;
	const bool savedHeadless = HeadlessMode;
	const bool savedRunGame = gbRunGame;
	const dungeon_type savedLevel = leveltype;

	GetOptions().Graphics.hybridLighting.SetValue(true);
	HeadlessMode = true;
	gbRunGame = false;
	leveltype = DTYPE_CATACOMBS;
	gbuffer::LatchInput in = CurrentHybridLatchInput();
	EXPECT_TRUE(in.optionEnabled);
	EXPECT_TRUE(in.headless);
	EXPECT_FALSE(in.gameRunning);
	EXPECT_EQ(in.level, DTYPE_CATACOMBS);
	EXPECT_EQ(in.logicalPalette, logical_palette.data());
	EXPECT_EQ(in.presenterReady, WebGpuIsReady());

	GetOptions().Graphics.hybridLighting.SetValue(false);
	HeadlessMode = false;
	gbRunGame = true;
	leveltype = DTYPE_TOWN;
	in = CurrentHybridLatchInput();
	EXPECT_FALSE(in.optionEnabled);
	EXPECT_FALSE(in.headless);
	EXPECT_TRUE(in.gameRunning);
	EXPECT_EQ(in.level, DTYPE_TOWN);

	GetOptions().Graphics.hybridLighting.SetValue(savedOption);
	HeadlessMode = savedHeadless;
	gbRunGame = savedRunGame;
	leveltype = savedLevel;
}

namespace {

struct PrewarmGameState {
	bool option = *GetOptions().Graphics.hybridLighting;
	bool normalMaps = *GetOptions().Graphics.hybridNormalMaps;
	bool headless = HeadlessMode;
	dungeon_type level = leveltype;
	size_t monsterTypes = LevelMonsterTypeCount;
	int objects = ActiveObjectCount;

	~PrewarmGameState()
	{
		GetOptions().Graphics.hybridLighting.SetValue(option);
		GetOptions().Graphics.hybridNormalMaps.SetValue(normalMaps);
		HeadlessMode = headless;
		leveltype = level;
		LevelMonsterTypeCount = monsterTypes;
		ActiveObjectCount = objects;
	}
};

} // namespace

TEST(HybridPrewarm, NoopWhenDisabled)
{
	const PrewarmGameState saved;
	HeadlessMode = false;
	leveltype = DTYPE_CATHEDRAL;
	const clx_normal_map::Stats before = clx_normal_map::GetStats();

	GetOptions().Graphics.hybridLighting.SetValue(false);
	GetOptions().Graphics.hybridNormalMaps.SetValue(true);
	PrewarmHybridSpriteNormals(/*presenterReady=*/true);
	GetOptions().Graphics.hybridLighting.SetValue(true);
	GetOptions().Graphics.hybridNormalMaps.SetValue(false);
	PrewarmHybridSpriteNormals(/*presenterReady=*/true);
	GetOptions().Graphics.hybridNormalMaps.SetValue(true);
	leveltype = DTYPE_TOWN;
	PrewarmHybridSpriteNormals(/*presenterReady=*/true);
	leveltype = DTYPE_CATHEDRAL;
	HeadlessMode = true;
	PrewarmHybridSpriteNormals(/*presenterReady=*/true);
	HeadlessMode = false;
	PrewarmHybridSpriteNormals(/*presenterReady=*/false); // no presenter: nothing would ever drain the queue

	const clx_normal_map::Stats after = clx_normal_map::GetStats();
	EXPECT_EQ(after.queued, before.queued);
	EXPECT_EQ(after.prewarmDropped, before.prewarmDropped);
}

TEST(HybridPrewarm, EnumeratesEmptyLevelWithoutSprites)
{
	const PrewarmGameState saved;
	// The real game loads the missile table at startup, before any level; the prewarm iterates every MissileGraphicID.
	LoadMissileData();
	GetOptions().Graphics.hybridLighting.SetValue(true);
	GetOptions().Graphics.hybridNormalMaps.SetValue(true);
	HeadlessMode = false;
	leveltype = DTYPE_CATHEDRAL;
	// One monster type and one object with no sprites loaded: every branch is walked and nothing is enqueued.
	LevelMonsterTypeCount = 1;
	ActiveObjectCount = 1;
	ActiveObjects[0] = 0;
	Objects[0]._oAnimData = std::nullopt;
	const clx_normal_map::Stats before = clx_normal_map::GetStats();

	PrewarmHybridSpriteNormals(/*presenterReady=*/true);

	const clx_normal_map::Stats after = clx_normal_map::GetStats();
	EXPECT_EQ(after.queued, before.queued);
	EXPECT_EQ(after.prewarmDropped, before.prewarmDropped);
}
