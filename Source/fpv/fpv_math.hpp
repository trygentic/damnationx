#pragma once
/**
 * @file fpv/fpv_math.hpp
 *
 * Pure maths for the first-person view (no game state): yaw <-> facing, the
 * camera basis, screen rays, ray vs tile grid (DDA), ray vs cylinder / box and
 * projection back to the screen. World space as in render3d/fpv_contract.hpp:
 * x = tile x, z = tile y, y up, 1 tile = 1 unit, yaw 0 looks toward +z and
 * grows counter-clockwise seen from above, so forward = (sin yaw, 0, cos yaw)
 * and right = (-cos yaw, 0, sin yaw).
 */

#include <algorithm>
#include <cmath>
#include <optional>

#include "engine/direction.hpp"
#include "render3d/fpv_contract.hpp"

namespace devilution::fpv {

constexpr float kPi = 3.14159265358979323846F;

struct Vec3f {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;

	constexpr Vec3f operator+(const Vec3f &o) const { return { x + o.x, y + o.y, z + o.z }; }
	constexpr Vec3f operator-(const Vec3f &o) const { return { x - o.x, y - o.y, z - o.z }; }
	constexpr Vec3f operator*(float s) const { return { x * s, y * s, z * s }; }
	[[nodiscard]] constexpr float dot(const Vec3f &o) const { return x * o.x + y * o.y + z * o.z; }
	[[nodiscard]] float length() const { return std::sqrt(dot(*this)); }
	[[nodiscard]] Vec3f normalized() const
	{
		const float len = length();
		return len > 0.0F ? *this * (1.0F / len) : *this;
	}
};

struct Ray {
	Vec3f origin;
	Vec3f dir; // normalised
	[[nodiscard]] Vec3f at(float t) const { return origin + dir * t; }
};

/** Wraps an angle to (-pi, pi]. */
float WrapAngle(float radians);

/** Classic facing for a yaw, see fpv_contract.hpp (also declared there). */
float DirectionToYaw(Direction dir);
Direction YawToDirection(float yaw);

/** Vertical FOV (degrees) for a horizontal FOV (degrees) at the given aspect (width / height). */
float HorizontalToVerticalFov(float horizontalDeg, float aspect);

struct CameraBasis {
	Vec3f forward;
	Vec3f right;
	Vec3f up;
};
CameraBasis MakeBasis(float yaw, float pitch);

/** Horizontal (ground plane) forward / right for a yaw, as (x, z) in tile units. */
inline void GroundAxes(float yaw, float &fwdX, float &fwdZ, float &rightX, float &rightZ)
{
	fwdX = std::sin(yaw);
	fwdZ = std::cos(yaw);
	rightX = -std::cos(yaw);
	rightZ = std::sin(yaw);
}

/**
 * Ray from the camera through a pixel of the 3D view (width x height pixels,
 * pixel centres at +0.5). Symmetric perspective, vertical FOV fovYDeg.
 */
Ray ScreenRay(const CameraState &camera, float px, float py, int width, int height);

/** Projects a world point into the 3D view; nullopt if behind the near plane. */
struct ScreenPoint {
	float x;
	float y;
	float depth; // distance along the view forward axis
};
std::optional<ScreenPoint> ProjectToScreen(const CameraState &camera, Vec3f point, int width, int height);

/** Ray vs vertical cylinder (axis through cx, cz, from y0 to y1). Returns the entry distance. */
std::optional<float> RayCylinder(const Ray &ray, float cx, float cz, float radius, float y0, float y1);

/** Ray vs axis aligned box. Returns the entry distance (0 if the origin is inside). */
std::optional<float> RayBox(const Ray &ray, Vec3f boxMin, Vec3f boxMax);

/** Ray vs the floor plane y = 0 (only hits from above). */
std::optional<float> RayFloor(const Ray &ray);

struct GridHit {
	bool hit = false;
	int tileX = 0;
	int tileY = 0;
	float distance = 0.0F; // distance along the 3D ray where the solid tile is entered
	/** Last open tile visited before the solid one (or before maxDistance). */
	int lastOpenX = 0;
	int lastOpenY = 0;
};

/**
 * Walks the tile grid along the horizontal projection of a 3D ray (Amanatides
 * and Woo DDA) and stops at the first tile isSolid(x, y) accepts. Distances are
 * measured along the 3D ray, so a steep ray reaches a wall later. visit(x, y,
 * tEnter, tExit) is called for every open tile crossed, in order. The start tile
 * is never reported as solid.
 */
template <typename IsSolid, typename Visit>
GridHit TraceGrid(const Ray &ray, float maxDistance, IsSolid &&isSolid, Visit &&visit)
{
	GridHit result;
	int x = static_cast<int>(std::floor(ray.origin.x));
	int y = static_cast<int>(std::floor(ray.origin.z));
	result.lastOpenX = x;
	result.lastOpenY = y;

	const float dx = ray.dir.x;
	const float dz = ray.dir.z;
	const int stepX = dx > 0 ? 1 : -1;
	const int stepY = dz > 0 ? 1 : -1;
	constexpr float Inf = 1e30F;
	const float tDeltaX = dx != 0 ? std::abs(1.0F / dx) : Inf;
	const float tDeltaY = dz != 0 ? std::abs(1.0F / dz) : Inf;
	float tMaxX = dx != 0 ? ((dx > 0 ? (static_cast<float>(x) + 1.0F - ray.origin.x) : (ray.origin.x - static_cast<float>(x))) * tDeltaX) : Inf;
	float tMaxY = dz != 0 ? ((dz > 0 ? (static_cast<float>(y) + 1.0F - ray.origin.z) : (ray.origin.z - static_cast<float>(y))) * tDeltaY) : Inf;

	float tEnter = 0.0F;
	for (int guard = 0; guard < 512; ++guard) {
		const float tExit = std::min(std::min(tMaxX, tMaxY), maxDistance);
		visit(x, y, tEnter, tExit);
		if (tExit >= maxDistance)
			return result;
		if (tMaxX < tMaxY) {
			x += stepX;
			tEnter = tMaxX;
			tMaxX += tDeltaX;
		} else {
			y += stepY;
			tEnter = tMaxY;
			tMaxY += tDeltaY;
		}
		if (isSolid(x, y)) {
			result.hit = true;
			result.tileX = x;
			result.tileY = y;
			result.distance = tEnter;
			return result;
		}
		result.lastOpenX = x;
		result.lastOpenY = y;
	}
	return result;
}

/** Exponential smoothing factor for a time step (seconds) and time constant (seconds). */
inline float SmoothingAlpha(float dt, float timeConstant)
{
	if (timeConstant <= 0.0F)
		return 1.0F;
	return 1.0F - std::exp(-dt / timeConstant);
}

} // namespace devilution::fpv
