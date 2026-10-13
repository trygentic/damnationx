#include <gtest/gtest.h>

#include <array>
#include <span>

#include "engine/lighting_defs.hpp"
#include "engine/render/gbuffer.hpp"

namespace {

TEST(ClassicAmbientCurve, FollowsTheLightTablesNotALine)
{
	// A palette of one 16-step ramp repeated: entry i has luminance 255 - 16 * (i % 16). Level s walks the ramp down s
	// steps and blacks out the last s entries, as MakeLightTable does for 16-step ramps.
	std::array<SDL_Color, 256> palette {};
	for (size_t i = 0; i < 256; ++i) {
		const uint8_t v = static_cast<uint8_t>(255 - 16 * (i % 16));
		palette[i] = SDL_Color { v, v, v, 255 };
	}
	palette[0] = SDL_Color { 0, 0, 0, 255 }; // index 0 is black in every game palette; blacked-out ramp entries land there
	std::array<std::array<uint8_t, devilution::LightTableSize>, devilution::NumLightingLevels> tables {};
	for (size_t level = 0; level < devilution::NumLightingLevels; ++level)
		for (size_t i = 0; i < 256; ++i) {
			const size_t step = i % 16 + level;
			tables[level][i] = static_cast<uint8_t>(step >= 16 ? 0 : i + level);
		}
	std::array<float, devilution::NumLightingLevels> curve {};
	devilution::gbuffer::BuildClassicAmbientCurve(palette, tables, curve);
	EXPECT_FLOAT_EQ(curve[0], 1.0F);
	EXPECT_NEAR(curve[15], 0.0F, 0.01F); // the synthetic tables keep each ramp top at level 15; real level-15 tables are all zero
	for (size_t level = 1; level < 15; ++level) {
		EXPECT_LT(curve[level], curve[level - 1]) << "level " << level;
		EXPECT_LT(curve[level], 1.0F - static_cast<float>(level) / 15.0F + 1e-4F) << "level " << level << " must darken at least as hard as the linear ambient";
	}
	// Half way down the ramp classic is far darker than the linear 7/15.
	EXPECT_LT(curve[8], 0.3F);
}

} // namespace


#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/rectangle.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/surface.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {
namespace {

using namespace gbuffer;

std::array<SDL_Color, 256> TestPalette {};

LatchInput EnabledInput()
{
	return { /*optionEnabled=*/true, /*presenterReady=*/true, /*headless=*/false, /*gameRunning=*/true, DTYPE_CATHEDRAL, TestPalette.data() };
}

LatchInput DisabledInput()
{
	LatchInput in = EnabledInput();
	in.optionEnabled = false;
	return in;
}

constexpr float Pi = 3.14159265358979323846F;

float DegreesBetween(float ax, float ay, float az, float bx, float by, float bz)
{
	const float d = std::clamp(ax * bx + ay * by + az * bz, -1.0F, 1.0F);
	return std::acos(d) * 180.0F / Pi;
}

/** Exact argmax over LUT entries 3..31 (the single quantisation rule), computed independently of QuantizeNormal. */
uint8_t ReferenceArgmax(float x, float y, float z)
{
	const std::array<NormalLutEntry, 32> &lut = NormalLut();
	uint8_t best = NormalCardFlat;
	float bestDot = -2.0F;
	for (size_t i = NormalCardFlat; i < lut.size(); ++i) {
		const float d = x * lut[i].x + y * lut[i].y + z * lut[i].z;
		if (d > bestDot) {
			bestDot = d;
			best = static_cast<uint8_t>(i);
		}
	}
	return best;
}

/**
 * Reference transliteration of Zoom() (scrollrt.cpp:1147-1197), which lives in an anonymous namespace and cannot be
 * called; the side-panel logic is replaced by the explicit `viewportWidth` / `viewportOffsetX` the real one computes.
 *
 * One deliberate difference: for an odd `out.h()` the upstream tail `memcpy(dst - pitch + 1, dst + 1, ...)` copies output
 * row 0 to row -1 (out of bounds; row 0 itself never receives the last source row). The reference doubles the last source
 * row into row 0 instead, which is what ZoomInPlace does with plane A; the mapping check below covers that row too.
 */
void ReferenceZoom(const Surface &out, int viewportWidth, int viewportOffsetX)
{
	// We round to even for the source width and height.
	// If the width / height was odd, we copy just one extra pixel / row later on.
	const int srcWidth = (viewportWidth + 1) / 2;
	const int doubleableWidth = viewportWidth / 2;
	const int srcHeight = (out.h() + 1) / 2;
	const int doubleableHeight = out.h() / 2;

	uint8_t *src = out.at(srcWidth - 1, srcHeight - 1);
	uint8_t *dst = out.at(viewportOffsetX + viewportWidth - 1, out.h() - 1);
	const bool oddViewportWidth = (viewportWidth % 2) == 1;

	const auto doubleLine = [&]() {
		for (int i = 0; i < doubleableWidth; i++) {
			*dst-- = *src;
			*dst-- = *src;
			--src;
		}

		// Copy a single extra pixel if the output width is odd.
		if (oddViewportWidth) {
			*dst-- = *src;
			--src;
		}
	};

	for (int hgt = 0; hgt < doubleableHeight; hgt++) {
		// Double the pixels in the line.
		doubleLine();

		// Skip the rest of the source line.
		src -= (out.pitch() - srcWidth);

		// Double the line.
		memcpy(dst - out.pitch() + 1, dst + 1, viewportWidth);

		// Skip the rest of the destination line.
		dst -= 2 * out.pitch() - viewportWidth;
	}
	if ((out.h() % 2) == 1) {
		doubleLine();
	}
}

/**
 * The pixel mapping Zoom() implements, stated independently of its pointer walk: output pixel `(x, y)` of the viewport
 * reads source pixel `(sx, sy)` of the top-left quarter. Even extents halve; odd extents (which get one extra source
 * pixel / row) round up, because the walk pairs output pixels from the far end.
 */
int ZoomSourceIndex(int outIndex, int outExtent)
{
	return (outExtent % 2) == 1 ? (outIndex + 1) / 2 : outIndex / 2;
}

/** Deterministic byte pattern that differs between neighbouring pixels and rows. */
uint8_t PatternAt(size_t offset)
{
	return static_cast<uint8_t>((offset * 31 + (offset >> 7) * 17 + 5) & 0xFF);
}

void FillPattern(uint8_t *dst, size_t bytes, uint8_t salt)
{
	for (size_t i = 0; i < bytes; ++i)
		dst[i] = static_cast<uint8_t>(PatternAt(i) ^ salt);
}

class GBufferTest : public ::testing::Test {
protected:
	void TearDown() override
	{
		Free();
		surface.reset();
	}

	/** Creates an 8-bit "back buffer" of `w` x `h` and mirrors it with planes. */
	void AllocatePlanes(int w, int h)
	{
		Free();
		surface = std::make_unique<OwnedSurface>(w, h);
		Allocate(surface->begin(), surface->pitch(), static_cast<uint16_t>(w), static_cast<uint16_t>(h));
		ASSERT_NE(CurrentPlanes.aux, nullptr);
	}

	void BeginHybridFrame(int rows)
	{
		LatchForFrame(EnabledInput());
		BeginWorldPass(rows);
		ASSERT_TRUE(BackBufferIsHybrid());
	}

	[[nodiscard]] size_t PlaneBytes() const
	{
		return static_cast<size_t>(CurrentPlanes.pitch) * CurrentPlanes.height;
	}

