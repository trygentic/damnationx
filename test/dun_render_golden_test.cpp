/**
 * @file dun_render_golden_test.cpp
 *
 * Classic byte-identity goldens for the dungeon tile blitter and the
 * G-buffer capture-mode coverage-parity checks.
 *
 * Every case is driven through the public `RenderTileFrame` +
 * `FlushTileRenderCommands` pair into a 96x96 surface. Frames come from
 * `test/fixtures/dun_render/frames.bin` (format in the README next to it).
 *
 * Re-record the goldens with:
 *   DUN_RENDER_GOLDEN_RECORD=/path/to/goldens.tsv ./dun_render_golden_test
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/lighting_defs.hpp"
#include "engine/point.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/light_render.hpp"
#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"
#include "options.h"
#include "utils/is_of.hpp"
#include "utils/palette_blending.hpp"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

constexpr const char *FramesPath = "test/fixtures/dun_render/frames.bin";
constexpr const char *GoldensPath = "test/fixtures/dun_render/goldens.tsv";

constexpr int OutSize = 96;

/** Plane byte no tile write can produce here: kind 7 with normal 31 is never set by the tests. */
constexpr uint8_t Sentinel = 0xFF;

/** Never 0 (black tiles write 0); variant 2 differs from variant 0 at every pixel. */
uint8_t Pattern(int x, int y, int variant)
{
	return static_cast<uint8_t>((((x * 7) + (y * 13)) << 1) | 1) + static_cast<uint8_t>(variant);
}

void FillPattern(const Surface &out, int variant)
{
	for (int y = 0; y < out.h(); ++y) {
		for (int x = 0; x < out.w(); ++x) {
			out[Point { x, y }] = Pattern(x, y, variant);
		}
	}
}

struct FrameRecord {
	std::string name;
	TileType tile;
	int height;
	std::vector<uint8_t> data;
	/** Decoded 32 x height grid, bottom row first; nullopt = transparent. */
	std::vector<std::optional<uint8_t>> pixels;

	[[nodiscard]] const std::optional<uint8_t> &at(int row, int col) const
	{
		return pixels[(row * DunFrameWidth) + col];
	}
};

/** [start, end) of the opaque span of `row` (from the bottom) for the fixed-shape tile types. */
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

void DecodeFrame(FrameRecord &frame)
{
	frame.pixels.assign(static_cast<size_t>(DunFrameWidth) * frame.height, std::nullopt);
	size_t pos = 0;
	if (frame.tile == TileType::TransparentSquare) {
		for (int row = 0; row < frame.height; ++row) {
			int col = 0;
			while (col < DunFrameWidth) {
				ASSERT_LT(pos, frame.data.size()) << frame.name;
				const auto v = static_cast<int8_t>(frame.data[pos++]);
				if (v > 0) {
					for (int i = 0; i < v; ++i) {
						ASSERT_LT(pos, frame.data.size()) << frame.name;
						frame.pixels[(row * DunFrameWidth) + col + i] = frame.data[pos++];
					}
					col += v;
				} else {
					col += -v;
				}
			}
			ASSERT_EQ(col, DunFrameWidth) << frame.name << " row " << row;
		}
	} else {
		for (int row = 0; row < frame.height; ++row) {
			const auto [start, end] = ShapeSpan(frame.tile, row);
			for (int col = start; col < end; ++col) {
				ASSERT_LT(pos, frame.data.size()) << frame.name;
				frame.pixels[(row * DunFrameWidth) + col] = frame.data[pos++];
			}
		}
	}
	ASSERT_EQ(pos, frame.data.size()) << frame.name << ": trailing bytes in the frame record";
}

std::string_view TileName(TileType tile)
{
	switch (tile) {
	case TileType::Square:
		return "Square";
	case TileType::TransparentSquare:
		return "TransparentSquare";
	case TileType::LeftTriangle:
		return "LeftTriangle";
	case TileType::RightTriangle:
		return "RightTriangle";
	case TileType::LeftTrapezoid:
		return "LeftTrapezoid";
	case TileType::RightTrapezoid:
		return "RightTrapezoid";
	}
	return "?";
}

uint32_t ReadU32(const std::vector<uint8_t> &bytes, size_t pos)
{
	return static_cast<uint32_t>(bytes[pos]) | (static_cast<uint32_t>(bytes[pos + 1]) << 8)
	    | (static_cast<uint32_t>(bytes[pos + 2]) << 16) | (static_cast<uint32_t>(bytes[pos + 3]) << 24);
}

