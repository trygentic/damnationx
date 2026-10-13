/**
 * @file scrollrt.h
 *
 * Interface of functionality for rendering the dungeons, monsters and calling other render routines.
 */
#pragma once

#include <cstdint>
#include <optional>

#include "engine/animationinfo.h"
#include "engine/clx_sprite.hpp"
#include "engine/direction.hpp"
#include "engine/displacement.hpp"
#include "engine/lighting_defs.hpp"
#include "engine/point.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_view_transform.hpp"
#include "engine/surface.hpp"

namespace devilution {

extern bool AutoMapShowItems;
extern bool frameflag;

/**
 * @brief Returns the offset for the walking animation
 * @param animationInfo the current active walking animation
 * @param dir walking direction
 * @param cameraMode Adjusts the offset relative to the camera
 */
Displacement GetOffsetForWalking(const AnimationInfo &animationInfo, Direction dir, bool cameraMode = false);

/**
 * @brief Clear cursor state
 */
void ClearCursor();

/**
 * @brief Shifting the view area along the logical grid
 *        Note: this won't allow you to shift between even and odd rows
 * @param horizontal Shift the screen left or right
 * @param vertical Shift the screen up or down
 */
void ShiftGrid(Point *offset, int horizontal, int vertical);

/**
 * @brief Gets the number of rows covered by the main panel
 */
int RowsCoveredByPanel();

/**
 * @brief Calculate the offset needed for centering tiles in view area
 * @param offsetX Offset in pixels
 * @param offsetY Offset in pixels
 */
void CalcTileOffset(int *offsetX, int *offsetY);

/**
 * @brief Calculate the needed diamond tile to cover the view area
 * @param columns Tiles needed per row
 * @param rows Both even and odd rows
 */
void TilesInView(int *columns, int *rows);
void CalcViewportGeometry();

/**
 * @brief Screen pixels per world pixel of the game view in quarters (4 = no zoom, 5 = 1.25x .. 16 = 4x).
 *
 * The Graphics "Zoom Level"; a fractional level is rounded to the nearest whole one unless FractionalZoomAvailable().
 * Purely presentational: the world pass draws ceil(4 w / q) x ceil(4 h / q) pixels and Zoom() scales them up.
 */
[[nodiscard]] int GetZoomQuarters();

/** @brief In-between zoom levels need the HD art to draw the whole world (HD tiles for this level and HD sprites). */
[[nodiscard]] bool FractionalZoomAvailable();

/** @brief Tests: force FractionalZoomAvailable() (std::nullopt = detect). */
void SetFractionalZoomForTesting(std::optional<bool> available);

/** @brief World-pass pixels covering `screenLength` screen pixels at the current zoom (rounded down). */
[[nodiscard]] int ScreenToWorldLength(int screenLength);

/**
 * @brief The current view's world-pass-to-screen transform (hd_view_transform.hpp): zoom level, open side panels.
 */
[[nodiscard]] HdWorldTransform GetViewTransform();

/**
 * @brief Changes the zoom by `steps` (positive zooms in): a quarter level per step when FractionalZoomAvailable(),
 * otherwise a whole level; clamped to the supported levels. Recomputes the view geometry.
 */
void StepZoomLevel(int steps);

/** @brief The "Zoom" key: back to 1x when zoomed, otherwise 2x. */
void ToggleZoom();

/**
 * @brief Calculate the screen position of a given tile
 * @param tile Position of a dungeon tile
 */
Point GetScreenPosition(Point tile);

/**
 * @brief Render the whole screen black
 */
void ClearScreenBuffer();
#ifdef _DEBUG

/**
 * @brief Scroll the screen when mouse is close to the edge
 */
void ScrollView();
#endif

/**
 * @brief Initialize the FPS meter
 */
void EnableFrameCount();

/**
 * @brief Redraw screen
 */
void scrollrt_draw_game_screen();

/**
 * @brief Render the game
 */
void DrawAndBlit();

/**
 * @brief Full-bright switch of the hybrid world pass.
 *
 * With `hybrid == false` the authoritative light table index is returned unchanged. With `hybrid == true`
 * everything that is not fully dark is drawn through LightTables[0] (the GPU applies the lighting) and a
 * fully dark index stays fully dark so that no hidden information is revealed.
 */
[[nodiscard]] constexpr int HybridShadeIndex(int authoritative, bool hybrid)
{
	if (!hybrid) return authoritative;
	return authoritative >= LightsMax ? LightsMax : 0;
}

/**
 * @brief Full-bright switch for tiles and wall faces: full-bright under the hybrid latch even at the fully dark index.
 *
 * Classic per-pixel lighting bleeds a lit cell's light across the edge of a fully dark neighbour, so a dark tile
 * shows a gradient, not a hard cell-shaped black edge. The GPU's bilinear ambient reproduces that only if the tile
 * is drawn full-bright; painting it black on the CPU left cell-shaped panels on floors and see-through walls.
 * Sprites keep HybridShadeIndex: a card in a dark cell stays black (no-reveal).
 */
[[nodiscard]] constexpr int HybridTileShadeIndex(int authoritative, bool hybrid)
{
	return hybrid ? 0 : authoritative;
}

/**
 * @brief One world sprite draw request for the DrawWorldSprite funnel.
 */
struct WorldSpriteDraw {
	/** ClxDraw position (bottom-left of the sprite, i.e. the feet row). */
	Point feet;
	ClxSprite sprite;
	/**
	 * LightTables index; world pass callers pass it through ShadeIndex (HybridShadeIndex with the frame latch).
	 * In hybrid mode the primitives run with 0 and the presenter lights the card, except for LightsMax (fully dark):
	 * that sprite keeps the dark table and is tagged pass-through, so nothing on a dLight == 15 cell is revealed.
	 */
	int shadeIndex = 0;
	/** Semantic TRN (infravision / stone / unique); kept in hybrid mode. */
	const uint8_t *trn = nullptr;
	bool blended = false;
	/** Self-lit sprite (!_miLightFlag missiles, !applyLighting objects, player icons): CardEmissive in hybrid mode. */
	bool selfLit = false;
	/** WallLeft / WallRight for arches (tile_geometry::ArchFacing); Unlit means "not an arch". */
	gbuffer::Kind archKind = gbuffer::Kind::Unlit;
};

/**
 * @brief The single world sprite funnel.
 *
 * Classic (`hybrid == false`) dispatch is bit-exact to the four ClxDraw primitives:
 * trn ? (blended ? ClxDrawBlendedTRN : ClxDrawTRN)(trn)
 *     : shadeIndex != 0 ? (blended ? ClxDrawBlendedTRN : ClxDrawTRN)(LightTables[shadeIndex])
 *                       : (blended ? ClxDrawBlended : ClxDraw).
 * Hybrid sets the G-buffer card / arch context (gbuffer::CurrentCardKind, arch tile context) and then runs the
 * same primitives with shadeIndex = 0: the TRN is kept, the light table is not applied. A request
 * with shadeIndex >= LightsMax and no TRN is the exception (no-reveal): it keeps the fully dark table and is
 * tagged CardEmissive (pass-through), arches included. The world pass calls it with gbuffer::Enabled(); exported
 * so scrollrt_test can drive both paths.
 */
void DrawWorldSprite(const Surface &out, const WorldSpriteDraw &draw, bool hybrid);

/**
 * @brief Latch input assembled from the live game state: hybridLighting option, presenter readiness,
 * HeadlessMode, gbRunGame, leveltype and the logical palette.
 */
[[nodiscard]] gbuffer::LatchInput CurrentHybridLatchInput();

/**
 * @brief Frame start of DrawAndBlit: latches the hybrid mode for this frame and, when enabled, applies the
 * normal-map options and pumps the deferred normal-plane generation queue.
 */
void LatchHybridWorldFrame(const gbuffer::LatchInput &in);

/**
 * @brief Frame start of scrollrt_draw_game_screen (a re-present without a world redraw): latches the hybrid
 * mode and, when enabled, enters StampUnlit mode so the cursor and DrawMain primitives stamp Unlit.
 */
void LatchHybridRepresentFrame(const gbuffer::LatchInput &in);

/**
 * @brief Enqueues the level's sprite frames for normal-plane generation, byte-bounded.
 *
 * Priority order: active players' Stand and Walk, each level monster type's Stand then Walk, the remaining player
 * animations, the remaining monster animations, then the resident object and missile sprite lists. Stops as soon
 * as clx_normal_map::EnqueuePrewarm reports the cache byte bound (Stats::prewarmDropped). Called once from
 * LoadGameLevel right before CompleteProgress(); a no-op unless hybrid lighting and normal maps are enabled and
 * `presenterReady` (WebGpuIsReady()): without a presenter no hybrid frame runs Pump, so the queue would never drain, and
 * natively the level loads on a loader thread while the main thread can evict from the same cache.
 */
void PrewarmHybridSpriteNormals(bool presenterReady);

/**
 * @brief Runs the world pass (floor, tile content, out-of-bounds fill, zoom) into `out` without SDL video.
 *
 * Test hook for the scrollrt goldens: `viewPosition` and `offset` are what CalcFirstTilePosition would
 * produce for ViewPosition. The caller must have set up the level globals, `MyPlayer` and the viewport
 * geometry (CalcViewportGeometry) first.
 */
void DrawWorldForTesting(const Surface &out, Point viewPosition, Displacement offset);

} // namespace devilution
