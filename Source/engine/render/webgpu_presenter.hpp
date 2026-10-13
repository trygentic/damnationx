#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct SDL_Surface;

namespace devilution {

constexpr size_t WebGpuMaxRayTracedLights = 8; // legacy 2D path, unchanged
constexpr size_t WebGpuMaxHybridLights = 32;   // LightRenderCapacityLow (lighting_defs.hpp:60); High uses 32, Low 16
constexpr size_t WebGpuNormalLutSize = 32;
constexpr size_t WebGpuOccluderGridWidth = 112;
constexpr size_t WebGpuOccluderGridHeight = 112;
constexpr size_t WebGpuUiRectCount = 5;

struct WebGpuUiRect {
	float x = 0.0F;
	float y = 0.0F;
	float width = 0.0F;
	float height = 0.0F;
};

struct WebGpuRayTracedLight {
	float worldX = 0.0F;
	float worldY = 0.0F;
	float radius = 0.0F;
	float intensity = 0.0F;
	float red = 1.0F;
	float green = 1.0F;
	float blue = 1.0F;
};

/** Light for the 2.5D hybrid pass: tile-space position incl. height. */
struct WebGpuHybridLight {
	float worldX = 0.0F;
	float worldY = 0.0F;
	float worldZ = 0.0F;
	float radius = 0.0F;
	float intensity = 0.0F;
	float red = 1.0F;
	float green = 1.0F;
	float blue = 1.0F;
};

/** CPU G-buffer planes uploaded per hybrid frame. */
struct WebGpuGBufferPlanes {
	const uint8_t *aux = nullptr, *base = nullptr, *index = nullptr; // aux/index rows [0, rows); base rows [0, world rows); index = PalSurface->pixels
	uint32_t pitch = 0, rows = 0;
};

/** CPU → GPU contract for the 2.5D hybrid ray-marched lighting. */
struct WebGpuHybridData {
	bool enabled = false;        // = gbuffer::BackBufferIsHybrid(): planes valid, frame is full-bright
	bool softShadows = false;    // lightingQuality == High
	bool fullResolution = false; // lightingQuality == High (maskScale 1)
	bool frameHasUnlit = false;  // virtual gamepad drawn into the RGBA frame: composite reads frameTexture for Unlit pixels; full RGBA upload
	uint8_t debugView = 0;       // GBufferDebugView
	uint32_t zoom = 4;           // bits 0..7: zoom in quarters (GetZoomQuarters()); bits 8..15: -offsetY (HdWorldTransform)
	uint32_t zoomOffsetX = 0;    // HdWorldTransform::offsetX as int32 bits (side panel and remainder)
	uint32_t frameCounter = 0;
	float ambientScale = 0.6F;              // hybridAmbient / 100
	float heightPixelsPerWorldUnit = 32.0F; // tunable
	float wrapCard = 0.6F, wrapSurface = 0.35F, receiverNudge = 0.05F, softTapOffset = 0.22F;
	float emissiveScale = 2.0F, exposure = 1.0F;
	uint32_t lightCount = 0;
	std::array<WebGpuHybridLight, WebGpuMaxHybridLights> lights {};
	std::array<std::array<float, 4>, WebGpuNormalLutSize> normalLut {};
	std::array<float, 256> paletteEmissive {};
	std::array<float, 16> ambientCurve {};              // classic darkening per dLight level (gbuffer::BuildClassicAmbientCurve)
	std::array<std::array<uint8_t, 4>, 256> palette {}; // the SDL palette BltFast expands with (rgba8)
	const uint32_t *volume = nullptr;                   // tile_geometry::OccluderVolume::cells (112*112), packed layout
	uint32_t volumeGeneration = 0;                      // upload the volume texture only when it changes
	WebGpuGBufferPlanes planes {};
};

/** Presentation-only data consumed by the portable WGSL ray tracing pass. */
struct WebGpuLightingData {
	bool enabled = false;
	bool debugOccluders = false;
	bool highQuality = false;
	bool bloom = false;
	bool isTown = false;
	uint32_t viewportHeight = 0;
	float screenOriginX = 0.0F;
	float screenOriginY = 0.0F;
	float screenToWorldXX = 0.0F;
	float screenToWorldXY = 0.0F;
	float screenToWorldYX = 0.0F;
	float screenToWorldYY = 0.0F;
	uint32_t lightCount = 0;
	std::array<WebGpuUiRect, WebGpuUiRectCount> uiRects {};
	std::array<WebGpuRayTracedLight, WebGpuMaxRayTracedLights> lights {};
	std::array<uint32_t, WebGpuOccluderGridWidth * WebGpuOccluderGridHeight> occluders {};
	/** 2.5D hybrid pass data; `enabled` follows gbuffer::BackBufferIsHybrid(). The legacy fields above are untouched. */
	WebGpuHybridData hybrid;
	/** First-person 3D pass (render3d/world_renderer.hpp): every game light, player first then nearest, filled only when
	 *  render3d::WorldPassActive(); tile space like the hybrid lights (worldZ = height in tiles). */
	uint32_t worldLightCount = 0;
	std::array<WebGpuHybridLight, WebGpuMaxHybridLights> worldLights {};
};

bool WebGpuInitialize(const char *canvasSelector, int sourceWidth, int sourceHeight);
/** Recreates the frame/aux/base/index/mask textures and bind groups for a new back buffer size; no-op (returns false) when !WebGpuIsReady(). */
bool WebGpuResize(int sourceWidth, int sourceHeight);
bool WebGpuPresent(SDL_Surface *surface, int drawableWidth, int drawableHeight, const WebGpuLightingData &lighting);
void WebGpuShutdown();
bool WebGpuIsReady();

// Temporary 4x Cathedral floor PNG path. The classic renderer remains the
// source of visibility and draw order; these calls record exposed floor pixels.
// Mark records the diamond halves in `halves` (bit 0 left, bit 1 right) of a floor tile (DrawFloorTile, before the
// capture) at dungeon tile tileX, tileY, whose diamond's bottom-left is x, y. Capture swaps each marked pixel's index
// with its ramp neighbour (index ^ 1 for indices 2 and up, so blended draws over it stay close to classic) and keeps a
// copy, so a later draw is detected even where the light tables collapse different art to the same dark index. The
// swapped index stays in the frame (the HD sprites' occlusion snapshots, taken during the world pass, see the same
// values); the shaders swap it back where they show the classic colour. Finish finds every marked pixel that still holds
// its swapped index (one bit each for the presenter); paletteRgba (256 x RGBA) tints the Cathedral atlas toward the
// level palette. The presenter derives each pixel's diamond from its position, so the diamonds must lie on one iso grid
// per world pass (the dungeon's: tile x + 1 is 32 pixels right and 16 down, tile y + 1 32 left and 16 down).
void WebGpuBeginCathedralFloor(int width, int height, bool enabled);
void WebGpuMarkCathedralFloor(int x, int y, int tileX, int tileY, int lightTableIndex, uint8_t halves = 3);
void WebGpuCaptureCathedralFloor(uint8_t *pixels, int pitch);
void WebGpuFinishCathedralFloor(const uint8_t *pixels, int pitch, const uint8_t *paletteRgba = nullptr);
// The same path for town (hd/town-floor.txt pages). Prepare loads the atlas pages on first use and returns whether
// the town floor can be replaced this frame (?hdFloor=1 and the pages loaded); Mark replaces one floor diamond whose
// piece (the dPiece value) has an atlas cell and leaves other pieces classic. Begin/Capture/Finish are shared.
bool WebGpuPrepareTownFloor();
void WebGpuMarkTownFloor(int x, int y, int tileX, int tileY, int pieceId, int lightTableIndex);
// Town only: solid pieces listed in hd/town-floor.txt (river banks, tree bases, wall feet), whose bottom floor blocks
// DrawCell draws after the capture. SolidFloorPiece says whether the piece is replaced this frame; CaptureTownSolidFloor
// takes the pixels of the diamond (bottom-left x, y; dungeon tile tileX, tileY) that those blocks wrote (written: 64 x 32
// mask, row 0 = y - 32), swaps and records them as Capture does for floor tiles.
bool WebGpuTownSolidFloorPiece(int pieceId);
void WebGpuCaptureTownSolidFloor(uint8_t *pixels, int pitch, int x, int y, int tileX, int tileY, int pieceId, int lightTableIndex, const uint8_t *written);

} // namespace devilution