std::vector<FrameRecord> LoadFrames()
{
	std::vector<FrameRecord> frames;
	std::ifstream in(FramesPath, std::ios::binary);
	if (!in) {
		ADD_FAILURE() << "cannot open " << FramesPath;
		return frames;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() < 12 || std::memcmp(bytes.data(), "DUNF", 4) != 0 || ReadU32(bytes, 4) != 1) {
		ADD_FAILURE() << FramesPath << ": bad header";
		return frames;
	}
	const uint32_t count = ReadU32(bytes, 8);
	size_t pos = 12;
	for (uint32_t i = 0; i < count; ++i) {
		if (pos + 8 > bytes.size()) {
			ADD_FAILURE() << FramesPath << ": truncated record header";
			return frames;
		}
		FrameRecord frame;
		frame.tile = static_cast<TileType>(bytes[pos]);
		frame.height = bytes[pos + 1];
		const uint32_t size = ReadU32(bytes, pos + 4);
		pos += 8;
		if (pos + size > bytes.size()) {
			ADD_FAILURE() << FramesPath << ": truncated record data";
			return frames;
		}
		frame.data.assign(bytes.begin() + static_cast<ptrdiff_t>(pos), bytes.begin() + static_cast<ptrdiff_t>(pos + size));
		pos += size;
		frame.name = std::string(TileName(frame.tile));
		const int nominalHeight = IsAnyOf(frame.tile, TileType::LeftTriangle, TileType::RightTriangle) ? DunFrameTriangleHeight : DunFrameHeight;
		if (frame.height != nominalHeight)
			frame.name = StrCat(frame.name, frame.height);
		DecodeFrame(frame);
		frames.push_back(std::move(frame));
	}
	return frames;
}

bool MaskValid(TileType tile, MaskType mask)
{
	switch (mask) {
	case MaskType::Solid:
	case MaskType::Transparent:
		return true;
	case MaskType::Left:
		return IsAnyOf(tile, TileType::TransparentSquare, TileType::LeftTrapezoid);
	case MaskType::Right:
		return IsAnyOf(tile, TileType::TransparentSquare, TileType::RightTrapezoid);
	}
	return false;
}

std::string_view MaskName(MaskType mask)
{
	switch (mask) {
	case MaskType::Solid:
		return "Solid";
	case MaskType::Transparent:
		return "Transparent";
	case MaskType::Left:
		return "Left";
	case MaskType::Right:
		return "Right";
	}
	return "?";
}

enum class LightCase : uint8_t {
	FullyDark,
	FullyLit,
	PartiallyLit,
	/** Hell: LightTables[0] carries the blood remap, so the lightmap has no fully-lit table. */
	HellLit,
};
constexpr LightCase AllLights[] = { LightCase::FullyDark, LightCase::FullyLit, LightCase::PartiallyLit, LightCase::HellLit };

std::string_view LightName(LightCase light)
{
	switch (light) {
	case LightCase::FullyDark:
		return "FullyDark";
	case LightCase::FullyLit:
		return "FullyLit";
	case LightCase::PartiallyLit:
		return "PartiallyLit";
	case LightCase::HellLit:
		return "HellLit";
	}
	return "?";
}

struct ClipCase {
	std::string_view name;
	Point position;
};

// Frame bottom-left positions in the 96x96 surface (frames are 32 wide, 31 or 32 tall).
constexpr ClipCase ClipCases[] = {
	{ "Full", { 32, 63 } },
	{ "ClipTop", { 32, 25 } },          // clip.top 6/5: inside the upper half
	{ "ClipTopDeep", { 32, 10 } },      // clip.top 21/20: reaches into the lower half
	{ "ClipBottom", { 32, 100 } },      // clip.bottom 5
	{ "ClipBottomDeep", { 32, 115 } },  // clip.bottom 20: past the lower half
	{ "ClipLeft", { -10, 63 } },        // clip.left 10
	{ "ClipRight", { 80, 63 } },        // clip.right 16
	{ "ClipLeftTop", { -10, 25 } },     // left + vertical
	{ "ClipRightBottom", { 80, 100 } }, // right + vertical
};

struct TestLighting {
	std::array<std::array<uint8_t, LightTableSize>, NumLightingLevels> tables;
	std::array<uint8_t, LightTableSize> hellTable;

