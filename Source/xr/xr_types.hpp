#pragma once
/**
 * @file xr/xr_types.hpp
 *
 * Runtime-neutral VR frame data (OpenXR and WebXR both fill it) and the backend interface. Tracking space is the
 * runtime's floor-level reference space in metres: right-handed, y up, -z forward (OpenXR LOCAL_FLOOR / STAGE,
 * WebXR "local-floor").
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <webgpu/webgpu.h>

#include "render3d/math3d.hpp"

namespace devilution::xr {

using render3d::Mat4;
using render3d::Vec3;

struct Quat {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;
	float w = 1.0F;
};

inline Vec3 Rotate(const Quat &q, const Vec3 &v)
{
	// v' = v + 2w (q x v) + 2 q x (q x v)
	const Vec3 u { q.x, q.y, q.z };
	const Vec3 t = render3d::Cross(u, v) * 2.0F;
	return v + t * q.w + render3d::Cross(u, t);
}

struct Pose {
	Quat orientation;
	Vec3 position;
	bool valid = false;

	[[nodiscard]] Mat4 Matrix() const
	{
		const Vec3 x = Rotate(orientation, { 1.0F, 0.0F, 0.0F });
		const Vec3 y = Rotate(orientation, { 0.0F, 1.0F, 0.0F });
		const Vec3 z = Rotate(orientation, { 0.0F, 0.0F, 1.0F });
		Mat4 r = Mat4::Identity();
		r.at(0, 0) = x.x;
		r.at(1, 0) = x.y;
		r.at(2, 0) = x.z;
		r.at(0, 1) = y.x;
		r.at(1, 1) = y.y;
		r.at(2, 1) = y.z;
		r.at(0, 2) = z.x;
		r.at(1, 2) = z.y;
		r.at(2, 2) = z.z;
		r.at(0, 3) = position.x;
		r.at(1, 3) = position.y;
		r.at(2, 3) = position.z;
		return r;
	}
	[[nodiscard]] Vec3 Forward() const { return Rotate(orientation, { 0.0F, 0.0F, -1.0F }); }
};

/** Half-angles in radians (left and down negative), as OpenXR XrFovf. */
struct Fov {
	float angleLeft = -0.8F;
	float angleRight = 0.8F;
	float angleUp = 0.8F;
	float angleDown = -0.8F;
};

struct EyeInput {
	Pose pose;
	Fov fov;
};

struct HandInput {
	bool active = false;
	Pose aim;
	float stickX = 0.0F;
	float stickY = 0.0F;
	float trigger = 0.0F;
	float grip = 0.0F;
	bool primary = false;   // A / X
	bool secondary = false; // B / Y
	bool stickClick = false;
	bool menu = false; // left menu button (the right one is reserved by the system)
};

enum Hand : uint8_t {
	LeftHand = 0,
	RightHand = 1,
};

struct FrameInput {
	/** The runtime wants pixels this frame (false while the headset is off the face or the app is hidden). */
	bool shouldRender = false;
	Pose head;
	std::array<EyeInput, 2> eyes;
	std::array<HandInput, 2> hands;
	/** Seconds since the previous frame. */
	float deltaSeconds = 1.0F / 72.0F;
	/** The tracking space has no floor (OpenXR LOCAL fallback): the eye is at height 0. */
	bool trackingHasNoFloor = false;
};

/** One headset runtime: OpenXR (xr_openxr.cpp) or WebXR (xr_webxr.cpp). */
class Backend {
public:
	virtual ~Backend() = default;
	[[nodiscard]] virtual const char *Name() const = 0;
	virtual bool PreDeviceInit() = 0;
	[[nodiscard]] virtual WGPUTextureFormat PreferredColorFormat() const { return WGPUTextureFormat_RGBA8Unorm; }
	virtual void AppendDeviceFeatures(WGPUAdapter adapter, std::vector<WGPUFeatureName> &features) { }
	virtual bool Initialize(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat, float renderScale) = 0;
	/** Polls runtime events; true while a session runs. */
	virtual bool Update() = 0;
	[[nodiscard]] virtual bool OwnsDisplay() const { return false; }
	/** Waits for the next headset frame and fills the poses / input. False: no frame this time (do not call EndFrame). */
	virtual bool BeginFrame(FrameInput &input) = 0;
	/** The eye images to render into this frame (after BeginFrame returned true and shouldRender). */
	virtual bool AcquireEyeTargets(std::array<WGPUTextureView, 2> &targets, uint32_t &width, uint32_t &height) = 0;
	/** Hands the rendered eyes (or nothing when `rendered` is false) to the headset. */
	virtual void EndFrame(bool rendered) = 0;
	/** The user quit from the system menu: the game should exit. */
	[[nodiscard]] virtual bool ExitRequested() const { return false; }
	virtual void Shutdown() = 0;
};

/** Implemented by the backend source compiled into this build. */
Backend *CreateBackend();

} // namespace devilution::xr
