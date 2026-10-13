/**
 * @file hd_view_transform.hpp
 *
 * The world-pass-to-screen transform shared by every DamnationX HD pass (floors, sprites, objects, tiles).
 *
 * The classic world pass (scrollrt.cpp DrawGame) draws into the top-left of the 8-bit back buffer. Without zoom that
 * is the screen. With zoom level k > 1 (GetZoomQuarters() / 4, fractional with the HD art) it draws the view at 1/k size
 * there and Zoom() then scales it up k times in place (nearest) into the viewport (shifted right by the left side panel when one
 * is open, HdUpscaleInPlace). HD passes hook the
 * world pass, so everything they learn there (draw positions, pixel masks, snapshots) is in world-pass pixels; they
 * must map it to the final screen with this transform before the presenter draws at canvas resolution:
 *
 *     screen = world * scale + offset        (pixel edges, per axis)
 *
 * World pixel p covers the screen pixels [p * scale + offset, (p + 1) * scale + offset), clipped to `viewport`.
 * Zoom() rounds the source up and gives the first source column/row only the remainder (w - k * (ceil(w / k) - 1)
 * output pixels); that is the negative part of `offset` here, so the mapping is exact for every viewport size.
 *
 * Usage (see the hd-zoom notes in the function docs):
 *  - DrawGame publishes the frame's transform with HdSetWorldTransform() before the world pass, so HD code may read
 *    HdCurrentWorldTransform() while recording and after Zoom().
 *  - Positions and lengths: ToScreenX/ToScreenY/ToScreenLength (floats, sub-pixel exact).
 *  - Per-pixel data recorded in world pixels (masks, snapshots, metadata grids): HdZoomGrid() resamples it the way
 *    Zoom() resamples the colour buffer (nearest), WorldX/WorldY give the source pixel of a screen pixel.
 *  - HD art resolution: Scale() screen pixels per world pixel times the canvas scale is the texel density to load
 *    (a zoomed view shows 2x the HD detail per world pixel).
 *
 * Pure data: no game state, so it is unit tested natively (test/hd_view_transform_test.cpp).
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace devilution {

/** A rectangle in back-buffer (or world-pass) pixels. */
struct HdRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	[[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
	[[nodiscard]] bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

struct HdWorldTransform {
	/** Screen pixels per world-pass pixel, in quarters: the zoom level times 4 (4 without zoom, 5 = 1.25x). */
	int quarters = 4;
	/** Screen position of world pixel 0's left / top edge (<= the viewport origin: it gets only the remainder). */
	int offsetX = 0;
	int offsetY = 0;
	/** Screen pixels the world pass ends up in (the zoomed viewport; the whole view without zoom). */
	HdRect viewport;
	/** World-pass pixels that reach the screen (Zoom()'s source: the top-left ceil(4 w / q) x ceil(4 h / q)). */
	HdRect world;

	[[nodiscard]] bool IsIdentity() const { return quarters == 4 && offsetX == 0 && offsetY == 0; }
	/** Screen pixels per world pixel (the zoom level). */
	[[nodiscard]] float Scale() const { return static_cast<float>(quarters) / 4.0F; }

	[[nodiscard]] float ToScreenX(float worldX) const { return worldX * Scale() + static_cast<float>(offsetX); }
	[[nodiscard]] float ToScreenY(float worldY) const { return worldY * Scale() + static_cast<float>(offsetY); }
	[[nodiscard]] float ToScreenLength(float worldLength) const { return worldLength * Scale(); }
	/** The first screen column showing world column `worldX` (its left edge; exact, see WorldX). */
	[[nodiscard]] int ToScreenX(int worldX) const { return offsetX + CeilDiv(worldX * quarters, 4); }
	[[nodiscard]] int ToScreenY(int worldY) const { return offsetY + CeilDiv(worldY * quarters, 4); }

	/** The world-pass pixel shown at screen column `screenX` (floor division; only meaningful inside `viewport`). */
	[[nodiscard]] int WorldX(int screenX) const { return FloorDiv((screenX - offsetX) * 4, quarters); }
	[[nodiscard]] int WorldY(int screenY) const { return FloorDiv((screenY - offsetY) * 4, quarters); }

	/** The screen pixels covering world rectangle `rect`, clipped to `viewport` (empty when nothing is shown). */
	[[nodiscard]] HdRect ToScreen(const HdRect &rect) const
	{
		const int left = std::max(ToScreenX(rect.x), viewport.x);
		const int top = std::max(ToScreenY(rect.y), viewport.y);
		const int right = std::min(ToScreenX(rect.x + rect.width), viewport.x + viewport.width);
		const int bottom = std::min(ToScreenY(rect.y + rect.height), viewport.y + viewport.height);
		if (right <= left || bottom <= top)
			return { left, top, 0, 0 };
		return { left, top, right - left, bottom - top };
	}

	[[nodiscard]] static int FloorDiv(int value, int divisor)
	{
		const int quotient = value / divisor;
		return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
	}
	[[nodiscard]] static int CeilDiv(int value, int divisor) { return -FloorDiv(-value, divisor); }
};

/**
 * @brief The transform of a frame.
 * @param zoomQuarters screen pixels per world pixel times 4 (GetZoomQuarters(); 4 = no zoom, 5 = 1.25x)
 * @param viewWidth width of the output (the back buffer width)
 * @param viewportHeight rows of the world view (gnViewportHeight)
 * @param zoomViewportWidth, zoomViewportOffsetX Zoom()'s geometry (scrollrt.cpp CalcZoomGeometry: the width left of
 *        open side panels and the left panel's width); ignored without zoom
 */
[[nodiscard]] inline HdWorldTransform ComputeHdWorldTransform(int zoomQuarters, int viewWidth, int viewportHeight, int zoomViewportWidth, int zoomViewportOffsetX)
{
	HdWorldTransform transform;
	if (zoomQuarters <= 4) {
		transform.viewport = { 0, 0, viewWidth, viewportHeight };
		transform.world = transform.viewport;
		return transform;
	}
	// Zoom() (scrollrt.cpp, HdUpscaleInPlace): the source is the top-left ceil(w / k) x ceil(h / k) world pixels; the
	// first source column (row) gets the remainder, every other one k output pixels (k * 4 = q quarters; at a fractional
	// k the columns alternate between floor(k) and ceil(k) pixels). World p's first screen pixel is offset + ceil(p q / 4).
	const int q = zoomQuarters;
	const int worldWidth = HdWorldTransform::CeilDiv(zoomViewportWidth * 4, q);
	const int worldHeight = HdWorldTransform::CeilDiv(viewportHeight * 4, q);
	transform.quarters = q;
	transform.offsetX = zoomViewportOffsetX + zoomViewportWidth - HdWorldTransform::CeilDiv(worldWidth * q, 4);
	transform.offsetY = viewportHeight - HdWorldTransform::CeilDiv(worldHeight * q, 4);
	transform.viewport = { zoomViewportOffsetX, 0, zoomViewportWidth, viewportHeight };
	transform.world = { 0, 0, worldWidth, worldHeight };
	return transform;
}

/** The transform of the current (or last) world pass; DrawGame sets it before drawing the world. */
[[nodiscard]] const HdWorldTransform &HdCurrentWorldTransform();
void HdSetWorldTransform(const HdWorldTransform &transform);

/**
 * @brief Resamples a grid recorded in world-pass pixels to screen pixels, nearest (exactly as Zoom() resamples the
 * colour buffer).
 *
 * Every pixel of `dst` (dstWidth x dstHeight, row pitch dstPitch elements) inside `transform.viewport` becomes
 * `map(src[world pixel])`; every other pixel becomes `empty`. `src` holds at least the `transform.world` rectangle
 * with row pitch srcPitch. `src` and `dst` must not overlap.
 */
template <typename T, typename Map>
void HdZoomGrid(const T *src, size_t srcPitch, T *dst, size_t dstPitch, int dstWidth, int dstHeight, const HdWorldTransform &transform,
    const T &empty, Map &&map)
{
	const HdRect &viewport = transform.viewport;
	for (int y = 0; y < dstHeight; ++y) {
		T *row = dst + static_cast<size_t>(y) * dstPitch;
		if (y < viewport.y || y >= viewport.y + viewport.height) {
			std::fill_n(row, dstWidth, empty);
			continue;
		}
		const int worldY = std::clamp(transform.WorldY(y), transform.world.y, transform.world.y + transform.world.height - 1);
		const T *srcRow = src + static_cast<size_t>(worldY) * srcPitch;
		for (int x = 0; x < dstWidth; ++x) {
			if (x < viewport.x || x >= viewport.x + viewport.width) {
				row[x] = empty;
				continue;
			}
			const int worldX = std::clamp(transform.WorldX(x), transform.world.x, transform.world.x + transform.world.width - 1);
			row[x] = map(srcRow[worldX]);
		}
	}
}

/**
 * @brief Scales the top-left world rectangle of a pixel grid up in place, nearest, exactly as `transform` maps it
 * (scrollrt's Zoom() and the plane zooms; any zoom level, also fractional).
 *
 * Writes the `transform.viewport` pixels (rows from 0); the source is `transform.world` of the same grid. Every
 * destination pixel is at or after its source (the zoom is >= 1 and the offset <= the viewport origin), so walking
 * backwards never reads an overwritten source.
 */
template <typename T>
void HdUpscaleInPlace(T *pixels, size_t pitch, const HdWorldTransform &transform)
{
	const HdRect &viewport = transform.viewport;
	if (transform.quarters <= 4 || viewport.empty() || viewport.x < 0 || viewport.y != 0)
		return;
	std::vector<int> sourceColumn(static_cast<size_t>(viewport.width));
	for (int x = 0; x < viewport.width; ++x)
		sourceColumn[static_cast<size_t>(x)] = std::clamp(transform.WorldX(viewport.x + x), 0, transform.world.width - 1);
	int previousSourceRow = -1;
	for (int y = viewport.height - 1; y >= 0; --y) {
		const int sourceRow = std::clamp(transform.WorldY(y), 0, transform.world.height - 1);
		T *row = pixels + static_cast<size_t>(y) * pitch + viewport.x;
		if (sourceRow == previousSourceRow) {
			std::copy_n(row + pitch, viewport.width, row);
			continue;
		}
		const T *src = pixels + static_cast<size_t>(sourceRow) * pitch;
		for (int x = viewport.width - 1; x >= 0; --x)
			row[x] = src[sourceColumn[static_cast<size_t>(x)]];
		previousSourceRow = sourceRow;
	}
}

} // namespace devilution