	TestLighting()
	{
		for (size_t level = 0; level < NumLightingLevels; ++level) {
			for (int c = 0; c < 256; ++c) {
				if (level == 0) {
					tables[level][c] = static_cast<uint8_t>(c);
				} else if (level == NumLightingLevels - 1) {
					tables[level][c] = 0;
				} else {
					// Odd multiplier: a bijection, so distinct source colours stay distinct.
					tables[level][c] = static_cast<uint8_t>((c * ((2 * level) + 1)) + (11 * level));
				}
			}
		}
		hellTable = tables[0];
		for (int i = 1; i < 32; ++i)
			hellTable[i] = static_cast<uint8_t>(200 + i);
	}

	[[nodiscard]] Lightmap lightmap(LightCase light) const
	{
		return Lightmap(/*outBuffer=*/nullptr, /*lightmapBuffer=*/ {}, /*pitch=*/1, tables,
		    light == LightCase::HellLit ? nullptr : tables[0].data(), tables[NumLightingLevels - 1].data());
	}

	[[nodiscard]] const uint8_t *tbl(LightCase light) const
	{
		switch (light) {
		case LightCase::FullyDark:
			return tables[NumLightingLevels - 1].data();
		case LightCase::FullyLit:
			return tables[0].data();
		case LightCase::PartiallyLit:
			return tables[5].data();
		case LightCase::HellLit:
			return hellTable.data();
		}
		return nullptr;
	}
};

void InitBlendLookup()
{
	[[maybe_unused]] static const bool Done = [] {
		// Asymmetric in (a, b) and injective in b for a fixed a (3 is odd), so the goldens
		// are sensitive to argument order and blended pixels always depend on the destination.
		for (int a = 0; a < 256; ++a) {
			for (int b = 0; b < 256; ++b) {
				paletteTransparencyLookup[a][b] = static_cast<uint8_t>((a * 5) + (b * 3) + 1);
			}
		}
		return true;
	}();
}

uint64_t HashSurface(const Surface &out)
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

std::string HexHash(uint64_t hash)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
	return buf;
}

std::map<std::string, uint64_t> ReadGoldens()
{
	std::map<std::string, uint64_t> goldens;
	std::ifstream in(GoldensPath);
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		const size_t tab = line.rfind('\t');
		if (tab == std::string::npos) {
			ADD_FAILURE() << GoldensPath << ": malformed line: " << line;
			continue;
		}
		goldens[line.substr(0, tab)] = std::strtoull(line.c_str() + tab + 1, nullptr, 16);
	}
	return goldens;
}

struct RenderCase {
	const FrameRecord *frame;
	MaskType mask;
	LightCase light;
	const ClipCase *clip;

	[[nodiscard]] std::string key() const
	{
		return StrCat(frame->name, "\t", MaskName(mask), "\t", LightName(light), "\t", clip->name);
	}
};

std::vector<uint8_t> CopyPixels(const Surface &out)
{
	std::vector<uint8_t> pixels(static_cast<size_t>(out.w()) * out.h());
	for (int y = 0; y < out.h(); ++y)
		std::memcpy(&pixels[static_cast<size_t>(y) * out.w()], out.at(0, y), out.w());
	return pixels;
}

/** Sentinel-filled G-buffer planes mirroring `out` (the test wires the globals instead of calling gbuffer::Allocate). */
struct TestPlanes {
	std::vector<uint8_t> aux;
	std::vector<uint8_t> base;

	void attach(const Surface &out)
	{
		const size_t size = static_cast<size_t>(out.pitch()) * out.h();
		aux.assign(size, Sentinel);
		base.assign(size, Sentinel);
		gbuffer::CurrentPlanes.aux = aux.data();
		gbuffer::CurrentPlanes.base = base.data();
		gbuffer::CurrentPlanes.outBase = out.begin();
		gbuffer::CurrentPlanes.pitch = out.pitch();
		gbuffer::CurrentPlanes.width = static_cast<uint16_t>(out.w());
		gbuffer::CurrentPlanes.height = static_cast<uint16_t>(out.h());
		gbuffer::CurrentPlanes.pitchRecip40 = ((uint64_t { 1 } << 40) + out.pitch() - 1) / out.pitch();
	}

	[[nodiscard]] static size_t offset(const Surface &out, int x, int y)
	{
		return static_cast<size_t>(out.at(x, y) - out.begin());
	}

