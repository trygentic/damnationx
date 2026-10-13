/**
 * @file hd_ui_webgpu.hpp
 *
 * WebGPU side of the HD UI (hd_ui.hpp): lazily loaded UI sheets under an LRU byte budget, the colour translation
 * tables and the layer pass the presenter encodes into its composite render pass after the HD sprites.
 */
#pragma once

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <webgpu/webgpu.h>

namespace devilution {

struct HdUiGpuFrame {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
	int sourceWidth = 0;
	int sourceHeight = 0;
	int drawableWidth = 0;
	int drawableHeight = 0;
};

/** Per presented frame, before encoding: finishes loads, evicts, uploads the layers, the plane and the tables. */
void HdUiGpuPrepare(const HdUiGpuFrame &frame);
/** Draws the prepared layers into the composite pass (its viewport maps the source frame). */
void HdUiGpuEncode(WGPURenderPassEncoder pass);
/** Releases every GPU object (device teardown). */
void HdUiGpuRelease();

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