	[[nodiscard]] uint8_t AuxAt(int x, int y) const
	{
		return CurrentPlanes.aux[static_cast<size_t>(y) * CurrentPlanes.pitch + x];
	}

	std::unique_ptr<OwnedSurface> surface;
};

// ---- Packing ----

TEST(GBufferPack, RoundTripsEveryKindAndNormal)
{
	for (uint8_t k = 0; k < 8; ++k) {
		for (uint8_t n = 0; n < 32; ++n) {
			const uint8_t a = PackAux(static_cast<Kind>(k), n);
			EXPECT_EQ(AuxKind(a), static_cast<Kind>(k));
			EXPECT_EQ(AuxNormal(a), n);
		}
	}
	EXPECT_EQ(PackAux(Kind::Card, 0xFF), PackAux(Kind::Card, 31)) << "normal index is masked to 5 bits";
	EXPECT_EQ(UnlitByte, 0);
	EXPECT_EQ(AuxKind(ShadowByte), Kind::Floor);
	EXPECT_EQ(AuxNormal(ShadowByte), NormalUp);
	EXPECT_EQ(AuxKind(FlatCardByte), Kind::Card);
	EXPECT_EQ(AuxNormal(FlatCardByte), NormalCardFlat);
	EXPECT_EQ(AuxKind(EmissiveCardByte), Kind::CardEmissive);
}

// ---- Lifecycle ----

TEST_F(GBufferTest, AllocateInitialisesPlanesAndFreesPrevious)
{
	AllocatePlanes(640, 480);
	EXPECT_EQ(CurrentPlanes.outBase, surface->begin());
	EXPECT_EQ(CurrentPlanes.pitch, surface->pitch());
	EXPECT_EQ(CurrentPlanes.width, 640);
	EXPECT_EQ(CurrentPlanes.height, 480);
	EXPECT_EQ(CurrentPlanes.pitchRecip40, ((uint64_t { 1 } << 40) + CurrentPlanes.pitch - 1) / CurrentPlanes.pitch);
	EXPECT_NE(CurrentPlanes.base, nullptr);
	EXPECT_NE(CurrentPlanes.aux, CurrentPlanes.base);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(CurrentPlanes.aux) % 64, 0U);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(CurrentPlanes.base) % 64, 0U);
	for (size_t i = 0; i < PlaneBytes(); ++i) {
		ASSERT_EQ(CurrentPlanes.aux[i], UnlitByte) << "aux byte " << i;
		ASSERT_EQ(CurrentPlanes.base[i], 0) << "base byte " << i;
	}
	EXPECT_EQ(AuxRows(480).size(), PlaneBytes());
	EXPECT_EQ(AuxRows(100).size(), static_cast<size_t>(CurrentPlanes.pitch) * 100);
	EXPECT_EQ(AuxRows(10000).size(), PlaneBytes()) << "clamped to the allocation";
	EXPECT_EQ(BaseRows(-5).size(), 0U);
	EXPECT_EQ(AuxRows(1).data(), CurrentPlanes.aux);
	EXPECT_EQ(BaseRows(1).data(), CurrentPlanes.base);

	// Re-entry (fullscreen / resolution change) frees the previous allocation and sizes the new one.
	AllocatePlanes(1280, 720);
	EXPECT_EQ(CurrentPlanes.width, 1280);
	EXPECT_EQ(CurrentPlanes.height, 720);
	EXPECT_EQ(AuxRows(720).size(), PlaneBytes());

	Free();
	EXPECT_EQ(CurrentPlanes.aux, nullptr);
	EXPECT_EQ(CurrentPlanes.base, nullptr);
	EXPECT_EQ(CurrentPlanes.pitch, 0);
	EXPECT_EQ(AuxRows(10).size(), 0U);
	EXPECT_EQ(BaseRows(10).size(), 0U);
	EXPECT_FALSE(BackBufferIsHybrid());
	EXPECT_EQ(CurrentMode, Mode::Off);
	Free(); // double free is harmless
}

TEST_F(GBufferTest, BeginWorldPassClearsExactlyTheViewportRows)
{
	AllocatePlanes(640, 480);
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	std::memset(CurrentPlanes.base, 0xCD, PlaneBytes());

	BeginHybridFrame(352);
	EXPECT_EQ(CurrentMode, Mode::CaptureWorld);
	const size_t clearedBytes = static_cast<size_t>(CurrentPlanes.pitch) * 352;
	for (size_t i = 0; i < PlaneBytes(); ++i) {
		ASSERT_EQ(CurrentPlanes.aux[i], i < clearedBytes ? UnlitByte : 0xAB) << "aux byte " << i;
		ASSERT_EQ(CurrentPlanes.base[i], 0xCD) << "base byte " << i << " must not be touched";
	}

	// Under zoom the caller still passes ALL viewport rows (gnViewportHeight), not the half the blitters write.
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	BeginHybridFrame(480);
	for (size_t i = 0; i < PlaneBytes(); ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], UnlitByte) << "aux byte " << i;

	// Out-of-range row counts are clamped.
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	BeginHybridFrame(100000);
	EXPECT_EQ(CurrentPlanes.aux[PlaneBytes() - 1], UnlitByte);
	BeginHybridFrame(-3);
	EXPECT_EQ(CurrentMode, Mode::CaptureWorld);
}

TEST_F(GBufferTest, BeginWorldPassDisabledLeavesPlanesAndModeAlone)
{
	AllocatePlanes(640, 480);
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	LatchForFrame(DisabledInput());
	BeginWorldPass(480);
	EXPECT_FALSE(BackBufferIsHybrid());
	EXPECT_EQ(CurrentMode, Mode::Off);
	for (size_t i = 0; i < PlaneBytes(); ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], 0xAB) << "aux byte " << i;
	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::Off) << "EndWorldPass is a no-op unless Enabled()";
	EndFrame();
	EXPECT_EQ(CurrentMode, Mode::Off);
}

TEST_F(GBufferTest, ModeAndHybridFlagLifecycle)
{
	AllocatePlanes(640, 480);
	EXPECT_FALSE(BackBufferIsHybrid());

	LatchForFrame(EnabledInput());
	EXPECT_TRUE(Enabled());
	EXPECT_EQ(CurrentMode, Mode::Off);
	EXPECT_FALSE(BackBufferIsHybrid()) << "only BeginWorldPass records the flag";

	BeginWorldPass(352);
	EXPECT_EQ(CurrentMode, Mode::CaptureWorld);
	EXPECT_TRUE(CaptureActive());
	EXPECT_TRUE(Active());
	EXPECT_TRUE(BackBufferIsHybrid());

	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::StampUnlit);
	EXPECT_TRUE(StampActive());
	EXPECT_TRUE(Active());

	EndFrame();
	EXPECT_EQ(CurrentMode, Mode::Off);
	EXPECT_FALSE(Active());
	EXPECT_TRUE(BackBufferIsHybrid()) << "kept across EndFrame: the back buffer still holds the hybrid frame";

	// scrollrt_draw_game_screen re-present while still enabled: no world redraw, straight to StampUnlit.
	LatchForFrame(EnabledInput());
	EXPECT_EQ(CurrentMode, Mode::Off);
	EXPECT_TRUE(BackBufferIsHybrid());
	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::StampUnlit);
	EndFrame();

	// Settings toggle between frames: the flag survives until the next world pass is drawn classic. A re-present of the
	// hybrid frame in between is still composited, so it stamps: the cursor and the gamepad must pass through.
	LatchForFrame(DisabledInput());
	EXPECT_FALSE(Enabled());
	EXPECT_TRUE(BackBufferIsHybrid());
	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::StampUnlit);
	EndFrame();
	LatchForFrame(DisabledInput());
	BeginWorldPass(352);
	EXPECT_FALSE(BackBufferIsHybrid());
	EXPECT_EQ(CurrentMode, Mode::Off);
	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::Off) << "a classic back buffer never arms the stamp";
}

