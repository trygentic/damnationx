#pragma once
/**
 * @file render3d/fpv_contract.hpp
 *
 * Shared contract between the first-person (FPV) / 3D subsystems.
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
constexpr float kWallHeight = 1.0F;          // dungeon wall height in world units
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

/** True when the FPV option is on and the WebGPU 3D renderer is usable. Implemented in Source/fpv. */
bool IsActive();

/** Camera for the current frame (interpolated). Implemented in Source/fpv. */
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
