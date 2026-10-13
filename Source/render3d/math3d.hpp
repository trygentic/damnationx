#pragma once
/**
 * @file render3d/math3d.hpp
 *
 * Small vector/matrix helpers for the first-person 3D renderer (see render3d/fpv_contract.hpp for the world space).
 *
 * Mat4 is column-major (m[column][row]), the memory layout WGSL's mat4x4f expects, so a Mat4 can be copied into a
 * uniform buffer as is. Projections target WebGPU clip space: x, y in [-1, 1], depth in [0, 1].
 */

#include <array>
#include <cmath>

namespace devilution::render3d {

struct Vec2 {
	float x = 0.0F;
	float y = 0.0F;
};

struct Vec3 {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;

	constexpr Vec3 operator+(const Vec3 &o) const { return { x + o.x, y + o.y, z + o.z }; }
	constexpr Vec3 operator-(const Vec3 &o) const { return { x - o.x, y - o.y, z - o.z }; }
	constexpr Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
	constexpr Vec3 operator-() const { return { -x, -y, -z }; }
	Vec3 &operator+=(const Vec3 &o)
	{
		x += o.x;
		y += o.y;
		z += o.z;
		return *this;
	}
};

struct Vec4 {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;
	float w = 0.0F;
};

constexpr float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 Cross(const Vec3 &a, const Vec3 &b)
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(const Vec3 &v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3 &v)
{
	const float len = Length(v);
	return len > 1e-12F ? v * (1.0F / len) : Vec3 {};
}

/** Column-major 4x4 matrix: m[c][r]. */
struct Mat4 {
	std::array<std::array<float, 4>, 4> m {};

	static constexpr Mat4 Identity()
	{
		Mat4 r;
		r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0F;
		return r;
	}

	[[nodiscard]] constexpr float at(int row, int column) const { return m[column][row]; }
	constexpr float &at(int row, int column) { return m[column][row]; }

	constexpr Mat4 operator*(const Mat4 &o) const
	{
		Mat4 r;
		for (int c = 0; c < 4; ++c) {
			for (int row = 0; row < 4; ++row) {
				float sum = 0.0F;
				for (int k = 0; k < 4; ++k)
					sum += at(row, k) * o.at(k, c);
				r.at(row, c) = sum;
			}
		}
		return r;
	}

	[[nodiscard]] constexpr Vec4 operator*(const Vec4 &v) const
	{
		return {
			at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
			at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
			at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
			at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w,
		};
	}

	[[nodiscard]] Vec3 TransformPoint(const Vec3 &p) const
	{
		const Vec4 r = *this * Vec4 { p.x, p.y, p.z, 1.0F };
		return { r.x, r.y, r.z };
	}

	static constexpr Mat4 Translation(const Vec3 &t)
	{
		Mat4 r = Identity();
		r.at(0, 3) = t.x;
		r.at(1, 3) = t.y;
		r.at(2, 3) = t.z;
		return r;
	}

	static constexpr Mat4 Scale(const Vec3 &s)
	{
		Mat4 r;
		r.at(0, 0) = s.x;
		r.at(1, 1) = s.y;
		r.at(2, 2) = s.z;
		r.at(3, 3) = 1.0F;
		return r;
	}

	/** Rotation about +Y (counter-clockwise seen from above, the fpv yaw convention). */
	static Mat4 RotationY(float radians)
	{
		const float c = std::cos(radians);
		const float s = std::sin(radians);
		Mat4 r = Identity();
		r.at(0, 0) = c;
		r.at(0, 2) = s;
		r.at(2, 0) = -s;
		r.at(2, 2) = c;
		return r;
	}
};

/** Right-handed perspective, WebGPU depth range [0, 1] (near -> 0, far -> 1). */
inline Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
	const float f = 1.0F / std::tan(fovYRadians * 0.5F);
	Mat4 r;
	r.at(0, 0) = f / aspect;
	r.at(1, 1) = f;
	r.at(2, 2) = zFar / (zNear - zFar);
	r.at(2, 3) = (zNear * zFar) / (zNear - zFar);
	r.at(3, 2) = -1.0F;
	return r;
}

/** Right-handed view matrix (camera looks down -Z in view space). */
inline Mat4 LookAt(const Vec3 &eye, const Vec3 &target, const Vec3 &up)
{
	const Vec3 f = Normalize(target - eye);
	const Vec3 s = Normalize(Cross(f, up));
	const Vec3 u = Cross(s, f);
	Mat4 r = Mat4::Identity();
	r.at(0, 0) = s.x;
	r.at(0, 1) = s.y;
	r.at(0, 2) = s.z;
	r.at(1, 0) = u.x;
	r.at(1, 1) = u.y;
	r.at(1, 2) = u.z;
	r.at(2, 0) = -f.x;
	r.at(2, 1) = -f.y;
	r.at(2, 2) = -f.z;
	r.at(0, 3) = -Dot(s, eye);
	r.at(1, 3) = -Dot(u, eye);
	r.at(2, 3) = Dot(f, eye);
	return r;
}

/** General 4x4 inverse (cofactor expansion). Returns identity for a singular matrix. */
inline Mat4 Inverse(const Mat4 &mat)
{
	// Row-major working copy.
	float a[16];
	for (int row = 0; row < 4; ++row)
		for (int c = 0; c < 4; ++c)
			a[row * 4 + c] = mat.at(row, c);
	float inv[16];
	inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
	inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
	inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
	inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
	inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
	inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
	inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
	inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
	inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
	inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
	inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
	inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
	inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
	inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
	inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
	inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
	const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
	if (std::fabs(det) < 1e-20F)
		return Mat4::Identity();
	const float invDet = 1.0F / det;
	Mat4 r;
	for (int row = 0; row < 4; ++row)
		for (int c = 0; c < 4; ++c)
			r.at(row, c) = inv[row * 4 + c] * invDet;
	return r;
}

/** Forward vector for the fpv camera (yaw 0 looks toward +z, yaw grows counter-clockwise seen from above). */
inline Vec3 YawPitchForward(float yaw, float pitch)
{
	const float cp = std::cos(pitch);
	return { std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp };
}

} // namespace devilution::render3d