TEST_F(GBufferTest, InvalidateBackBufferEndsTheHybridFrame)
{
	AllocatePlanes(640, 480);
	LatchForFrame(EnabledInput());
	BeginWorldPass(352);
	EndWorldPass();
	EndFrame();
	ASSERT_TRUE(BackBufferIsHybrid());

	// ClearScreenBuffer (loading screens) and play_movie (in-game movies) repurpose the back buffer while the game runs.
	InvalidateBackBuffer();
	EXPECT_FALSE(BackBufferIsHybrid());

	// Their presents are classic: the re-present latch arms nothing and the plane helpers are no-ops.
	LatchForFrame(DisabledInput());
	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::Off);
	std::vector<uint8_t> saved(16, 0xCD);
	SaveRect({ { 0, 0 }, { 4, 4 } }, saved);
	EXPECT_EQ(saved[0], 0xCD) << "SaveRect is gated on BackBufferIsHybrid()";
	EndFrame();

	// The next world pass latches the flag again.
	LatchForFrame(EnabledInput());
	BeginWorldPass(352);
	EXPECT_TRUE(BackBufferIsHybrid());
	EndFrame();
}

TEST_F(GBufferTest, LatchForFrameRules)
{
	AllocatePlanes(640, 480);
	const uint32_t before = FrameCounter();

	LatchForFrame(EnabledInput());
	EXPECT_TRUE(Enabled());
	EXPECT_EQ(FrameCounter(), before + 1);

	LatchInput in = EnabledInput();
	in.level = DTYPE_TOWN;
	LatchForFrame(in);
	EXPECT_FALSE(Enabled()) << "town is disabled";
	EXPECT_EQ(FrameCounter(), before + 2);

	in = EnabledInput();
	in.headless = true;
	LatchForFrame(in);
	EXPECT_FALSE(Enabled()) << "headless is disabled";

	in = EnabledInput();
	in.presenterReady = false;
	LatchForFrame(in);
	EXPECT_FALSE(Enabled());

	in = EnabledInput();
	in.gameRunning = false;
	LatchForFrame(in);
	EXPECT_FALSE(Enabled());

	in = EnabledInput();
	in.optionEnabled = false;
	LatchForFrame(in);
	EXPECT_FALSE(Enabled());

	for (const dungeon_type level : { DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CAVES, DTYPE_HELL, DTYPE_NEST, DTYPE_CRYPT }) {
		in = EnabledInput();
		in.level = level;
		LatchForFrame(in);
		EXPECT_TRUE(Enabled()) << "level " << static_cast<int>(level);
	}

	// LatchForFrame resets CurrentMode whatever it was.
	CurrentMode = Mode::StampUnlit;
	LatchForFrame(EnabledInput());
	EXPECT_EQ(CurrentMode, Mode::Off);
	CurrentMode = Mode::CaptureWorld;
	LatchForFrame(DisabledInput());
	EXPECT_EQ(CurrentMode, Mode::Off);

	// A mid-frame option flip has no effect until the next latch: the value is a copy taken at latch time.
	LatchInput flipped = EnabledInput();
	LatchForFrame(flipped);
	flipped.optionEnabled = false;
	EXPECT_TRUE(Enabled());
	BeginWorldPass(352);
	EXPECT_TRUE(BackBufferIsHybrid());
	EXPECT_EQ(CurrentMode, Mode::CaptureWorld);
	EndWorldPass();
	EXPECT_EQ(CurrentMode, Mode::StampUnlit);
	LatchForFrame(flipped);
	EXPECT_FALSE(Enabled());

	// Without planes nothing can be captured: Enabled() stays false (CreateBackBuffer always allocates first).
	Free();
	LatchForFrame(EnabledInput());
	EXPECT_FALSE(Enabled());
	BeginWorldPass(352);
	EXPECT_FALSE(BackBufferIsHybrid());
	EXPECT_EQ(CurrentMode, Mode::Off);
}

TEST_F(GBufferTest, CurrentPaletteIsLatched)
{
	AllocatePlanes(64, 64);
	std::array<SDL_Color, 256> other {};
	LatchInput in = EnabledInput();
	in.logicalPalette = other.data();
	LatchForFrame(in);
	EXPECT_EQ(CurrentPalette(), other.data());
	in.logicalPalette = TestPalette.data();
	EXPECT_EQ(CurrentPalette(), other.data()) << "not re-read until the next latch";
	LatchForFrame(in);
	EXPECT_EQ(CurrentPalette(), TestPalette.data());
	LatchForFrame(DisabledInput());
	EXPECT_EQ(CurrentPalette(), TestPalette.data()) << "latched even when disabled";
}

// ---- Planes helpers ----

TEST(GBufferPlanes, RowOfIsExactForEveryOffset)
{
	struct Case {
		uint16_t pitch;
		uint16_t height;
	};
	for (const Case c : { Case { 640, 480 }, Case { 1280, 720 }, Case { 1366, 768 }, Case { 1920, 1080 }, Case { 2560, 1440 } }) {
		Planes planes;
		planes.pitch = c.pitch;
		planes.height = c.height;
		planes.pitchRecip40 = ((uint64_t { 1 } << 40) + c.pitch - 1) / c.pitch;
		const size_t bytes = static_cast<size_t>(c.pitch) * c.height;
		size_t mismatches = 0;
		size_t firstMismatch = 0;
		for (size_t off = 0; off < bytes; ++off) {
			if (planes.rowOf(static_cast<ptrdiff_t>(off)) != off / c.pitch) {
				if (mismatches == 0)
					firstMismatch = off;
				++mismatches;
			}
		}
		EXPECT_EQ(mismatches, 0U) << "pitch " << c.pitch << ": first mismatch at offset " << firstMismatch;
	}
}

TEST_F(GBufferTest, CoversSubregionsButNotForeignSurfaces)
{
	AllocatePlanes(640, 480);
	EXPECT_TRUE(CurrentPlanes.covers(*surface));
	EXPECT_TRUE(CurrentPlanes.covers(surface->subregion(10, 20, 100, 100)));
	EXPECT_TRUE(CurrentPlanes.covers(surface->subregionY(0, 352)));
	EXPECT_TRUE(CurrentPlanes.covers(surface->subregionX(320, 320)));
	EXPECT_TRUE(CurrentPlanes.covers(surface->subregion(639, 479, 1, 1)));

	const OwnedSurface foreign(640, 480);
	EXPECT_FALSE(CurrentPlanes.covers(foreign));
	EXPECT_FALSE(CurrentPlanes.covers(foreign.subregion(1, 1, 8, 8)));

	Free();
	EXPECT_FALSE(CurrentPlanes.covers(*surface)) << "no planes: nothing is covered";
}

// ---- Stamping ----

