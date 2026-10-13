/**
 * @file clx_normal_map_test.cpp
 *
 * Unit tests for the sprite normal-plane generator and cache.
 *
 * Synthetic frames are encoded in-test with the same clx_encode.hpp primitives SurfaceToClx uses; the committed
 * fixtures (disc.clx, ramp.clx) were produced by SurfaceToClx from the same kind of surfaces (see the fixture README).
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/clx_sprite.hpp"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/gbuffer.hpp"
#include "utils/clx_encode.hpp"
#include "utils/endian_write.hpp"

namespace devilution::clx_normal_map {
// Test seams defined in clx_normal_map.cpp (not part of the public header).
Scratch &SharedScratchForTesting();
bool GenerateCardNormalsForTesting(ClxSprite sprite, std::span<const SDL_Color, 256> logicalPalette, const Options &opts, bool mirrored, std::vector<float> &out, Scratch &scratch);
uint8_t QuantizeCardNormalForTesting(float cx, float cy, float cz);
} // namespace devilution::clx_normal_map

namespace devilution {
namespace {

using clx_normal_map::Options;
using clx_normal_map::Plane;

constexpr uint8_t Transparent = 255;

// ---- Palettes ----

float SrgbEncode(float linear)
{
	return linear <= 0.0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

float SrgbDecode(uint8_t v)
{
	const float c = static_cast<float>(v) / 255.0F;
	return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

float Luminance(const SDL_Color &c)
{
	return 0.2126F * SrgbDecode(c.r) + 0.7152F * SrgbDecode(c.g) + 0.0722F * SrgbDecode(c.b);
}

/**
 * Palette whose *linear* luminance is proportional to the index (so an index ramp is a uniform luminance gradient).
 * A grey ramp rounded to 8 bits is only linear to ~10 %; searching the red and blue channels around the grey
 * solution brings every entry within ~0.1 % of its target.
 */
std::array<SDL_Color, 256> LinearLuminancePalette()
{
	std::array<SDL_Color, 256> palette {};
	for (size_t i = 0; i < palette.size(); ++i) {
		const float target = static_cast<float>(i) / 255.0F;
		const int g0 = static_cast<int>(std::lround(255.0F * SrgbEncode(target)));
		SDL_Color best { 0, 0, 0, 255 };
		float bestErr = 1e9F;
		for (int g = std::max(0, g0 - 2); g <= std::min(255, g0 + 2); ++g) {
			for (int r = 0; r < 256; ++r) {
				// Solve the blue channel for the residual, then check the rounded value.
				const float residual = target - 0.2126F * SrgbDecode(static_cast<uint8_t>(r)) - 0.7152F * SrgbDecode(static_cast<uint8_t>(g));
				const float linB = residual / 0.0722F;
				if (linB < 0.0F || linB > 1.0F)
					continue;
				const int b0 = static_cast<int>(std::lround(255.0F * SrgbEncode(linB)));
				for (int b = std::max(0, b0 - 1); b <= std::min(255, b0 + 1); ++b) {
					const SDL_Color c { static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255 };
					const float err = std::fabs(Luminance(c) - target);
					if (err < bestErr) {
						bestErr = err;
						best = c;
					}
				}
			}
		}
		palette[i] = best;
	}
	return palette;
}

const std::array<SDL_Color, 256> &TestPalette()
{
	static const std::array<SDL_Color, 256> Palette = LinearLuminancePalette();
	return Palette;
}

std::span<const SDL_Color, 256> PaletteSpan()
{
	return std::span<const SDL_Color, 256>(TestPalette().data(), 256);
}

void LatchTestPalette()
{
	gbuffer::LatchInput in {};
	in.logicalPalette = TestPalette().data();
	gbuffer::LatchForFrame(in);
}

// ---- Synthetic frames ----

/** A top-down 8-bit image (like a Surface) of one or more vertically stacked frames. */
struct Image {
	unsigned width = 0;
	unsigned height = 0; // total height (numFrames * frameHeight)
	std::vector<uint8_t> pixels;

	Image(unsigned w, unsigned h)
	    : width(w)
	    , height(h)
	    , pixels(static_cast<size_t>(w) * h, Transparent)
	{
	}

	uint8_t &at(unsigned x, unsigned yTopDown) { return pixels[static_cast<size_t>(yTopDown) * width + x]; }
	[[nodiscard]] uint8_t at(unsigned x, unsigned yTopDown) const { return pixels[static_cast<size_t>(yTopDown) * width + x]; }
	/** Row from the feet (0 = bottom row of frame `frame`). */
	uint8_t &feet(unsigned x, unsigned yFromFeet, unsigned frameHeight, unsigned frame = 0)
	{
		return at(x, frame * frameHeight + frameHeight - 1 - yFromFeet);
	}
};

/** Mirrors SurfaceToClx (Source/utils/surface_to_clx.cpp) byte for byte, using the shared clx_encode.hpp primitives. */
std::vector<uint8_t> EncodeClxBytes(const Image &img, unsigned numFrames, uint8_t transparentColor)
{
	std::vector<uint8_t> clxData(4 * (2 + static_cast<size_t>(numFrames)));
	WriteLE32(clxData.data(), numFrames);
	const unsigned frameHeight = img.height / numFrames;
	const uint8_t *dataPtr = img.pixels.data();
	for (unsigned frame = 1; frame <= numFrames; ++frame) {
		WriteLE32(&clxData[4 * static_cast<size_t>(frame)], static_cast<uint32_t>(clxData.size()));
		const size_t frameHeaderPos = clxData.size();
		clxData.resize(clxData.size() + ClxFrameHeaderSize);
		WriteLE16(&clxData[frameHeaderPos], ClxFrameHeaderSize);
		WriteLE16(&clxData[frameHeaderPos + 2], static_cast<uint16_t>(img.width));
		WriteLE16(&clxData[frameHeaderPos + 4], static_cast<uint16_t>(frameHeight));
		unsigned transparentRunWidth = 0;
		for (unsigned line = 0; line != frameHeight; ++line) {
			const uint8_t *src = &dataPtr[(frameHeight - (line + 1)) * img.width];
			unsigned solidRunWidth = 0;
			for (const uint8_t *srcEnd = src + img.width; src != srcEnd; ++src) {
				if (*src == transparentColor) {
					if (solidRunWidth != 0) {
						AppendClxPixelsOrFillRun(src - transparentRunWidth - solidRunWidth, solidRunWidth, clxData);
						solidRunWidth = 0;
					}
					++transparentRunWidth;
				} else {
					AppendClxTransparentRun(transparentRunWidth, clxData);
					transparentRunWidth = 0;
					++solidRunWidth;
				}
			}
			if (solidRunWidth != 0)
				AppendClxPixelsOrFillRun(src - solidRunWidth, solidRunWidth, clxData);
		}
		AppendClxTransparentRun(transparentRunWidth, clxData);
		dataPtr += static_cast<size_t>(img.width) * frameHeight;
	}
	WriteLE32(&clxData[4 * (1 + static_cast<size_t>(numFrames))], static_cast<uint32_t>(clxData.size()));
	return clxData;
}

OwnedClxSpriteList EncodeClx(const Image &img, unsigned numFrames = 1)
{
	const std::vector<uint8_t> bytes = EncodeClxBytes(img, numFrames, Transparent);
	std::unique_ptr<uint8_t[]> data { new uint8_t[bytes.size()] };
	std::memcpy(data.get(), bytes.data(), bytes.size());
	return OwnedClxSpriteList { std::move(data) };
}

Image DiscImage(unsigned size, unsigned radius, uint8_t color)
{
	Image img(size, size);
	const float cx = static_cast<float>(size) / 2.0F;
	const float cy = static_cast<float>(size) / 2.0F;
	for (unsigned y = 0; y < size; ++y) {
		for (unsigned x = 0; x < size; ++x) {
			const float dx = static_cast<float>(x) + 0.5F - cx;
			const float dy = static_cast<float>(y) + 0.5F - cy;
			if (dx * dx + dy * dy <= static_cast<float>(radius) * radius)
				img.at(x, y) = color;
		}
	}
	return img;
}

