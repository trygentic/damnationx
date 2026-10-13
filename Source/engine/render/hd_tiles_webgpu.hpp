/**
 * @file hd_tiles_webgpu.hpp
 *
 * WebGPU side of the HD level tiles (hd_tiles.hpp): lazily loaded atlas pages under an LRU byte budget and the
 * instanced quad pass the presenter encodes into its composite render pass, before the HD sprites.
 */
#pragma once

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <cstdint>

#include <webgpu/webgpu.h>

namespace devilution {

struct HdTilesGpuFrame {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
	int sourceWidth = 0;
	int sourceHeight = 0;
	/** False for frames presented through the hybrid composite. */
	bool legacyComposite = true;
	/** Legacy: the ray traced light mask the composite multiplies the frame with (half resolution). */
	WGPUTextureView lightMaskView = nullptr;
	/** Hybrid: the light mask and G-buffer planes the hybrid composite lights the classic pixel with, and its params. */
	WGPUTextureView hybridMaskView = nullptr;
	WGPUTextureView auxView = nullptr;
	WGPUTextureView baseView = nullptr;
	WGPUTextureView indexView = nullptr;
	/** Hybrid: the frame's palette (256x1 rgba8unorm), for the classic colour match of the Cathedral tiles. */
	WGPUTextureView paletteView = nullptr;
	WGPUBuffer presentParamsBuffer = nullptr;
	uint64_t presentParamsSize = 0;
};

/** Per presented frame, before encoding: finishes loads, evicts over budget, uploads instances and the dynamic plane. */
void HdTilesGpuPrepare(const HdTilesGpuFrame &frame);
/** Draws the prepared instances into the composite pass (its viewport maps the source frame). */
void HdTilesGpuEncode(WGPURenderPassEncoder pass);
/** Releases every GPU object (device teardown). */
void HdTilesGpuRelease();

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