TEST_F(GBufferTest, StampUnlitClipsToSurfaceAndPlanesAndTranslatesSubregions)
{
	AllocatePlanes(640, 480);
	BeginHybridFrame(480);
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	std::memset(CurrentPlanes.base, 0xCD, PlaneBytes());

	// Not in StampUnlit mode: nothing happens.
	StampUnlit(*surface, { { 0, 0 }, { 10, 10 } });
	EXPECT_EQ(AuxAt(0, 0), 0xAB);
	EndWorldPass();
	ASSERT_TRUE(StampActive());

	// Top-left corner, partially outside.
	StampUnlit(*surface, { { -10, -10 }, { 20, 20 } });
	for (int y = 0; y < 12; ++y) {
		for (int x = 0; x < 12; ++x)
			ASSERT_EQ(AuxAt(x, y), (x < 10 && y < 10) ? UnlitByte : 0xAB) << "(" << x << ", " << y << ")";
	}

	// Bottom-right corner, partially outside.
	StampUnlit(*surface, { { 630, 470 }, { 100, 100 } });
	for (int y = 468; y < 480; ++y) {
		for (int x = 628; x < 640; ++x)
			ASSERT_EQ(AuxAt(x, y), (x >= 630 && y >= 470) ? UnlitByte : 0xAB) << "(" << x << ", " << y << ")";
	}

	// Entirely outside / empty.
	StampUnlit(*surface, { { 640, 0 }, { 10, 10 } });
	StampUnlit(*surface, { { 0, 480 }, { 10, 10 } });
	StampUnlit(*surface, { { 100, 100 }, { 0, 10 } });
	StampUnlit(*surface, { { 100, 100 }, { 10, -1 } });
	EXPECT_EQ(AuxAt(100, 100), 0xAB);

	// Subregion: rect is in the subregion's coordinates and is clipped to it before translation.
	const Surface sub = surface->subregion(100, 200, 50, 50);
	StampUnlit(sub, { { -5, -5 }, { 10, 10 } });
	for (int y = 198; y < 208; ++y) {
		for (int x = 98; x < 108; ++x)
			ASSERT_EQ(AuxAt(x, y), (x >= 100 && x < 105 && y >= 200 && y < 205) ? UnlitByte : 0xAB) << "(" << x << ", " << y << ")";
	}
	StampUnlit(sub, { { 45, 45 }, { 20, 20 } });
	for (int y = 243; y < 253; ++y) {
		for (int x = 143; x < 153; ++x)
			ASSERT_EQ(AuxAt(x, y), (x >= 145 && x < 150 && y >= 245 && y < 250) ? UnlitByte : 0xAB) << "(" << x << ", " << y << ")";
	}

	// A subregion that starts at a non-zero x on a non-zero row (both offsets must be recovered from the pointer).
	const Surface panel = surface->subregion(320, 352, 320, 128);
	StampUnlit(panel, { { 0, 0 }, { 320, 128 } });
	for (int y = 350; y < 480; ++y) {
		for (int x = 318; x < 640; ++x)
			ASSERT_EQ(AuxAt(x, y), (x >= 320 && y >= 352) ? UnlitByte : 0xAB) << "(" << x << ", " << y << ")";
	}

	// Foreign surface: never touched.
	const OwnedSurface foreign(640, 480);
	StampUnlit(foreign, { { 300, 300 }, { 10, 10 } });
	EXPECT_EQ(AuxAt(300, 300), 0xAB);

	// Plane B is never written by stamping.
	for (size_t i = 0; i < PlaneBytes(); ++i)
		ASSERT_EQ(CurrentPlanes.base[i], 0xCD) << "base byte " << i;

	EndFrame();
	StampUnlit(*surface, { { 300, 300 }, { 10, 10 } });
	EXPECT_EQ(AuxAt(300, 300), 0xAB) << "Off mode: no-op";
}

TEST_F(GBufferTest, SurfaceBlitsStampTheUiTheyDrawUnlit)
{
	// Surface::BlitFrom / BlitFromSkipColorIndexZero draw the main panel, buttons, belt, info and chat boxes and the
	// flasks: once the world pass has ended, the pixels they write must pass through the composite.
	AllocatePlanes(64, 32);
	OwnedSurface art(8, 4);
	for (int y = 0; y < art.h(); ++y) {
		for (int x = 0; x < art.w(); ++x)
			art[{ x, y }] = static_cast<uint8_t>((x + y) % 3 == 0 ? 0 : 40 + x); // colour 0 is the skip-zero transparent key
	}
	const SDL_Rect whole { 0, 0, art.w(), art.h() };

	// Capture (and classic) mode: the blit is a plain copy.
	BeginHybridFrame(32);
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	surface->BlitFrom(art, whole, { 2, 3 });
	EXPECT_EQ(AuxAt(2, 3), 0xAB);
	EndWorldPass();
	ASSERT_TRUE(StampActive());

	// Opaque blit: the whole destination rectangle, clipped to the surface.
	surface->BlitFrom(art, whole, { 60, 30 });
	for (int y = 28; y < 32; ++y) {
		for (int x = 58; x < 64; ++x)
			ASSERT_EQ(AuxAt(x, y), (x >= 60 && y >= 30) ? UnlitByte : 0xAB) << "(" << x << ", " << y << ")";
	}

	// Skip-zero blit into a subregion: only the pixels it wrote.
	const Surface sub = surface->subregion(10, 5, 20, 20);
	sub.BlitFromSkipColorIndexZero(art, whole, { 1, 2 });
	for (int y = 0; y < art.h(); ++y) {
		for (int x = 0; x < art.w(); ++x) {
			const uint8_t expected = art[Point { x, y }] != 0 ? UnlitByte : 0xAB;
			ASSERT_EQ(AuxAt(11 + x, 7 + y), expected) << "(" << x << ", " << y << ")";
		}
	}
	EXPECT_EQ(AuxAt(10, 7), 0xAB);
	EXPECT_EQ(AuxAt(19, 7), 0xAB);

	// Blits into foreign surfaces (BottomBuffer, the flask scratch buffers) never touch the planes.
	OwnedSurface foreign(64, 32);
	foreign.BlitFrom(art, whole, { 40, 20 });
	EXPECT_EQ(AuxAt(40, 20), 0xAB);

	// After the frame, blits stop stamping.
	EndFrame();
	surface->BlitFrom(art, whole, { 40, 20 });
	EXPECT_EQ(AuxAt(40, 20), 0xAB);
}

TEST_F(GBufferTest, StampUnlitRunWritesThroughTheOutputPointer)
{
	AllocatePlanes(640, 480);
	BeginHybridFrame(480);
	std::memset(CurrentPlanes.aux, 0xAB, PlaneBytes());
	EndWorldPass();
	StampUnlitRun(surface->at(10, 20), 5);
	for (int x = 8; x < 17; ++x)
		EXPECT_EQ(AuxAt(x, 20), (x >= 10 && x < 15) ? UnlitByte : 0xAB) << x;
	EXPECT_EQ(AuxAt(10, 19), 0xAB);
	EXPECT_EQ(AuxAt(10, 21), 0xAB);
}

// ---- Tile context ----