/** `count` distinct small blob frames (each a different colour so content hashes never collide). */
Image BlobFrames(unsigned count, unsigned size)
{
	Image img(size, size * count);
	for (unsigned f = 0; f < count; ++f) {
		const auto color = static_cast<uint8_t>(1 + (f * 7) % 200);
		for (unsigned y = 2; y + 2 < size; ++y) {
			for (unsigned x = 2; x + 2 < size; ++x) {
				const int dx = static_cast<int>(x) - static_cast<int>(size / 2);
				const int dy = static_cast<int>(y) - static_cast<int>(size / 2);
				if (dx * dx + dy * dy <= static_cast<int>((size / 2 - 2) * (size / 2 - 2)))
					img.at(x, f * size + y) = static_cast<uint8_t>(color + ((x ^ y) & 3));
			}
		}
	}
	return img;
}

std::optional<OwnedClxSpriteList> LoadFixture(const char *name)
{
	const std::string path = std::string("test/fixtures/normal_map/") + name;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return std::nullopt;
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() < 12)
		return std::nullopt;
	std::unique_ptr<uint8_t[]> data { new uint8_t[bytes.size()] };
	std::memcpy(data.get(), bytes.data(), bytes.size());
	return OwnedClxSpriteList { std::move(data) };
}

// ---- Plane helpers ----

/** Byte at (x, yFromFeet), or nullopt when the column is outside the row's span. */
std::optional<uint8_t> PlaneByte(const Plane &plane, unsigned x, unsigned y)
{
	if (plane.rowOffset == nullptr || y >= plane.height)
		return std::nullopt;
	const uint32_t len = plane.rowOffset[y + 1] - plane.rowOffset[y];
	if (x < plane.rowX0[y] || x >= plane.rowX0[y] + len)
		return std::nullopt;
	return plane.data[plane.rowOffset[y] + (x - plane.rowX0[y])];
}

bool IsFlat(const Plane &plane)
{
	return plane.data == nullptr && plane.rowOffset == nullptr;
}

/** True when the real LUT is linked (a stub fills entries 4..31 with entry 3). */
bool LutIsReal()
{
	const auto &lut = gbuffer::NormalLut();
	return lut[4].x != lut[3].x || lut[4].y != lut[3].y || lut[4].z != lut[3].z;
}

Plane Generate(ClxSprite sprite, const Options &opts, bool mirrored, std::vector<uint8_t> &out)
{
	return clx_normal_map::GeneratePlane(sprite, PaletteSpan(), opts, mirrored, out, clx_normal_map::SharedScratchForTesting());
}

std::vector<float> CardNormals(ClxSprite sprite, const Options &opts, bool mirrored)
{
	std::vector<float> out;
	EXPECT_TRUE(clx_normal_map::GenerateCardNormalsForTesting(sprite, PaletteSpan(), opts, mirrored, out, clx_normal_map::SharedScratchForTesting()));
	return out;
}

/**
 * The fixtures' per-game-frame budget: 8 generations like the game, but no practical wall-clock cap. A 1500 us cap made
 * the lookups depend on the machine (one disc generation takes ~1 ms at -O0 with coverage and more under sanitizers or
 * QEMU), turning expected generations into flat-tier misses; MicrosecondBudgetQueuesOnceSpent covers the cap itself.
 */
constexpr uint32_t TestFrameBudget = 8;
constexpr uint32_t TestMicrosBudget = 1000000000;

class NormalMapTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		clx_normal_map::SetNormalPlaneProviderForTesting(nullptr);
		clx_normal_map::SetOptions(Options {});
		clx_normal_map::SetBudget(TestFrameBudget, TestMicrosBudget);
		clx_normal_map::SetCacheBudget(1024 * 1024);
		clx_normal_map::ResetFrameStats();
		LatchTestPalette();
	}

	void TearDown() override
	{
		clx_normal_map::SetNormalPlaneProviderForTesting(nullptr);
		clx_normal_map::ClearNormalPlaneCache();
	}
};

// =========================================================================================================
// Generator
// =========================================================================================================

TEST_F(NormalMapTest, DiscDomeNormalsPointOutward)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value()) << "test/fixtures/normal_map/disc.clx missing";
	const ClxSprite sprite = (*disc)[0];
	ASSERT_EQ(sprite.width(), 128);
	ASSERT_EQ(sprite.height(), 128);

	// The dome term alone (silhouetteWeight 1): the detail term of a uniform-colour disc is flat and would only
	// dilute the rim tilt in the blend.
	Options dome;
	dome.silhouetteWeight = 1.0F;
	const std::vector<float> normals = CardNormals(sprite, dome, false);
	const std::vector<float> blended = CardNormals(sprite, Options {}, false);
	std::vector<uint8_t> out;
	const Plane plane = Generate(sprite, dome, false, out);
	ASSERT_FALSE(IsFlat(plane));
	std::vector<uint8_t> blendedOut;
	const Plane blendedPlane = Generate(sprite, Options {}, false, blendedOut);
	ASSERT_FALSE(IsFlat(blendedPlane));

	const auto opaque = [&](int x, int y) {
		if (x < 0 || y < 0 || x >= 128 || y >= 128)
			return false;
		return normals[(static_cast<size_t>(y) * 128 + x) * 3 + 2] > 0.0F;
	};
	// Disc centre from the opaque pixels.
	double sx = 0.0;
	double sy = 0.0;
	size_t count = 0;
	for (int y = 0; y < 128; ++y) {
		for (int x = 0; x < 128; ++x) {
			if (!opaque(x, y))
				continue;
			sx += x + 0.5;
			sy += y + 0.5;
			++count;
		}
	}
	ASSERT_GT(count, 4000U);
	const double cx = sx / static_cast<double>(count);
	const double cy = sy / static_cast<double>(count);

	// Centre: flat (card z), packed index 3 (= C', the flat card normal).
	const size_t centre = (static_cast<size_t>(std::lround(cy)) * 128 + static_cast<size_t>(std::lround(cx))) * 3;
	EXPECT_GT(normals[centre + 2], 0.99F);
	EXPECT_GT(blended[centre + 2], 0.99F);
	for (const Plane *p : { &plane, &blendedPlane }) {
		const std::optional<uint8_t> centreByte = PlaneByte(*p, static_cast<unsigned>(std::lround(cx)), static_cast<unsigned>(std::lround(cy)));
		ASSERT_TRUE(centreByte.has_value());
		EXPECT_EQ(gbuffer::AuxKind(*centreByte), gbuffer::Kind::Card);
		EXPECT_EQ(gbuffer::AuxNormal(*centreByte), gbuffer::NormalCardFlat);
	}

	// Rim: card-space normal points outward along the radial direction. Every rim pixel leans outward; the pixel
	// staircase of a diagonal edge leaves a few pixels short of the 0.7 line, so that bound holds for the rim as
	// a whole (mean, and at least 90 % of the pixels).
	size_t rim = 0;
	size_t rimAbove07 = 0;
	size_t rimQuantOk = 0;
	double dotSum = 0.0;
	double blendedDotSum = 0.0;
	for (int y = 0; y < 128; ++y) {
		for (int x = 0; x < 128; ++x) {
			if (!opaque(x, y))
				continue;
			const bool edge = !opaque(x - 1, y) || !opaque(x + 1, y) || !opaque(x, y - 1) || !opaque(x, y + 1);
			if (!edge)
				continue;
			++rim;
			const double rx = x + 0.5 - cx;
			const double ry = y + 0.5 - cy;
			const double rl = std::sqrt(rx * rx + ry * ry);
			const size_t i = (static_cast<size_t>(y) * 128 + x) * 3;
			const double dot = (normals[i] * rx + normals[i + 1] * ry) / rl;
			EXPECT_GT(dot, 0.4) << "rim pixel (" << x << ", " << y << ")";
			if (dot > 0.7)
				++rimAbove07;
			dotSum += dot;
			blendedDotSum += (blended[i] * rx + blended[i + 1] * ry) / rl;
			EXPECT_GT(normals[i + 2], 0.0F);
			if (LutIsReal()) {
				// The quantised (tile-space) normal must still lean toward the radial direction.
				const std::optional<uint8_t> b = PlaneByte(plane, x, y);
				ASSERT_TRUE(b.has_value());
				const gbuffer::NormalLutEntry &n = gbuffer::NormalLut()[gbuffer::AuxNormal(*b)];
				float tx;
				float ty;
				float tz;
				gbuffer::CardToTile(static_cast<float>(rx / rl), static_cast<float>(ry / rl), 0.0F, tx, ty, tz);
				if (n.x * tx + n.y * ty + n.z * tz > 0.55F)
					++rimQuantOk;
			}
		}
	}
	EXPECT_GT(rim, 200U);
	EXPECT_GT(dotSum / static_cast<double>(rim), 0.7);
	EXPECT_GE(rimAbove07 * 10, rim * 9);
	// Default blend (0.6 dome + 0.4 flat detail) still leans outward at the rim.
	EXPECT_GT(blendedDotSum / static_cast<double>(rim), 0.5);
	if (LutIsReal())
		EXPECT_GT(rimQuantOk, rim * 9 / 10);
}

