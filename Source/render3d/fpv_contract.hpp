#pragma once
/**
 * @file render3d/fpv_contract.hpp
 *
 * Shared contract between the first-person (FPV) / 3D subsystems. See
 * docs/design/first-person-3d.md for the full design.
 *
 *   Source/fpv/        owns gameplay: camera state, input, picking, HUD overlays.
 *   Source/render3d/   owns the WebGPU 3D world renderer (geometry, billboards,
 *                      ray-traced lighting) and the skinned glTF mesh renderer.
 *   hd toggle          owns runtime HD pack on/off.
 *
 * World space used by everything 3D (right-handed, Y up):
 *   x = dungeon tile x (east-ish in tile space), z = dungeon tile y,
 *   y = height in tiles. 1 tile = 1 world unit (~2 m of character scale;
 *   the 1.85 m warrior glb is scaled by kWarriorMetersToWorld).
 *   The centre of tile (tx, ty) is (tx + 0.5, 0, ty + 0.5).
 *   Yaw 0 looks toward +z (tile +y); yaw grows counter-clockwise when viewed
 *   from above. DirectionToYaw maps the classic 8 facings onto this.
 */

#include <cstdint>

namespace devilution {

enum class Direction : std::uint8_t;

namespace fpv {

constexpr float kEyeHeight = 0.62F;          // world units above floor
// Host heroes stand up to ~1.52 tiles tall. Architecture follows that contract;
// changing a doorway must also change the surrounding masonry and lintel.
constexpr float kDungeonOpeningHeight = 3.2F;
constexpr float kDungeonOpeningWidth = 1.6F;
constexpr float kWallHeight = 3.65F;
constexpr float kWarriorMetersToWorld = 0.36F; // 1.85 m -> ~0.67 tiles tall
constexpr float kDefaultFovYDeg = 70.0F;     // ~95 deg horizontal at 16:9

struct CameraState {
	float x = 0.0F; // world position (see file comment)
	float y = kEyeHeight;
	float z = 0.0F;
	float yaw = 0.0F;   // radians
	float pitch = 0.0F; // radians, clamped to about +-75 deg
	float fovYDeg = kDefaultFovYDeg;
};

/**
 * True when a 3D view is on (Graphics > Camera Mode first or third person, or a VR view provider is installed) and the
 * WebGPU 3D renderer is usable. Implemented in Source/fpv.
 */
bool IsActive();

/** IsActive() with the first-person camera (weapon viewmodel, no local player body except the legs). */
bool IsFirstPerson();
/** IsActive() with the third-person chase camera (the local player is drawn). */
bool IsThirdPerson();

/** Camera for the current frame (interpolated), from the active view provider (fpv/view_provider.hpp). */
const CameraState &GetCamera();

/** Yaw (radians) that matches a classic 8-way facing, and the inverse. Implemented in Source/fpv. */
float DirectionToYaw(Direction dir);
Direction YawToDirection(float yaw);

/** What the crosshair is on. Filled by Source/fpv each frame, read by render3d (red outline). */
enum class TargetKind : std::uint8_t { None, Monster, Player, Item, Object, Door, Trigger, Towner };
struct CrosshairTarget {
	TargetKind kind = TargetKind::None;
	int id = -1;       // monster/player/item/object index, trigger index
	float distance = 0.0F;
	bool inRange = false; // red outline / attackable now
};
const CrosshairTarget &GetCrosshairTarget();

} // namespace fpv

namespace render3d {

/**
 * Called from the WebGPU presenter when fpv::IsActive(). Renders the 3D world
 * (and skinned actors) into the swapchain before the 8-bit UI layer is
 * composited on top. Implemented in Source/render3d.
 */
struct FrameTargets; // opaque, defined by render3d/presenter glue

/** Invalidate cached level geometry (new level, door state change, HD toggle). */
void InvalidateLevel();

} // namespace render3d

} // namespace devilution
