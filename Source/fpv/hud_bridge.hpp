#pragma once
/**
 * @file fpv/hud_bridge.hpp
 *
 * Pointer ownership between the 3D views and the HUD core (modules/hud, diablo_hud::core).
 *
 * While mouselook holds the pointer (relative mouse / pointer lock), the world owns it and ordinary HUD windows,
 * the clock, the minimap and elements must not claim it; only a modal still does (and pauses mouselook through
 * fpv::SetHudWantsCursorQuery). When mouselook starts or stops, any HUD button pair or drag in progress is cancelled:
 * its press and its release belong to different owners. Pure; the host passes its own snapshots.
 */

#include <cstdint>
#include <vector>

#include <diablo_hud/input.hpp>

namespace devilution::fpv {

/**
 * HUD owner of a pointer position (in the host's logical HUD space). `worldCaptured` is fpv::IsMouseLook(): the
 * crosshair drives the world and the HUD declines everything but a modal.
 */
inline diablo_hud::Owner HudOwnerAt(diablo_hud::Point point, const std::vector<diablo_hud::WindowRecord> &windows,
    bool worldCaptured, bool modal = false, diablo_hud::Overlay clock = {}, diablo_hud::Overlay minimap = {},
    const std::vector<diablo_hud::Overlay> &elements = {})
{
	return diablo_hud::ResolvePhysicalOwner(point, windows, clock, minimap, elements, modal, worldCaptured);
}

/**
 * Tracks mouselook on/off edges for a HUD host: call every frame with fpv::IsMouseLook(). On an edge the HUD's owned
 * buttons are cancelled (a press under the free cursor never gets its release delivered through mouselook, and the
 * other way round). Returns true on an edge.
 */
class HudCaptureTracker {
public:
	bool update(bool worldCaptured, diablo_hud::InputPairs &pairs)
	{
		if (worldCaptured == captured_)
			return false;
		captured_ = worldCaptured;
		pairs.cancel();
		++generation_;
		return true;
	}
	[[nodiscard]] bool captured() const { return captured_; }
	/** Bumped on every edge, for hosts that keep drags elsewhere. */
	[[nodiscard]] uint32_t generation() const { return generation_; }

private:
	bool captured_ = false;
	uint32_t generation_ = 0;
};

} // namespace devilution::fpv
