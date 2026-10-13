/**
 * @file hd_tiles.hpp
 *
 * Opt-in HD (4x art, stored at 1..4 texels per game pixel) level tiles in the WebGPU browser build (`?hdTiles=1`): town and Cathedral walls, roofs, trees, bridges, rocks, graves, pillars,
 * arches, doors, stairs and floors.
 *
 * The classic renderer stays the source of visibility and draw order: every piece is still drawn into the 8-bit back
 * buffer. The world pass (scrollrt.cpp) records an HD instance for each piece layer it draws that the manifest
 * (hd/tiles-<tileset>.txt) replaces: `floor` in DrawFloorTile, `cell` in DrawDungeon right before DrawCell, and the
 * dSpecial overlays (arches, door leaves, town tree crowns). Each image covers exactly the pixels its classic call
 * writes, so drawing the instances over the composite in recording order reproduces the classic painter order between
 * tiles. Characters, items, objects and missiles drawn between the tiles are found by diffing a rectangle around each
 * cell's non-tile draws (hd_tiles::DynamicPlane); they and the UI hide the tile instances recorded before them.
 *
 * HD sprites (hd_sprites.hpp) need no change: they are occluded by the classic tiles drawn after them and are drawn
 * after all tile instances on the GPU, so each sprite lands over the tiles behind it and under the ones in front.
 *
 * See-through walls (TransList) draw their HD cell at half opacity. Lighting follows the classic shade of the piece
 * (legacy composite: light-table brightness x the legacy light mask) or, under hybrid lighting, the hybrid light the
 * G-buffer gives the classic pixel under each texel.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "engine/point.hpp"
#include "engine/render/hd_tiles_core.hpp"

namespace devilution {

struct Surface;

struct HdTileInstance {
	/** Index into the frame manifest's images. */
	uint32_t image = 0;
	/** Quad in back-buffer pixels. */
	float x = 0.0F;
	float y = 0.0F;
	float width = 0.0F;
	float height = 0.0F;
	/** Classic light table index the piece was drawn with (0 = full bright, also under hybrid lighting). */
	int lightTableIndex = 0;
	/** See-through wall: half opacity. */
	bool translucent = false;
};

/** Starts a world pass; `allowed` is false when this frame cannot use HD tiles (zoom). */
void HdTilesBeginWorldPass(const Surface &out, bool allowed);
/** True while the current world pass records HD tiles. */
[[nodiscard]] bool HdTilesActive();
/** `?hdTiles=1` and the current level type has an HD tile manifest (even while its pages are still loading). */
[[nodiscard]] bool HdTilesWantedForLevel();
/**
 * `?hdAtlasFloor=1` (with `?hdFloor=1`): the generic Cathedral floor atlas keeps the plain floor and HD tiles skip
 * those floor layers. By default the tiles' own floor layers win, so floors match the walls and stairs around them.
 */
[[nodiscard]] bool HdTilesPreferFloorAtlas();

/** DrawFloorTile: records the `floor` layer of the piece at `tilePosition`; true when it is drawn in HD. */
bool HdTilesRecordFloor(const Surface &out, Point tilePosition, int pieceId, Point targetBufferPosition, int lightTableIndex);
/** DrawDungeon, right before DrawCell: records the `cell` layer. */
bool HdTilesRecordCell(const Surface &out, Point tilePosition, int pieceId, Point targetBufferPosition, int lightTableIndex, bool translucent);
/**
 * Records the dSpecial overlay `frame` of `specialPosition` drawn with its bottom-left pixel at `bottomLeft`. Ends an
 * open dynamic section first (the overlay is drawn after the cell's characters). When the overlay stays classic, a new
 * dynamic section is opened around it (closed by the caller's HdTilesEndDynamic).
 */
bool HdTilesRecordSpecial(const Surface &out, Point specialPosition, int frame, Point bottomLeft, int lightTableIndex, bool translucent);
/** Opens a dynamic section around the non-tile draws of the cell at `targetBufferPosition`. */
void HdTilesBeginDynamic(const Surface &out, Point targetBufferPosition);
/** Closes the open dynamic section (no-op without one). */
void HdTilesEndDynamic(const Surface &out);
/** After DrawTileContent: everything drawn later (OOB tiles, UI, cursor) hides every tile instance. */
void HdTilesEndWorldContent(const Surface &out);
/**
 * After scrollrt's Zoom() scaled the world up `zoomQuarters` / 4 times (`viewportWidth` x `height` at `offsetX` of `zoomOut`):
 * scales the recorded instances (by HdCurrentWorldTransform()) and the dynamic plane the same way and retakes the
 * end-of-world snapshot of the zoomed viewport.
 */
void HdTilesApplyZoom(const Surface &zoomOut, int viewportWidth, int offsetX, int zoomQuarters);
/** The back buffer no longer holds the recorded world pass: drop the instances. */
void HdTilesInvalidate();
/** RenderPresent, right before presenting: marks what was drawn after the world content. */
void HdTilesFinishFrame(const uint8_t *pixels, int pitch, int width, int height);

/**
 * Bracket a recolour of the whole view drawn after the world (RedBack: pause and death screens), the top `height` rows of
 * `out`, so the HD layers do not take it for a draw over their art (hd_tiles::RecolourPass). A new world pass forgets it.
 */
void HdLayersBeginRecolour(const Surface &out, int height);
void HdLayersEndRecolour(const Surface &out);
void HdLayersResetRecolour();
/** RenderPresent: the frame every HD tracker diffs (`pixels` itself unless a recolour is recorded; see RecolourPass). */
const uint8_t *HdLayersTrackedFrame(const uint8_t *pixels, int pitch, int width, int height, int &outPitch);

// ---- Presenter side ----
[[nodiscard]] std::span<const HdTileInstance> HdTilesFrameInstances();
[[nodiscard]] const hd_tiles::DynamicPlane &HdTilesDynamicPlane();
/** Manifest of the recorded instances (nullptr when nothing is recorded). */
[[nodiscard]] const hd_tiles::Manifest *HdTilesFrameManifest();
/** The world viewport of the recorded pass in back-buffer pixels: instances are clipped to it (panels lie below). */
[[nodiscard]] hd_tiles::PixelRect HdTilesFrameViewport();
/** Tileset of the recorded pass (0 town, 1 Cathedral, -1 none). */
[[nodiscard]] int HdTilesFrameTileset();

// ---- Implemented by hd_tiles_webgpu.cpp (stubs without DEVILUTIONX_WEBGPU) ----
[[nodiscard]] bool HdTilesGpuAvailable();
[[nodiscard]] bool HdTilesGpuFlag(const char *name);
/** Text of a small file next to the page (browser fetch); see hd_loader::FetchText. */
bool HdTilesGpuFetchText(const char *url, std::string &text, bool &failed);
/** Makes `manifest` (identified by `tileset`) the atlas set pages are requested from; releases another set's pages. */
void HdTilesGpuUseManifest(int tileset, const hd_tiles::Manifest &manifest);
/** True when the page's texture is resident; otherwise starts loading it (the piece stays classic meanwhile). */
bool HdTilesGpuRequestPage(int page);

} // namespace devilution
