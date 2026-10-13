#include "engine/render/ui_scale_core.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace devilution {
namespace {

struct ScreenSize {
	int width;
	int height;
};

constexpr ScreenSize Screens[] = {
	{ 640, 480 }, { 1280, 720 }, { 1366, 768 }, { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 }, { 1727, 1117 },
};

TEST(UiScale, EffectiveScaleKeepsTheUiSpaceWideAndTall)
{
	EXPECT_EQ(EffectiveUiScaleQuarters(8, 640, 480), 4);
	EXPECT_EQ(EffectiveUiScaleQuarters(16, 640, 480), 4);
	EXPECT_EQ(EffectiveUiScaleQuarters(4, 3440, 1440), 4);
	EXPECT_EQ(EffectiveUiScaleQuarters(8, 1920, 1080), 8);
	EXPECT_EQ(EffectiveUiScaleQuarters(16, 1920, 1080), 9);  // 2.25x: 853 x 480
	EXPECT_EQ(EffectiveUiScaleQuarters(16, 3440, 1440), 12); // 3x: 1146 x 480
	EXPECT_EQ(EffectiveUiScaleQuarters(16, 3840, 2160), 16); // 4x: 960 x 540
	EXPECT_EQ(EffectiveUiScaleQuarters(8, 1280, 720), 6);    // 1.5x: 853 x 480
	EXPECT_EQ(EffectiveUiScaleQuarters(5, 1280, 720), 5);
	for (const ScreenSize screen : Screens) {
		for (int requested = 4; requested <= 16; ++requested) {
			const int q = EffectiveUiScaleQuarters(requested, screen.width, screen.height);
			ASSERT_GE(q, 4);
			ASSERT_LE(q, requested);
			if (q == 4)
				continue;
			const HdWorldTransform t = ComputeUiTransform(q, screen.width, screen.height);
			EXPECT_GE(t.world.width, UiSpaceMinWidth);
			EXPECT_GE(t.world.height, UiSpaceMinHeight);
		}
	}
}

TEST(UiScale, OneXIsTheIdentity)
{
	const HdWorldTransform t = ComputeUiTransform(4, 1920, 1080);
	EXPECT_TRUE(t.IsIdentity());
	EXPECT_EQ(t.world.width, 1920);
	EXPECT_EQ(t.world.height, 1080);
	EXPECT_EQ(ScreenToUiX(t, 1234), 1234);
	EXPECT_EQ(UiToScreenCentreY(t, 777), 777);
}

TEST(UiScale, UiSpaceCoversTheScreenWithTheBottomRightEdgesAligned)
{
	for (const ScreenSize screen : Screens) {
		for (int q = 4; q <= 16; ++q) {
			const HdWorldTransform t = ComputeUiTransform(q, screen.width, screen.height);
			SCOPED_TRACE(testing::Message() << screen.width << "x" << screen.height << " q=" << q);
			// The last UI column / row ends exactly at the screen edge: the bottom-anchored control panel is not cut.
			EXPECT_EQ(t.ToScreenX(t.world.width), screen.width);
			EXPECT_EQ(t.ToScreenY(t.world.height), screen.height);
			EXPECT_EQ(t.world.width, screen.width * 4 / q);
			EXPECT_EQ(t.world.height, screen.height * 4 / q);
			// Every UI pixel covers floor(q / 4) or ceil(q / 4) screen pixels; the first one also the remainder.
			const std::vector<int> columns = UiScreenEdges(t, false);
			EXPECT_EQ(columns.front(), 0);
			EXPECT_EQ(columns.back(), screen.width);
			for (size_t u = 0; u + 1 < columns.size(); ++u) {
				const int width = columns[u + 1] - columns[u];
				ASSERT_GE(width, q / 4) << u;
				if (u > 0)
					ASSERT_LE(width, (q + 3) / 4) << u;
			}
			const std::vector<int> rows = UiScreenEdges(t, true);
			EXPECT_EQ(rows.front(), 0);
			EXPECT_EQ(rows.back(), screen.height);
			for (size_t u = 0; u + 1 < rows.size(); ++u)
				ASSERT_GE(rows[u + 1] - rows[u], q / 4) << u;
		}
	}
}

TEST(UiScale, EveryScreenPixelMapsIntoTheUiPixelThatCoversIt)
{
	for (const ScreenSize screen : Screens) {
		for (int q = 5; q <= 16; ++q) {
			const HdWorldTransform t = ComputeUiTransform(q, screen.width, screen.height);
			for (int x = 0; x < screen.width; ++x) {
				const int u = ScreenToUiX(t, x);
				ASSERT_GE(x, UiEdgeX(t, u)) << screen.width << " q=" << q << " x=" << x;
				ASSERT_LT(x, UiEdgeX(t, u + 1)) << screen.width << " q=" << q << " x=" << x;
			}
			for (int y = 0; y < screen.height; ++y) {
				const int u = ScreenToUiY(t, y);
				ASSERT_GE(y, UiEdgeY(t, u));
				ASSERT_LT(y, UiEdgeY(t, u + 1));
			}
		}
	}
}

TEST(UiScale, UiPixelCentresMapBackToTheSameUiPixel)
{
	for (const ScreenSize screen : Screens) {
		for (int q = 5; q <= 16; ++q) {
			const HdWorldTransform t = ComputeUiTransform(q, screen.width, screen.height);
			for (int u = 0; u < t.world.width; ++u)
				ASSERT_EQ(ScreenToUiX(t, UiToScreenCentreX(t, u)), u) << screen.width << " q=" << q;
			for (int u = 0; u < t.world.height; ++u)
				ASSERT_EQ(ScreenToUiY(t, UiToScreenCentreY(t, u)), u) << screen.height << " q=" << q;
		}
	}
}

TEST(UiScale, IntegerScalesHaveSquareBlocks)
{
	const HdWorldTransform t = ComputeUiTransform(8, 3440, 1440);
	EXPECT_EQ(t.world.width, 1720);
	EXPECT_EQ(t.world.height, 720);
	EXPECT_EQ(t.offsetX, 0);
	EXPECT_EQ(t.offsetY, 0);
	for (int u = 0; u < 1720; ++u)
		ASSERT_EQ(t.ToScreenX(u), 2 * u);
}

std::vector<uint8_t> Pattern(int pitch, int height)
{
	std::vector<uint8_t> buffer(static_cast<size_t>(pitch) * height);
	for (size_t i = 0; i < buffer.size(); ++i)
		buffer[i] = static_cast<uint8_t>((i * 37 + i / pitch * 11) % 251 + 1);
	return buffer;
}

TEST(UiScale, CompositeWritesExactlyTheBlocksOfTheDrawnUiPixels)
{
	for (const int q : { 5, 6, 8, 10, 12 }) {
		SCOPED_TRACE(testing::Message() << "q=" << q);
		constexpr int Width = 803;
		constexpr int Height = 611;
		constexpr int Pitch = 816;
		const HdWorldTransform t = ComputeUiTransform(q, Width, Height);
		const std::vector<uint8_t> world = Pattern(Pitch, Height);
		std::vector<uint8_t> screen = world;

		const int uiPitch = t.world.width + 3;
		std::vector<uint8_t> baseline(static_cast<size_t>(uiPitch) * t.world.height);
		UiSampleScreen(screen.data(), Pitch, baseline.data(), uiPitch, t);
		for (int y = 0; y < t.world.height; ++y) {
			for (int x = 0; x < t.world.width; ++x) {
				const int sx = UiEdgeX(t, x);
				const int sy = UiEdgeY(t, y);
				ASSERT_EQ(baseline[static_cast<size_t>(y) * uiPitch + x], world[static_cast<size_t>(sy) * Pitch + sx]);
			}
		}

		// The UI draws a box (including the top-left and bottom-right UI pixels) and one pixel.
		std::vector<uint8_t> ui = baseline;
		const auto drawn = [&](int x, int y) {
			return (x >= 10 && x < 40 && y >= 20 && y < 33) || (x == 0 && y == 0) || (x == t.world.width - 1 && y == t.world.height - 1)
			    || (x == 100 && y == 50);
		};
		for (int y = 0; y < t.world.height; ++y) {
			for (int x = 0; x < t.world.width; ++x) {
				if (drawn(x, y))
					ui[static_cast<size_t>(y) * uiPitch + x] = static_cast<uint8_t>(baseline[static_cast<size_t>(y) * uiPitch + x] ^ 0x80);
			}
		}

		std::vector<int> writtenRows(Height, 0);
		UiComposite(screen.data(), Pitch, nullptr, ui.data(), baseline.data(), uiPitch, t, [&](const uint8_t *dst, int n) {
			const size_t offset = static_cast<size_t>(dst - screen.data());
			writtenRows[offset / Pitch] += n;
		});
		std::vector<int> expectedRows(Height, 0);
		for (int sy = 0; sy < Height; ++sy) {
			const int uy = ScreenToUiY(t, sy);
			for (int sx = 0; sx < Width; ++sx) {
				const int ux = ScreenToUiX(t, sx);
				const uint8_t got = screen[static_cast<size_t>(sy) * Pitch + sx];
				if (drawn(ux, uy)) {
					++expectedRows[sy];
					ASSERT_EQ(got, ui[static_cast<size_t>(uy) * uiPitch + ux]) << sx << "," << sy;
				} else {
					ASSERT_EQ(got, world[static_cast<size_t>(sy) * Pitch + sx]) << sx << "," << sy; // full resolution kept
				}
			}
		}
		EXPECT_EQ(writtenRows, expectedRows);
		// The padding past the screen width is never touched.
		for (int sy = 0; sy < Height; ++sy)
			for (int sx = Width; sx < Pitch; ++sx)
				ASSERT_EQ(screen[static_cast<size_t>(sy) * Pitch + sx], world[static_cast<size_t>(sy) * Pitch + sx]);

		// A re-present restores the world where the UI no longer draws (the old cursor) from the copy.
		std::vector<uint8_t> uiWithoutPixel = ui;
		uiWithoutPixel[static_cast<size_t>(50) * uiPitch + 100] = baseline[static_cast<size_t>(50) * uiPitch + 100];
		UiComposite(screen.data(), Pitch, world.data(), uiWithoutPixel.data(), baseline.data(), uiPitch, t, [](const uint8_t *, int) { });
		for (int sy = 0; sy < Height; ++sy) {
			for (int sx = 0; sx < Width; ++sx) {
				const int ux = ScreenToUiX(t, sx);
				const int uy = ScreenToUiY(t, sy);
				if (!drawn(ux, uy) || (ux == 100 && uy == 50))
					ASSERT_EQ(screen[static_cast<size_t>(sy) * Pitch + sx], world[static_cast<size_t>(sy) * Pitch + sx]);
			}
		}
	}
}

} // namespace
} // namespace devilution
