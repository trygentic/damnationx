/**
 * @file webgpu_platform.hpp
 *
 * Platform hooks for the WebGPU presenter: surface creation, waiting, presentation,
 * launcher state notifications, error reporting and launch flags. Implemented by webgpu_platform_emscripten.cpp (browser)
 * and webgpu_platform_native.cpp (desktop Dawn).
 *
 * webgpu_presenter.cpp only talks to the platform through this table, so it includes no
 * <emscripten.h> and no windowing headers.
 */
#pragma once

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <string_view>

#include <webgpu/webgpu.h>

namespace devilution {

struct WebGpuPlatform {
	/** Creates the presentation surface; `canvasSelectorOrNull` is the HTML canvas selector in the browser (unused natively). */
	WGPUSurface (*createSurface)(WGPUInstance instance, const char *canvasSelectorOrNull);
	/** Blocks (yielding to the platform event loop) until `*flag` becomes true; emscripten_sleep(1) loop in the browser. */
	void (*waitUntil)(bool *flag);
	/** Presents the surface's current texture; a no-op in the browser (Emdawnwebgpu presents on return to the event loop). */
	void (*presentSurface)(WGPUSurface surface);
	/** Launcher state notification (`launch.setState` in the browser). `failure` marks a terminal renderer failure, `retry` shows the retry control. */
	void (*notifyState)(const char *state, const char *message, bool failure, bool retry);
	/** Shows the first WebGPU error of a device to the user (prepended to the page's #output log in the browser). */
	void (*reportError)(const char *detail);
	/** True when the launch configuration sets `name` to "1" (the page URL's query string in the browser). */
	bool (*queryFlag)(const char *name);
};

const WebGpuPlatform &GetWebGpuPlatform();

#ifndef __EMSCRIPTEN__
// Desktop launch configuration (command line, diablo.cpp), the counterpart of the browser's URL query flags.

/** `--hd-flag NAME`: queryFlag(NAME) is true (the browser's ?NAME=1). */
void SetWebGpuLaunchFlag(std::string_view name);
/** hdFloor, hdSprites, hdTiles and hdUi. */
void EnableAllHdLaunchFlags();
/** `--hd`: all the HD flags, whatever the HD Art setting says. */
void RequestHdFromCommandLine();
/** True when any HD flag is on (--hd, --hd-flag, or the HD Art setting with an HD folder found). */
bool HdArtEnabled();
/** Turns the HD flags off again: the SDL renderer fallback cannot draw HD art. */
void ClearHdLaunchFlags();
/** True after `--no-webgpu`. */
bool WebGpuDisabledByCommandLine();
/** `--no-webgpu`: skip Dawn and present with the SDL renderer. */
void SetWebGpuDisabled(bool disabled);
/** `--hd-dir PATH`: the HD art folder (sprites.txt, ui.txt, ...); by default FindHdArtDirectory() looks for one. */
void SetHdDirectory(std::string_view path);
#endif

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
