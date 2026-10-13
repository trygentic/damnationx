/**
 * @file webgpu_platform_native.cpp
 *
 * Native (Dawn) implementation of WebGpuPlatform: the surface comes from the game's SDL window, waits pump Dawn's event queue, frames are presented with wgpuSurfacePresent, launcher notifications go to
 * the log, and the launch flags come from the command line (--hd, --hd-flag NAME) or the HD Art setting with an `hd` folder
 * in place (ResolveHdDirectory) instead of the page URL.
 */
#include "engine/render/webgpu_platform.hpp"

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif

#include <webgpu/webgpu.h>

#include "engine/assets.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution {
namespace {

std::vector<std::string> LaunchFlags;
bool WebGpuDisabled = false;
std::string HdDirectory;
bool HdRequestedByCommandLine = false;
WGPUInstance CurrentInstance = nullptr;
#ifdef __APPLE__
SDL_MetalView MetalView = nullptr;
#endif

/**
 * Points `hd/...` assets at the HD folder and turns the HD art on: --hd-dir, else an `hd` folder found next to the game
 * data, in the pref/config folder or next to the executable (FindHdArtDirectory). Without --hd the HD Art setting
 * (Graphics, on by default) decides, so a player who drops the `hd` folder in place gets HD art without arguments.
 */
void ResolveHdDirectory()
{
	static bool resolved = false;
	if (resolved)
		return;
	resolved = true;
	std::string directory = HdDirectory;
	if (directory.empty())
		directory = FindHdArtDirectory();
	const bool found = !directory.empty();
	if (!found)
		directory = paths::BasePath() + "hd"; // the documented default, for --hd-flag without a pack
	if (directory.back() != '/' && directory.back() != '\\')
		directory += DIRECTORY_SEPARATOR_STR;
	SetHdAssetDirectory(directory);
	if (HdRequestedByCommandLine) {
		Log("WebGPU (Dawn): HD art folder {} (--hd)", directory);
	} else if (!found) {
		Log("WebGPU (Dawn): no HD art folder (an \"hd\" folder with sprites.txt next to DIABDAT.MPQ, in {} or next to the game); classic art", paths::PrefPath());
	} else if (!*GetOptions().Graphics.hdArt) {
		Log("WebGPU (Dawn): HD art folder {} found but HD Art is off in the Graphics settings", directory);
	} else {
		EnableAllHdLaunchFlags();
		Log("WebGPU (Dawn): HD art folder {} (HD Art setting)", directory);
	}
}

WGPUSurface CreateWindowSurface(WGPUInstance instance, const char * /*canvasSelector*/)
{
	CurrentInstance = instance;
	ResolveHdDirectory();
	if (WebGpuDisabled) {
		Log("WebGPU (Dawn): disabled by --no-webgpu");
		return nullptr;
	}
	if (instance == nullptr || ghMainWnd == nullptr)
		return nullptr;
	const SDL_PropertiesID properties = SDL_GetWindowProperties(ghMainWnd);
	if (properties == 0) {
		LogError("WebGPU (Dawn): SDL_GetWindowProperties failed: {}", SDL_GetError());
		return nullptr;
	}
	WGPUSurfaceDescriptor surfaceDescriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
#ifdef __APPLE__
	if (MetalView == nullptr)
		MetalView = SDL_Metal_CreateView(ghMainWnd);
	if (MetalView == nullptr) {
		LogError("WebGPU (Dawn): SDL_Metal_CreateView failed: {}", SDL_GetError());
		return nullptr;
	}
	WGPUSurfaceSourceMetalLayer metalSource = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
	metalSource.layer = SDL_Metal_GetLayer(MetalView);
	if (metalSource.layer == nullptr) {
		LogError("WebGPU (Dawn): SDL_Metal_GetLayer failed: {}", SDL_GetError());
		return nullptr;
	}
	surfaceDescriptor.nextInChain = &metalSource.chain;
	return wgpuInstanceCreateSurface(instance, &surfaceDescriptor);
#elif defined(_WIN32)
	WGPUSurfaceSourceWindowsHWND windowsSource = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
	windowsSource.hinstance = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
	windowsSource.hwnd = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	if (windowsSource.hinstance == nullptr || windowsSource.hwnd == nullptr) {
		LogError("WebGPU (Dawn): SDL did not provide a Win32 window handle");
		return nullptr;
	}
	surfaceDescriptor.nextInChain = &windowsSource.chain;
	return wgpuInstanceCreateSurface(instance, &surfaceDescriptor);
#elif defined(__ANDROID__)
	WGPUSurfaceSourceAndroidNativeWindow androidSource = WGPU_SURFACE_SOURCE_ANDROID_NATIVE_WINDOW_INIT;
	androidSource.window = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
	if (androidSource.window == nullptr) {
		LogError("WebGPU (Dawn): SDL did not provide an Android native window");
		return nullptr;
	}
	surfaceDescriptor.nextInChain = &androidSource.chain;
	return wgpuInstanceCreateSurface(instance, &surfaceDescriptor);
#elif defined(__linux__)
	WGPUSurfaceSourceWaylandSurface waylandSource = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
	waylandSource.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
	waylandSource.surface = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
	if (waylandSource.display != nullptr && waylandSource.surface != nullptr) {
		surfaceDescriptor.nextInChain = &waylandSource.chain;
		return wgpuInstanceCreateSurface(instance, &surfaceDescriptor);
	}
	WGPUSurfaceSourceXlibWindow xlibSource = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
	xlibSource.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
	xlibSource.window = static_cast<uint64_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
	if (xlibSource.display != nullptr && xlibSource.window != 0) {
		surfaceDescriptor.nextInChain = &xlibSource.chain;
		return wgpuInstanceCreateSurface(instance, &surfaceDescriptor);
	}
	LogError("WebGPU (Dawn): SDL did not provide a Wayland or X11 window handle");
	return nullptr;
#else
	LogError("WebGPU (Dawn): no window surface support on this platform yet");
	return nullptr;
#endif
}

// Native Dawn fires AllowSpontaneous callbacks from ProcessEvents (or on its own); pump until the flag is set.
void WaitUntilFlag(bool *flag)
{
	while (!*flag) {
		if (CurrentInstance != nullptr)
			wgpuInstanceProcessEvents(CurrentInstance);
		if (!*flag)
			SDL_Delay(1);
	}
}

void PresentSurface(WGPUSurface surface)
{
	wgpuSurfacePresent(surface);
	// Delivers device-lost and error callbacks between frames.
	if (CurrentInstance != nullptr)
		wgpuInstanceProcessEvents(CurrentInstance);
}

void NotifyLaunchState(const char *state, const char *message, bool failure, bool /*retry*/)
{
	if (failure)
		LogError("WebGPU (Dawn): {}: {}", state, message);
	else
		Log("WebGPU (Dawn): {}: {}", state, message);
}

void ReportError(const char *detail)
{
	LogError("First WebGPU error: {}", detail);
}

bool QueryLaunchFlag(const char *name)
{
	return std::find(LaunchFlags.begin(), LaunchFlags.end(), name) != LaunchFlags.end();
}

} // namespace