TEST_F(NormalMapTest, RampTiltSign)
{
	std::optional<OwnedClxSpriteList> ramp = LoadFixture("ramp.clx");
	ASSERT_TRUE(ramp.has_value()) << "test/fixtures/normal_map/ramp.clx missing";
	const ClxSprite sprite = (*ramp)[0];
	ASSERT_EQ(sprite.width(), 64);
	ASSERT_EQ(sprite.height(), 128);

	// Detail only, no de-lighting: luminance increases upward, so the detail normal tilts toward the feet (cy < 0).
	// The fixture palette is a linear-luminance ramp (its sRGB bytes are not monotonic in perceptual luma), so this
	// test runs the generator on linear light with an absolute strength.
	Options opts;
	opts.silhouetteWeight = 0.0F;
	opts.detailStrength = 8.0F;
	opts.delight = false;
	opts.perceptualDetail = false;
	opts.normalizeDetail = false;
	const std::vector<float> normals = CardNormals(sprite, opts, false);
	// Interior of the 32x112 body (x in [16,48), feet rows [2,114)), away from the rim.
	size_t checked = 0;
	for (unsigned y = 20; y < 100; y += 4) {
		for (unsigned x = 24; x < 40; x += 4) {
			const size_t i = (static_cast<size_t>(y) * 64 + x) * 3;
			ASSERT_GT(normals[i + 2], 0.0F);
			EXPECT_LT(normals[i + 1], -0.02F) << "(" << x << ", " << y << ")";
			EXPECT_NEAR(normals[i], 0.0F, 1e-3F);
			++checked;
		}
	}
	EXPECT_GT(checked, 0U);
}

TEST_F(NormalMapTest, DelightingRemovesUniformGradient)
{
	std::optional<OwnedClxSpriteList> ramp = LoadFixture("ramp.clx");
	ASSERT_TRUE(ramp.has_value());
	const ClxSprite sprite = (*ramp)[0];

	Options lit;
	lit.silhouetteWeight = 0.0F;
	lit.detailStrength = 8.0F;
	lit.delight = false;
	lit.perceptualDetail = false; // linear-luminance fixture palette (see RampTiltSign)
	lit.normalizeDetail = false;
	Options delit = lit;
	delit.delight = true;
	const std::vector<float> withLight = CardNormals(sprite, lit, false);
	const std::vector<float> withoutLight = CardNormals(sprite, delit, false);

	double sumLit = 0.0;
	double maxDelit = 0.0;
	size_t count = 0;
	for (unsigned y = 20; y < 100; ++y) {
		for (unsigned x = 24; x < 40; ++x) {
			const size_t i = (static_cast<size_t>(y) * 64 + x) * 3;
			sumLit += std::fabs(withLight[i + 1]);
			maxDelit = std::max(maxDelit, static_cast<double>(std::fabs(withoutLight[i + 1])));
			++count;
		}
	}
	const double meanLit = sumLit / static_cast<double>(count);
	EXPECT_GT(meanLit, 0.02);
	EXPECT_LT(maxDelit, meanLit * 0.15) << "de-lighting left a residual gradient";
	// With the gradient removed the interior is flat.
	for (unsigned y = 40; y < 80; y += 8) {
		const size_t i = (static_cast<size_t>(y) * 64 + 32) * 3;
		EXPECT_GT(withoutLight[i + 2], 0.999F);
	}
}

/**
 * Dark painted relief: a wide body (the silhouette dome is flat in its middle) shaded with horizontal ridges of
 * period 8 px between sRGB greys 30 and 70, the contrast of Diablo's leather and mail. The default options must turn
 * that relief into normals that leave the flat LUT cell; the old linear-light, absolute-strength detail term moved
 * them by ~1 degree, so every interior pixel quantised to the flat card normal.
 */
TEST_F(NormalMapTest, DarkPaintedReliefLeavesTheFlatCell)
{
	std::array<SDL_Color, 256> grey {};
	for (size_t i = 0; i < grey.size(); ++i)
		grey[i] = SDL_Color { static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 255 };
	const std::span<const SDL_Color, 256> greySpan(grey.data(), 256);

	constexpr unsigned W = 64;
	constexpr unsigned H = 96;
	Image img(W, H);
	for (unsigned y = 4; y < 92; ++y) {
		const double phase = 2.0 * 3.14159265358979 * static_cast<double>(y) / 8.0;
		const auto shade = static_cast<uint8_t>(std::lround(50.0 + 20.0 * std::sin(phase)));
		for (unsigned x = 4; x < 60; ++x)
			img.feet(x, y, H) = shade;
	}
	OwnedClxSpriteList list = EncodeClx(img);
	const ClxSprite sprite = list[0];

	const auto leavesFlat = [&](const Options &opts) {
		std::vector<uint8_t> buffer;
		const Plane plane = clx_normal_map::GeneratePlane(sprite, greySpan, opts, false, buffer, clx_normal_map::SharedScratchForTesting());
		EXPECT_NE(plane.data, nullptr);
		size_t total = 0;
		size_t tilted = 0;
		// Interior: at least 18 px from every edge of the body, where the dome (R = 16) is flat.
		for (unsigned y = 24; y < 72; ++y) {
			for (unsigned x = 22; x < 42; ++x) {
				const std::optional<uint8_t> b = PlaneByte(plane, x, y);
				EXPECT_TRUE(b.has_value());
				if (!b)
					continue;
				EXPECT_EQ(gbuffer::AuxKind(*b), gbuffer::Kind::Card);
				++total;
				if (gbuffer::AuxNormal(*b) != gbuffer::NormalCardFlat)
					++tilted;
			}
		}
		return static_cast<double>(tilted) / static_cast<double>(std::max<size_t>(total, 1));
	};

	Options legacy;
	legacy.detailStrength = 2.0F;
	legacy.perceptualDetail = false;
	legacy.normalizeDetail = false;
	Options silhouetteOnly;
	silhouetteOnly.detailStrength = 0.0F;

	EXPECT_EQ(leavesFlat(silhouetteOnly), 0.0) << "the dome is flat in the interior";
	EXPECT_LT(leavesFlat(legacy), 0.02) << "linear-light detail is invisible on dark art";
	if (LutIsReal())
		EXPECT_GT(leavesFlat(Options {}), 0.5) << "default detail must survive quantisation";

	// Sign: where luminance rises upward the surface faces down toward the feet (cy < 0), as in RampTiltSign.
	std::vector<float> normals;
	ASSERT_TRUE(clx_normal_map::GenerateCardNormalsForTesting(sprite, greySpan, Options {}, false, normals, clx_normal_map::SharedScratchForTesting()));
	const size_t rising = (static_cast<size_t>(48) * W + 32) * 3; // sin rises fastest at y = 48 (phase 12*pi)
	const size_t falling = (static_cast<size_t>(52) * W + 32) * 3;
	EXPECT_LT(normals[rising + 1], -0.15F);
	EXPECT_GT(normals[falling + 1], 0.15F);
}

