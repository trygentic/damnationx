#pragma once

#include <cstdint>

#include "engine/animationinfo.h"
#include "engine/freeform_movement.hpp"
#include "engine/point.hpp"
#include "engine/world_tile.hpp"

namespace devilution {

struct ActorPosition {
	WorldTilePosition tile;
	/** Future tile position. Set at start of walking animation. */
	WorldTilePosition future;
	/** Tile position of player. Set via network on player input. */
	WorldTilePosition last;
	/** Most recent position in dPlayer. */
	WorldTilePosition old;
	/** Used for referring to position of player when finishing moving one tile (also used to define target coordinates for spells and ranged attacks) */
	WorldTilePosition temp;
	/** Drawn path of the current walk step (free-form movement). Inactive means the classic fixed 8-direction offset is used. */
	FreeformStep freeform;
	/**
	 * Drawn offset from the centre of the current tile, carried from one walk step to the next.
	 * While not walking the actor is drawn at this offset and it eases back to zero (see SettleFreeformCarry).
	 */
	SubTileDisplacement freeformCarry;

	/** @brief Calculates the offset for the walking animation. */
	DisplacementOf<int8_t> CalculateWalkingOffset(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Calculates the offset for the walking animation. */
	DisplacementOf<int16_t> CalculateWalkingOffsetShifted4(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Calculates the offset for the walking animation. */
	DisplacementOf<int16_t> CalculateWalkingOffsetShifted8(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Returns Pixel velocity while walking. */
	DisplacementOf<int16_t> GetWalkingVelocityShifted4(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Returns Pixel velocity while walking. */
	DisplacementOf<int16_t> GetWalkingVelocityShifted8(Direction dir, const AnimationInfo &animInfo) const;

	/**
	 * @brief Drawn world offset from the centre of the current tile in sub-tile units.
	 * @param walking Whether the actor is in a walk mode; the step is then given by dir and animInfo.
	 */
	[[nodiscard]] SubTileDisplacement GetDrawnSubTileOffset(bool walking, Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Drawn screen offset in pixels (free-form aware replacement for GetOffsetForWalking). */
	[[nodiscard]] Displacement GetDrawnScreenOffset(bool walking, Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Drawn light offset (free-form aware replacement for CalculateWalkingOffset(...).screenToLight()). */
	[[nodiscard]] Displacement GetDrawnLightOffset(bool walking, Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Eases freeformCarry back towards the tile centre. Call once per game tick while not walking. */
	void SettleFreeformCarry();
	/** @brief Forgets all free-form state, e.g. on teleport, level change or load. */
	void ResetFreeform();
};

} // namespace devilution