void SetWebGpuLaunchFlag(std::string_view name)
{
	if (!QueryLaunchFlag(std::string(name).c_str()))
		LaunchFlags.emplace_back(name);
}

void RequestHdFromCommandLine()
{
	HdRequestedByCommandLine = true;
	EnableAllHdLaunchFlags();
}

bool HdArtEnabled()
{
	return QueryLaunchFlag("hdSprites") || QueryLaunchFlag("hdTiles") || QueryLaunchFlag("hdUi") || QueryLaunchFlag("hdFloor");
}

void ClearHdLaunchFlags()
{
	std::erase_if(LaunchFlags, [](const std::string &flag) {
		return flag == "hdFloor" || flag == "hdSprites" || flag == "hdTiles" || flag == "hdUi";
	});
}

bool WebGpuDisabledByCommandLine()
{
	return WebGpuDisabled;
}

void EnableAllHdLaunchFlags()
{
	for (const char *name : { "hdFloor", "hdSprites", "hdTiles", "hdUi" })
		SetWebGpuLaunchFlag(name);
}

void SetWebGpuDisabled(bool disabled)
{
	WebGpuDisabled = disabled;
}

void SetHdDirectory(std::string_view path)
{
	HdDirectory = path;
}

const WebGpuPlatform &GetWebGpuPlatform()
{
	static const WebGpuPlatform Platform {
		CreateWindowSurface,
		WaitUntilFlag,
		PresentSurface,
		NotifyLaunchState,
		ReportError,
		QueryLaunchFlag,
	};
	return Platform;
}

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
