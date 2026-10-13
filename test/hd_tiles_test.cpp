#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/render/hd_tiles_core.hpp"

namespace devilution {
namespace {

using hd_tiles::DynamicPlane;
using hd_tiles::Layer;
using hd_tiles::Manifest;
using hd_tiles::ParseManifest;
using hd_tiles::Parts;

constexpr char Sample[] = "\xEF\xBB\xBF# DamnationX HD tiles\n"
                          "scale 2\n"
                          "atlas 1 tiles-town-1.webp 4096 1344  # short page\r\n"
                          "atlas 0 hd/tiles-town-0.webp 4096 4096\n"
                          "\n"
                          "at 46 46 468 floor 0 0 0 128 62 0 -60\n"
                          "at 46 46 468 cell 0 130 0 100 64 10 -126\n"
                          "at 46 46 468 cell 1 0 0 120 200 4 -510\n"
                          "at 46 46 999 cell 0 0 70 16 16 0 -2\n"
                          "piece 468\tcell\t1 200 0 64 64 0 -62\n"
                          "piece 3 floor 1 0 300 128 62 0 -60\n"
                          "special 7 1 300 0 88 224 0 -236\n"
                          "at_special 10 12 2 0 400 0 50 100 -4 -198\n";

TEST(HdTilesManifest, ParsesPagesAndEntries)
{
	const auto manifest = ParseManifest(Sample, "tiles-town.txt");
	ASSERT_TRUE(manifest.has_value()) << manifest.error();
	EXPECT_EQ(manifest->scale, 2);
	ASSERT_EQ(manifest->pages.size(), 2U);
	EXPECT_EQ(manifest->pages[0].path, "hd\\tiles-town-0.webp");
	EXPECT_EQ(manifest->pages[1].path, "hd\\tiles-town-1.webp");
	EXPECT_EQ(manifest->pages[1].height, 1344);
	EXPECT_EQ(manifest->maxPageHeight(), 4096);
	EXPECT_EQ(manifest->maxPageWidth(), 4096);
	EXPECT_EQ(manifest->images.size(), 8U);

	const Parts floor = manifest->findPiece(46, 46, 468, Layer::Floor);
	ASSERT_EQ(floor.count, 1U);
	const hd_tiles::Image &floorImage = manifest->images[floor.first];
	EXPECT_EQ(floorImage.page, 0);
	EXPECT_EQ(floorImage.width, 128);
	EXPECT_EQ(floorImage.height, 62);
	EXPECT_EQ(floorImage.dy, -60);

	// A repeated key is one image in several parts, contiguous and in file order.
	const Parts cell = manifest->findPiece(46, 46, 468, Layer::Cell);
	ASSERT_EQ(cell.count, 2U);
	EXPECT_EQ(manifest->images[cell.first].u, 130);
	EXPECT_EQ(manifest->images[cell.first].dx, 10);
	EXPECT_EQ(manifest->images[cell.first + 1].page, 1);
	EXPECT_EQ(manifest->images[cell.first + 1].dy, -510);
}

TEST(HdTilesManifest, PositionalEntriesWinAndFollowThePiece)
{
	const auto manifest = ParseManifest(Sample, "tiles-town.txt");
	ASSERT_TRUE(manifest.has_value()) << manifest.error();
	// Same piece elsewhere: the generic entry.
	const Parts generic = manifest->findPiece(10, 10, 468, Layer::Cell);
	ASSERT_EQ(generic.count, 1U);
	EXPECT_EQ(manifest->images[generic.first].u, 200);
	// The position holds another piece (open/closed variant): its own entry, not the 468 one.
	const Parts variant = manifest->findPiece(46, 46, 999, Layer::Cell);
	ASSERT_EQ(variant.count, 1U);
	EXPECT_EQ(manifest->images[variant.first].v, 70);
	EXPECT_TRUE(manifest->findPiece(46, 46, 999, Layer::Floor).empty());
	// No floor entry for 468 except at (46, 46); generic floor of piece 3 anywhere.
	EXPECT_TRUE(manifest->findPiece(0, 0, 468, Layer::Floor).empty());
	EXPECT_FALSE(manifest->findPiece(0, 0, 3, Layer::Floor).empty());
	EXPECT_TRUE(manifest->findPiece(0, 0, 3, Layer::Cell).empty());
	EXPECT_TRUE(manifest->findPiece(0, 0, 4, Layer::Floor).empty());
	EXPECT_TRUE(manifest->findPiece(0, 0, 60000, Layer::Cell).empty());
}

TEST(HdTilesManifest, FindsSpecials)
{
	const auto manifest = ParseManifest(Sample, "tiles-town.txt");
	ASSERT_TRUE(manifest.has_value()) << manifest.error();
	const Parts arch = manifest->findSpecial(5, 5, 7);
	ASSERT_EQ(arch.count, 1U);
	EXPECT_EQ(manifest->images[arch.first].height, 224);
	const Parts crown = manifest->findSpecial(10, 12, 2);
	ASSERT_EQ(crown.count, 1U);
	EXPECT_EQ(manifest->images[crown.first].dx, -4);
	EXPECT_TRUE(manifest->findSpecial(10, 13, 2).empty());
	EXPECT_TRUE(manifest->findSpecial(0, 0, 1).empty());
}

TEST(HdTilesManifest, DenseGridAnswersLikeTheHashMaps)
{
	const auto manifest = ParseManifest(Sample, "tiles-town.txt");
	ASSERT_TRUE(manifest.has_value()) << manifest.error();
	ASSERT_GT(manifest->gridWidth, 0);
	Manifest hashed = *manifest;
	hashed.gridWidth = 0; // the hash map path the grid replaces
	const auto same = [](Parts a, Parts b) { return a.first == b.first && a.count == b.count; };
	for (int x = -2; x < 64; ++x) {
		for (int y = -2; y < 64; ++y) {
			for (const int piece : { 0, 3, 4, 468, 999, 65535, 70000 }) {
				for (const Layer layer : { Layer::Floor, Layer::Cell })
					EXPECT_TRUE(same(manifest->findPiece(x, y, piece, layer), hashed.findPiece(x, y, piece, layer))) << x << ' ' << y << ' ' << piece;
			}
			for (const int frame : { 0, 1, 2, 7, 8 })
				EXPECT_TRUE(same(manifest->findSpecial(x, y, frame), hashed.findSpecial(x, y, frame))) << x << ' ' << y << ' ' << frame;
		}
	}
}

TEST(HdTilesManifest, FarPositionsKeepTheHashMaps)
{
	const auto manifest = ParseManifest("scale 2\n"
	                                    "atlas 0 a.webp 64 64\n"
	                                    "at 5000 3 7 cell 0 0 0 8 8 0 0\n"
	                                    "at 3 4 7 floor 0 8 0 8 8 0 0\n",
	    "far.txt");
	ASSERT_TRUE(manifest.has_value()) << manifest.error();
	EXPECT_EQ(manifest->gridWidth, 0);
	EXPECT_EQ(manifest->findPiece(5000, 3, 7, Layer::Cell).count, 1U);
	EXPECT_EQ(manifest->findPiece(3, 4, 7, Layer::Floor).count, 1U);
	EXPECT_TRUE(manifest->findPiece(3, 4, 7, Layer::Cell).empty());
}

TEST(HdTilesManifest, RejectsMalformedInput)
{
	const char *const invalid[] = {
		"",                                                                      // nothing
		"atlas 0 a.webp 4096 4096\n",                                            // no scale
		"scale 2\n",                                                             // no pages
		"scale 5\natlas 0 a.webp 64 64\n",                                       // scale out of range
		"scale 2\nscale 2\natlas 0 a.webp 64 64\n",                              // scale twice
		"scale 2\natlas 1 a.webp 64 64\n",                                       // page 0 missing
		"scale 2\natlas 0 a.webp 64 64\natlas 0 b.webp 64 64\n",                 // page twice
		"scale 2\natlas 0 a.webp 8192 64\n",                                     // page too wide
		"scale 2\natlas 0 a.webp 64 64\npiece 1 wall 0 0 0 8 8 0 0\n",           // bad layer
		"scale 2\natlas 0 a.webp 64 64\npiece 1 cell 0 0 0 8 8 0\n",             // missing field
		"scale 2\natlas 0 a.webp 64 64\npiece 1 cell 1 0 0 8 8 0 0\n",           // undefined page
		"scale 2\natlas 0 a.webp 64 64\npiece 1 cell 0 60 0 8 8 0 0\n",          // outside the page
		"scale 2\natlas 0 a.webp 64 64\npiece 1 cell 0 0 0 0 8 0 0\n",           // empty image
		"scale 2\natlas 0 a.webp 64 64\npiece -1 cell 0 0 0 8 8 0 0\n",          // negative piece
		"scale 2\natlas 0 a.webp 64 64\nat 1 2 3 floor 0 0 0 8 8 0 x\n",         // not a number
		"scale 2\natlas 0 a.webp 64 64\nwall 1 0 0 0 8 8 0 0\n",                 // unknown directive
		"scale 2\natlas 0 a.webp 64 64\nspecial 1 0 0 0 8 8 0 40000\n",          // offset out of range
	};
	for (const char *text : invalid) {
		const auto manifest = ParseManifest(text, "tiles-test.txt");
		EXPECT_FALSE(manifest.has_value()) << "accepted: " << text;
		if (!manifest.has_value())
			EXPECT_NE(manifest.error().find("tiles-test.txt"), std::string::npos) << manifest.error();
	}
}

TEST(HdTilesManifest, ErrorNamesTheLine)
{
	const auto manifest = ParseManifest("scale 2\natlas 0 a.webp 64 64\n# ok\npiece 1 cell 0 60 0 8 8 0 0\n", "tiles-x.txt");
	ASSERT_FALSE(manifest.has_value());
	EXPECT_NE(manifest.error().find("line 4"), std::string::npos) << manifest.error();
}

class DynamicPlaneTest : public ::testing::Test {
protected:
	static constexpr int Width = 16;
	static constexpr int Height = 8;
	static constexpr int Pitch = 20;