TEST(GBufferFoot, FootRowsAndBaseRowTables)
{
	EXPECT_EQ(FootRows(FootKind::SW, 0), 15);
	EXPECT_EQ(FootRows(FootKind::SW, 1), 15);
	EXPECT_EQ(FootRows(FootKind::SW, 2), 14);
	EXPECT_EQ(FootRows(FootKind::SW, 30), 0);
	EXPECT_EQ(FootRows(FootKind::SW, 31), 0);
	EXPECT_EQ(FootRows(FootKind::SE, 0), 0);
	EXPECT_EQ(FootRows(FootKind::SE, 1), 0);
	EXPECT_EQ(FootRows(FootKind::SE, 2), 1);
	EXPECT_EQ(FootRows(FootKind::SE, 31), 15);
	EXPECT_EQ(FootRows(FootKind::None, 0), 0);
	EXPECT_EQ(FootRows(FootKind::None, 31), 0);

	std::array<uint8_t, 32> row {};
	for (const int anchorRow : { 0, 7, 200, 300, 511, -3 }) {
		BuildBaseRow(row, anchorRow, FootKind::SW);
		for (int c = 0; c < 32; ++c)
			EXPECT_EQ(row[c], static_cast<uint8_t>((anchorRow - (31 - c) / 2) & 0xFF)) << "SW anchor " << anchorRow << " c " << c;
		BuildBaseRow(row, anchorRow, FootKind::SE);
		for (int c = 0; c < 32; ++c)
			EXPECT_EQ(row[c], static_cast<uint8_t>((anchorRow - c / 2) & 0xFF)) << "SE anchor " << anchorRow << " c " << c;
		BuildBaseRow(row, anchorRow, FootKind::None);
		for (int c = 0; c < 32; ++c)
			EXPECT_EQ(row[c], static_cast<uint8_t>(anchorRow & 0xFF)) << "None anchor " << anchorRow << " c " << c;
	}
	// The diagonal foot edge reads h = 0: at the foot row of column c, B - y == 0.
	BuildBaseRow(row, 300, FootKind::SW);
	for (int c = 0; c < 32; ++c)
		EXPECT_EQ(static_cast<uint8_t>((row[c] - (300 - FootRows(FootKind::SW, c))) & 0xFF), 0);
}

TEST(GBufferTile, SetTileContextFillsEveryGlobal)
{
	SetTileContext(Kind::WallLeft, NormalWallLeft, 200, -12, FootKind::SW);
	EXPECT_EQ(CurrentTileAux, PackAux(Kind::WallLeft, NormalWallLeft));
	EXPECT_EQ(CurrentTileAuxBlended, PackAux(Kind::WallBlended, NormalWallLeft));
	EXPECT_EQ(CurrentTileLeftX, -12);
	std::array<uint8_t, 32> expected {};
	BuildBaseRow(expected, 200, FootKind::SW);
	EXPECT_EQ(CurrentTileBaseRow, expected);

	SetTileContext(Kind::WallRight, NormalWallRight, 40, 300, FootKind::SE);
	EXPECT_EQ(CurrentTileAux, PackAux(Kind::WallRight, NormalWallRight));
	EXPECT_EQ(CurrentTileAuxBlended, PackAux(Kind::WallBlended, NormalWallRight));
	EXPECT_EQ(CurrentTileLeftX, 300);
	BuildBaseRow(expected, 40, FootKind::SE);
	EXPECT_EQ(CurrentTileBaseRow, expected);

	for (const Kind kind : { Kind::Floor, Kind::WallTop, Kind::Unlit, Kind::Card, Kind::CardEmissive, Kind::WallBlended }) {
		SetTileContext(kind, NormalUp, 100, 0, FootKind::None);
		EXPECT_EQ(CurrentTileAux, PackAux(kind, NormalUp));
		EXPECT_EQ(CurrentTileAuxBlended, CurrentTileAux) << "non-wall kinds blend to themselves";
		for (const uint8_t b : CurrentTileBaseRow)
			EXPECT_EQ(b, 100);
	}
}

TEST_F(GBufferTest, AnchorArchOnWallLineReanchorsOnlyTheArchPixels)
{
	AllocatePlanes(320, 240);
	BeginHybridFrame(240);
	ASSERT_TRUE(CaptureActive());
	const Surface out = *surface;
	const int anchorRow = 200;
	const int leftX = 40;
	const uint8_t arch = PackAux(Kind::WallRight, NormalWallRight);
	const auto at = [&](uint8_t *plane, int x, int y) -> uint8_t & { return plane[static_cast<size_t>(y) * CurrentPlanes.pitch + x]; };
	// What the constant-aux arch blit leaves: the arch byte and B = the tile base on every opaque pixel...
	for (int y = 100; y <= anchorRow; ++y) {
		for (int x = leftX; x < leftX + 64; ++x) {
			at(CurrentPlanes.aux, x, y) = arch;
			at(CurrentPlanes.base, x, y) = static_cast<uint8_t>(anchorRow & 0xFF);
		}
	}
	// ...next to pixels it must not touch: a card the see-through arch kept, and a wall pixel with another B.
	at(CurrentPlanes.aux, leftX + 5, 150) = FlatCardByte;
	at(CurrentPlanes.base, leftX + 6, 150) = 77;
	AnchorArchOnWallLine(out, Rectangle { { leftX, 100 }, Size { 64, anchorRow - 100 + 1 } }, arch, anchorRow, leftX, FootKind::SE);
	for (int x = leftX; x < leftX + 64; ++x) {
		const int c = x - leftX;
		if (c == 5 || c == 6) continue;
		EXPECT_EQ(at(CurrentPlanes.base, x, 150), static_cast<uint8_t>(anchorRow - c / 2)) << "c " << c;
		// The left half joins the SE wall pieces of the cell exactly (same foot table).
		if (c < 32) EXPECT_EQ(at(CurrentPlanes.base, x, 150), static_cast<uint8_t>(anchorRow - FootRows(FootKind::SE, c)));
	}
	EXPECT_EQ(at(CurrentPlanes.base, leftX + 5, 150), static_cast<uint8_t>(anchorRow & 0xFF)) << "card pixels keep their B";
	EXPECT_EQ(at(CurrentPlanes.base, leftX + 6, 150), 77) << "pixels with another B were not written by the arch";

	// SW arches run the other way: (63 - c) / 2, joining the right half's SW foot table.
	EXPECT_EQ(ArchFootRows(FootKind::SW, 0), 31);
	EXPECT_EQ(ArchFootRows(FootKind::SW, 63), 0);
	for (int c = 32; c < 64; ++c)
		EXPECT_EQ(ArchFootRows(FootKind::SW, c), FootRows(FootKind::SW, c - 32));
	EXPECT_EQ(ArchFootRows(FootKind::None, 10), 0);

	// Outside CaptureWorld it does nothing.
	EndWorldPass();
	at(CurrentPlanes.base, leftX + 10, 120) = static_cast<uint8_t>(anchorRow & 0xFF);
	AnchorArchOnWallLine(out, Rectangle { { leftX, 100 }, Size { 64, 101 } }, arch, anchorRow, leftX, FootKind::SE);
	EXPECT_EQ(at(CurrentPlanes.base, leftX + 10, 120), static_cast<uint8_t>(anchorRow & 0xFF));
}

// ---- Normals ----

