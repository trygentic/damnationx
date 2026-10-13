/**
 * @file hd_sprites_webgpu.hpp
 *
 * WebGPU side of the HD sprites (hd_sprites.hpp): lazily loaded clip textures under an LRU byte budget and the
 * instanced quad pass that the presenter encodes into its composite render pass.
 */
#pragma once

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <cstdint>

#include <webgpu/webgpu.h>

namespace devilution {

struct HdSpritesGpuFrame {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
	/** The legacy ray traced light mask the composite multiplies the frame with (half resolution). */
	WGPUTextureView lightMaskView = nullptr;
	int sourceWidth = 0;
	int sourceHeight = 0;
	int drawableWidth = 0;
	int drawableHeight = 0;
	/** False for frames presented through the hybrid composite: the instances are lit as hybrid cards instead. */
	bool legacyComposite = true;
	/** Hybrid frames: the presenter's HybridParams uniform (lights, screen-to-world, ambient curve) and occluder volume. */
	WGPUBuffer hybridParamsBuffer = nullptr;
	uint64_t hybridParamsSize = 0;
	WGPUTextureView hybridVolumeView = nullptr;
	/** Hybrid composite exposure (PresentParams.values.z), applied to the lit HD texels as to the frame. */
	float hybridExposure = 1.0F;
};

/** Per presented frame, before encoding: finishes loads, evicts over budget, uploads instances and the occlusion plane. */
void HdSpritesGpuPrepare(const HdSpritesGpuFrame &frame);
/** Draws the prepared instances into the composite pass (its viewport maps the source frame). */
void HdSpritesGpuEncode(WGPURenderPassEncoder pass);
/** Releases every GPU object (device teardown). */
void HdSpritesGpuRelease();

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