TEST_F(NormalMapTest, DeterministicAcrossScratchReuse)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	const OwnedClxSpriteList small = EncodeClx(DiscImage(24, 9, 90));

	std::vector<uint8_t> a;
	std::vector<uint8_t> b;
	std::vector<uint8_t> c;
	(void)Generate((*disc)[0], Options {}, false, a);
	(void)Generate(small[0], Options {}, false, b); // dirties the shared scratch with a smaller frame
	(void)Generate((*disc)[0], Options {}, false, c);
	ASSERT_FALSE(a.empty());
	EXPECT_EQ(a, c);
	EXPECT_NE(a, b);
}

TEST_F(NormalMapTest, MirroredNegatesCardX)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	const ClxSprite sprite = (*disc)[0];
	const std::vector<float> plain = CardNormals(sprite, Options {}, false);
	const std::vector<float> mirrored = CardNormals(sprite, Options {}, true);
	ASSERT_EQ(plain.size(), mirrored.size());
	size_t nonZeroX = 0;
	for (size_t i = 0; i < plain.size(); i += 3) {
		EXPECT_FLOAT_EQ(mirrored[i], -plain[i]);
		EXPECT_FLOAT_EQ(mirrored[i + 1], plain[i + 1]);
		EXPECT_FLOAT_EQ(mirrored[i + 2], plain[i + 2]);
		if (plain[i] != 0.0F)
			++nonZeroX;
	}
	EXPECT_GT(nonZeroX, 100U);
}

TEST_F(NormalMapTest, ShadowByteOnColourZero)
{
	// 8x4 frame: feet row is a colour-0 shadow, the row above is body.
	Image img(8, 4);
	for (unsigned x = 1; x < 7; ++x)
		img.feet(x, 0, 4) = 0;
	for (unsigned x = 2; x < 6; ++x)
		img.feet(x, 1, 4) = 50;
	const OwnedClxSpriteList list = EncodeClx(img);
	std::vector<uint8_t> out;
	const Plane plane = Generate(list[0], Options {}, false, out);
	ASSERT_FALSE(IsFlat(plane));
	EXPECT_EQ(plane.rowX0[0], 1);
	EXPECT_EQ(plane.rowOffset[1] - plane.rowOffset[0], 6U);
	for (unsigned x = 1; x < 7; ++x)
		EXPECT_EQ(PlaneByte(plane, x, 0), gbuffer::ShadowByte);
	for (unsigned x = 2; x < 6; ++x) {
		const std::optional<uint8_t> b = PlaneByte(plane, x, 1);
		ASSERT_TRUE(b.has_value());
		EXPECT_EQ(gbuffer::AuxKind(*b), gbuffer::Kind::Card);
	}
	EXPECT_FALSE(PlaneByte(plane, 0, 0).has_value());
	EXPECT_FALSE(PlaneByte(plane, 1, 1).has_value());
	EXPECT_EQ(plane.bytes(), 10U);
}

TEST_F(NormalMapTest, CrossLineTransparentRuns)
{
	// One pixel at the far left of the feet row, an empty row, then one pixel at the far right: the encoder emits a
	// transparent run that crosses two line boundaries (as SurfaceToClx does).
	Image img(8, 4);
	img.feet(1, 0, 4) = 30;
	img.feet(6, 2, 4) = 31;
	const std::vector<uint8_t> bytes = EncodeClxBytes(img, 1, Transparent);
	// List header (4 * (2 + 1) bytes) + frame header (6) + [1 transparent][1 pixel][20 transparent][1 pixel][9 transparent]
	ASSERT_GE(bytes.size(), 12U + 6U + 7U);
	const uint8_t *pix = bytes.data() + 12 + 6;
	EXPECT_EQ(pix[0], 1);           // transparent x=0
	EXPECT_EQ(pix[1], 0xFF);        // 1 pixel
	EXPECT_EQ(pix[2], 30);          // colour
	EXPECT_EQ(pix[3], 6 + 8 + 6);   // transparent: rest of row 0, all of row 1, six of row 2
	EXPECT_EQ(pix[4], 0xFF);
	EXPECT_EQ(pix[5], 31);

	const OwnedClxSpriteList list = EncodeClx(img);
	std::vector<uint8_t> out;
	const Plane plane = Generate(list[0], Options {}, false, out);
	ASSERT_FALSE(IsFlat(plane));
	EXPECT_EQ(plane.bytes(), 2U);
	EXPECT_EQ(plane.rowX0[0], 1);
	EXPECT_EQ(plane.rowOffset[1] - plane.rowOffset[0], 1U);
	EXPECT_EQ(plane.rowOffset[2] - plane.rowOffset[1], 0U);
	EXPECT_EQ(plane.rowX0[2], 6);
	EXPECT_EQ(plane.rowOffset[3] - plane.rowOffset[2], 1U);
	EXPECT_EQ(plane.rowOffset[4] - plane.rowOffset[3], 0U);
	EXPECT_EQ(gbuffer::AuxKind(*PlaneByte(plane, 1, 0)), gbuffer::Kind::Card);
	EXPECT_EQ(gbuffer::AuxKind(*PlaneByte(plane, 6, 2)), gbuffer::Kind::Card);
}

TEST_F(NormalMapTest, RowSpanCoverageExact)
{
	// Deterministic noisy blob with holes, shadows and ragged rows.
	constexpr unsigned W = 40;
	constexpr unsigned H = 30;
	Image img(W, H);
	uint32_t lcg = 12345;
	for (unsigned y = 0; y < H; ++y) {
		for (unsigned x = 0; x < W; ++x) {
			lcg = lcg * 1664525U + 1013904223U;
			const int dx = static_cast<int>(x) - 20;
			const int dy = static_cast<int>(y) - 15;
			if (dx * dx + dy * dy > 14 * 14)
				continue;
			const uint32_t r = lcg >> 24;
			if (r < 40)
				continue; // hole
			img.at(x, y) = r < 70 ? 0 : static_cast<uint8_t>(1 + r % 100);
		}
	}
	const OwnedClxSpriteList list = EncodeClx(img);
	std::vector<uint8_t> out;
	const Plane plane = Generate(list[0], Options {}, false, out);
	ASSERT_FALSE(IsFlat(plane));
	ASSERT_EQ(plane.width, W);
	ASSERT_EQ(plane.height, H);

	size_t expectedBytes = 0;
	for (unsigned y = 0; y < H; ++y) {
		unsigned x0 = W;
		unsigned x1 = 0;
		for (unsigned x = 0; x < W; ++x) {
			if (img.at(x, H - 1 - y) == Transparent)
				continue;
			x0 = std::min(x0, x);
			x1 = std::max(x1, x + 1);
		}
		const uint32_t len = plane.rowOffset[y + 1] - plane.rowOffset[y];
		if (x1 <= x0) {
			EXPECT_EQ(len, 0U) << "row " << y;
			continue;
		}
		EXPECT_EQ(plane.rowX0[y], x0) << "row " << y;
		EXPECT_EQ(len, x1 - x0) << "row " << y;
		expectedBytes += x1 - x0;
		for (unsigned x = 0; x < W; ++x) {
			const uint8_t src = img.at(x, H - 1 - y);
			const std::optional<uint8_t> b = PlaneByte(plane, x, y);
			if (x < x0 || x >= x1) {
				EXPECT_FALSE(b.has_value());
				continue;
			}
			ASSERT_TRUE(b.has_value());
			if (src == Transparent)
				EXPECT_EQ(*b, 0);
			else if (src == 0)
				EXPECT_EQ(*b, gbuffer::ShadowByte);
			else
				EXPECT_EQ(gbuffer::AuxKind(*b), gbuffer::Kind::Card);
		}
	}
	EXPECT_EQ(plane.bytes(), expectedBytes);
	EXPECT_LE(plane.bytes(), static_cast<size_t>(W) * H);
}

