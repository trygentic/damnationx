/**
 * @file xr/xr_null.cpp
 *
 * Desktops without an OpenXR graphics bridge (macOS, Linux for now): VR is unavailable natively. On a Mac, play
 * the web build in Quest Browser instead (docs/design/vr.md, "macOS").
 */
#include "xr/xr_types.hpp"

#include "utils/log.hpp"

namespace devilution::xr {

namespace {

class NullBackend final : public Backend {
public:
	[[nodiscard]] const char *Name() const override
	{
		return "none";
	}
	bool PreDeviceInit() override
	{
		LogError("VR: this desktop build has no OpenXR graphics bridge (Windows and the Quest app do); see docs/design/vr.md for the WebXR route");
		return false;
	}
	bool Initialize(WGPUInstance, WGPUAdapter, WGPUDevice, WGPUQueue, WGPUTextureFormat, float) override { return false; }
	bool Update() override { return false; }
	bool BeginFrame(FrameInput &) override { return false; }
	bool AcquireEyeTargets(std::array<WGPUTextureView, 2> &, uint32_t &, uint32_t &) override { return false; }
	void EndFrame(bool) override { }
	void Shutdown() override { }
};

} // namespace

Backend *CreateBackend()
{
	return new NullBackend();
}

} // namespace devilution::xr
