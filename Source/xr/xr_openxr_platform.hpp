#pragma once
/**
 * @file xr/xr_openxr_platform.hpp
 *
 * OpenXR with the platform and graphics-API defines of this build: Android + Vulkan (Quest), Windows + D3D11
 * (PC VR through Quest Link / Air Link / Virtual Desktop / SteamVR), macOS + Metal (OXRSys streaming to a Quest),
 * and the graphics bridge interface.
 *
 * The bridge owns a small graphics device of its own that the OpenXR session is created with. Dawn renders each
 * eye into a texture that is shared between the two devices (an AHardwareBuffer on Android, a DXGI NT handle on
 * Windows); the bridge copies it into the runtime's swapchain image, synchronised by shared fences. Dawn itself
 * never sees an OpenXR object. See docs/design/vr.md, "Graphics bridge".
 */

#include <array>
#include <cstdint>
#include <vector>

#include <webgpu/webgpu.h>

#if defined(__ANDROID__)
#include <jni.h>
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#define DEVILUTIONX_XR_BRIDGE_VULKAN 1
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define DEVILUTIONX_XR_BRIDGE_D3D11 1
#elif defined(__APPLE__)
#define XR_USE_GRAPHICS_API_METAL
#define DEVILUTIONX_XR_BRIDGE_METAL 1
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace devilution::xr {

class GraphicsBridge {
public:
	virtual ~GraphicsBridge() = default;
	[[nodiscard]] virtual const char *Name() const = 0;
	/** The OpenXR instance extension the bridge needs (XR_KHR_vulkan_enable2, XR_KHR_D3D11_enable). */
	[[nodiscard]] virtual const char *RequiredExtension() const = 0;
	/** Format of the shared eye textures; the presenter must render in it (its surface format). */
	[[nodiscard]] virtual WGPUTextureFormat ColorFormat() const { return WGPUTextureFormat_RGBA8Unorm; }
	/** Optional extra extensions enabled when the runtime has them. */
	[[nodiscard]] virtual std::vector<const char *> OptionalExtensions() const { return {}; }
	/** Creates the bridge's own device for this system (before the Dawn device exists). */
	virtual bool CreateDevice(XrInstance instance, XrSystemId system, const std::vector<const char *> &enabledExtensions) = 0;
	/** The XrGraphicsBinding* struct to chain into XrSessionCreateInfo. */
	[[nodiscard]] virtual const void *SessionBinding() const = 0;
	/** Dawn features needed for the sharing; appended when the adapter has them. */
	[[nodiscard]] virtual std::vector<WGPUFeatureName> DeviceFeatures() const = 0;
	/** Swapchain formats in order of preference (sRGB first: the game writes sRGB-encoded colour). */
	[[nodiscard]] virtual std::vector<int64_t> PreferredSwapchainFormats() const = 0;
	/** Enumerates the swapchain images and creates the shared eye textures (imported into Dawn). */
	virtual bool CreateEyes(const std::array<XrSwapchain, 2> &swapchains, int64_t swapchainFormat, uint32_t width, uint32_t height,
	    WGPUDevice device, WGPUTextureFormat colorFormat)
	    = 0;
	/** Waits until the eye texture is free and starts Dawn's access to it. */
	virtual WGPUTextureView BeginEye(int eye) = 0;
	/** Ends Dawn's access and, when `copy`, copies the eye into swapchain image `imageIndex` on the bridge's queue. */
	virtual bool EndEye(int eye, bool copy, uint32_t imageIndex) = 0;
	virtual void Destroy() = 0;
};

/** The bridge of this platform, or nullptr (no OpenXR graphics path for this platform yet). */
GraphicsBridge *CreateGraphicsBridge();

/** Fills Android loader / instance creation data; no-ops elsewhere. Implemented next to the backend. */
bool PlatformInitializeLoader();
const void *PlatformInstanceCreateNext();

} // namespace devilution::xr