TEST_F(NormalMapTest, QuantisationTableAgreesWithQuantizeNormal)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	const ClxSprite sprite = (*disc)[0];
	const std::vector<float> normals = CardNormals(sprite, Options {}, false);
	std::vector<uint8_t> out;
	const Plane plane = Generate(sprite, Options {}, false, out);
	size_t total = 0;
	size_t agree = 0;
	for (unsigned y = 0; y < 128; ++y) {
		for (unsigned x = 0; x < 128; ++x) {
			const size_t i = (static_cast<size_t>(y) * 128 + x) * 3;
			if (normals[i + 2] <= 0.0F)
				continue;
			float tx;
			float ty;
			float tz;
			gbuffer::CardToTile(normals[i], normals[i + 1], normals[i + 2], tx, ty, tz);
			const uint8_t direct = gbuffer::QuantizeNormal(tx, ty, tz);
			const std::optional<uint8_t> b = PlaneByte(plane, x, y);
			ASSERT_TRUE(b.has_value());
			EXPECT_EQ(gbuffer::AuxNormal(*b), clx_normal_map::QuantizeCardNormalForTesting(normals[i], normals[i + 1], normals[i + 2]));
			EXPECT_GE(gbuffer::AuxNormal(*b), gbuffer::NormalCardFlat);
			++total;
			if (gbuffer::AuxNormal(*b) == direct)
				++agree;
		}
	}
	ASSERT_GT(total, 4000U);
	// The 128x128 card-space table has cells of ~1 degree near the card normal and ~3.5 degrees at 75 degrees from
	// it; only directions on a LUT Voronoi boundary can disagree with the direct argmax.
	EXPECT_GE(agree * 100, total * 95);
}

TEST_F(NormalMapTest, GenerateDisabledYieldsFlatTier)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	Options off;
	off.generate = false;
	std::vector<uint8_t> out;
	EXPECT_TRUE(IsFlat(Generate((*disc)[0], off, false, out)));
	clx_normal_map::SetOptions(off);
	EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane((*disc)[0], false)));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
}

TEST_F(NormalMapTest, CRandStateUnchangedAroundGeneratePlane)
{
	// The generator is a pure function of frame + palette + options: no RNG is consulted.
	// (The engine LCG lives in libdevilutionx_random, which this standalone test does not link; the C library
	// generator is the one shared global RNG reachable from here.)
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	std::srand(4242);
	std::vector<uint8_t> out;
	(void)Generate((*disc)[0], Options {}, false, out);
	(void)Generate((*disc)[0], Options {}, true, out);
	const int afterGenerate = std::rand();
	std::srand(4242);
	const int fresh = std::rand();
	EXPECT_EQ(afterGenerate, fresh);
}

// =========================================================================================================
// Cache
// =========================================================================================================

TEST_F(NormalMapTest, HitAfterMissAndMirroredIsDistinctKey)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	const ClxSprite sprite = (*disc)[0];
	const Plane a = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	ASSERT_FALSE(IsFlat(a));
	clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_EQ(s.entries, 1U);
	EXPECT_EQ(s.misses, 1U);
	EXPECT_EQ(s.hits, 0U);
	EXPECT_EQ(s.generatedThisFrame, 1U);

	const Plane b = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	EXPECT_EQ(a.data, b.data);
	s = clx_normal_map::GetStats();
	EXPECT_EQ(s.hits, 1U);
	EXPECT_EQ(s.entries, 1U);

	const Plane m = clx_normal_map::GetOrBuildNormalPlane(sprite, true);
	ASSERT_FALSE(IsFlat(m));
	EXPECT_NE(m.data, a.data);
	s = clx_normal_map::GetStats();
	EXPECT_EQ(s.entries, 2U);
	EXPECT_EQ(s.misses, 2U);
	// Same shape, different normals (x negated) unless the LUT is a stub where every index is 3.
	EXPECT_EQ(m.bytes(), a.bytes());
	if (LutIsReal())
		EXPECT_NE(0, std::memcmp(m.data, a.data, a.bytes()));

	// The generated plane equals the pure generator's output byte for byte.
	std::vector<uint8_t> out;
	const Plane pure = Generate(sprite, Options {}, false, out);
	ASSERT_EQ(pure.bytes(), a.bytes());
	EXPECT_EQ(0, std::memcmp(pure.data, a.data, a.bytes()));
	EXPECT_EQ(0, std::memcmp(pure.rowOffset, a.rowOffset, (a.height + 1) * sizeof(uint32_t)));
	EXPECT_EQ(0, std::memcmp(pure.rowX0, a.rowX0, a.height * sizeof(uint16_t)));
}

TEST_F(NormalMapTest, LruEvictionKeepsBytesWithinBudget)
{
	constexpr size_t Budget = 64 * 1024;
	clx_normal_map::SetCacheBudget(Budget);
	clx_normal_map::SetBudget(1000, 1000000);
	constexpr unsigned Frames = 120;
	const OwnedClxSpriteList list = EncodeClx(BlobFrames(Frames, 32), Frames);
	ASSERT_EQ(list.numSprites(), Frames);
	size_t maxBytes = 0;
	for (unsigned i = 0; i < Frames; ++i) {
		const Plane p = clx_normal_map::GetOrBuildNormalPlane(list[i], false);
		ASSERT_FALSE(IsFlat(p)) << i;
		const clx_normal_map::Stats s = clx_normal_map::GetStats();
		EXPECT_LE(s.bytes, Budget);
		EXPECT_EQ(s.arenaBytes, Budget);
		maxBytes = std::max(maxBytes, s.bytes);
	}
	const clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_GT(s.evictions, 0U);
	EXPECT_LT(s.entries, static_cast<size_t>(Frames));
	EXPECT_GT(s.entries, 4U);
	EXPECT_GT(maxBytes, Budget / 2);
	// The most recently generated frames are resident, the oldest were evicted.
	clx_normal_map::ResetFrameStats();
	(void)clx_normal_map::GetOrBuildNormalPlane(list[Frames - 1], false);
	EXPECT_EQ(clx_normal_map::GetStats().hits, 1U);
	(void)clx_normal_map::GetOrBuildNormalPlane(list[0], false);
	EXPECT_EQ(clx_normal_map::GetStats().misses, 1U);
	EXPECT_LE(clx_normal_map::GetStats().bytes, Budget);
}

TEST_F(NormalMapTest, EvictByRange)
{
	const OwnedClxSpriteList a = EncodeClx(BlobFrames(4, 16), 4);
	const OwnedClxSpriteList b = EncodeClx(BlobFrames(3, 20), 3);
	for (unsigned i = 0; i < 4; ++i)
		ASSERT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(a[i], false)));
	for (unsigned i = 0; i < 3; ++i)
		ASSERT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(b[i], false)));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 7U);
	const size_t bytesBefore = clx_normal_map::GetStats().bytes;

	clx_normal_map::EvictNormalPlanes(ClxSpriteList { a }.data(), a.dataSize());
	EXPECT_EQ(clx_normal_map::GetStats().entries, 3U);
	EXPECT_LT(clx_normal_map::GetStats().bytes, bytesBefore);
	clx_normal_map::ResetFrameStats();
	for (unsigned i = 0; i < 3; ++i)
		(void)clx_normal_map::GetOrBuildNormalPlane(b[i], false);
	EXPECT_EQ(clx_normal_map::GetStats().hits, 3U);
	// A range that touches nothing evicts nothing.
	clx_normal_map::EvictNormalPlanes(ClxSpriteList { a }.data(), a.dataSize());
	EXPECT_EQ(clx_normal_map::GetStats().entries, 3U);
	// Range evictions are not LRU evictions.
	EXPECT_EQ(clx_normal_map::GetStats().evictions, 0U);
}