	[[nodiscard]] bool untouched() const
	{
		for (const uint8_t a : aux)
			if (a != Sentinel) return false;
		for (const uint8_t b : base)
			if (b != Sentinel) return false;
		return true;
	}
};

/** Equivalent of gbuffer::SetTileContext: fills the four globals the blitter reads. */
void SetContext(gbuffer::Kind kind, uint8_t normal, int anchorRow, int leftX, gbuffer::FootKind foot)
{
	gbuffer::CurrentTileAux = gbuffer::PackAux(kind, normal);
	gbuffer::CurrentTileAuxBlended = IsAnyOf(kind, gbuffer::Kind::WallLeft, gbuffer::Kind::WallRight)
	    ? gbuffer::PackAux(gbuffer::Kind::WallBlended, normal)
	    : gbuffer::CurrentTileAux;
	gbuffer::BuildBaseRow(gbuffer::CurrentTileBaseRow, anchorRow, foot);
	gbuffer::CurrentTileLeftX = static_cast<int16_t>(leftX);
}

struct TileContext {
	gbuffer::Kind kind;
	uint8_t normal;
	gbuffer::FootKind foot;
};

/** The kind a world-pass caller would set for this frame shape (call sites). */
TileContext ContextFor(TileType tile)
{
	switch (tile) {
	case TileType::LeftTrapezoid:
	case TileType::LeftTriangle: // deliberately a wall: the blitter must override triangles to Floor
		return { gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, gbuffer::FootKind::SW };
	case TileType::RightTrapezoid:
	case TileType::RightTriangle:
		return { gbuffer::Kind::WallRight, gbuffer::NormalWallRight, gbuffer::FootKind::SE };
	case TileType::Square:
		return { gbuffer::Kind::WallTop, gbuffer::NormalUp, gbuffer::FootKind::None };
	case TileType::TransparentSquare:
		return { gbuffer::Kind::Floor, gbuffer::NormalUp, gbuffer::FootKind::None };
	}
	return { gbuffer::Kind::Unlit, 0, gbuffer::FootKind::None };
}

class DunRenderGoldenTest : public ::testing::Test {
protected:
	static void SetUpTestSuite()
	{
		InitBlendLookup();
		GetOptions().Graphics.perPixelLighting.SetValue(false);
		Frames = LoadFrames();
	}

	void SetUp() override
	{
		ASSERT_FALSE(Frames.empty()) << "no frames loaded from " << FramesPath;
		gbuffer::CurrentMode = gbuffer::Mode::Off;
	}

	void TearDown() override
	{
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		gbuffer::CurrentPlanes = gbuffer::Planes {};
	}

	template <typename F>
	static void ForEachCase(F &&fn)
	{
		for (const FrameRecord &frame : Frames) {
			for (const MaskType mask : { MaskType::Solid, MaskType::Transparent, MaskType::Left, MaskType::Right }) {
				if (!MaskValid(frame.tile, mask)) continue;
				for (const LightCase light : AllLights) {
					for (const ClipCase &clip : ClipCases) {
						fn(RenderCase { &frame, mask, light, &clip });
					}
				}
			}
		}
	}

	void Render(const Surface &out, const RenderCase &c) const
	{
		const Lightmap lightmap = lighting.lightmap(c.light);
		RenderTileFrame(out, lightmap, c.clip->position, c.frame->tile, c.frame->data.data(), c.frame->height, c.mask, lighting.tbl(c.light));
		FlushTileRenderCommands();
	}

	/** Classic render over pattern `variant`. */
	std::vector<uint8_t> RenderClassic(const Surface &out, const RenderCase &c, int variant) const
	{
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		FillPattern(out, variant);
		Render(out, c);
		return CopyPixels(out);
	}

	/** Source pixel of the frame under screen pixel (x, y), if the frame covers it. */
	static std::optional<uint8_t> Covered(const RenderCase &c, int x, int y)
	{
		const int col = x - c.clip->position.x;
		const int row = c.clip->position.y - y;
		if (col < 0 || col >= DunFrameWidth || row < 0 || row >= c.frame->height)
			return std::nullopt;
		return c.frame->at(row, col);
	}

	static std::vector<FrameRecord> Frames;
	TestLighting lighting;
};

std::vector<FrameRecord> DunRenderGoldenTest::Frames;

