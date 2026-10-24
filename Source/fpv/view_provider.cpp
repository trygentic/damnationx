/**
 * @file fpv/view_provider.cpp
 *
 * The built-in first- and third-person cameras, the default gameplay stage of render3d::ViewProvider and the provider
 * registry (pure, see render3d/view_provider.hpp and fpv/view_provider.hpp).
 */
#include "fpv/view_provider.hpp"

#include <algorithm>
#include <cmath>

#include "fpv/camera_rig.hpp"

namespace devilution::fpv {

namespace {

constexpr float kMaxFirstPersonPitch = 75.0F * kPi / 180.0F;
constexpr float kHeadBobAmplitude = 0.012F;

render3d::ViewProvider *Override = nullptr;

} // namespace

ViewOutput FirstPersonCamera(const ViewInputs &inputs)
{
	ViewOutput out;
	out.camera.x = inputs.feet.x;
	out.camera.z = inputs.feet.z;
	out.camera.y = inputs.floorY + kEyeHeight + (inputs.headBob ? kHeadBobAmplitude * std::sin(inputs.bobPhase) : 0.0F);
	out.camera.yaw = inputs.yaw;
	out.camera.pitch = std::clamp(inputs.pitch, -kMaxFirstPersonPitch, kMaxFirstPersonPitch);
	out.camera.fovYDeg = inputs.fovYDeg;
	out.drawViewmodel = true;
	out.drawFirstPersonBody = inputs.fullBody;
	out.aimPivot = { out.camera.x, out.camera.y, out.camera.z };
	return out;
}

void ThirdPersonView::Reset(const ViewInputs &inputs)
{
	valid_ = false;
	yaw_ = inputs.yaw;
	side_ = inputs.style == ThirdPersonStyle::OverShoulder ? inputs.shoulderSide : 0.0F;
}

ViewOutput ThirdPersonView::Update(const ViewInputs &inputs)
{
	const float dt = std::max(inputs.dt, 0.0F);

	// Orbit chase: while the player walks to a clicked spot and the mouse rests, swing in behind it.
	if (inputs.style == ThirdPersonStyle::Orbit && inputs.autoWalking && inputs.idleLookSeconds > 1.0F)
		yaw_ = EaseAngle(yaw_, inputs.playerYaw, dt, 0.6F);
	else
		yaw_ = inputs.yaw;
	const float pitch = std::clamp(inputs.pitch, kThirdPersonMinPitch, kThirdPersonMaxPitch);

	const float wantedSide = inputs.style == ThirdPersonStyle::OverShoulder ? inputs.shoulderSide : 0.0F;
	side_ = valid_ ? side_ + (wantedSide - side_) * SmoothingAlpha(dt, 0.12F) : wantedSide;

	const float distance = std::clamp(inputs.distance, kThirdPersonMinDistance, kThirdPersonMaxDistance);
	Vec3f pivot { inputs.feet.x, inputs.floorY + kThirdPersonPivotHeight, inputs.feet.z };
	pivot.y = std::min(pivot.y, inputs.ceilingY - kCameraCollisionRadius);
	// The shoulder offset shrinks with the boom so a close camera does not end up beside the player.
	const float shoulder = side_ * kShoulderOffset * std::min(1.0F, distance / 2.0F);
	const Vec3f desired = ThirdPersonDesiredEye(pivot, yaw_, pitch, distance, shoulder);
	const Vec3f arm = desired - pivot;
	const float armLength = arm.length();
	const Vec3f armDir = armLength > 1e-5F ? arm * (1.0F / armLength) : Vec3f { 0.0F, 0.0F, 0.0F };

	float fraction = 1.0F;
	if (inputs.isBlocked != nullptr)
		fraction = SweepCamera(pivot, desired, kCameraCollisionRadius, inputs.isBlocked);
	const float target = armLength * fraction;
	boom_ = valid_ ? EaseBoom(boom_, target, dt) : target;
	valid_ = true;

	const Vec3f eye = pivot + armDir * boom_;
	ViewOutput out;
	out.camera.x = eye.x;
	out.camera.y = eye.y;
	out.camera.z = eye.z;
	out.camera.yaw = yaw_;
	out.camera.pitch = pitch;
	out.camera.fovYDeg = inputs.fovYDeg;
	out.drawLocalPlayer = boom_ > kHidePlayerDistance;
	out.chaseAim = true;
	out.aimPivot = pivot;
	out.resolvedDistance = boom_;
	return out;
}

} // namespace devilution::fpv

namespace devilution::render3d {

fpv::ViewOutput ViewProvider::Update(const fpv::ViewInputs &inputs)
{
	return fpv::FirstPersonCamera(inputs);
}

void SetViewProvider(ViewProvider *provider)
{
	fpv::Override = provider;
}

ViewProvider *GetViewProvider()
{
	return fpv::Override;
}

} // namespace devilution::render3d