TEST_F(NormalMapTest, OwnedSpriteDestructorEvicts)
{
	{
		const OwnedClxSpriteList list = EncodeClx(DiscImage(20, 8, 33));
		ASSERT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[0], false)));
		EXPECT_EQ(clx_normal_map::GetStats().entries, 1U);
	}
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
	EXPECT_EQ(clx_normal_map::GetStats().bytes, 0U);
}

TEST_F(NormalMapTest, AbaSafeAfterFreeAndReallocAtSameAddress)
{
	// Two different frames of identical dimensions and encoded size occupying the same memory in turn.
	Image first(12, 6);
	Image second(12, 6);
	for (unsigned x = 2; x < 10; ++x) {
		for (unsigned y = 0; y < 4; ++y) {
			first.feet(x, y, 6) = 40;
			second.feet(x, y, 6) = 0; // pure shadow: every byte differs from `first`
		}
	}
	const std::vector<uint8_t> firstBytes = EncodeClxBytes(first, 1, Transparent);
	const std::vector<uint8_t> secondBytes = EncodeClxBytes(second, 1, Transparent);
	ASSERT_EQ(firstBytes.size(), secondBytes.size());
	std::vector<uint8_t> memory = firstBytes;

	const ClxSpriteList list { memory.data() };
	const Plane before = clx_normal_map::GetOrBuildNormalPlane(list[0], false);
	ASSERT_FALSE(IsFlat(before));
	ASSERT_EQ(before.bytes(), 32U);
	EXPECT_EQ(gbuffer::AuxKind(*PlaneByte(before, 5, 1)), gbuffer::Kind::Card);
	std::vector<uint8_t> beforeCopy(before.data, before.data + before.bytes());

	// "Free" and "reallocate" at the same address with different content, telling the cache as the destructors do.
	clx_normal_map::EvictNormalPlanes(memory.data(), memory.size());
	std::memcpy(memory.data(), secondBytes.data(), secondBytes.size());

	const Plane after = clx_normal_map::GetOrBuildNormalPlane(list[0], false);
	ASSERT_FALSE(IsFlat(after));
	ASSERT_EQ(after.bytes(), 32U);
	EXPECT_EQ(*PlaneByte(after, 5, 1), gbuffer::ShadowByte);
	EXPECT_NE(0, std::memcmp(beforeCopy.data(), after.data, after.bytes()));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 1U);
	clx_normal_map::EvictNormalPlanes(memory.data(), memory.size());
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
}

TEST_F(NormalMapTest, BudgetMissReturnsFlatThenPumpFills)
{
	const OwnedClxSpriteList list = EncodeClx(BlobFrames(3, 24), 3);
	clx_normal_map::SetBudget(0, 0); // nothing may be generated synchronously
	for (unsigned i = 0; i < 3; ++i)
		EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[i], false)));
	clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_EQ(s.queued, 3U);
	EXPECT_EQ(s.entries, 3U);
	EXPECT_EQ(s.generatedThisFrame, 0U);
	// Re-lookups of a queued key stay flat and do not queue twice.
	EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[1], false)));
	EXPECT_EQ(clx_normal_map::GetStats().queued, 3U);

	EXPECT_EQ(clx_normal_map::Pump(0), 0U); // a zero budget generates nothing
	EXPECT_EQ(clx_normal_map::GetStats().queued, 3U);
	EXPECT_EQ(clx_normal_map::Pump(1000000), 3U);
	s = clx_normal_map::GetStats();
	EXPECT_EQ(s.queued, 0U);
	EXPECT_EQ(s.entries, 3U);
	clx_normal_map::ResetFrameStats();
	for (unsigned i = 0; i < 3; ++i)
		EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[i], false)));
	EXPECT_EQ(clx_normal_map::GetStats().hits, 3U);
}

TEST_F(NormalMapTest, QueuedEntryGeneratesSynchronouslyUnderBudget)
{
	// Queued while over budget in an earlier frame: once the budget allows, a lookup generates now
	// instead of staying flat until Pump reaches the key in FIFO order.
	const OwnedClxSpriteList list = EncodeClx(BlobFrames(3, 24), 3);
	clx_normal_map::SetBudget(0, 0);
	for (unsigned i = 0; i < 3; ++i)
		EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[i], false)));
	EXPECT_EQ(clx_normal_map::GetStats().queued, 3U);

	clx_normal_map::SetBudget(1, 1000000); // one frame per game frame
	clx_normal_map::ResetFrameStats();
	EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[1], false)));
	clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_EQ(s.generatedThisFrame, 1U);
	EXPECT_EQ(s.misses, 1U);
	EXPECT_EQ(s.entries, 3U);
	EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[2], false))); // budget spent: still flat
	EXPECT_EQ(clx_normal_map::GetStats().generatedThisFrame, 1U);
	EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[1], false)));
	EXPECT_EQ(clx_normal_map::GetStats().hits, 1U);
	// Pump skips the entry that was built synchronously and fills the rest.
	EXPECT_EQ(clx_normal_map::Pump(1000000), 2U);
	EXPECT_EQ(clx_normal_map::GetStats().queued, 0U);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 3U);

	// Prewarm-queued keys are generated by the first on-screen lookup as well.
	clx_normal_map::ClearNormalPlaneCache();
	clx_normal_map::SetBudget(TestFrameBudget, TestMicrosBudget);
	clx_normal_map::ResetFrameStats();
	clx_normal_map::EnqueuePrewarm(list);
	EXPECT_EQ(clx_normal_map::GetStats().queued, 3U);
	EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[0], false)));
	s = clx_normal_map::GetStats();
	EXPECT_EQ(s.generatedThisFrame, 1U);
	EXPECT_EQ(s.entries, 3U);
	EXPECT_EQ(clx_normal_map::Pump(1000000), 2U);
	EXPECT_EQ(clx_normal_map::GetStats().queued, 0U);
}

TEST_F(NormalMapTest, MicrosecondBudgetQueuesOnceSpent)
{
	const OwnedClxSpriteList list = EncodeClx(BlobFrames(2, 24), 2);
	clx_normal_map::SetBudget(TestFrameBudget, 0); // frames left, but the game frame's generation time is spent
	EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[0], false)));
	const clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_EQ(s.queued, 1U);
	EXPECT_EQ(s.generatedThisFrame, 0U);
	EXPECT_EQ(s.misses, 1U);
	EXPECT_EQ(clx_normal_map::Pump(1000000), 1U);
	EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[0], false)));
}

TEST_F(NormalMapTest, PumpDropsKeysWhoseMemoryWasFreed)
{
	clx_normal_map::SetBudget(0, 0);
	{
		const OwnedClxSpriteList doomed = EncodeClx(BlobFrames(2, 16), 2);
		EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(doomed[0], false)));
		EXPECT_TRUE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(doomed[1], false)));
		EXPECT_EQ(clx_normal_map::GetStats().queued, 2U);
	}
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
	EXPECT_EQ(clx_normal_map::Pump(1000000), 0U);
	EXPECT_EQ(clx_normal_map::GetStats().queued, 0U);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
}

TEST_F(NormalMapTest, PrewarmStopsAtSeventyPercentWithoutEvictions)
{
	constexpr size_t Budget = 64 * 1024;
	clx_normal_map::SetCacheBudget(Budget);
	constexpr unsigned Frames = 300;
	const OwnedClxSpriteList list = EncodeClx(BlobFrames(Frames, 24), Frames);
	clx_normal_map::EnqueuePrewarm(ClxSpriteList { list });
	clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_GT(s.prewarmDropped, 0U);
	EXPECT_LT(s.queued, static_cast<size_t>(Frames));
	EXPECT_GT(s.queued, 10U);
	EXPECT_EQ(s.queued + s.prewarmDropped, static_cast<size_t>(Frames));
	const size_t accepted = s.queued;

	// Re-enqueueing the same list accepts nothing new (known keys are skipped, the rest is still over the line).
	clx_normal_map::EnqueuePrewarm(ClxSpriteList { list });
	EXPECT_EQ(clx_normal_map::GetStats().queued, accepted);

	size_t generated = 0;
	while (clx_normal_map::GetStats().queued > 0)
		generated += clx_normal_map::Pump(1000000);
	EXPECT_EQ(generated, accepted);
	s = clx_normal_map::GetStats();
	EXPECT_EQ(s.evictions, 0U);
	EXPECT_EQ(s.entries, accepted);
	EXPECT_LE(s.bytes, Budget);
	EXPECT_GT(s.bytes, Budget / 4);
	clx_normal_map::ResetFrameStats();
	for (unsigned i = 0; i < accepted; ++i)
		EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(list[i], false)));
	EXPECT_EQ(clx_normal_map::GetStats().hits, accepted);
}

