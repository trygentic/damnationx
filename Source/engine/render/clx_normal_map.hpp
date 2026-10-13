// Source/engine/render/clx_normal_map.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "engine/clx_sprite.hpp"
#include "engine/render/gbuffer.hpp"
struct SDL_Color;

namespace devilution::clx_normal_map {

/** Row-span packed aux plane for one frame. Row r (0 = feet row) holds bytes for columns [rowX0[r], rowX0[r] + (rowOffset[r+1] - rowOffset[r]))
 *  covering every opaque pixel of that row: PackAux(Card, idx) | ShadowByte (colour 0) | 0 (transparent inside the span, never read).
 *  data == nullptr means "flat tier": the blitter writes FlatCardByte for every opaque pixel (no shadow distinction) and no plane is stored. */
struct Plane {
	const uint8_t *data = nullptr;
	const uint32_t *rowOffset = nullptr; // height + 1 entries
	const uint16_t *rowX0 = nullptr;     // height entries
	uint16_t width = 0, height = 0;
	[[nodiscard]] size_t bytes() const { return rowOffset == nullptr ? 0 : rowOffset[height]; }
};

struct Options {
	bool generate = true; // false → flat tier (Plane{} returned, nothing cached)
	float silhouetteWeight = 0.6F;
	/** Detail tilt: with normalizeDetail, tan(tilt) of a pixel whose (de-lit) luminance gradient equals the frame's RMS
	 *  gradient; without it, tan(tilt) per unit of luminance per pixel. */
	float detailStrength = 1.0F;
	uint8_t maxInflateRadius = 16;
	bool delight = true; // subtract the mean luminance gradient over opaque pixels (Diablo art is baked-lit from the upper-left)
	/** Sobel over the gamma-encoded luma the artist painted (sRGB bytes) instead of linear light. Linear luminance
	 *  compresses the dark Diablo palette to gradients of ~0.01 per pixel, which tilted the detail normal by ~1 degree:
	 *  every sprite was a smooth silhouette pillow with no surface relief. */
	bool perceptualDetail = true;
	/** Divide the gradient by the frame's RMS gradient (floored at detailContrastFloor) so dark, low-contrast frames get
	 *  the same relief as bright ones and detailStrength means the same tilt for every sprite. */
	bool normalizeDetail = true;
	float detailContrastFloor = 0.04F;
};

/** Packed plane for a Card frame. Key = {pixelData(), pixelDataSize(), width, height, mirrored} (no hashing on lookup).
 *  Returns Plane{} (flat) when generation is disabled or the per-frame budget is exhausted (the key is queued). Palette = gbuffer::CurrentPalette(). */
[[nodiscard]] Plane GetOrBuildNormalPlane(ClxSprite sprite, bool mirrored);
/** Pure generator (testable): packs the plane for `sprite` into `out` (row-span layout, returns the view). `scratch` is reused across calls. */
struct Scratch; // opaque, sized by the largest frame seen; one static instance in the .cpp, tests may own their own
[[nodiscard]] Plane GeneratePlane(ClxSprite sprite, std::span<const SDL_Color, 256> logicalPalette, const Options &opts, bool mirrored, std::vector<uint8_t> &out, Scratch &scratch);
[[nodiscard]] uint64_t HashPixels(ClxSprite sprite); // FNV-1a 64 over pixelData(); called once per generated entry (stored), never per lookup

/** Authored override (DXM sidecar / material pack): tangent-space RGB8 rows top-down, green = up. One package per materialVersion (no cross-pack mixing). */
void RegisterAuthoredNormalMap(uint64_t contentHash, uint64_t materialVersion, std::span<const uint8_t> rgbTopDown, uint16_t width, uint16_t height);

// Budget / cache
void SetOptions(const Options &opts);                                     // hybridNormalMaps option → generate
void SetBudget(uint32_t framesPerGameFrame, uint32_t microsPerGameFrame); // default 8 / 1500
void SetCacheBudget(size_t bytes);                                        // reserves the arena (one allocation); default 64 MiB native, 24 MiB Emscripten (measured; see DefaultEmscriptenBudget)
void EnqueuePrewarm(ClxSpriteList list);                                  // stops accepting once projected bytes reach 70 % of the budget
void EnqueuePrewarm(ClxSpriteSheet sheet);
size_t Pump(uint32_t budgetMicros);                             // generates queued planes; returns planes generated
void ClearNormalPlaneCache();                                   // from ClearClxDrawCache (clx_render.cpp:628-631)
void EvictNormalPlanes(const void *dataBegin, size_t dataSize); // evict-on-free (destructors + monster.cpp hooks)
struct Stats {
	size_t entries, bytes, arenaBytes, generatedThisFrame, queued, prewarmDropped, evictions, hits, misses, authoredHits;
};
[[nodiscard]] Stats GetStats();
void ResetFrameStats();

// Test seam (tests can exercise BlitWithGBuffer without the generator): when set, GetOrBuildNormalPlane returns provider(sprite, mirrored) and touches no cache.
using PlaneProvider = Plane (*)(ClxSprite sprite, bool mirrored);
void SetNormalPlaneProviderForTesting(PlaneProvider provider); // nullptr restores normal behaviour

} // namespace devilution::clx_normal_map
