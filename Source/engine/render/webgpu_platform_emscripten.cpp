/**
 * @file webgpu_platform_emscripten.cpp
 *
 * Browser implementation of WebGpuPlatform: surface from the canvas selector,
 * emscripten_sleep wait loop, no-op present, launcher state notifications, the #output error
 * line and URL query flags.
 *
 * This is the only WebGPU translation unit that includes <emscripten.h>.
 */
#include "engine/render/webgpu_platform.hpp"

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <emscripten.h>
#include <webgpu/webgpu.h>

namespace devilution {
namespace {

WGPUSurface CreateCanvasSurface(WGPUInstance instance, const char *canvasSelector)
{
	if (instance == nullptr || canvasSelector == nullptr)
		return nullptr;
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
	canvasSource.selector = { canvasSelector, WGPU_STRLEN };
	WGPUSurfaceDescriptor surfaceDescriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
	surfaceDescriptor.nextInChain = &canvasSource.chain;
	return wgpuInstanceCreateSurface(instance, &surfaceDescriptor);
}

// Emdawnwebgpu delivers request callbacks from the browser event loop; ASYNCIFY
// sleeping keeps the main thread responsive while the request is pending.
void WaitUntilFlag(bool *flag)
{
	while (!*flag)
		emscripten_sleep(1);
}

// Emdawnwebgpu presents the canvas texture automatically when the browser
// returns to its event loop. wgpuSurfacePresent is a native Dawn operation
// and deliberately aborts in the browser implementation.
void PresentSurfaceNoop(WGPUSurface /*surface*/)
{
}

void NotifyLaunchState(const char *state, const char *message, bool /*failure*/, bool retry)
{
	// The launcher styles failures by state name (Packaging/emscripten/index.html launch.setState);
	// `retry` toggles its retry control.
	EM_ASM({
		if (window.launch)
			window.launch.setState(UTF8ToString($0), UTF8ToString($1), $2 !== 0);
	},
	    state, message, retry ? 1 : 0);
}

void ReportErrorToOutput(const char *detail)
{
	EM_ASM({
		const output = document.getElementById('output');
		if (output) output.value = 'First WebGPU error: ' + UTF8ToString($0) + '\n' + output.value;
	},
	    detail);
}

bool QueryUrlFlag(const char *name)
{
	return EM_ASM_INT({ return new URLSearchParams(window.location.search).get(UTF8ToString($0)) === '1' ? 1 : 0; }, name) != 0;
}

} // namespace

const WebGpuPlatform &GetWebGpuPlatform()
{
	static const WebGpuPlatform Platform {
		CreateCanvasSurface,
		WaitUntilFlag,
		PresentSurfaceNoop,
		NotifyLaunchState,
		ReportErrorToOutput,
		QueryUrlFlag,
	};
	return Platform;
}

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
