/**
 * @file dx.h
 *
 * Interface of functions setting up the graphics pipeline.
 */
#pragma once

#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "config.h"
#include "engine/surface.hpp"

#if DEVILUTIONX_WEBGPU
#include "engine/render/webgpu_presenter.hpp"
#endif

namespace devilution {

/** Whether we render directly to the screen surface, i.e. `PalSurface == GetOutputSurface()` */
extern bool RenderDirectlyToOutputSurface;

extern SDL_Surface *PalSurface;

Surface GlobalBackBuffer();

void dx_init();
/** Shows the in-game note about HD art this run cannot draw (no WebGPU in this build, or Dawn failed), once. */
void ShowHdArtNoticeOnce();
void dx_cleanup();
void CreateBackBuffer();
void BltFast(SDL_Rect *srcRect, SDL_Rect *dstRect);
void Blit(SDL_Surface *src, SDL_Rect *srcRect, SDL_Rect *dstRect);
void RenderPresent();

/** Lightweight lifecycle diagnostics; GPU timing remains optional and non-blocking. */
struct RendererDiagnostics {
	uint64_t frameCount = 0;
	uint64_t uploadCount = 0;
	uint64_t atlasCount = 0;
	uint64_t recoveryCount = 0;
	uint64_t validationErrorCount = 0;
	uint64_t deviceLossCount = 0;
	uint64_t surfaceLossCount = 0;
	uint64_t resizeFailureCount = 0;
	uint64_t capabilityFailureCount = 0;
	bool gpuTimingAvailable = false;
	uint64_t cpuFrameTimeMicroseconds = 0;
};

RendererDiagnostics GetRendererDiagnostics();
void ResetRendererDiagnostics();
/** The live instance, for the presenter's counters (webgpu_presenter.cpp). */
RendererDiagnostics &MutableRendererDiagnostics();

#if DEVILUTIONX_WEBGPU
/**
 * @brief Presentation-only lighting data for WebGpuPresent.
 *
 * Fills a static instance (~52 KB) and returns it; disabled (legacy and hybrid) when there is no running game
 * (MyPlayer == nullptr || !gbRunGame). `hybrid.enabled` follows gbuffer::BackBufferIsHybrid().
 */
const WebGpuLightingData &BuildWebGpuLightingData();
#endif

} // namespace devilution
