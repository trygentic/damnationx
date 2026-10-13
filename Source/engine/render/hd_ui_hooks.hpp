/**
 * @file hd_ui_hooks.hpp
 *
 * Call-outs from the low-level drawing and loading code (sprite loaders, CLX blits, surface blits, SurfaceToClx,
 * off-screen surface creation, the inventory slot tint) to the opt-in HD UI (hd_ui.hpp).
 *
 * The table is a header-only inline variable so the small object libraries that draw (clx_render, surface, load_*)
 * need no link dependency on the HD UI: the hooks stay null, and cost one predictable branch, unless hd_ui.cpp
 * installs them (`?hdUi=1` in the WebGPU build).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/clx_sprite.hpp"

struct SDL_Surface;

namespace devilution {

struct Surface;

struct HdUiHookTable {
	/** A CLX list (numLists == 0) or sheet was loaded from `path` into `data` (its whole buffer). */
	void (*spritesLoaded)(const char *path, const uint8_t *data, size_t size, uint16_t numLists);
	/** A sprite buffer is being freed (clx_sprite.hpp Owned* destructors). */
	void (*spritesFreed)(const void *data, size_t size);
	/**
	 * A CLX frame was just drawn (classic pixels written) with its bottom-left corner at (x, y) of `out`.
	 * `trn` is the colour translation (nullptr: none); `blended` = drawn half transparent.
	 */
	void (*clxDrawn)(const Surface &out, int x, int y, const uint8_t *frameData, uint32_t frameSize, const uint8_t *trn, bool blended);
	/** `srcRect` of `src` was just copied to `dst` at (dstX, dstY) (both already clipped); skipZero: index 0 kept dst. */
	void (*blitted)(const Surface &dst, const Surface &src, int srcX, int srcY, int width, int height, int dstX, int dstY, bool skipZero);
	/** SurfaceToClx turned `surface` into `numFrames` stacked frames stored in `data`. */
	void (*surfaceToClx)(const Surface &surface, unsigned numFrames, const uint8_t *data, size_t size);
	/** A new off-screen surface (OwnedSurface) was created, or one was cleared: it holds no HD content. */
	void (*surfaceCreated)(const SDL_Surface *surface);
	/** `src` was downscaled by half into `dst` (BilinearDownscaleByHalf8, the store's half-size item sprites). */
	void (*surfaceDownscaled)(const Surface &src, const Surface &dst);
	/** The inventory recoloured the grey slot background in (x, y, w, h) of `out` to the colour ramp starting at `ramp`. */
	void (*slotTinted)(const Surface &out, int x, int y, int width, int height, uint8_t ramp);
};

inline const HdUiHookTable *HdUiHooks = nullptr;

/** Reports a freshly loaded sprite list or sheet to the HD UI (no-op unless it is enabled). */
inline void HdUiNotifyLoaded(const char *path, const OwnedClxSpriteListOrSheet &sprites)
{
	if (HdUiHooks == nullptr) [[likely]]
		return;
	const ClxSpriteListOrSheet view { sprites };
	const uint8_t *data = view.isSheet() ? view.sheet().data() : view.list().data();
	HdUiHooks->spritesLoaded(path, data, view.dataSize(), sprites.numLists());
}

inline void HdUiNotifyLoaded(const char *path, ClxSpriteList sprites)
{
	if (HdUiHooks == nullptr) [[likely]]
		return;
	HdUiHooks->spritesLoaded(path, sprites.data(), sprites.dataSize(), 0);
}

} // namespace devilution