TEST_F(NormalMapTest, IdenticalFramesAliasOnePayload)
{
	// Two frames with identical pixels at different addresses share the packed bytes (refcounted).
	Image img(20, 40);
	for (unsigned f = 0; f < 2; ++f)
		for (unsigned y = 4; y < 16; ++y)
			for (unsigned x = 4; x < 16; ++x)
				img.at(x, f * 20 + y) = 77;
	const OwnedClxSpriteList list = EncodeClx(img, 2);
	ASSERT_NE(list[0].pixelData(), list[1].pixelData());
	const Plane a = clx_normal_map::GetOrBuildNormalPlane(list[0], false);
	const size_t bytesAfterFirst = clx_normal_map::GetStats().bytes;
	const Plane b = clx_normal_map::GetOrBuildNormalPlane(list[1], false);
	const size_t bytesAfterSecond = clx_normal_map::GetStats().bytes;
	ASSERT_FALSE(IsFlat(a));
	ASSERT_FALSE(IsFlat(b));
	EXPECT_EQ(a.data, b.data);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 2U);
	EXPECT_LT(bytesAfterSecond - bytesAfterFirst, a.bytes()); // only a header was added
	// Evicting one keeps the shared bytes alive for the other.
	clx_normal_map::EvictNormalPlanes(list[0].pixelData(), list[0].pixelDataSize());
	EXPECT_EQ(clx_normal_map::GetStats().entries, 1U);
	const Plane c = clx_normal_map::GetOrBuildNormalPlane(list[1], false);
	EXPECT_EQ(c.data, a.data);
	EXPECT_EQ(gbuffer::AuxKind(*PlaneByte(c, 8, 8)), gbuffer::Kind::Card);
}

TEST_F(NormalMapTest, AuthoredOverrideWinsAndSurvivesLru)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	const ClxSprite sprite = (*disc)[0];

	// Tangent-space RGB, uniform tilt to the right and up: (2R/255-1, 2G/255-1, B/255) = (0.569, 0.255, 1) normalised.
	constexpr uint8_t R = 200;
	constexpr uint8_t G = 160;
	constexpr uint8_t B = 255;
	std::vector<uint8_t> rgb(static_cast<size_t>(128) * 128 * 3);
	for (size_t i = 0; i < rgb.size(); i += 3) {
		rgb[i] = R;
		rgb[i + 1] = G;
		rgb[i + 2] = B;
	}
	float nx = R * (2.0F / 255.0F) - 1.0F;
	float ny = G * (2.0F / 255.0F) - 1.0F;
	float nz = B / 255.0F;
	const float inv = 1.0F / std::sqrt(nx * nx + ny * ny + nz * nz);
	nx *= inv;
	ny *= inv;
	nz *= inv;
	const uint8_t expectedIdx = clx_normal_map::QuantizeCardNormalForTesting(nx, ny, nz);
	const uint8_t expectedMirroredIdx = clx_normal_map::QuantizeCardNormalForTesting(-nx, ny, nz);

	clx_normal_map::RegisterAuthoredNormalMap(clx_normal_map::HashPixels(sprite), 1, rgb, 128, 128);
	std::vector<uint8_t> generatedOut;
	const Plane generated = Generate(sprite, Options {}, false, generatedOut);

	const Plane plane = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	ASSERT_FALSE(IsFlat(plane));
	EXPECT_EQ(clx_normal_map::GetStats().authoredHits, 1U);
	ASSERT_EQ(plane.bytes(), generated.bytes());
	size_t opaqueCount = 0;
	for (size_t i = 0; i < plane.bytes(); ++i) {
		if (generated.data[i] == 0 || generated.data[i] == gbuffer::ShadowByte) {
			EXPECT_EQ(plane.data[i], generated.data[i]);
			continue;
		}
		EXPECT_EQ(plane.data[i], gbuffer::PackAux(gbuffer::Kind::Card, expectedIdx));
		++opaqueCount;
	}
	EXPECT_GT(opaqueCount, 4000U);
	const Plane mirrored = clx_normal_map::GetOrBuildNormalPlane(sprite, true);
	ASSERT_FALSE(IsFlat(mirrored));
	EXPECT_EQ(gbuffer::AuxNormal(*PlaneByte(mirrored, 64, 64)), expectedMirroredIdx);

	// Churn the LRU far past the budget; the authored plane is neither evicted nor regenerated.
	clx_normal_map::SetCacheBudget(64 * 1024);
	clx_normal_map::SetBudget(1000, 1000000);
	clx_normal_map::RegisterAuthoredNormalMap(clx_normal_map::HashPixels(sprite), 1, rgb, 128, 128);
	(void)clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	const OwnedClxSpriteList churn = EncodeClx(BlobFrames(150, 32), 150);
	for (unsigned i = 0; i < 150; ++i)
		(void)clx_normal_map::GetOrBuildNormalPlane(churn[i], false);
	EXPECT_GT(clx_normal_map::GetStats().evictions, 0U);
	clx_normal_map::ResetFrameStats();
	const Plane again = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	ASSERT_FALSE(IsFlat(again));
	EXPECT_EQ(clx_normal_map::GetStats().authoredHits, 1U);
	EXPECT_EQ(clx_normal_map::GetStats().generatedThisFrame, 0U);
	EXPECT_EQ(*PlaneByte(again, 64, 64), gbuffer::PackAux(gbuffer::Kind::Card, expectedIdx));
	EXPECT_LE(clx_normal_map::GetStats().bytes, 64U * 1024U);

	// A new material version replaces the pack wholesale.
	clx_normal_map::RegisterAuthoredNormalMap(0x1234, 2, rgb, 128, 128);
	clx_normal_map::ResetFrameStats();
	const Plane generatedAgain = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	ASSERT_FALSE(IsFlat(generatedAgain));
	EXPECT_EQ(clx_normal_map::GetStats().authoredHits, 0U);
	EXPECT_EQ(clx_normal_map::GetStats().generatedThisFrame, 1U);
}

