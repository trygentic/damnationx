#include "engine/surface.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/rectangle.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_ui_hooks.hpp"

namespace devilution {

namespace {

/**
 * @brief Tags the pixels a blit into the back buffer writes as Unlit in the G-buffer.
 *
 * Surface blits only draw UI (the main panel, buttons, belt, info and chat boxes, flasks): after the world pass they
 * must pass through the hybrid composite like every other UI chokepoint. The skip-zero variant stamps only the pixels
 * it writes, so the world behind a flask dome's transparent pixels keeps its kind.
 *
 * @param srcBuf First source pixel of the clipped rectangle.
 * @param dstBuf First destination pixel of the clipped rectangle (inside `dst`).
 */
template <bool SkipColorIndexZero>
void StampBlitUnlit(const std::uint8_t *srcBuf, int srcPitch, const Surface &dst, std::uint8_t *dstBuf, Point dstPosition, int width, int height)
{
	if (!gbuffer::StampActive() || !gbuffer::CurrentPlanes.covers(dst))
		return;
	if (!SkipColorIndexZero) {
		gbuffer::StampUnlit(dst, Rectangle { dstPosition, Size { width, height } });
		return;
	}
	const gbuffer::Planes &planes = gbuffer::CurrentPlanes;
	if (dst.pitch() != planes.pitch)
		return;
	const ptrdiff_t first = dstBuf - planes.outBase;
	const ptrdiff_t end = first + static_cast<ptrdiff_t>(height - 1) * planes.pitch + width;
	if (first < 0 || end > static_cast<ptrdiff_t>(static_cast<size_t>(planes.pitch) * planes.height))
		return;
	std::uint8_t *aux = planes.aux + first;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (srcBuf[x] != 0)
				aux[x] = gbuffer::UnlitByte;
		}
		srcBuf += srcPitch;
		aux += planes.pitch;
	}
}

template <bool SkipColorIndexZero>
void SurfaceBlit(const Surface &src, SDL_Rect srcRect, const Surface &dst, Point dstPosition)
{
	// We do not use `SDL_BlitSurface` here because the palettes may be different objects
	// and SDL would attempt to map them.

	dst.Clip(&srcRect, &dstPosition);
	if (srcRect.w <= 0 || srcRect.h <= 0)
		return;

	const std::uint8_t *srcBuf = src.at(srcRect.x, srcRect.y);
	const SDL_Rect clippedSrc = srcRect;
	const Point clippedDst = dstPosition;
	const auto srcPitch = src.pitch();
	std::uint8_t *dstBuf = &dst[dstPosition];
	const auto dstPitch = dst.pitch();
	// Stamped before the copy, so the skip-zero test sees the source pixels even when a blit within the back buffer overlaps them.
	StampBlitUnlit<SkipColorIndexZero>(srcBuf, srcPitch, dst, dstBuf, dstPosition, srcRect.w, srcRect.h);

	for (unsigned h = srcRect.h; h != 0; --h) {
		if (SkipColorIndexZero) {
			for (unsigned w = srcRect.w; w != 0; --w) {
				if (*srcBuf != 0)
					*dstBuf = *srcBuf;
				++srcBuf, ++dstBuf;
			}
			srcBuf += srcPitch - srcRect.w;
			dstBuf += dstPitch - srcRect.w;
		} else {
			std::memcpy(dstBuf, srcBuf, srcRect.w);
			srcBuf += srcPitch;
			dstBuf += dstPitch;
		}
	}
	if (HdUiHooks != nullptr) [[unlikely]]
		HdUiHooks->blitted(dst, src, clippedSrc.x, clippedSrc.y, clippedSrc.w, clippedSrc.h, clippedDst.x, clippedDst.y, SkipColorIndexZero);
}

} // namespace

void Surface::BlitFrom(const Surface &src, SDL_Rect srcRect, Point targetPosition) const
{
	SurfaceBlit</*SkipColorIndexZero=*/false>(src, srcRect, *this, targetPosition);
}

void Surface::BlitFromSkipColorIndexZero(const Surface &src, SDL_Rect srcRect, Point targetPosition) const
{
	SurfaceBlit</*SkipColorIndexZero=*/true>(src, srcRect, *this, targetPosition);
}

} // namespace devilution