TEST_F(DunRenderGoldenTest, FixtureShape)
{
	EXPECT_EQ(Frames.size(), 7U);
	size_t triangles = 0;
	size_t trapezoids = 0;
	for (const FrameRecord &frame : Frames) {
		if (IsAnyOf(frame.tile, TileType::LeftTriangle, TileType::RightTriangle)) {
			EXPECT_EQ(frame.data.size(), ReencodedTriangleFrameSize) << frame.name;
			++triangles;
		}
		if (IsAnyOf(frame.tile, TileType::LeftTrapezoid, TileType::RightTrapezoid)) {
			EXPECT_EQ(frame.data.size(), ReencodedTrapezoidFrameSize) << frame.name;
			++trapezoids;
		}
		if (frame.tile == TileType::Square)
			EXPECT_EQ(frame.data.size(), static_cast<size_t>(DunFrameWidth * DunFrameHeight));
		for (const std::optional<uint8_t> &px : frame.pixels) {
			if (px.has_value())
				EXPECT_NE(*px, 0) << frame.name << ": fixture pixels must be non-zero";
		}
	}
	EXPECT_EQ(triangles, 2U);
	EXPECT_EQ(trapezoids, 2U);
}

// Every classic case matches the FNV-64 recorded in goldens.tsv (or records it).
TEST_F(DunRenderGoldenTest, ClassicByteIdentity)
{
	const char *recordPath = std::getenv("DUN_RENDER_GOLDEN_RECORD");
	std::map<std::string, uint64_t> goldens;
	if (recordPath == nullptr) {
		goldens = ReadGoldens();
		ASSERT_FALSE(goldens.empty()) << GoldensPath << " holds no cases; record with DUN_RENDER_GOLDEN_RECORD=<path>";
	}

	OwnedSurface out(OutSize, OutSize);
	std::vector<std::string> lines;
	size_t cases = 0;
	ForEachCase([&](const RenderCase &c) {
		++cases;
		FillPattern(out, 0);
		Render(out, c);
		const uint64_t hash = HashSurface(out);
		const std::string key = c.key();
		if (recordPath != nullptr) {
			lines.push_back(StrCat(key, "\t", HexHash(hash)));
			return;
		}
		const auto it = goldens.find(key);
		if (it == goldens.end()) {
			ADD_FAILURE() << "missing golden for " << key;
			return;
		}
		EXPECT_EQ(HexHash(hash), HexHash(it->second)) << key;
	});

	if (recordPath != nullptr) {
		std::ofstream outFile(recordPath);
		outFile << "# tile\tmask\tlight\tclip\tfnv1a64 of the 96x96 surface (dun_render_golden_test.cpp)\n";
		for (const std::string &line : lines)
			outFile << line << '\n';
		ASSERT_TRUE(outFile.good()) << "failed to write " << recordPath;
		SUCCEED() << "recorded " << lines.size() << " goldens to " << recordPath;
	} else {
		EXPECT_EQ(goldens.size(), cases) << "goldens.tsv has cases the test no longer produces";
	}
}