	DynamicPlaneTest()
	    : pixels(static_cast<size_t>(Pitch) * Height, 7)
	{
		plane.reset(Width, Height);
	}

	void write(int x, int y, uint8_t value) { pixels[static_cast<size_t>(y) * Pitch + x] = value; }

	std::vector<uint8_t> pixels;
	DynamicPlane plane;
};

TEST_F(DynamicPlaneTest, SectionMarksChangedPixelsWithTheInstanceCount)
{
	plane.beginSection(pixels.data(), Pitch, { 2, 1, 6, 4 }, 5);
	EXPECT_TRUE(plane.sectionOpen());
	write(3, 2, 9);  // changed inside the section
	write(4, 2, 7);  // same index: invisible
	write(12, 2, 9); // outside the section
	plane.endSection(pixels.data(), Pitch);
	EXPECT_FALSE(plane.sectionOpen());
	EXPECT_EQ(plane.at(3, 2), 5);
	EXPECT_EQ(plane.at(4, 2), 0);
	EXPECT_EQ(plane.at(12, 2), 0);
	// Instance i is hidden iff plane > i: instances 0..4 are hidden at (3, 2), instance 5 (recorded after) is not.
}

TEST_F(DynamicPlaneTest, LaterSectionsRaiseButNeverLowerTheMark)
{
	plane.beginSection(pixels.data(), Pitch, { 0, 0, Width, Height }, 9);
	write(1, 1, 3);
	plane.endSection(pixels.data(), Pitch);
	plane.beginSection(pixels.data(), Pitch, { 0, 0, Width, Height }, 4);
	write(1, 1, 4);
	write(2, 1, 4);
	plane.endSection(pixels.data(), Pitch);
	EXPECT_EQ(plane.at(1, 1), 9);
	EXPECT_EQ(plane.at(2, 1), 4);
}

TEST_F(DynamicPlaneTest, SectionBeforeAnyInstanceAndClippingAreHarmless)
{
	plane.beginSection(pixels.data(), Pitch, { -5, -5, 8, 8 }, 0);
	write(0, 0, 1);
	plane.endSection(pixels.data(), Pitch);
	EXPECT_EQ(plane.at(0, 0), 0);
	plane.beginSection(pixels.data(), Pitch, { 40, 40, 8, 8 }, 3); // fully outside: no section
	EXPECT_FALSE(plane.sectionOpen());
	plane.endSection(pixels.data(), Pitch);
	// Beginning a section while one is open ends the first one with its own count.
	plane.beginSection(pixels.data(), Pitch, { 0, 0, 4, 4 }, 2);
	write(1, 0, 8);
	plane.beginSection(pixels.data(), Pitch, { 0, 0, 4, 4 }, 6);
	plane.endSection(pixels.data(), Pitch);
	EXPECT_EQ(plane.at(1, 0), 2);
}

TEST_F(DynamicPlaneTest, FrameFinishHidesEverythingDrawnAfterTheWorldAndRecomputesOnRepresent)
{
	plane.beginSection(pixels.data(), Pitch, { 0, 0, Width, Height }, 3);
	write(5, 5, 1);
	plane.endSection(pixels.data(), Pitch);
	EXPECT_FALSE(plane.finishFrame(pixels.data(), Pitch)); // no snapshot yet
	plane.snapshotFrame(pixels.data(), Pitch, { 0, 0, Width, Height - 2 });
	write(6, 1, 2); // UI over the world
	write(6, 7, 2); // outside the snapshot rectangle (panel rows): ignored
	ASSERT_TRUE(plane.finishFrame(pixels.data(), Pitch));
	EXPECT_EQ(plane.at(6, 1), DynamicPlane::HiddenAll);
	EXPECT_EQ(plane.at(6, 7), 0);
	EXPECT_EQ(plane.at(5, 5), 3);
	// The cursor moves away on a re-present: the old mark goes, the world marks stay.
	write(6, 1, 7);
	write(9, 1, 2);
	ASSERT_TRUE(plane.finishFrame(pixels.data(), Pitch));
	EXPECT_EQ(plane.at(6, 1), 0);
	EXPECT_EQ(plane.at(9, 1), DynamicPlane::HiddenAll);
	EXPECT_EQ(plane.at(5, 5), 3);
	plane.reset(Width, Height);
	EXPECT_EQ(plane.at(5, 5), 0);
	EXPECT_FALSE(plane.finishFrame(pixels.data(), Pitch));
}

TEST_F(DynamicPlaneTest, ZoomDoublesThePlaneLikeTheBackBuffer)
{
	// Marks at source (1, 0) and (2, 1) of the half-size world.
	plane.beginSection(pixels.data(), Pitch, { 0, 0, Width, Height }, 4);
	write(1, 0, 1);
	write(2, 1, 1);
	plane.endSection(pixels.data(), Pitch);
	// 12 px wide zoomed viewport at x = 2, 6 rows: source (x, y) -> (2 + 2x .. 3 + 2x, 2y .. 2y + 1).
	plane.zoomInPlace(12, 2, 6);
	for (int y = 0; y < 6; ++y) {
		for (int x = 0; x < Width; ++x) {
			const bool marked = (x >= 4 && x <= 5 && y <= 1) || (x >= 6 && x <= 7 && y >= 2 && y <= 3);
			EXPECT_EQ(plane.at(x, y), marked ? 4 : 0) << x << "," << y;
		}
	}
	// Rows below the zoomed viewport keep their values (none here).
	EXPECT_EQ(plane.at(5, 7), 0);
}

// ---- Classic colour match (hybrid tile shader tileClassicTint) ----

float SrgbByteToLinear(int v)
{
	const float c = static_cast<float>(v) / 255.0F;
	return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

/** DynamicPlane's contract restated pixel by pixel (no row spans, no word compares): the randomized test's reference. */
class ReferencePlane {
public:
	ReferencePlane(int width, int height)
	    : width_(width)
	    , height_(height)
	    , plane_(static_cast<size_t>(width) * height, 0)
	{
	}
	void begin(const std::vector<uint8_t> &pixels, hd_tiles::PixelRect rect, uint16_t value)
	{
		rect_ = clip(rect);
		value_ = value;
		snapshot_ = pixels;
	}
	void end(const std::vector<uint8_t> &pixels)
	{
		if (value_ != 0)
			mark(pixels, snapshot_, rect_, value_, plane_);
	}
	void snapshotFrame(const std::vector<uint8_t> &pixels, hd_tiles::PixelRect rect)
	{
		frameRect_ = clip(rect);
		frame_ = pixels;
		world_ = plane_;
	}
	void finishFrame(const std::vector<uint8_t> &pixels)
	{
		plane_ = world_;
		mark(pixels, frame_, frameRect_, DynamicPlane::HiddenAll, plane_);
	}
	[[nodiscard]] uint16_t at(int x, int y) const { return plane_[static_cast<size_t>(y) * width_ + x]; }

private:
	hd_tiles::PixelRect clip(hd_tiles::PixelRect r) const
	{
		const int left = std::max(r.x, 0);
		const int top = std::max(r.y, 0);
		const int right = std::min(r.x + r.width, width_);
		const int bottom = std::min(r.y + r.height, height_);
		return { left, top, std::max(right - left, 0), std::max(bottom - top, 0) };
	}
	void mark(const std::vector<uint8_t> &now, const std::vector<uint8_t> &before, hd_tiles::PixelRect rect, uint16_t value, std::vector<uint16_t> &plane) const
	{
		for (int y = rect.y; y < rect.y + rect.height; ++y) {
			for (int x = rect.x; x < rect.x + rect.width; ++x) {
				const size_t i = static_cast<size_t>(y) * width_ + x;
				if (now[i] != before[i] && plane[i] < value)
					plane[i] = value;
			}
		}
	}
	int width_;
	int height_;
	std::vector<uint16_t> plane_;
	std::vector<uint16_t> world_;
	std::vector<uint8_t> snapshot_;
	std::vector<uint8_t> frame_;
	hd_tiles::PixelRect rect_;
	hd_tiles::PixelRect frameRect_;
	uint16_t value_ = 0;
};

TEST(HdTilesDynamicPlane, MatchesThePixelByPixelReference)
{
	constexpr int Width = 301; // odd: rows end off the 8 and 32 byte steps
	constexpr int Height = 97;
	std::mt19937 rng(1234);
	const auto randomInt = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
	std::vector<uint8_t> pixels(static_cast<size_t>(Width) * Height);
	for (uint8_t &p : pixels)
		p = static_cast<uint8_t>(randomInt(0, 255));
	// Draws: short runs (characters, missiles), single pixels, and runs of the colour already there (invisible to the diff).
	const auto draw = [&]() {
		const int runs = randomInt(0, 12);
		for (int r = 0; r < runs; ++r) {
			const int y = randomInt(0, Height - 1);
			const int x = randomInt(0, Width - 1);
			const int length = std::min(randomInt(1, 70), Width - x);
			const bool same = randomInt(0, 4) == 0;
			for (int i = 0; i < length; ++i) {
				uint8_t &p = pixels[static_cast<size_t>(y) * Width + x + i];
				if (!same)
					p = static_cast<uint8_t>(randomInt(0, 255));
			}
		}
	};
	for (int pass = 0; pass < 40; ++pass) {
		DynamicPlane plane;
		plane.reset(Width, Height);
		ReferencePlane reference(Width, Height);
		uint16_t instances = 0;
		for (int section = 0; section < 30; ++section) {
			instances = static_cast<uint16_t>(instances + randomInt(0, 3));
			const hd_tiles::PixelRect rect { randomInt(-40, Width), randomInt(-40, Height), randomInt(0, 160), randomInt(0, 120) };
			plane.beginSection(pixels.data(), Width, rect, instances);
			reference.begin(pixels, rect, instances);
			draw();
			plane.endSection(pixels.data(), Width);
			reference.end(pixels);
			draw(); // a tile drawn between sections: not marked
		}
		const hd_tiles::PixelRect viewport { 0, 0, Width, Height - 20 };
		plane.snapshotFrame(pixels.data(), Width, viewport);
		reference.snapshotFrame(pixels, viewport);
		for (int present = 0; present < 3; ++present) {
			draw();
			ASSERT_TRUE(plane.finishFrame(pixels.data(), Width));
			reference.finishFrame(pixels);
			for (int y = 0; y < Height; ++y) {
				const DynamicPlane::RowSpan span = plane.rowSpans()[y];
				for (int x = 0; x < Width; ++x) {
					ASSERT_EQ(plane.at(x, y), reference.at(x, y)) << "pass " << pass << " present " << present << " at " << x << ',' << y;
					if (plane.at(x, y) != 0)
						ASSERT_TRUE(x >= span.first && x < span.end) << x << ',' << y;
				}
			}
		}
	}
}

TEST(HdTilesClassicTint, NeutralForGreyOrNoSamples)
{
	EXPECT_EQ(hd_tiles::ClassicTint({}), (std::array<float, 3> { 1.0F, 1.0F, 1.0F }));
	const std::vector<std::array<float, 3>> grey { { 0.2F, 0.2F, 0.2F }, { 0.4F, 0.4F, 0.4F } };
	const std::array<float, 3> tint = hd_tiles::ClassicTint(grey);
	for (float v : tint)
		EXPECT_NEAR(v, 1.0F, 1e-5F);
	// Fully dark samples carry no hue: no tint.
	const std::vector<std::array<float, 3>> dark { { 0.001F, 0.0F, 0.0F } };
	EXPECT_EQ(hd_tiles::ClassicTint(dark), (std::array<float, 3> { 1.0F, 1.0F, 1.0F }));
}

TEST(HdTilesClassicTint, TintHasUnitLuminanceAndKeepsTheAlbedoLuminance)
{
	// Torch-lit classic Cathedral floor (measured sRGB 60, 41, 38) over grey HD stone (69, 63, 57).
	const std::vector<std::array<float, 3>> classic { { SrgbByteToLinear(60), SrgbByteToLinear(41), SrgbByteToLinear(38) } };
	const std::array<float, 3> tint = hd_tiles::ClassicTint(classic);
	EXPECT_NEAR(hd_tiles::LinearLuminance(tint), 1.0F, 1e-4F);
	EXPECT_GT(tint[0], 1.3F);
	EXPECT_LT(tint[2], 0.8F);
	const std::array<float, 3> albedo { SrgbByteToLinear(69), SrgbByteToLinear(63), SrgbByteToLinear(57) };
	const std::array<float, 3> tinted = hd_tiles::ApplyClassicTint(albedo, tint, 1.0F);
	// The luminance is kept exactly (tint and the deviation term both have zero luminance change) ...
	EXPECT_NEAR(hd_tiles::LinearLuminance(tinted), hd_tiles::LinearLuminance(albedo), 1e-5F);
	// ... and the chromaticity is the tint plus half the texel's own deviation from neutral.
	const float l = hd_tiles::LinearLuminance(albedo);
	for (size_t i = 0; i < 3; ++i)
		EXPECT_NEAR(tinted[i] / l, tint[i] + (albedo[i] / l - 1.0F) * hd_tiles::ClassicTintHdChromaKeep, 1e-5F);
	// Red now leads green by close to the classic ratio (60/41 in sRGB) instead of the grey art's 69/63.
	EXPECT_GT(tinted[0] / tinted[1], 1.8F);
}

TEST(HdTilesClassicTint, ClampsAndStrength)
{
	const std::vector<std::array<float, 3>> pureBlue { { 0.0F, 0.0F, 0.5F } };
	const std::array<float, 3> tint = hd_tiles::ClassicTint(pureBlue);
	EXPECT_FLOAT_EQ(tint[0], hd_tiles::ClassicTintMin);
	EXPECT_FLOAT_EQ(tint[2], hd_tiles::ClassicTintMax);
	const std::array<float, 3> albedo { 0.5F, 0.5F, 0.5F }; // neutral: no own deviation
	EXPECT_EQ(hd_tiles::ApplyClassicTint(albedo, tint, 0.0F), albedo);
	const std::array<float, 3> full = hd_tiles::ApplyClassicTint(albedo, tint, 1.0F);
	EXPECT_FLOAT_EQ(full[0], 0.2F);
	EXPECT_FLOAT_EQ(full[2], 1.0F); // 0.5 x 2, clamped to 1
	const std::array<float, 3> half = hd_tiles::ApplyClassicTint(albedo, tint, 0.5F);
	EXPECT_FLOAT_EQ(half[0], 0.35F);
	EXPECT_FLOAT_EQ(half[2], 0.75F);
	EXPECT_EQ(hd_tiles::ApplyClassicTint({ 0.0F, 0.0F, 0.0F }, tint, 1.0F), (std::array<float, 3> { 0.0F, 0.0F, 0.0F }));
}

TEST(HdTilesClassicTint, OnlyTheCathedralIsMatched)
{
	EXPECT_EQ(hd_tiles::ClassicTintStrength(0), 0.0F); // town keeps its painted HD colour
	EXPECT_EQ(hd_tiles::ClassicTintStrength(1), 1.0F);
	EXPECT_EQ(hd_tiles::ClassicTintStrength(-1), 0.0F);
}

// RedBack (pause / death screen) recolours the whole view after the world pass. The trackers must see the frame as if
// the recolour had not happened, but still see what was drawn over the world before it (a panel) and after it (text).
TEST(HdTilesRecolourPass, TrackersSeeTheFrameWithoutTheRecolour)
{
	constexpr int W = 8;
	constexpr int H = 4;
	constexpr int Pitch = 10; // a back buffer row is wider than the frame
	std::vector<uint8_t> frame(Pitch * H, 7);
	DynamicPlane plane;
	plane.reset(W, H);
	plane.snapshotFrame(frame.data(), Pitch, { 0, 0, W, H }); // end of the world content: all 7
	frame[1 * Pitch + 1] = 50;                                // a panel pixel drawn before the recolour

	hd_tiles::RecolourPass pass;
	pass.begin(frame.data(), Pitch, W, H, { 0, 0, W, 3 }); // the view: rows 0..2, row 3 is the control panel
	for (int y = 0; y < 3; ++y)
		for (int x = 0; x < W; ++x)
			frame[y * Pitch + x] = static_cast<uint8_t>(frame[y * Pitch + x] + 100); // the pause TRN
	pass.end(frame.data(), Pitch);
	frame[2 * Pitch + 5] = 9; // the "Pause" text, drawn after the recolour
	frame[3 * Pitch + 0] = 8; // the panel row, outside the recolour

	int pitch = 0;
	const uint8_t *tracked = pass.trackedFrame(frame.data(), Pitch, W, H, pitch);
	ASSERT_NE(tracked, frame.data());
	EXPECT_EQ(pitch, W);
	ASSERT_TRUE(plane.finishFrame(tracked, pitch));
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			const bool drawnOver = (x == 1 && y == 1) || (x == 5 && y == 2) || (x == 0 && y == 3);
			EXPECT_EQ(plane.at(x, y), drawnOver ? DynamicPlane::HiddenAll : 0) << x << "," << y;
		}
	}
	// The real frame keeps the recolour; only the trackers' copy undoes it.
	EXPECT_EQ(frame[0], 107);
	EXPECT_EQ(tracked[0], 7);
}

TEST(HdTilesRecolourPass, InactiveOrResetPassesThePixelsThrough)
{
	std::vector<uint8_t> frame(16, 3);
	hd_tiles::RecolourPass pass;
	int pitch = 0;
	EXPECT_EQ(pass.trackedFrame(frame.data(), 4, 4, 4, pitch), frame.data());
	EXPECT_EQ(pitch, 4);
	pass.begin(frame.data(), 4, 4, 4, { 0, 0, 4, 4 });
	EXPECT_EQ(pass.trackedFrame(frame.data(), 4, 4, 4, pitch), frame.data()); // begun, not ended
	pass.end(frame.data(), 4);
	EXPECT_TRUE(pass.active());
	pass.reset(); // a new world pass
	EXPECT_FALSE(pass.active());
	EXPECT_EQ(pass.trackedFrame(frame.data(), 4, 4, 4, pitch), frame.data());
}

TEST(HdTilesRecolourPass, SecondRecolourChainsOntoTheFirst)
{
	std::vector<uint8_t> frame(4, 1);
	hd_tiles::RecolourPass pass;
	const auto recolour = [&](uint8_t add) {
		pass.begin(frame.data(), 4, 4, 1, { 0, 0, 4, 1 });
		for (uint8_t &p : frame)
			p = static_cast<uint8_t>(p + add);
		pass.end(frame.data(), 4);
	};
	recolour(10);
	frame[2] = 60; // drawn between the two passes
	recolour(20);
	int pitch = 0;
	const uint8_t *tracked = pass.trackedFrame(frame.data(), 4, 4, 1, pitch);
	EXPECT_EQ(tracked[0], 1);
	EXPECT_EQ(tracked[1], 1);
	EXPECT_EQ(tracked[2], 60);
	EXPECT_EQ(tracked[3], 1);
}

} // namespace
} // namespace devilution