TEST(GBufferNormals, LutEntriesAreUnitLengthWithTheFixedSlots)
{
	const std::array<NormalLutEntry, 32> &lut = NormalLut();
	for (size_t i = 0; i < lut.size(); ++i) {
		const float length = std::sqrt(lut[i].x * lut[i].x + lut[i].y * lut[i].y + lut[i].z * lut[i].z);
		EXPECT_NEAR(length, 1.0F, 1e-5F) << "entry " << i;
		EXPECT_EQ(lut[i].pad, 0.0F);
	}
	EXPECT_NEAR(lut[NormalUp].x, 0.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalUp].y, 0.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalUp].z, 1.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalWallLeft].x, 0.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalWallLeft].y, 1.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalWallLeft].z, 0.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalWallRight].x, 1.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalWallRight].y, 0.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalWallRight].z, 0.0F, 1e-6F);
	EXPECT_NEAR(lut[NormalCardFlat].x, 0.6124F, 1e-4F);
	EXPECT_NEAR(lut[NormalCardFlat].y, 0.6124F, 1e-4F);
	EXPECT_NEAR(lut[NormalCardFlat].z, 0.5F, 1e-4F);

	// Entries 4..31 are distinct directions on the card hemisphere (dot with C' > 0).
	for (size_t i = 4; i < lut.size(); ++i) {
		EXPECT_GT(lut[i].x * lut[NormalCardFlat].x + lut[i].y * lut[NormalCardFlat].y + lut[i].z * lut[NormalCardFlat].z, 0.0F) << "entry " << i;
		for (size_t j = 3; j < i; ++j)
			EXPECT_GT(DegreesBetween(lut[i].x, lut[i].y, lut[i].z, lut[j].x, lut[j].y, lut[j].z), 5.0F) << "entries " << i << " and " << j;
	}
}

TEST(GBufferNormals, CardSignTest)
{
	// A light south of a card (toward the viewer and up), L = P + (1, 1, 0.5), must light the flat card.
	const NormalLutEntry &n = NormalLut()[NormalCardFlat];
	const float lx = 1.0F, ly = 1.0F, lz = 0.5F;
	const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
	EXPECT_GT(n.x * lx / len + n.y * ly / len + n.z * lz / len, 0.0F);
	// The rejected east/south/up variant (0, -0.866, 0.5) would point away: dot with (1, 1, 0.5) in tile space is negative
	// once mapped (east = (tx - ty)/sqrt2, south = (tx + ty)/sqrt2): south component of (1, 1, 0.5) is +1.414.
	EXPECT_LT(0.0F * 0.0F + (-0.866F) * (2.0F / std::sqrt(2.0F)) + 0.5F * 0.5F, 0.0F);
}

TEST(GBufferNormals, CardToTileMapsTheCardBasis)
{
	float x, y, z;
	CardToTile(1.0F, 0.0F, 0.0F, x, y, z);
	EXPECT_NEAR(x, 0.7071F, 1e-4F);
	EXPECT_NEAR(y, -0.7071F, 1e-4F);
	EXPECT_NEAR(z, 0.0F, 1e-4F);
	CardToTile(0.0F, 1.0F, 0.0F, x, y, z);
	EXPECT_NEAR(x, -0.3536F, 1e-4F);
	EXPECT_NEAR(y, -0.3536F, 1e-4F);
	EXPECT_NEAR(z, 0.8660F, 1e-4F);
	CardToTile(0.0F, 0.0F, 1.0F, x, y, z);
	EXPECT_NEAR(x, 0.6124F, 1e-4F);
	EXPECT_NEAR(y, 0.6124F, 1e-4F);
	EXPECT_NEAR(z, 0.5F, 1e-4F);
	EXPECT_EQ(QuantizeNormal(x, y, z), NormalCardFlat) << "the card normal quantises to the flat slot";

	// Orthonormal: lengths and the card-space up direction are preserved.
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
	for (int i = 0; i < 100; ++i) {
		const float cx = dist(rng), cy = dist(rng), cz = dist(rng);
		CardToTile(cx, cy, cz, x, y, z);
		EXPECT_NEAR(x * x + y * y + z * z, cx * cx + cy * cy + cz * cz, 1e-4F);
	}
	// Card "up" (cy) tilts the normal toward screen-up: tile z grows with cy.
	CardToTile(0.0F, 0.5F, 1.0F, x, y, z);
	EXPECT_GT(z, 0.5F);
	// Card "right" (cx) tilts toward screen-right: tile x - y grows with cx.
	CardToTile(0.5F, 0.0F, 1.0F, x, y, z);
	EXPECT_GT(x - y, 0.0F);
}

TEST(GBufferNormals, QuantizeAgreesWithTheArgmaxOnRandomDirections)
{
	std::mt19937 rng(12345);
	std::normal_distribution<float> gauss(0.0F, 1.0F);
	const std::array<NormalLutEntry, 32> &lut = NormalLut();
	int mismatches = 0;
	for (int i = 0; i < 10000; ++i) {
		float x = gauss(rng), y = gauss(rng), z = gauss(rng);
		const float len = std::sqrt(x * x + y * y + z * z);
		if (len < 1e-3F)
			continue;
		x /= len;
		y /= len;
		z /= len;
		const uint8_t expected = ReferenceArgmax(x, y, z);
		const uint8_t got = QuantizeNormal(x, y, z);
		ASSERT_GE(got, NormalCardFlat);
		ASSERT_LT(got, 32);
		if (got == expected)
			continue;
		// Only an exact tie may pick the other entry.
		const float dGot = x * lut[got].x + y * lut[got].y + z * lut[got].z;
		const float dExp = x * lut[expected].x + y * lut[expected].y + z * lut[expected].z;
		if (std::fabs(dGot - dExp) > 1e-6F)
			++mismatches;
	}
	EXPECT_EQ(mismatches, 0);

	// Scale invariance: the argmax does not depend on the input length.
	EXPECT_EQ(QuantizeNormal(0.3F, -0.2F, 0.9F), QuantizeNormal(30.0F, -20.0F, 90.0F));
	EXPECT_EQ(QuantizeNormal(0.3F, -0.2F, 0.9F), QuantizeNormal(0.003F, -0.002F, 0.009F));
}

TEST(GBufferNormals, QuantizationErrorBound)
{
	// Dense sweep of the card hemisphere in (elevation from C', azimuth) around the card basis.
	const float rx = 0.70710678F, ry = -0.70710678F, rz = 0.0F;
	const float ux = -0.35355339F, uy = -0.35355339F, uz = 0.86602540F;
	const float cx = 0.61237244F, cy = 0.61237244F, cz = 0.5F;
	const std::array<NormalLutEntry, 32> &lut = NormalLut();
	float worst80 = 0.0F;
	float worst90 = 0.0F;
	for (int ei = 0; ei <= 360; ++ei) { // 0..90° in 0.25° steps
		const float e = static_cast<float>(ei) * 0.25F * Pi / 180.0F;
		for (int ai = 0; ai < 720; ++ai) { // 0.5° steps
			const float a = static_cast<float>(ai) * 0.5F * Pi / 180.0F;
			const float x = std::cos(e) * cx + std::sin(e) * (std::cos(a) * rx + std::sin(a) * ux);
			const float y = std::cos(e) * cy + std::sin(e) * (std::cos(a) * ry + std::sin(a) * uy);
			const float z = std::cos(e) * cz + std::sin(e) * (std::cos(a) * rz + std::sin(a) * uz);
			const uint8_t idx = QuantizeNormal(x, y, z);
			ASSERT_GE(idx, NormalCardFlat);
			const float err = DegreesBetween(x, y, z, lut[idx].x, lut[idx].y, lut[idx].z);
			worst90 = std::max(worst90, err);
			if (ei <= 320)
				worst80 = std::max(worst80, err);
		}
	}
	// asks for <= 16° with 29 entries; a covering-density bound shows that is unreachable over a hemisphere
	// (see the LUT comment in gbuffer.cpp). The swept ring layout measures 18.6° over the whole hemisphere, against
	// 21.1° for the SPEC's literal {30°, 55°, 75°} rings.
	EXPECT_LE(worst90, 19.0F);
	EXPECT_LE(worst80, 19.0F);
	// The centre itself is exact and the flat slot wins around it.
	EXPECT_EQ(QuantizeNormal(cx, cy, cz), NormalCardFlat);
	EXPECT_EQ(QuantizeNormal(cx + 0.01F, cy - 0.01F, cz), NormalCardFlat);
}