// Independent geometry reference: the decoded frame says which pixels are written and with what.
// Left/Right masks only pin the blended set on the Full clip case (see the fixture README: the
// clipped trapezoid prefix is pre-existing behaviour the goldens preserve).
TEST_F(DunRenderGoldenTest, ClassicMatchesReference)
{
	OwnedSurface out(OutSize, OutSize);
	ForEachCase([&](const RenderCase &c) {
		const std::vector<uint8_t> a = RenderClassic(out, c, 0);
		const std::vector<uint8_t> b = RenderClassic(out, c, 2);
		const uint8_t *tbl = lighting.tbl(c.light);
		const bool pinBlend = (c.mask != MaskType::Left && c.mask != MaskType::Right) || c.clip->name == "Full";
		for (int y = 0; y < OutSize; ++y) {
			for (int x = 0; x < OutSize; ++x) {
				const size_t i = (static_cast<size_t>(y) * OutSize) + x;
				const std::optional<uint8_t> src = Covered(c, x, y);
				if (!src.has_value()) {
					ASSERT_EQ(a[i], Pattern(x, y, 0)) << c.key() << " (" << x << "," << y << ") written outside the frame";
					continue;
				}
				uint8_t opaque = 0;
				uint8_t blendedA = 0;
				uint8_t blendedB = 0;
				const uint8_t dstA = Pattern(x, y, 0);
				const uint8_t dstB = Pattern(x, y, 2);
				switch (c.light) {
				case LightCase::FullyDark:
					opaque = 0;
					blendedA = paletteTransparencyLookup[0][dstA];
					blendedB = paletteTransparencyLookup[0][dstB];
					break;
				case LightCase::FullyLit:
					opaque = *src;
					blendedA = paletteTransparencyLookup[*src][dstA];
					blendedB = paletteTransparencyLookup[*src][dstB];
					break;
				case LightCase::PartiallyLit:
				case LightCase::HellLit:
					opaque = tbl[*src];
					blendedA = paletteTransparencyLookup[dstA][tbl[*src]];
					blendedB = paletteTransparencyLookup[dstB][tbl[*src]];
					break;
				}
				const bool isBlended = a[i] == blendedA && b[i] == blendedB;
				const bool isOpaque = a[i] == opaque && b[i] == opaque;
				ASSERT_TRUE(isBlended || isOpaque) << c.key() << " (" << x << "," << y << ") unexpected colour";
				if (!pinBlend) continue;
				const int col = x - c.clip->position.x;
				const int row = c.clip->position.y - y;
				const bool expectBlended = c.mask == MaskType::Transparent
				    || (c.mask == MaskType::Left && row >= 16 && col < 2 * (row - 16))
				    || (c.mask == MaskType::Right && row >= 16 && col >= 2 * (32 - row));
				ASSERT_EQ(isBlended, expectBlended) << c.key() << " (" << x << "," << y << ") blend mismatch";
			}
		}
	});
}

// With CurrentMode == Off (and StampUnlit) the classic dispatch must never reach a GBuffer* leaf:
// sentinel-filled planes stay untouched and the output is the classic output.
TEST_F(DunRenderGoldenTest, ModeOffNeverTouchesPlanes)
{
	OwnedSurface out(OutSize, OutSize);
	TestPlanes planes;
	planes.attach(out);
	SetContext(gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, 63, 32, gbuffer::FootKind::SW);
	for (const gbuffer::Mode mode : { gbuffer::Mode::Off, gbuffer::Mode::StampUnlit }) {
		ForEachCase([&](const RenderCase &c) {
			const std::vector<uint8_t> classic = RenderClassic(out, c, 0);
			gbuffer::CurrentMode = mode;
			FillPattern(out, 0);
			Render(out, c);
			gbuffer::CurrentMode = gbuffer::Mode::Off;
			EXPECT_EQ(CopyPixels(out), classic) << c.key();
		});
		EXPECT_TRUE(planes.untouched()) << "planes written with CurrentMode = " << static_cast<int>(mode);
	}
}

// Capture mode: colour output identical to classic; aux written <=> colour written; B per column
// from the foot table; blended pixels carry WallBlended; FullyDark rows are black and Unlit;
// triangle frames are Floor regardless of the context kind.
TEST_F(DunRenderGoldenTest, CaptureCoverageParity)
{
	OwnedSurface out(OutSize, OutSize);
	TestPlanes planes;
	ForEachCase([&](const RenderCase &c) {
		const std::vector<uint8_t> classicA = RenderClassic(out, c, 0);
		const std::vector<uint8_t> classicB = RenderClassic(out, c, 2);

		const TileContext ctx = ContextFor(c.frame->tile);
		const int anchorRow = c.clip->position.y;
		const int leftX = c.clip->position.x;
		planes.attach(out);
		SetContext(ctx.kind, ctx.normal, anchorRow, leftX, ctx.foot);
		const std::array<uint8_t, 32> table = gbuffer::CurrentTileBaseRow;
		FillPattern(out, 0);
		gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
		Render(out, c);
		gbuffer::CurrentMode = gbuffer::Mode::Off;

		ASSERT_EQ(CopyPixels(out), classicA) << c.key() << ": capture colour output differs from classic";

		const bool dark = c.light == LightCase::FullyDark;
		const bool triangle = IsAnyOf(c.frame->tile, TileType::LeftTriangle, TileType::RightTriangle);
		const uint8_t auxOpaque = triangle ? gbuffer::ShadowByte : gbuffer::CurrentTileAux;
		const uint8_t auxBlended = triangle ? gbuffer::ShadowByte : gbuffer::CurrentTileAuxBlended;
		for (int y = 0; y < OutSize; ++y) {
			for (int x = 0; x < OutSize; ++x) {
				const size_t i = (static_cast<size_t>(y) * OutSize) + x;
				const size_t off = TestPlanes::offset(out, x, y);
				const bool covered = Covered(c, x, y).has_value();
				ASSERT_EQ(planes.aux[off] != Sentinel, covered) << c.key() << " (" << x << "," << y << ") aux coverage";
				if (!covered) {
					ASSERT_EQ(planes.base[off], Sentinel) << c.key() << " (" << x << "," << y << ") B written outside the frame";
					continue;
				}
				const bool blended = classicA[i] != classicB[i];
				if (dark) {
					ASSERT_EQ(planes.aux[off], gbuffer::UnlitByte) << c.key() << " (" << x << "," << y << ")";
					ASSERT_EQ(planes.base[off], Sentinel) << c.key() << " (" << x << "," << y << ") GBufferDark must not write B";
					if (!blended)
						ASSERT_EQ(classicA[i], 0) << c.key() << " (" << x << "," << y << ") dark pixel not black";
					continue;
				}
				ASSERT_EQ(planes.aux[off], blended ? auxBlended : auxOpaque) << c.key() << " (" << x << "," << y << ") aux byte";
				const uint8_t expectedBase = triangle ? static_cast<uint8_t>(anchorRow & 0xFF) : table[x - leftX];
				ASSERT_EQ(planes.base[off], expectedBase) << c.key() << " (" << x << "," << y << ") B byte";
			}
		}
	});
}

