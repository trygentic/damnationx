/**
 * @file ui_scale_core.hpp
 *
 * The pure geometry of the in-game UI scale (ui_scale.hpp): which scale applies to a screen, how screen pixels map to
 * UI pixels and back, and how the UI layer is sampled from and composited onto the back buffer.
 *
 * The in-game UI lays out and draws in a smaller "UI space" (the screen divided by the scale, rounded down) and is
 * scaled up nearest onto the screen. The mapping is an HdWorldTransform like the world zoom's: UI pixel u > 0 covers
 * the screen pixels [ToScreenX(u), ToScreenX(u + 1)) (at a fractional scale the widths alternate between the two
 * nearest whole numbers), UI pixel 0 covers [0, ToScreenX(1)) and so also takes the remainder. The bottom and right
 * edges (the control panel) line up with the screen exactly and every UI pixel shows at every quarter scale.
 *
 * Pure data: no game state, covered by test/ui_scale_test.cpp.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "engine/render/hd_view_transform.hpp"

namespace devilution {

/**
 * The UI space keeps at least this many columns / rows. Wider than 640 means the classic panels never cover the view
 * (CanPanelsCoverView() stays false and the world keeps the whole screen), so the world geometry does not depend on
 * the UI scale; 480 rows fit every panel and dialog.
 */
constexpr int UiSpaceMinWidth = 641;
constexpr int UiSpaceMinHeight = 480;

/** The largest scale in quarters, at most `requestedQuarters`, whose UI space still is UiSpaceMinWidth x UiSpaceMinHeight. */
[[nodiscard]] inline int EffectiveUiScaleQuarters(int requestedQuarters, int screenWidth, int screenHeight)
{
	for (int q = requestedQuarters; q > 4; --q) {
		if (screenWidth * 4 / q >= UiSpaceMinWidth && screenHeight * 4 / q >= UiSpaceMinHeight)
			return q;
	}
	return 4;
}

/**
 * The UI to screen transform: `world` is the UI space (top-left at 0, 0, the screen times 4 / q rounded down),
 * `viewport` the whole screen. `offset` >= 0 is the remainder UI pixel 0 gets on top of its own width.
 */
[[nodiscard]] inline HdWorldTransform ComputeUiTransform(int quarters, int screenWidth, int screenHeight)
{
	HdWorldTransform t;
	t.quarters = quarters;
	t.viewport = { 0, 0, screenWidth, screenHeight };
	t.world = { 0, 0, std::max(screenWidth * 4 / quarters, 1), std::max(screenHeight * 4 / quarters, 1) };
	t.offsetX = screenWidth - HdWorldTransform::CeilDiv(t.world.width * quarters, 4);
	t.offsetY = screenHeight - HdWorldTransform::CeilDiv(t.world.height * quarters, 4);
	return t;
}

/** The first screen column of UI column u (u = UI width: the screen width). */
[[nodiscard]] inline int UiEdgeX(const HdWorldTransform &t, int u)
{
	return u <= 0 ? 0 : std::min(t.ToScreenX(u), t.viewport.width);
}

[[nodiscard]] inline int UiEdgeY(const HdWorldTransform &t, int u)
{
	return u <= 0 ? 0 : std::min(t.ToScreenY(u), t.viewport.height);
}

/** The UI column / row shown at a screen column / row (clamped into the UI space). */
[[nodiscard]] inline int ScreenToUiX(const HdWorldTransform &t, int screenX)
{
	return std::clamp(t.WorldX(screenX), 0, t.world.width - 1);
}

[[nodiscard]] inline int ScreenToUiY(const HdWorldTransform &t, int screenY)
{
	return std::clamp(t.WorldY(screenY), 0, t.world.height - 1);
}

/** The screen pixel at the centre of UI pixel (uiX, uiY) (where a warped mouse lands). */
[[nodiscard]] inline int UiToScreenCentreX(const HdWorldTransform &t, int uiX)
{
	return std::clamp((UiEdgeX(t, uiX) + UiEdgeX(t, uiX + 1) - 1) / 2, 0, t.viewport.width - 1);
}