TEST(GBufferNormals, QuantizeDegenerateInputsFallBackToFlat)
{
	EXPECT_EQ(QuantizeNormal(0.0F, 0.0F, 0.0F), NormalCardFlat);
	EXPECT_EQ(QuantizeNormal(std::nanf(""), 0.0F, 0.0F), NormalCardFlat);
	EXPECT_EQ(QuantizeNormal(INFINITY, 0.0F, 0.0F), NormalCardFlat);
	// The fixed non-card slots are never returned.
	EXPECT_GE(QuantizeNormal(0.0F, 0.0F, 1.0F), NormalCardFlat);
	EXPECT_GE(QuantizeNormal(0.0F, 1.0F, 0.0F), NormalCardFlat);
	EXPECT_GE(QuantizeNormal(-1.0F, -1.0F, -1.0F), NormalCardFlat);
}

// ---- Zoom ----

TEST_F(GBufferTest, ZoomInPlaceMatchesTheReferenceZoomAndLeavesPlaneBAlone)
{
	struct Case {
		int width, height;       // screen (planes / surface)
		int viewportWidth;       // after subtracting an open side panel
		int viewportOffsetX;     // SidePanelSize.width when the left panel is open
		int outHeight;           // gnViewportHeight
	};
	const Case cases[] = {
		{ 640, 480, 640, 0, 480 },   // full screen
		{ 640, 480, 640, 0, 352 },   // main panel below the viewport
		{ 640, 480, 320, 320, 352 }, // left panel open
		{ 640, 480, 320, 0, 352 },   // right panel open
		{ 641, 481, 641, 0, 481 },   // odd width and height (pitch is padded to 644)
		{ 641, 481, 321, 320, 481 }, // odd viewport with a left panel
		{ 639, 479, 319, 0, 479 },   // odd, right panel
		{ 641, 481, 641, 0, 353 },   // odd viewport height below an odd screen
		{ 800, 600, 800, 0, 600 },
	};
	for (const Case &c : cases) {
		SCOPED_TRACE(::testing::Message() << c.width << "x" << c.height << " vw " << c.viewportWidth << " off " << c.viewportOffsetX << " rows " << c.outHeight);
		AllocatePlanes(c.width, c.height);
		BeginHybridFrame(c.outHeight);
		const size_t bytes = PlaneBytes();
		FillPattern(surface->begin(), bytes, 0);
		FillPattern(CurrentPlanes.aux, bytes, 0);
		FillPattern(CurrentPlanes.base, bytes, 0x5A);

		ReferenceZoom(surface->subregionY(0, c.outHeight), c.viewportWidth, c.viewportOffsetX);
		ZoomInPlace(c.viewportWidth, c.viewportOffsetX, c.outHeight);

		size_t firstDiff = bytes;
		for (size_t i = 0; i < bytes; ++i) {
			if (CurrentPlanes.aux[i] != surface->begin()[i]) {
				firstDiff = i;
				break;
			}
		}
		EXPECT_EQ(firstDiff, bytes) << "plane A differs from the reference at byte " << firstDiff
		                            << " (row " << firstDiff / CurrentPlanes.pitch << ", col " << firstDiff % CurrentPlanes.pitch << ")";
		for (size_t i = 0; i < bytes; ++i)
			ASSERT_EQ(CurrentPlanes.base[i], static_cast<uint8_t>(PatternAt(i) ^ 0x5A)) << "plane B byte " << i << " must stay half-size";

		// Independently of the reference walk: every viewport pixel holds the source pixel of the quarter-size world
		// pass, and every pixel outside the zoomed viewport (side panel columns, rows below the viewport) is untouched.
		const size_t pitch = CurrentPlanes.pitch;
		for (int y = 0; y < c.height; ++y) {
			for (int x = 0; x < c.width; ++x) {
				const bool inViewport = y < c.outHeight && x >= c.viewportOffsetX && x < c.viewportOffsetX + c.viewportWidth;
				uint8_t expected;
				if (inViewport) {
					const int sx = ZoomSourceIndex(x - c.viewportOffsetX, c.viewportWidth);
					const int sy = ZoomSourceIndex(y, c.outHeight);
					expected = PatternAt(static_cast<size_t>(sy) * pitch + sx);
				} else {
					expected = PatternAt(static_cast<size_t>(y) * pitch + x);
				}
				ASSERT_EQ(AuxAt(x, y), expected) << "(" << x << ", " << y << ")";
			}
		}
		// The zoom really changed something (the pattern is not zoom-invariant).
		bool changed = false;
		for (size_t i = 0; i < bytes && !changed; ++i)
			changed = CurrentPlanes.aux[i] != static_cast<uint8_t>(PatternAt(i));
		EXPECT_TRUE(changed);
	}
}

TEST_F(GBufferTest, ZoomInPlaceIsGatedAndBoundsChecked)
{
	AllocatePlanes(640, 480);
	const size_t bytes = PlaneBytes();

	// Classic frame: no-op even though planes exist.
	LatchForFrame(DisabledInput());
	BeginWorldPass(480);
	FillPattern(CurrentPlanes.aux, bytes, 0);
	ZoomInPlace(640, 0, 480);
	for (size_t i = 0; i < bytes; ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], PatternAt(i)) << i;

	// Hybrid frame but impossible geometry: no-op rather than an out-of-bounds write.
	BeginHybridFrame(480);
	FillPattern(CurrentPlanes.aux, bytes, 0);
	ZoomInPlace(641, 0, 480);
	ZoomInPlace(640, 1, 480);
	ZoomInPlace(640, 0, 481);
	ZoomInPlace(0, 0, 480);
	ZoomInPlace(640, -1, 480);
	ZoomInPlace(640, 0, 0);
	for (size_t i = 0; i < bytes; ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], PatternAt(i)) << i;

	Free();
	ZoomInPlace(640, 0, 480); // no planes: no-op
}

// ---- Cursor ----

