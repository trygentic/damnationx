/**
 * @file dun_render.hpp
 *
 * Interface of functionality for rendering the level tiles.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/point.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/light_render.hpp"
#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"
#include "utils/attributes.h"
#include "utils/endian_swap.hpp"

// #define DUN_RENDER_STATS
#ifdef DUN_RENDER_STATS
#include <ankerl/unordered_dense.h>
#endif

namespace devilution {

/**
 * @brief Specifies the mask to use for rendering.
 */
enum class MaskType : uint8_t {
	/** @brief The entire tile is opaque. */
	Solid,

	/** @brief The entire tile is blended with transparency. */
	Transparent,

	/**
	 * @brief Upper-right triangle is blended with transparency.
	 *
	 * Can only be used with `TileType::LeftTrapezoid` and
	 * `TileType::TransparentSquare`.
	 *
	 * The lower 16 rows are opaque.
	 * The upper 16 rows are arranged like this (🮆 = opaque, 🮐 = blended):
	 *
	 * 🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮐🮐
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 */
	Right,

	/**
	 * @brief Upper-left triangle is blended with transparency.
	 *
	 * Can only be used with `TileType::RightTrapezoid` and
	 * `TileType::TransparentSquare`.
	 *
	 * The lower 16 rows are opaque.
	 * The upper 16 rows are arranged like this (🮆 = opaque, 🮐 = blended):
	 *
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮐🮐🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 * 🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆🮆
	 */
	Left,
};

#ifdef DUN_RENDER_STATS
struct DunRenderType {
	TileType tileType;
	MaskType maskType;
	bool operator==(const DunRenderType &other) const
	{
		return tileType == other.tileType && maskType == other.maskType;
	}
};
struct DunRenderTypeHash {
	size_t operator()(DunRenderType t) const noexcept
	{
		return std::hash<uint32_t> {}((1 < static_cast<uint8_t>(t.tileType)) | static_cast<uint8_t>(t.maskType));
	}
};
extern ankerl::unordered_dense::map<DunRenderType, size_t, DunRenderTypeHash> DunRenderStats;

std::string_view TileTypeToString(TileType tileType);

std::string_view MaskTypeToString(MaskType maskType);
#endif

/**
 * @brief Low-level tile rendering function.
 */
void RenderTileFrame(const Surface &out, const Lightmap &lightmap, const Point &position, TileType tile, const uint8_t *src, int_fast16_t height,
    MaskType maskType, const uint8_t *tbl);

/**
 * @brief Submit and execute the ordered terrain command stream.
 */
void FlushTileRenderCommands();

/**
 * @brief Whether any queued tile command has not been executed yet (debug assertion support).
 */
[[nodiscard]] bool HasPendingTileRenderCommands();

/**
 * @brief Returns the raw data for the given dungeon frame.
 */
DVL_ALWAYS_INLINE const uint8_t *GetDunFrame(const std::byte *dungeonCelData, uint32_t frame)
{
	const auto *frameTable = reinterpret_cast<const uint32_t *>(dungeonCelData);
	return reinterpret_cast<const uint8_t *>(&dungeonCelData[Swap32LE(frameTable[frame])]);
}

/**
 * @brief Returns the raw data for the given dungeon frame's foliage.
 */
DVL_ALWAYS_INLINE const uint8_t *GetDunFrameFoliage(const std::byte *dungeonCelData, uint32_t frame)
{
	return GetDunFrame(dungeonCelData, frame) + ReencodedTriangleFrameSize;
}

/**
 * @brief Sets the G-buffer tile context for the next RenderTile* call.
 *
 * No-op unless the hybrid world pass is capturing (gbuffer::CaptureActive()).
 *
 * @param kind G-buffer kind of the tile (Floor, WallLeft, WallRight, WallTop, ...).
 * @param normalIdx Normal LUT index (gbuffer::NormalUp / NormalWallLeft / NormalWallRight).
 * @param anchorRow Screen row of the tile base.
 * @param leftX Screen x of tile-local column 0 (may be negative when clipped).
 * @param foot Foot table shape (SW / SE trapezoid foot or None).
 */
DVL_ALWAYS_INLINE void SetTileGBufferContext(gbuffer::Kind kind, uint8_t normalIdx, int anchorRow, int leftX, gbuffer::FootKind foot)
{
	if (gbuffer::CaptureActive())
		gbuffer::SetTileContext(kind, normalIdx, anchorRow, leftX, foot);
}

/**
 * @brief Blit current world CEL to the given buffer
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param position Target buffer coordinates
 * @param dungeonCelData Dungeon CEL data.
 * @param levelCelBlock The MIN block of the level CEL file.
 * @param maskType The mask to use,
 * @param tbl LightTable or TRN for a tile.
 */
DVL_ALWAYS_INLINE void RenderTile(const Surface &out, const Lightmap &lightmap, const Point &position,
    const std::byte *dungeonCelData, LevelCelBlock levelCelBlock, MaskType maskType, const uint8_t *tbl)
{
	const TileType tileType = levelCelBlock.type();
	RenderTileFrame(out, lightmap, position, tileType,
	    GetDunFrame(dungeonCelData, levelCelBlock.frame()),
	    (tileType == TileType::LeftTriangle || tileType == TileType::RightTriangle)
	        ? DunFrameTriangleHeight
	        : DunFrameHeight,
	    maskType, tbl);
}

/**
 * @brief Renders a floor foliage tile.
 */
DVL_ALWAYS_INLINE void RenderTileFoliage(const Surface &out, const Lightmap &lightmap, const Point &position,
    const std::byte *dungeonCelData, LevelCelBlock levelCelBlock, const uint8_t *tbl)
{
	RenderTileFrame(out, lightmap, Point { position.x, position.y - 16 }, TileType::TransparentSquare,
	    GetDunFrameFoliage(dungeonCelData, levelCelBlock.frame()), /*height=*/16, MaskType::Solid, tbl);
}

/**
 * @brief Render a black 64x31 tile ◆
 * @param out Target buffer
 * @param sx Target buffer coordinate (left corner of the tile)
 * @param sy Target buffer coordinate (bottom corner of the tile)
 */
void world_draw_black_tile(const Surface &out, int sx, int sy);

} // namespace devilution
