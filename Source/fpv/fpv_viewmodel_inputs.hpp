#pragma once
/**
 * @file fpv/fpv_viewmodel_inputs.hpp
 *
 * Motion inputs for the first-person weapon viewmodel (the Hexen-style 3D
 * sword / shield / arms from warrior.glb drawn by render3d). Source/fpv fills
 * them once per drawn frame in fpv::UpdateFrame(); the renderer reads them to
 * drive bob, sway, turn lag and the swing animation.
 *
 * While a viewmodel is shown, the 2D slash overlay is hidden: ShowSlashOverlay()
 * is true only when the "First-Person Weapon" option is off or no viewmodel is
 * available. The crosshair and the hit marker are always drawn.
 */

#include <cstdint>

namespace devilution::fpv {

struct ViewmodelInputs {
	/** Horizontal speed of the camera, 0 (standing) .. 1 (walking / running at full speed). */
	float moveSpeed = 0.0F;
	/** Movement direction relative to the view, each -1 .. 1 (forward + / back -, strafe right + / left -), scaled by moveSpeed. */
	float forward = 0.0F;
	float strafe = 0.0F;
	/** Camera turn rates this frame in radians per second (yaw positive = turning left / counter-clockwise, pitch positive = looking up). */
	float yawRate = 0.0F;
	float pitchRate = 0.0F;
	/** Head-bob phase in radians (advances while walking). */
	float bobPhase = 0.0F;
	/** Incremented every time a melee or ranged attack starts: compare with the previous value to detect a new swing. */
	uint32_t swingCount = 0;
	/** SDL_GetTicks() at the start of the latest swing and whether it is the mirrored (alternate) swing. */
	uint32_t swingStartMs = 0;
	bool swingMirrored = false;
	/** The player is in an attack animation right now; rangedWeapon tells a bow from a melee weapon. */
	bool attacking = false;
	bool rangedWeapon = false;
	/** SDL_GetTicks() until which the hit marker shows (a melee hit landed). */
	uint32_t hitMarkerUntilMs = 0;
};

/** Inputs for the current frame (valid while fpv::IsActive()). */
const ViewmodelInputs &GetViewmodelInputs();

/**
 * Whether a 3D weapon viewmodel can be drawn this frame. Defaults to false;
 * render3d installs its query with SetViewmodelAvailableQuery() (to be wired at merge).
 */
bool ViewmodelAvailable();
using ViewmodelAvailableQuery = bool (*)();
void SetViewmodelAvailableQuery(ViewmodelAvailableQuery query);

/** True when the HUD draws the 2D slash overlay: option "First-Person Weapon" off, or no viewmodel available. */
bool ShowSlashOverlay();

} // namespace devilution::fpv