// The lower diagonal edge of a wall trapezoid reads h = (B - y) & 0xFF == 0 for every row.
TEST_F(DunRenderGoldenTest, WallDiagonalEdgeHeightZero)
{
	OwnedSurface out(OutSize, OutSize);
	TestPlanes planes;
	const ClipCase &full = ClipCases[0];
	for (const FrameRecord &frame : Frames) {
		if (!IsAnyOf(frame.tile, TileType::LeftTrapezoid, TileType::RightTrapezoid)) continue;
		const bool left = frame.tile == TileType::LeftTrapezoid;
		const RenderCase c { &frame, MaskType::Solid, LightCase::FullyLit, &full };
		planes.attach(out);
		SetContext(left ? gbuffer::Kind::WallLeft : gbuffer::Kind::WallRight,
		    left ? gbuffer::NormalWallLeft : gbuffer::NormalWallRight, full.position.y, full.position.x,
		    left ? gbuffer::FootKind::SW : gbuffer::FootKind::SE);
		FillPattern(out, 0);
		gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
		Render(out, c);
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		for (int row = 0; row < 16; ++row) {
			const int y = full.position.y - row;
			// The two edge columns of this row of the lower triangle.
			const int edge0 = left ? 30 - (2 * row) : 2 * row;
			for (int col = edge0; col < edge0 + 2; ++col) {
				const int x = full.position.x + col;
				const uint8_t b = planes.base[TestPlanes::offset(out, x, y)];
				EXPECT_EQ((b - y) & 0xFF, 0) << frame.name << " row " << row << " col " << col;
			}
			// One column pair inward the height is one row.
			const int inner = left ? edge0 + 2 : edge0 - 1;
			if (inner >= 0 && inner < 32 && row < 15) {
				const uint8_t b = planes.base[TestPlanes::offset(out, full.position.x + inner, y)];
				EXPECT_EQ((b - y) & 0xFF, 1) << frame.name << " row " << row << " inner col " << inner;
			}
		}
	}
}

