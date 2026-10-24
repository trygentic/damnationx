#pragma once
/**
 * @file xr/xr.hpp
 *
 * Virtual reality (docs/design/vr.md). The game talks to VR only through these hooks; everything else (OpenXR,
 * WebXR, the graphics bridges, input mapping, comfort) stays inside Source/xr.
 *
 * Frame flow (WebGPU presenter, webgpu_presenter.cpp):
 *  1. WebGpuInitialize: xr::PreDeviceInit() (OpenXR instance + system + the bridge's own graphics device), then the
 *     Dawn device is requested with xr::AppendDeviceFeatures(), then xr::Initialize() (session, swapchains, shared eye
 *     textures).
 *  2. Every WebGpuPresent: render3d::WorldGpuPrepare() asks the registered view provider (xr's head pose) for the
 *     centre view, the classic composite is rendered as usual and, while xr::WantsUiImage(), a second time into
 *     xr's UI image with a transparent world area. Then xr::RenderFrame() waits for the headset's frame, renders both
 *     eyes with render3d::WorldGpuRenderViews() (world + the UI image as a panel + laser + vignette) and submits.
 *  3. Input: xr::RenderFrame() reads the controllers and drives the game (fpv analog movement, turning, injected key
 *     and mouse events, the laser cursor on the panel).
 *
 * Without DEVILUTIONX_XR every hook is an inline no-op.
 */

#include <cstdint>
#include <vector>

#include "config.h"

#if DEVILUTIONX_WEBGPU
#include <webgpu/webgpu.h>
#endif

namespace devilution::xr {

#if DEVILUTIONX_XR && DEVILUTIONX_WEBGPU

/** `--vr` on the command line (desktop): request a VR session regardless of the VR Mode option. */
void RequestFromCommandLine();

/** True when this build and run want VR (the Quest app always, desktop with the option or --vr, the web with ?vr=1). */
bool Requested();

/** Before the Dawn device exists: creates the runtime instance and finds a headset. Returns false (VR off) on failure. */
bool PreDeviceInit();

/** Surface format the presenter should prefer so the eye images can be shared with the headset (or Undefined). */
WGPUTextureFormat PreferredColorFormat();

/** Adds the Dawn features the bridge needs (shared textures and fences) when the adapter has them. */
void AppendDeviceFeatures(WGPUAdapter adapter, std::vector<WGPUFeatureName> &features);

/** After the Dawn device exists: session, swapchains and shared eye textures. */
bool Initialize(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat);

/** True while a session is running (frames go to the headset). */
bool IsActive();

/** True when the headset is the only display (the Quest): the presenter skips the window surface. */
bool OwnsDisplay();

/** True when the presenter should render the 2D composite into UiImageView() this frame. */
bool WantsUiImage();

/** The UI image (back buffer size, presenter colour format, cleared to transparent by the presenter). */
WGPUTextureView UiImageView(uint32_t width, uint32_t height);

/** Once per presented frame, after the regular frame was submitted. `worldPrepared` = render3d::WorldGpuPrepare() succeeded. */
void RenderFrame(bool worldPrepared, int sourceWidth, int sourceHeight);

/** Presenter shutdown / device loss. */
void Shutdown();

#else

inline void RequestFromCommandLine() { }
inline bool Requested() { return false; }
inline bool PreDeviceInit() { return false; }
#if DEVILUTIONX_WEBGPU
inline WGPUTextureFormat PreferredColorFormat() { return WGPUTextureFormat_Undefined; }
inline void AppendDeviceFeatures(WGPUAdapter, std::vector<WGPUFeatureName> &) { }
inline bool Initialize(WGPUInstance, WGPUAdapter, WGPUDevice, WGPUQueue, WGPUTextureFormat) { return false; }
inline WGPUTextureView UiImageView(uint32_t, uint32_t) { return nullptr; }
#endif
inline bool IsActive() { return false; }
inline bool OwnsDisplay() { return false; }
inline bool WantsUiImage() { return false; }
inline void RenderFrame(bool, int, int) { }
inline void Shutdown() { }

#endif

} // namespace devilution::xr
