#include "engine/render/primitive_render.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/size.hpp"
#include "engine/surface.hpp"
#include "utils/attributes.h"
#include "utils/palette_blending.hpp"

namespace devilution {
namespace {

/**
 * @brief Tags the pixels a primitive just wrote as Unlit in the G-buffer.
 *
 * `rect` is in `out` coordinates and may be unclipped; StampUnlit clips it. Classic mode costs one predictable branch.
 */
DVL_ALWAYS_INLINE void StampUnlitRect(const Surface &out, Rectangle rect)
{
	if (gbuffer::StampActive())
		gbuffer::StampUnlit(out, rect);
}

/**
 * @brief Tags a single pixel `pix` of `out` (already bounds-checked against `out`) as Unlit in the G-buffer.
 */
DVL_ALWAYS_INLINE void StampUnlitPixel(const Surface &out, const uint8_t *pix)
{
	if (!gbuffer::StampActive() || !gbuffer::CurrentPlanes.covers(out))
		return;
	const gbuffer::Planes &planes = gbuffer::CurrentPlanes;
	const ptrdiff_t offset = pix - planes.outBase;
	if (offset >= 0 && offset < static_cast<ptrdiff_t>(static_cast<size_t>(planes.pitch) * planes.height))
		planes.aux[offset] = gbuffer::UnlitByte;
}

void DrawHalfTransparentUnalignedBlendedRectTo(const Surface &out, unsigned sx, unsigned sy, unsigned width, unsigned height, uint8_t color)
{
	uint8_t *pix = out.at(static_cast<int>(sx), static_cast<int>(sy));
	const uint8_t *const lookupTable = paletteTransparencyLookup[color];
	const unsigned skipX = out.pitch() - width;
	for (unsigned y = 0; y < height; ++y) {
		for (unsigned x = 0; x < width; ++x, ++pix) {
			*pix = lookupTable[*pix];
		}
		pix += skipX;
	}
}

#if DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT
// Expects everything to be 4-byte aligned.
void DrawHalfTransparentAligned32BlendedRectTo(const Surface &out, unsigned sx, unsigned sy, unsigned width, unsigned height)
{
	assert(out.pitch() % 4 == 0);

	auto *pix = reinterpret_cast<uint32_t *>(out.at(static_cast<int>(sx), static_cast<int>(sy)));
	assert(reinterpret_cast<intptr_t>(pix) % 4 == 0);

	const uint16_t *lookupTable = paletteTransparencyLookupBlack16;

	const unsigned skipX = (out.pitch() - width) / 4;
	width /= 4;
	while (height-- > 0) {
		for (unsigned i = 0; i < width; ++i, ++pix) {
			const uint32_t v = *pix;
			*pix = lookupTable[v & 0xFFFF] | (lookupTable[(v >> 16) & 0xFFFF] << 16);
		}
		pix += skipX;
	}
}

void DrawHalfTransparentBlendedRectTo(const Surface &out, unsigned sx, unsigned sy, unsigned width, unsigned height)
{
	// All SDL surfaces are 4-byte aligned and divisible by 4.
	// However, our coordinates and widths may not be.

	// First, draw the leading unaligned part.
	if (sx % 4 != 0) {
		const unsigned w = 4 - (sx % 4);
		DrawHalfTransparentUnalignedBlendedRectTo(out, sx, sy, w, height, 0);
		sx += w;
		width -= w;
	}

	if (static_cast<int>(sx + width) == out.w()) {
		// The pitch is 4-byte aligned, so we can simply extend the width to the pitch.
		width = out.pitch() - sx;
	} else if (width % 4 != 0) {
		// Draw the trailing unaligned part.
		const unsigned w = width % 4;
		DrawHalfTransparentUnalignedBlendedRectTo(out, sx + ((width / 4) * 4), sy, w, height, 0);
		width -= w;
	}

	// Now everything is divisible by 4. Draw the aligned part.
	DrawHalfTransparentAligned32BlendedRectTo(out, sx, sy, width, height);
}
#else
#define DrawHalfTransparentBlendedRectTo DrawHalfTransparentUnalignedBlendedRectTo
#endif

} // namespace

void FillRect(const Surface &out, int x, int y, int width, int height, uint8_t colorIndex)
{
	for (int j = 0; j < height; j++) {
		DrawHorizontalLine(out, { x, y + j }, width, colorIndex);
	}
}

void DrawHorizontalLine(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	if (from.y < 0 || from.y >= out.h() || from.x >= out.w() || width <= 0 || from.x + width <= 0)
		return;
	if (from.x < 0) {
		width += from.x;
		from.x = 0;
	}
	if (from.x + width > out.w())
		width = out.w() - from.x;
	return UnsafeDrawHorizontalLine(out, from, width, colorIndex);
}

void UnsafeDrawHorizontalLine(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	std::memset(&out[from], colorIndex, width);
	StampUnlitRect(out, { from, { width, 1 } });
}

void DrawVerticalLine(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	if (from.x < 0 || from.x >= out.w() || from.y >= out.h() || height <= 0 || from.y + height <= 0)
		return;
	if (from.y < 0) {
		height += from.y;
		from.y = 0;
	}
	if (from.y + height > out.h())
		height = (from.y + height) - out.h();
	return UnsafeDrawVerticalLine(out, from, height, colorIndex);
}

void UnsafeDrawVerticalLine(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	auto *dst = &out[from];
	const auto pitch = out.pitch();
	StampUnlitRect(out, { from, { 1, height } });
	while (height-- > 0) {
		*dst = colorIndex;
		dst += pitch;
	}
}

void DrawHalfTransparentHorizontalLine(const Surface &out, Point from, int width, uint8_t colorIndex)
{
	// completely off-bounds?
	if (from.y < 0 || from.y >= out.h() || width <= 0 || from.x >= out.w() || from.x + width <= 0)
		return;

	const int x0 = std::max(0, from.x);
	const int x1 = std::min(out.w(), from.x + width);

	for (int x = x0; x < x1; ++x) {
		SetHalfTransparentPixel(out, { x, from.y }, colorIndex);
	}
}

// Draw a half-transparent vertical line of `height` pixels starting at `from`.
void DrawHalfTransparentVerticalLine(const Surface &out, Point from, int height, uint8_t colorIndex)
{
	// completely off-bounds?
	if (from.x < 0 || from.x >= out.w() || height <= 0 || from.y >= out.h() || from.y + height <= 0)
		return;

	const int y0 = std::max(0, from.y);
	const int y1 = std::min(out.h(), from.y + height);

	for (int y = y0; y < y1; ++y) {
		SetHalfTransparentPixel(out, { from.x, y }, colorIndex);
	}
}

void DrawHalfTransparentRectTo(const Surface &out, int sx, int sy, int width, int height)
{
	if (sx + width < 0)
		return;
	if (sy + height < 0)
		return;
	if (sx >= out.w())
		return;
	if (sy >= out.h())
		return;

	if (sx < 0) {
		width += sx;
		sx = 0;
	} else if (sx + width >= out.w()) {
		width = out.w() - sx;
	}

	if (sy < 0) {
		height += sy;
		sy = 0;
	} else if (sy + height >= out.h()) {
		height = out.h() - sy;
	}

	DrawHalfTransparentBlendedRectTo(out, sx, sy, width, height);
	StampUnlitRect(out, { { sx, sy }, { width, height } });
}

void DrawHalfTransparentRectTo(const Surface &out, int sx, int sy, int width, int height, uint8_t color)
{
	if (sx + width < 0)
		return;
	if (sy + height < 0)
		return;
	if (sx >= out.w())
		return;
	if (sy >= out.h())
		return;

	if (sx < 0) {
		width += sx;
		sx = 0;
	} else if (sx + width >= out.w()) {
		width = out.w() - sx;
	}

	if (sy < 0) {
		height += sy;
		sy = 0;
	} else if (sy + height >= out.h()) {
		height = out.h() - sy;
	}

	DrawHalfTransparentUnalignedBlendedRectTo(out, sx, sy, width, height, color);
	StampUnlitRect(out, { { sx, sy }, { width, height } });
}

void SetHalfTransparentPixel(const Surface &out, Point position, uint8_t color)
{
	if (out.InBounds(position)) {
		uint8_t *pix = out.at(position.x, position.y);
		const auto &lookupTable = paletteTransparencyLookup[color];
		*pix = lookupTable[*pix];
		StampUnlitPixel(out, pix);
	}
}

void UnsafeDrawBorder2px(const Surface &out, Rectangle rect, uint8_t color)
{
	const size_t width = rect.size.width;
	const size_t height = rect.size.height;
	uint8_t *buf = &out[rect.position];
	std::memset(buf, color, width);
	buf += out.pitch();
	std::memset(buf, color, width);
	buf += out.pitch();
	for (size_t i = 4; i < height; ++i) {
		buf[0] = buf[1] = color;
		buf[width - 2] = buf[width - 1] = color;
		buf += out.pitch();
	}
	std::memset(buf, color, width);
	buf += out.pitch();
	std::memset(buf, color, width);

	// Exactly the border pixels: the interior is not written.
	if (gbuffer::StampActive()) {
		const int w = rect.size.width;
		const int h = rect.size.height;
		const Point topLeft = rect.position;
		gbuffer::StampUnlit(out, { topLeft, { w, 2 } });
		gbuffer::StampUnlit(out, { topLeft + Displacement { 0, h - 2 }, { w, 2 } });
		gbuffer::StampUnlit(out, { topLeft + Displacement { 0, 2 }, { 2, h - 4 } });
		gbuffer::StampUnlit(out, { topLeft + Displacement { w - 2, 2 }, { 2, h - 4 } });
	}
}

} // namespace devilution
