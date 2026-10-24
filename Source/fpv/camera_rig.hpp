#pragma once
/**
 * @file fpv/camera_rig.hpp
 *
 * Pure maths of the third-person chase camera (no game state): where the eye wants to be behind the player, how far
 * it may go before it hits the 3D world, and how its boom length eases. World space as in render3d/fpv_contract.hpp.
 *
 * Collision is a sphere swept from the pivot (above the player) towards the wanted eye through an occupancy query
 * isBlocked(x, y, z) (in the game: the 3D level's occluder columns, the floor, the dungeon ceiling and the map edge).
 * The sweep samples the segment densely and tests the centre plus six points on the sphere, which is exact enough for
 * a world made of tile-sized columns and cheap (a few hundred queries per frame).
 */

#include <algorithm>
#include <cmath>

#include "fpv/fpv_math.hpp"

namespace devilution::fpv {

/** Third-person limits (world units = tiles). */
constexpr float kThirdPersonMinDistance = 0.8F;
constexpr float kThirdPersonMaxDistance = 6.0F;
/** Pivot above the player's feet the camera orbits (about the warrior's shoulders). */
constexpr float kThirdPersonPivotHeight = 0.55F;
/** Lateral offset of the over-the-shoulder camera at full boom length. */
constexpr float kShoulderOffset = 0.32F;
/** Sphere kept around the eye so the near plane never enters a wall. */
constexpr float kCameraCollisionRadius = 0.1F;
/** Below this resolved boom length the local player is hidden (the camera would be inside it). */
constexpr float kHidePlayerDistance = 0.32F;
/** Third-person pitch range (radians, positive looks up). */
constexpr float kThirdPersonMinPitch = -70.0F * kPi / 180.0F;
constexpr float kThirdPersonMaxPitch = 30.0F * kPi / 180.0F;

/**
 * The eye a boom of `distance` behind `pivot` wants, looking along (yaw, pitch), shifted sideways by `shoulder`
 * (positive: the camera sits to the right of the player, so the player is on the left of the crosshair).
 */
inline Vec3f ThirdPersonDesiredEye(Vec3f pivot, float yaw, float pitch, float distance, float shoulder)
{
	const CameraBasis basis = MakeBasis(yaw, pitch);
	return pivot - basis.forward * distance + basis.right * shoulder;
}

/**
 * Sweeps a sphere of `radius` from `from` to `to`. Returns the largest fraction in [0, 1] of the segment the sphere
 * can travel without touching a blocked point. `from` itself is assumed free (the pivot inside the player).
 */
template <typename IsBlocked>
float SweepCamera(Vec3f from, Vec3f to, float radius, IsBlocked &&isBlocked)
{
	const Vec3f delta = to - from;
	const float length = delta.length();
	if (length < 1e-5F)
		return 1.0F;
	const float step = std::max(radius * 0.5F, 0.02F);
	const int samples = std::min(static_cast<int>(std::ceil(length / step)), 600);
	const auto blockedAt = [&](Vec3f p) {
		return isBlocked(p.x, p.y, p.z)
		    || isBlocked(p.x + radius, p.y, p.z) || isBlocked(p.x - radius, p.y, p.z)
		    || isBlocked(p.x, p.y + radius, p.z) || isBlocked(p.x, p.y - radius, p.z)
		    || isBlocked(p.x, p.y, p.z + radius) || isBlocked(p.x, p.y, p.z - radius);
	};
	float safe = 0.0F;
	for (int i = 1; i <= samples; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(samples);
		if (blockedAt(from + delta * t))
			return safe;
		safe = t;
	}
	return 1.0F;
}

/**
 * Eases the camera's boom length towards the collision-free length: it snaps in at once when something comes between
 * the player and the camera (never show a wall's inside) and slides back out over `outTimeConstant` seconds.
 */
inline float EaseBoom(float current, float target, float dt, float outTimeConstant = 0.25F)
{
	if (target <= current)
		return target;
	return current + (target - current) * SmoothingAlpha(dt, outTimeConstant);
}

/** One mouse-wheel notch of third-person zoom: multiplicative, clamped to the limits. */
inline float ZoomBoom(float distance, float notches)
{
	return std::clamp(distance * std::pow(0.88F, notches), kThirdPersonMinDistance, kThirdPersonMaxDistance);
}

/**
 * Picking from a chase camera: moves the ray's origin up to the point closest to the player (minus `margin`), so
 * nothing between the camera and the player is selected and the distances stay comparable with first person.
 */
inline Ray ChaseAimRay(const Ray &cameraRay, Vec3f pivot, float margin = 0.35F)
{
	const float along = (pivot - cameraRay.origin).dot(cameraRay.dir) - margin;
	if (along <= 0.0F)
		return cameraRay;
	return Ray { cameraRay.at(along), cameraRay.dir };
}

/** Wraps the shortest way from `from` to `to` (radians), moving at most the smoothing share for this step. */
inline float EaseAngle(float from, float to, float dt, float timeConstant)
{
	return WrapAngle(from + WrapAngle(to - from) * SmoothingAlpha(dt, timeConstant));
}

} // namespace devilution::fpv
