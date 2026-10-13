#pragma once
/**
 * @file fpv/fpv_state.hpp
 *
 * Internal state shared by the FPV gameplay (fpv.cpp) and HUD (fpv_hud.cpp).
 */

#include <cstdint>
#include <string>

#include "engine/point.hpp"
#include "fpv/fpv_math.hpp"
#include "render3d/fpv_contract.hpp"

namespace devilution::fpv {

/** Furthest the crosshair picks (tiles along the ray). */
constexpr float kPickRange = 16.0F;
/** E prompt / E use distance from the player (tiles). */
constexpr float kUseRange = 3.5F;
/** Highlight range for bows and attack spells (tiles). */
constexpr float kRangedAttackRange = 12.0F;

struct PickResult {
	TargetKind kind = TargetKind::None;
	int id = -1;
	float distance = 0.0F;
	/** Tile the cursor uses: the entity's tile, the trigger tile or the floor tile under the crosshair. */
	Point tile {};
	/** Horizontal distance from the player to the target (tiles). */
	float playerDistance = 0.0F;
	/** The ray hit something (entity, wall or floor) inside kPickRange. */
	bool hitWorld = false;
};

struct HudState {
	uint32_t helpUntil = 0;
	uint32_t bannerUntil = 0;
	std::string bannerText;
	uint32_t slashStart = 0;
	bool slashMirrored = false;
	bool slashActive = false;
	uint32_t hitMarkerUntil = 0;
};

const PickResult &GetPick();
HudState &GetHud();
/** Size of the 3D view in back-buffer pixels: full width, gnViewportHeight rows. */
int ViewWidth();
int ViewHeight();
/** SDL_GetTicks() in milliseconds as uint32. */
uint32_t NowMs();

} // namespace devilution::fpv