[[nodiscard]] inline int UiToScreenCentreY(const HdWorldTransform &t, int uiY)
{
	return std::clamp((UiEdgeY(t, uiY) + UiEdgeY(t, uiY + 1) - 1) / 2, 0, t.viewport.height - 1);
}

/** The screen columns [first[u], first[u + 1]) of every UI column u (UI width + 1 entries; also used for rows). */
[[nodiscard]] inline std::vector<int> UiScreenEdges(const HdWorldTransform &t, bool rows)
{
	const int count = rows ? t.world.height : t.world.width;
	std::vector<int> edges(static_cast<size_t>(count) + 1);
	for (int u = 0; u <= count; ++u)
		edges[static_cast<size_t>(u)] = rows ? UiEdgeY(t, u) : UiEdgeX(t, u);
	return edges;
}

/**
 * @brief Samples the screen into the UI space, nearest: UI pixel u gets the first screen pixel it covers.
 *
 * This is the backdrop the UI is drawn over (half transparent boxes blend with it) and the baseline the composite
 * compares against.
 */
inline void UiSampleScreen(const uint8_t *screen, size_t screenPitch, uint8_t *ui, size_t uiPitch, const HdWorldTransform &t)
{
	const std::vector<int> columns = UiScreenEdges(t, false);
	const std::vector<int> rows = UiScreenEdges(t, true);
	for (int y = 0; y < t.world.height; ++y) {
		const uint8_t *src = screen + static_cast<size_t>(std::min(rows[static_cast<size_t>(y)], t.viewport.height - 1)) * screenPitch;
		uint8_t *dst = ui + static_cast<size_t>(y) * uiPitch;
		for (int x = 0; x < t.world.width; ++x)
			dst[x] = src[std::min(columns[static_cast<size_t>(x)], t.viewport.width - 1)];
	}
}

/**
 * @brief Scales the UI layer up onto the screen, nearest.
 *
 * Every UI pixel that differs from `baseline` (what UiSampleScreen took from the screen) was drawn by the UI and fills
 * its block of screen pixels; every other block keeps the full-resolution screen. With `restore` (not null) the
 * untouched blocks are copied back from it (a screen without UI, same pitch as `screen`) instead of kept.
 * `written(dst, n)` is called for every run of screen pixels the UI wrote (the hybrid G-buffer marks them unlit).
 */
template <typename Written>
void UiComposite(uint8_t *screen, size_t screenPitch, const uint8_t *restore, const uint8_t *ui, const uint8_t *baseline, size_t uiPitch,
    const HdWorldTransform &t, Written &&written)
{
	const std::vector<int> columns = UiScreenEdges(t, false);
	const std::vector<int> rows = UiScreenEdges(t, true);
	const size_t width = static_cast<size_t>(t.world.width);
	for (int y = 0; y < t.world.height; ++y) {
		const int top = rows[static_cast<size_t>(y)];
		const int bottom = rows[static_cast<size_t>(y) + 1];
		if (top >= bottom)
			continue;
		const uint8_t *uiRow = ui + static_cast<size_t>(y) * uiPitch;
		const uint8_t *baseRow = baseline + static_cast<size_t>(y) * uiPitch;
		const bool anyUi = std::memcmp(uiRow, baseRow, width) != 0;
		for (int sy = top; sy < bottom; ++sy) {
			uint8_t *dst = screen + static_cast<size_t>(sy) * screenPitch;
			if (restore != nullptr)
				std::memcpy(dst, restore + static_cast<size_t>(sy) * screenPitch, static_cast<size_t>(t.viewport.width));
			if (!anyUi)
				continue;
			int x = 0;
			while (x < t.world.width) {
				if (uiRow[x] == baseRow[x]) {
					++x;
					continue;
				}
				const int runStart = columns[static_cast<size_t>(x)];
				while (x < t.world.width && uiRow[x] != baseRow[x]) {
					const int left = columns[static_cast<size_t>(x)];
					std::memset(dst + left, uiRow[x], static_cast<size_t>(columns[static_cast<size_t>(x) + 1] - left));
					++x;
				}
				const int runEnd = columns[static_cast<size_t>(x)];
				if (runEnd > runStart)
					written(dst + runStart, runEnd - runStart);
			}
		}
	}
}

} // namespace devilution