TEST_F(NormalMapTest, AuthoredRegistrationSupersedesCachedGenerated)
{
	// A generated plane that is already cached is invalidated when an authored plane for its content hash is
	// registered afterwards: "authored wins" does not depend on registration order.
	const OwnedClxSpriteList list = EncodeClx(DiscImage(24, 10, 60));
	const ClxSprite sprite = list[0];
	const Plane generated = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	ASSERT_FALSE(IsFlat(generated));
	const Plane generatedMirrored = clx_normal_map::GetOrBuildNormalPlane(sprite, true);
	ASSERT_FALSE(IsFlat(generatedMirrored));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 2U);
	EXPECT_EQ(clx_normal_map::GetStats().authoredHits, 0U);

	constexpr uint8_t R = 200;
	constexpr uint8_t G = 160;
	constexpr uint8_t B = 255;
	std::vector<uint8_t> rgb(static_cast<size_t>(24) * 24 * 3);
	for (size_t i = 0; i < rgb.size(); i += 3) {
		rgb[i] = R;
		rgb[i + 1] = G;
		rgb[i + 2] = B;
	}
	float nx = R * (2.0F / 255.0F) - 1.0F;
	float ny = G * (2.0F / 255.0F) - 1.0F;
	float nz = B / 255.0F;
	const float inv = 1.0F / std::sqrt(nx * nx + ny * ny + nz * nz);
	nx *= inv;
	ny *= inv;
	nz *= inv;
	const uint8_t expectedIdx = clx_normal_map::QuantizeCardNormalForTesting(nx, ny, nz);
	const uint8_t expectedMirroredIdx = clx_normal_map::QuantizeCardNormalForTesting(-nx, ny, nz);

	clx_normal_map::RegisterAuthoredNormalMap(clx_normal_map::HashPixels(sprite), 7, rgb, 24, 24);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U); // both generated entries (plain and mirrored) dropped
	EXPECT_EQ(clx_normal_map::GetStats().bytes, 0U);

	clx_normal_map::ResetFrameStats();
	const Plane authored = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	ASSERT_FALSE(IsFlat(authored));
	EXPECT_EQ(clx_normal_map::GetStats().authoredHits, 1U);
	EXPECT_EQ(clx_normal_map::GetStats().generatedThisFrame, 0U);
	EXPECT_EQ(*PlaneByte(authored, 12, 12), gbuffer::PackAux(gbuffer::Kind::Card, expectedIdx));
	const Plane authoredMirrored = clx_normal_map::GetOrBuildNormalPlane(sprite, true);
	ASSERT_FALSE(IsFlat(authoredMirrored));
	EXPECT_EQ(*PlaneByte(authoredMirrored, 12, 12), gbuffer::PackAux(gbuffer::Kind::Card, expectedMirroredIdx));
	EXPECT_EQ(clx_normal_map::GetStats().authoredHits, 2U);
	EXPECT_EQ(clx_normal_map::GetStats().generatedThisFrame, 0U);

	// An unrelated hash leaves cached generated entries alone.
	const OwnedClxSpriteList other = EncodeClx(DiscImage(24, 6, 90));
	ASSERT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(other[0], false)));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 3U);
	clx_normal_map::RegisterAuthoredNormalMap(0x9999, 7, rgb, 24, 24);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 3U);
}

TEST_F(NormalMapTest, ProviderBypassesCache)
{
	std::optional<OwnedClxSpriteList> disc = LoadFixture("disc.clx");
	ASSERT_TRUE(disc.has_value());
	const ClxSprite sprite = (*disc)[0];
	clx_normal_map::SetNormalPlaneProviderForTesting([](ClxSprite /*sprite*/, bool mirrored) {
		Plane p;
		p.width = mirrored ? 2 : 1;
		p.height = 1;
		return p;
	});
	EXPECT_EQ(clx_normal_map::GetOrBuildNormalPlane(sprite, false).width, 1);
	EXPECT_EQ(clx_normal_map::GetOrBuildNormalPlane(sprite, true).width, 2);
	const clx_normal_map::Stats s = clx_normal_map::GetStats();
	EXPECT_EQ(s.entries, 0U);
	EXPECT_EQ(s.hits, 0U);
	EXPECT_EQ(s.misses, 0U);
	clx_normal_map::SetNormalPlaneProviderForTesting(nullptr);
	EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(sprite, false)));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 1U);
}

TEST_F(NormalMapTest, ClearAndOptionsChangeResetTheCache)
{
	const OwnedClxSpriteList list = EncodeClx(BlobFrames(3, 16), 3);
	for (unsigned i = 0; i < 3; ++i)
		(void)clx_normal_map::GetOrBuildNormalPlane(list[i], false);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 3U);
	clx_normal_map::ClearNormalPlaneCache();
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
	EXPECT_EQ(clx_normal_map::GetStats().bytes, 0U);
	for (unsigned i = 0; i < 3; ++i)
		(void)clx_normal_map::GetOrBuildNormalPlane(list[i], false);
	Options opts;
	opts.detailStrength = 3.0F;
	clx_normal_map::SetOptions(opts);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
	clx_normal_map::SetOptions(opts); // unchanged options keep the cache
	(void)clx_normal_map::GetOrBuildNormalPlane(list[0], false);
	clx_normal_map::SetOptions(opts);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 1U);
}

TEST_F(NormalMapTest, HashPixelsIsFnv1a64OverPixelData)
{
	Image img(4, 1);
	img.at(0, 0) = 5;
	img.at(1, 0) = 5;
	const OwnedClxSpriteList list = EncodeClx(img);
	const ClxSprite sprite = list[0];
	uint64_t h = 0xcbf29ce484222325ULL;
	for (uint32_t i = 0; i < sprite.pixelDataSize(); ++i) {
		h ^= sprite.pixelData()[i];
		h *= 0x100000001b3ULL;
	}
	EXPECT_EQ(clx_normal_map::HashPixels(sprite), h);
	Image other = img;
	other.at(1, 0) = 6;
	const OwnedClxSpriteList otherList = EncodeClx(other);
	EXPECT_NE(clx_normal_map::HashPixels(otherList[0]), h);
}

// =========================================================================================================
// Static-storage teardown
// =========================================================================================================

/**
 * Mirrors the production exit path: Owned* sprites with static storage duration (Players' animation sheets, the
 * gmenu/cursor/inventory sprites, ...) are constructed at static-init time, before the cache is first touched, and
 * their destructors call EvictNormalPlanes after every function-local static constructed later has already been
 * destroyed. `StaticSpriteList` is built here, at static init; its planes are cached and left resident by the global
 * environment's TearDown (after every test's own TearDown has cleared the cache), so process teardown runs
 * ~OwnedClxSpriteList -> EvictNormalPlanes with live entries for it. The cache must be immortal for that to be
 * well-defined. ASan only sees the resulting heap use-after-free on libstdc++ (libc++'s ~vector leaves begin == end,
 * so a destroyed table is walked as empty and the entries silently survive), hence `StaticTeardownSentinel`: defined
 * first, so destroyed last, it verifies that the static destructor's eviction reached a live cache.
 */
struct StaticTeardownSentinel {
	~StaticTeardownSentinel()
	{
		const clx_normal_map::Stats s = clx_normal_map::GetStats();
		if (s.entries != 0 || s.bytes != 0) {
			std::fprintf(stderr, "clx_normal_map cache did not see the static sprite's eviction at exit (entries=%zu bytes=%zu): the cache was destroyed before a static Owned* sprite\n", s.entries, s.bytes);
			std::_Exit(3);
		}
	}
};
const StaticTeardownSentinel StaticSentinel;
const OwnedClxSpriteList StaticSpriteList = EncodeClx(DiscImage(20, 8, 44));

class StaticSpriteTeardownEnvironment : public ::testing::Environment {
public:
	void TearDown() override
	{
		clx_normal_map::SetNormalPlaneProviderForTesting(nullptr);
		clx_normal_map::SetOptions(Options {});
		clx_normal_map::SetBudget(TestFrameBudget, TestMicrosBudget);
		clx_normal_map::SetCacheBudget(1024 * 1024);
		clx_normal_map::ResetFrameStats(); // the last test's generation count would otherwise exhaust the budget
		LatchTestPalette();
		EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(StaticSpriteList[0], false)));
		EXPECT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(StaticSpriteList[0], true)));
		EXPECT_EQ(clx_normal_map::GetStats().entries, 2U);
		// Deliberately no ClearNormalPlaneCache(): the entries stay resident until the static destructor evicts them.
	}
};

const ::testing::Environment *const StaticSpriteTeardown = ::testing::AddGlobalTestEnvironment(new StaticSpriteTeardownEnvironment);

TEST_F(NormalMapTest, StaticSpriteListPlaneIsCacheable)
{
	ASSERT_FALSE(IsFlat(clx_normal_map::GetOrBuildNormalPlane(StaticSpriteList[0], false)));
	EXPECT_EQ(clx_normal_map::GetStats().entries, 1U);
	// The evict-on-free path the static destructor will take at exit.
	const ClxSpriteList view = StaticSpriteList;
	clx_normal_map::EvictNormalPlanes(view.data(), view.dataSize());
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
}

} // namespace
} // namespace devilution
