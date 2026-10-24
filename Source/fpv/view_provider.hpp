#pragma once
/**
 * @file fpv/view_provider.hpp
 *
 * The built-in 3D cameras, as render3d::ViewProvider implementations (one interface for every 3D camera, see
 * render3d/view_provider.hpp): FirstPersonView and ThirdPersonView, chosen by Graphics > Camera Mode. Source/fpv
 * owns gameplay (look angles, movement, picking) and feeds the active provider ViewInputs once per drawn frame; an
 * overriding provider installed with render3d::SetViewProvider() (VR) takes their place.
 *
 * These providers only implement the gameplay stage (pure: everything arrives in ViewInputs, including the collision
 * query), so they are unit-tested on their own (test/fpv_camera_test.cpp); render3d draws them with the flat camera.
 */

#include "render3d/view_provider.hpp"

namespace devilution::fpv {

class FirstPersonView final : public render3d::ViewProvider {
public:
	[[nodiscard]] const char *Name() const override { return "first-person"; }
	ViewOutput Update(const ViewInputs &inputs) override { return FirstPersonCamera(inputs); }
};

class ThirdPersonView final : public render3d::ViewProvider {
public:
	[[nodiscard]] const char *Name() const override { return "third-person"; }
	void Reset(const ViewInputs &inputs) override;
	ViewOutput Update(const ViewInputs &inputs) override;

	/** The yaw this camera is looking along (the orbit chase may have swung it); fpv feeds it back as look yaw. */
	[[nodiscard]] float yaw() const { return yaw_; }

private:
	float boom_ = 2.2F;
	float side_ = 1.0F;
	float yaw_ = 0.0F;
	bool valid_ = false;
};

/** The active provider's output for the current frame (valid while fpv::IsActive()). Implemented in fpv.cpp. */
const ViewOutput &GetViewOutput();

} // namespace devilution::fpv
