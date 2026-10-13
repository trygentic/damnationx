#include "fpv/fpv_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace devilution::fpv {

namespace {
constexpr float QuarterPi = kPi / 4.0F;
constexpr float NearPlane = 0.02F;
} // namespace

float WrapAngle(float radians)
{
	if (!std::isfinite(radians))
		return 0.0F;
	radians = std::fmod(radians, 2.0F * kPi);
	if (radians <= -kPi)
		radians += 2.0F * kPi;
	else if (radians > kPi)
		radians -= 2.0F * kPi;
	return radians;
}

// Direction index d (South = 0 ... SouthEast = 7, turning right each step) faces
// the tile step TileStep(d); its yaw is atan2(step.x, step.y) = pi/4 - d * pi/4.
float DirectionToYaw(Direction dir)
{
	if (dir == Direction::NoDirection)
		return 0.0F;
	const int d = static_cast<int>(dir);
	return WrapAngle(QuarterPi - static_cast<float>(d) * QuarterPi);
}

Direction YawToDirection(float yaw)
{
	const float steps = (QuarterPi - WrapAngle(yaw)) / QuarterPi;
	int d = static_cast<int>(std::lround(steps)) % 8;
	if (d < 0)
		d += 8;
	return static_cast<Direction>(d);
}

float HorizontalToVerticalFov(float horizontalDeg, float aspect)
{
	if (aspect <= 0.0F)
		aspect = 1.0F;
	horizontalDeg = std::clamp(horizontalDeg, 10.0F, 170.0F);
	const float halfH = horizontalDeg * kPi / 360.0F;
	const float halfV = std::atan(std::tan(halfH) / aspect);
	return halfV * 360.0F / kPi;
}

CameraBasis MakeBasis(float yaw, float pitch)
{
	CameraBasis basis;
	const float cp = std::cos(pitch);
	basis.forward = { std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp };
	basis.right = { -std::cos(yaw), 0.0F, std::sin(yaw) };
	const Vec3f &r = basis.right;
	const Vec3f &f = basis.forward;
	basis.up = { r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x };
	return basis;
}

Ray ScreenRay(const CameraState &camera, float px, float py, int width, int height)
{
	const CameraBasis basis = MakeBasis(camera.yaw, camera.pitch);
	const float w = static_cast<float>(std::max(width, 1));
	const float h = static_cast<float>(std::max(height, 1));
	const float tanY = std::tan(camera.fovYDeg * kPi / 360.0F);
	const float aspect = w / h;
	const float ndcX = 2.0F * px / w - 1.0F;
	const float ndcY = 1.0F - 2.0F * py / h;
	const Vec3f dir = basis.forward + basis.right * (ndcX * tanY * aspect) + basis.up * (ndcY * tanY);
	return Ray { { camera.x, camera.y, camera.z }, dir.normalized() };
}

std::optional<ScreenPoint> ProjectToScreen(const CameraState &camera, Vec3f point, int width, int height)
{
	const CameraBasis basis = MakeBasis(camera.yaw, camera.pitch);
	const Vec3f v = point - Vec3f { camera.x, camera.y, camera.z };
	const float depth = v.dot(basis.forward);
	if (depth < NearPlane)
		return std::nullopt;
	const float w = static_cast<float>(std::max(width, 1));
	const float h = static_cast<float>(std::max(height, 1));
	const float tanY = std::tan(camera.fovYDeg * kPi / 360.0F);
	const float aspect = w / h;
	const float sx = v.dot(basis.right) / (depth * tanY * aspect);
	const float sy = v.dot(basis.up) / (depth * tanY);
	return ScreenPoint { (sx + 1.0F) * 0.5F * w, (1.0F - sy) * 0.5F * h, depth };
}

std::optional<float> RayCylinder(const Ray &ray, float cx, float cz, float radius, float y0, float y1)
{
	const float ox = ray.origin.x - cx;
	const float oz = ray.origin.z - cz;
	const float dx = ray.dir.x;
	const float dz = ray.dir.z;
	const float r2 = radius * radius;

	float best = -1.0F;
	const auto inHeight = [&](float t) {
		const float y = ray.origin.y + ray.dir.y * t;
		return y >= y0 && y <= y1;
	};

	// Origin inside the cylinder.
	if (ox * ox + oz * oz <= r2 && inHeight(0.0F))
		return 0.0F;

	// Side.
	const float a = dx * dx + dz * dz;
	if (a > 1e-9F) {
		const float b = 2.0F * (ox * dx + oz * dz);
		const float c = ox * ox + oz * oz - r2;
		const float disc = b * b - 4.0F * a * c;
		if (disc >= 0.0F) {
			const float sq = std::sqrt(disc);
			const float t0 = (-b - sq) / (2.0F * a);
			const float t1 = (-b + sq) / (2.0F * a);
			if (t0 >= 0.0F && inHeight(t0))
				best = t0;
			else if (t0 < 0.0F && t1 >= 0.0F && inHeight(t1))
				best = t1;
		}
	}

	// Caps.
	if (std::abs(ray.dir.y) > 1e-9F) {
		for (const float capY : { y0, y1 }) {
			const float t = (capY - ray.origin.y) / ray.dir.y;
			if (t < 0.0F)
				continue;
			const float px = ox + dx * t;
			const float pz = oz + dz * t;
			if (px * px + pz * pz <= r2 && (best < 0.0F || t < best))
				best = t;
		}
	}

	if (best < 0.0F)
		return std::nullopt;
	return best;
}

std::optional<float> RayBox(const Ray &ray, Vec3f boxMin, Vec3f boxMax)
{
	float tMin = 0.0F;
	float tMax = 1e30F;
	const float o[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
	const float d[3] = { ray.dir.x, ray.dir.y, ray.dir.z };
	const float lo[3] = { boxMin.x, boxMin.y, boxMin.z };
	const float hi[3] = { boxMax.x, boxMax.y, boxMax.z };
	for (int i = 0; i < 3; ++i) {
		if (std::abs(d[i]) < 1e-9F) {
			if (o[i] < lo[i] || o[i] > hi[i])
				return std::nullopt;
			continue;
		}
		float t0 = (lo[i] - o[i]) / d[i];
		float t1 = (hi[i] - o[i]) / d[i];
		if (t0 > t1)
			std::swap(t0, t1);
		tMin = std::max(tMin, t0);
		tMax = std::min(tMax, t1);
		if (tMin > tMax)
			return std::nullopt;
	}
	return tMin;
}

std::optional<float> RayFloor(const Ray &ray)
{
	if (ray.dir.y >= -1e-6F || ray.origin.y < 0.0F)
		return std::nullopt;
	return -ray.origin.y / ray.dir.y;
}

} // namespace devilution::fpv