TEST_F(GBufferTest, SaveRestoreRectRoundTripsPlaneAOnly)
{
	AllocatePlanes(640, 480);
	BeginHybridFrame(480);
	const size_t bytes = PlaneBytes();
	FillPattern(CurrentPlanes.aux, bytes, 0);
	FillPattern(CurrentPlanes.base, bytes, 0x33);
	EndWorldPass();

	std::array<uint8_t, 8192> behind {};
	behind.fill(0xEE);
	const Rectangle rect { { 100, 100 }, { 40, 60 } };
	SaveRect(rect, behind);
	for (int y = 0; y < 60; ++y) {
		for (int x = 0; x < 40; ++x)
			ASSERT_EQ(behind[static_cast<size_t>(y) * 40 + x], AuxAt(100 + x, 100 + y)) << "(" << x << ", " << y << ")";
	}
	EXPECT_EQ(behind[40 * 60], 0xEE) << "nothing beyond the rect is written";

	// The cursor draw stamps the rect Unlit; UndrawCursor restores.
	StampUnlit(*surface, rect);
	EXPECT_EQ(AuxAt(100, 100), UnlitByte);
	EXPECT_EQ(AuxAt(139, 159), UnlitByte);
	RestoreRect(rect, behind);
	for (size_t i = 0; i < bytes; ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], PatternAt(i)) << "aux byte " << i;
	for (size_t i = 0; i < bytes; ++i)
		ASSERT_EQ(CurrentPlanes.base[i], static_cast<uint8_t>(PatternAt(i) ^ 0x33)) << "base byte " << i;

	// Clipped at the top-left and bottom-right corners: save and restore clip identically.
	for (const Rectangle clipped : { Rectangle { { -10, -20 }, { 40, 60 } }, Rectangle { { 620, 450 }, { 40, 60 } } }) {
		behind.fill(0xEE);
		SaveRect(clipped, behind);
		StampUnlit(*surface, clipped);
		RestoreRect(clipped, behind);
		for (size_t i = 0; i < bytes; ++i)
			ASSERT_EQ(CurrentPlanes.aux[i], PatternAt(i)) << "aux byte " << i;
		EXPECT_EQ(behind[30 * 40], 0xEE) << "only the clipped 30x40 pixels are stored";
	}

	// A buffer smaller than the rect only covers whole leading rows and never overflows.
	std::vector<uint8_t> small(100, 0xEE);
	SaveRect(rect, small);
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 40; ++x)
			ASSERT_EQ(small[static_cast<size_t>(y) * 40 + x], AuxAt(100 + x, 100 + y));
	}
	EXPECT_EQ(small[80], 0xEE);
	StampUnlit(*surface, rect);
	RestoreRect(rect, small);
	EXPECT_EQ(AuxAt(100, 100), PatternAt(100 * static_cast<size_t>(CurrentPlanes.pitch) + 100));
	EXPECT_EQ(AuxAt(100, 101), PatternAt(101 * static_cast<size_t>(CurrentPlanes.pitch) + 100));
	EXPECT_EQ(AuxAt(100, 102), UnlitByte) << "third row was not saved, so it is not restored";

	// Entirely off-screen rects are no-ops.
	behind.fill(0xEE);
	SaveRect({ { 700, 700 }, { 10, 10 } }, behind);
	EXPECT_EQ(behind[0], 0xEE);
	RestoreRect({ { 700, 700 }, { 10, 10 } }, behind);
}

TEST_F(GBufferTest, SaveRestoreRectAreGatedOnTheHybridFlag)
{
	AllocatePlanes(640, 480);
	LatchForFrame(DisabledInput());
	BeginWorldPass(480);
	ASSERT_FALSE(BackBufferIsHybrid());
	const size_t bytes = PlaneBytes();
	FillPattern(CurrentPlanes.aux, bytes, 0);

	std::array<uint8_t, 8192> behind {};
	behind.fill(0xEE);
	const Rectangle rect { { 100, 100 }, { 40, 60 } };
	SaveRect(rect, behind);
	for (const uint8_t b : behind)
		ASSERT_EQ(b, 0xEE);
	behind.fill(0x11);
	RestoreRect(rect, behind);
	for (size_t i = 0; i < bytes; ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], PatternAt(i)) << "aux byte " << i;

	// The flag (not Enabled()) is the gate: a re-present of a hybrid frame after the option was switched off still restores.
	BeginHybridFrame(480);
	FillPattern(CurrentPlanes.aux, bytes, 0);
	EndWorldPass();
	SaveRect(rect, behind);
	StampUnlit(*surface, rect);
	EndFrame();
	LatchForFrame(DisabledInput());
	ASSERT_FALSE(Enabled());
	ASSERT_TRUE(BackBufferIsHybrid());
	RestoreRect(rect, behind);
	for (size_t i = 0; i < bytes; ++i)
		ASSERT_EQ(CurrentPlanes.aux[i], PatternAt(i)) << "aux byte " << i;

	Free();
	SaveRect(rect, behind);
	RestoreRect(rect, behind); // no planes: no-op
}

// ---- Palette emissive ----

TEST(GBufferEmissive, HeuristicAndLevelOverrides)
{
	std::array<SDL_Color, 256> palette {};
	const auto set = [&palette](size_t i, uint8_t r, uint8_t g, uint8_t b) {
		palette[i].r = r;
		palette[i].g = g;
		palette[i].b = b;
	};
	set(0, 255, 255, 255);   // index 0 is never emissive, however bright
	set(5, 255, 220, 0);     // bright level entry: only emissive through the NEST/CRYPT override
	set(100, 255, 220, 0);   // bright but < 128: never emissive
	set(130, 255, 200, 0);   // bright saturated fire
	set(131, 20, 20, 20);    // dark
	set(132, 255, 255, 255); // bright unsaturated
	set(133, 255, 40, 0);    // saturated but too dark in luminance
	set(134, 200, 200, 255); // bright, slightly saturated
	set(135, 255, 255, 0);   // bright saturated yellow

	std::array<float, 256> out {};
	out.fill(-1.0F);
	BuildPaletteEmissive(palette, DTYPE_CATHEDRAL, out);
	for (size_t i = 0; i < 128; ++i)
		EXPECT_EQ(out[i], 0.0F) << "index " << i;
	EXPECT_GT(out[130], 0.0F);
	EXPECT_EQ(out[131], 0.0F);
	EXPECT_NEAR(out[132], 0.3F, 1e-5F) << "Y = 1, saturation 0";
	EXPECT_EQ(out[133], 0.0F);
	EXPECT_GT(out[134], 0.0F);
	EXPECT_LT(out[134], out[132]) << "less luminous than white and not saturated enough to make up for it";
	EXPECT_LT(out[130], out[132]) << "Y = 0.63 orange: the luminance ramp dominates the saturation bonus";
	EXPECT_GT(out[135], out[132]) << "at equal luminance saturation weighs in";
	for (size_t i = 136; i < 256; ++i)
		EXPECT_EQ(out[i], 0.0F) << "black index " << i;

	// Explicit value for index 130: lin(255) = 1, lin(200) = 0.5776, Y = 0.2126 + 0.7152 * 0.5776.
	const float lin200 = std::pow((200.0F / 255.0F + 0.055F) / 1.055F, 2.4F);
	const float y130 = 0.2126F + 0.7152F * lin200;
	const float expected130 = std::pow(std::clamp((y130 - 0.45F) / 0.55F, 0.0F, 1.0F), 1.5F) * (0.3F + 0.7F * 1.0F);
	EXPECT_NEAR(out[130], expected130, 1e-5F);

	for (const dungeon_type level : { DTYPE_NEST, DTYPE_CRYPT }) {
		out.fill(-1.0F);
		BuildPaletteEmissive(palette, level, out);
		EXPECT_EQ(out[0], 0.0F) << "level " << static_cast<int>(level);
		for (size_t i = 1; i <= 15; ++i)
			EXPECT_FLOAT_EQ(out[i], 0.4F) << "level " << static_cast<int>(level) << " index " << i;
		for (size_t i = 16; i < 128; ++i)
			EXPECT_EQ(out[i], 0.0F) << "level " << static_cast<int>(level) << " index " << i;
		EXPECT_NEAR(out[130], expected130, 1e-5F) << "the heuristic part is unchanged";
	}

	for (const dungeon_type level : { DTYPE_TOWN, DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CAVES, DTYPE_HELL }) {
		out.fill(-1.0F);
		BuildPaletteEmissive(palette, level, out);
		for (size_t i = 0; i < 128; ++i)
			EXPECT_EQ(out[i], 0.0F) << "level " << static_cast<int>(level) << " index " << i << ": no override";
		EXPECT_NEAR(out[130], expected130, 1e-5F);
	}

	// Every output is in [0, 1].
	for (const float v : out) {
		EXPECT_GE(v, 0.0F);
		EXPECT_LE(v, 1.0F);
	}
}

} // namespace
} // namespace devilution
