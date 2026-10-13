#pragma once
/**
 * @file render3d/skinned_math.hpp
 *
 * Small vector / quaternion / matrix helpers for the skinned glTF renderer (render3d/gltf_model.hpp,
 * render3d/skinned_renderer.hpp). Matrices are column-major float[16] (m[column * 4 + row]), the layout WGSL's
 * mat4x4f reads from a buffer, and act on column vectors (p' = M * p). Clip space follows WebGPU (z in [0, 1]).
 *
 * Private to the skinned renderer for now; render3d/math3d.hpp (world renderer) uses the same conventions, so the two
 * can be merged.
 */

#include <array>
#include <cmath>

namespace devilution::render3d::skm {

struct Vec3 {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline Vec3 Normalize(Vec3 a)
{
	const float length = std::sqrt(Dot(a, a));
	return length > 0.0F ? a * (1.0F / length) : a;
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
/** `v` rotated by `radians` about the unit vector `axis` (right-handed, Rodrigues). */
inline Vec3 RotateAround(Vec3 v, Vec3 axis, float radians)
{
	const float c = std::cos(radians);
	const float s = std::sin(radians);
	return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.0F - c));
}
/** Spherical interpolation of two unit directions (normalised lerp when they are (anti)parallel). */
inline Vec3 SlerpDirection(Vec3 a, Vec3 b, float t)
{
	const float cosine = std::fmax(-1.0F, std::fmin(1.0F, Dot(a, b)));
	const float angle = std::acos(cosine);
	const float s = std::sin(angle);
	if (s < 1e-3F)
		return Normalize(Lerp(a, b, t));
	return a * (std::sin((1.0F - t) * angle) / s) + b * (std::sin(t * angle) / s);
}

/** Rotation quaternion (x, y, z, w), glTF order. */
struct Quat {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;
	float w = 1.0F;
};

inline Quat Normalize(Quat q)
{
	const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (length <= 0.0F)
		return {};
	const float inv = 1.0F / length;
	return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

/** Spherical interpolation along the shorter arc (normalised lerp when the keys are nearly equal). */
inline Quat Slerp(Quat a, Quat b, float t)
{
	float cosTheta = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (cosTheta < 0.0F) {
		b = { -b.x, -b.y, -b.z, -b.w };
		cosTheta = -cosTheta;
	}
	float wa = 1.0F - t;
	float wb = t;
	if (cosTheta < 0.9995F) {
		const float theta = std::acos(cosTheta);
		const float sinTheta = std::sin(theta);
		wa = std::sin((1.0F - t) * theta) / sinTheta;
		wb = std::sin(t * theta) / sinTheta;
	}
	return Normalize(Quat { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb });
}

inline Quat AxisAngle(Vec3 axis, float radians)
{
	axis = Normalize(axis);
	const float s = std::sin(radians * 0.5F);
	return { axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5F) };
}

/** Column-major 4x4 matrix: m[column * 4 + row]. */
struct Mat4 {
	std::array<float, 16> m { 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F };

	float &operator()(int row, int column) { return m[static_cast<size_t>(column * 4 + row)]; }
	float operator()(int row, int column) const { return m[static_cast<size_t>(column * 4 + row)]; }

	static Mat4 Identity() { return {}; }
	static Mat4 Zero()
	{
		Mat4 r;
		r.m.fill(0.0F);
		return r;
	}
	static Mat4 FromArray(const float *columnMajor)
	{
		Mat4 r;
		for (size_t i = 0; i < 16; ++i)
			r.m[i] = columnMajor[i];
		return r;
	}
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
	Mat4 r = Mat4::Zero();
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			float sum = 0.0F;
			for (int k = 0; k < 4; ++k)
				sum += a(row, k) * b(k, column);
			r(row, column) = sum;
		}
	}
	return r;
}

inline Vec3 TransformPoint(const Mat4 &a, Vec3 p)
{
	return { a(0, 0) * p.x + a(0, 1) * p.y + a(0, 2) * p.z + a(0, 3),
		a(1, 0) * p.x + a(1, 1) * p.y + a(1, 2) * p.z + a(1, 3),
		a(2, 0) * p.x + a(2, 1) * p.y + a(2, 2) * p.z + a(2, 3) };
}