// Each queued command carries its own context; the flush restores it before rendering.
TEST_F(DunRenderGoldenTest, CommandsCarryTheirOwnContext)
{
	OwnedSurface out(OutSize, OutSize);
	TestPlanes planes;
	const FrameRecord *leftTrap = nullptr;
	const FrameRecord *rightTrap = nullptr;
	for (const FrameRecord &frame : Frames) {
		if (frame.tile == TileType::LeftTrapezoid) leftTrap = &frame;
		if (frame.tile == TileType::RightTrapezoid) rightTrap = &frame;
	}
	ASSERT_NE(leftTrap, nullptr);
	ASSERT_NE(rightTrap, nullptr);
	const Lightmap lightmap = lighting.lightmap(LightCase::FullyLit);
	const uint8_t *tbl = lighting.tbl(LightCase::FullyLit);

	planes.attach(out);
	FillPattern(out, 0);
	gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	const Point posA { 32, 63 };
	const Point posB { 0, 95 };
	SetContext(gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, posA.y, posA.x, gbuffer::FootKind::SW);
	RenderTileFrame(out, lightmap, posA, leftTrap->tile, leftTrap->data.data(), leftTrap->height, MaskType::Transparent, tbl);
	SetContext(gbuffer::Kind::WallRight, gbuffer::NormalWallRight, posB.y, posB.x, gbuffer::FootKind::SE);
	RenderTileFrame(out, lightmap, posB, rightTrap->tile, rightTrap->data.data(), rightTrap->height, MaskType::Solid, tbl);
	// Clobber the globals: the flush must not use them.
	SetContext(gbuffer::Kind::Unlit, 0, 0, 90, gbuffer::FootKind::None);
	FlushTileRenderCommands();
	gbuffer::CurrentMode = gbuffer::Mode::Off;

	std::array<uint8_t, 32> tableA {};
	std::array<uint8_t, 32> tableB {};
	gbuffer::BuildBaseRow(tableA, posA.y, gbuffer::FootKind::SW);
	gbuffer::BuildBaseRow(tableB, posB.y, gbuffer::FootKind::SE);
	for (int row = 0; row < DunFrameHeight; ++row) {
		for (int col = 0; col < DunFrameWidth; ++col) {
			if (leftTrap->at(row, col).has_value()) {
				const size_t off = TestPlanes::offset(out, posA.x + col, posA.y - row);
				EXPECT_EQ(planes.aux[off], gbuffer::PackAux(gbuffer::Kind::WallBlended, gbuffer::NormalWallLeft)) << "A " << row << "," << col;
				EXPECT_EQ(planes.base[off], tableA[col]) << "A " << row << "," << col;
			}
			if (rightTrap->at(row, col).has_value()) {
				const size_t off = TestPlanes::offset(out, posB.x + col, posB.y - row);
				EXPECT_EQ(planes.aux[off], gbuffer::PackAux(gbuffer::Kind::WallRight, gbuffer::NormalWallRight)) << "B " << row << "," << col;
				EXPECT_EQ(planes.base[off], tableB[col]) << "B " << row << "," << col;
			}
		}
	}
}

// world_draw_black_tile stamps UnlitByte over exactly the pixels it blackens, for every clip.
TEST_F(DunRenderGoldenTest, BlackTileStampsUnlit)
{
	OwnedSurface out(OutSize, OutSize);
	TestPlanes planes;
	constexpr Point Positions[] = { { 16, 63 }, { -20, 63 }, { 50, 63 }, { 16, 20 }, { 16, 110 }, { -40, 10 }, { 60, 115 }, { -70, 63 }, { 90, 63 } };
	for (const Point &pos : Positions) {
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		FillPattern(out, 0);
		world_draw_black_tile(out, pos.x, pos.y);
		const std::vector<uint8_t> classic = CopyPixels(out);

		planes.attach(out);
		SetContext(gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, pos.y, pos.x, gbuffer::FootKind::SW);
		FillPattern(out, 0);
		gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
		world_draw_black_tile(out, pos.x, pos.y);
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		ASSERT_EQ(CopyPixels(out), classic) << "black tile at " << pos.x << "," << pos.y;

		size_t covered = 0;
		for (int y = 0; y < OutSize; ++y) {
			for (int x = 0; x < OutSize; ++x) {
				const bool black = classic[(static_cast<size_t>(y) * OutSize) + x] == 0;
				const size_t off = TestPlanes::offset(out, x, y);
				ASSERT_EQ(planes.aux[off] != Sentinel, black) << "black tile at " << pos.x << "," << pos.y << " (" << x << "," << y << ")";
				if (black) {
					++covered;
					ASSERT_EQ(planes.aux[off], gbuffer::UnlitByte) << "(" << x << "," << y << ")";
				}
				ASSERT_EQ(planes.base[off], Sentinel) << "black tile wrote B at (" << x << "," << y << ")";
			}
		}
		if (pos.x == 16 && pos.y == 63)
			EXPECT_EQ(covered, 2U * 512U) << "full diamond is two 512-pixel triangles";
		else if (pos.x == -70)
			EXPECT_EQ(covered, 0U) << "diamond entirely left of the surface draws nothing";
		else
			EXPECT_GT(covered, 0U);
	}
}

} // namespace
} // namespace devilution