inline Vec3 TransformDirection(const Mat4 &a, Vec3 v)
{
	return { a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z,
		a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z,
		a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z };
}

/** General 4x4 inverse (cofactors); returns false (and leaves `out` untouched) for a singular matrix. */
inline bool Inverse(const Mat4 &a, Mat4 &out)
{
	const std::array<float, 16> &m = a.m;
	std::array<float, 16> inv;
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (std::abs(det) < 1e-20F)
		return false;
	for (size_t i = 0; i < 16; ++i)
		out.m[i] = inv[i] / det;
	return true;
}

/** translation * rotation * scale (glTF node TRS). */
inline Mat4 ComposeTrs(Vec3 t, Quat q, Vec3 s)
{
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	Mat4 r;
	r(0, 0) = (1.0F - 2.0F * (yy + zz)) * s.x;
	r(1, 0) = (2.0F * (xy + wz)) * s.x;
	r(2, 0) = (2.0F * (xz - wy)) * s.x;
	r(0, 1) = (2.0F * (xy - wz)) * s.y;
	r(1, 1) = (1.0F - 2.0F * (xx + zz)) * s.y;
	r(2, 1) = (2.0F * (yz + wx)) * s.y;
	r(0, 2) = (2.0F * (xz + wy)) * s.z;
	r(1, 2) = (2.0F * (yz - wx)) * s.z;
	r(2, 2) = (1.0F - 2.0F * (xx + yy)) * s.z;
	r(0, 3) = t.x;
	r(1, 3) = t.y;
	r(2, 3) = t.z;
	return r;
}

inline Mat4 Translation(Vec3 t) { return ComposeTrs(t, {}, { 1.0F, 1.0F, 1.0F }); }
inline Mat4 Scale(float s) { return ComposeTrs({}, {}, { s, s, s }); }
/** Rotation about +Y by `radians` (counter-clockwise seen from above: +Z turns toward +X). */
inline Mat4 RotationY(float radians) { return ComposeTrs({}, AxisAngle({ 0.0F, 1.0F, 0.0F }, radians), { 1.0F, 1.0F, 1.0F }); }

/**
 * View matrix from an orthonormal camera basis (right, up, back = towards the viewer) and the eye position: camera
 * space has +x right, +y up and looks down -z.
 */
inline Mat4 ViewFromBasis(Vec3 right, Vec3 up, Vec3 back, Vec3 eye)
{
	Mat4 r;
	r(0, 0) = right.x;
	r(0, 1) = right.y;
	r(0, 2) = right.z;
	r(0, 3) = -Dot(right, eye);
	r(1, 0) = up.x;
	r(1, 1) = up.y;
	r(1, 2) = up.z;
	r(1, 3) = -Dot(up, eye);
	r(2, 0) = back.x;
	r(2, 1) = back.y;
	r(2, 2) = back.z;
	r(2, 3) = -Dot(back, eye);
	return r;
}

/** Orthographic projection of camera space (looking down -z) to WebGPU clip space (z in [0, 1]). */
inline Mat4 Orthographic(float left, float right, float bottom, float top, float zNear, float zFar)
{
	Mat4 r;
	r(0, 0) = 2.0F / (right - left);
	r(1, 1) = 2.0F / (top - bottom);
	r(2, 2) = -1.0F / (zFar - zNear);
	r(0, 3) = -(right + left) / (right - left);
	r(1, 3) = -(top + bottom) / (top - bottom);
	r(2, 3) = -zNear / (zFar - zNear);
	return r;
}

/** Right-handed perspective (looking down -z) to WebGPU clip space (z in [0, 1]). */
inline Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
	const float f = 1.0F / std::tan(fovYRadians * 0.5F);
	Mat4 r = Mat4::Zero();
	r(0, 0) = f / aspect;
	r(1, 1) = f;
	r(2, 2) = zFar / (zNear - zFar);
	r(2, 3) = zNear * zFar / (zNear - zFar);
	r(3, 2) = -1.0F;
	return r;
}

} // namespace devilution::render3d::skm
