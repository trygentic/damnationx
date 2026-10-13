/**
 * @file dx.cpp
 *
 * Implementation of functions setting up the graphics pipeline.
 */
#include "engine/dx.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "controls/plrctrls.h"
#include "config.h"
#include "engine/assets.hpp"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_tiles.hpp"
#include "engine/render/hd_ui.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/webgpu_platform.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "init.hpp"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_geometry.hpp"
#include "lighting.h"
#include "missiles.h"
#include "minitext.h"
#include "objects.h"
#include "options.h"
#include "panels/spell_list.hpp"
#include "player.h"
#include "plrmsg.h"
#include "render3d/world_renderer.hpp"
#include "stores.h"
#include "utils/display.h"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/sdl_wrap.h"

#ifndef USE_SDL1
#include "controls/touch/renderers.h"
#endif

#ifdef __3DS__
#include <3ds.h>
#endif

#ifdef __EMSCRIPTEN__
// Ends a frame by returning to the browser event loop so it can present the canvas and deliver input, without the
// >= 4 ms clamp that browsers put on chained setTimeout calls (emscripten_sleep). `mode` follows Frame Rate Control:
// 1 = Vertical Sync: wait for requestAnimationFrame (50 ms timer in case the page gets no animation frames).
// 0 = None: resume from a MessageChannel task as soon as the event loop is free.
// 2 = Limit FPS: sleep until a deadline `intervalMs` after the previous one; the timer is armed from a message task,
//     so it is never clamped.
// An uninterrupted chain of message tasks starves the browser's rendering steps, and with them the presentation of
// the canvas and the delivery of input (Chrome aligns input to animation frames). So in modes 0 and 2 an animation
// frame is always requested, and once it is overdue (pending for longer than the last measured display interval)
// the frame waits for it. A hidden tab uses a timer the browser throttles, so it never spins.
// Module.dxFrameYield.idleMs/frames count the time spent yielded (for profiling).
// Declared like EM_ASYNC_JS (the __asyncjs__ name puts it on the Asyncify import list) but resumed through
// Asyncify.handleSleep + callUserCallback, the same path emscripten_sleep uses, so exit() and aborts are handled.
_EM_JS(void, dx_browser_yield_frame, __asyncjs__dx_browser_yield_frame, (int mode, double intervalMs), R"js({
	var y = Module.dxFrameYield;
	if (!y) {
		y = Module.dxFrameYield = {
			deadline: 0, idleMs: 0, frames: 0, queue: [], channel: new MessageChannel(),
			rafPending: false, rafAskedAt: 0, rafLastAt: 0, rafInterval: 1000 / 60, rafWaiters: []
		};
		y.channel.port1.onmessage = function () { var f = y.queue.shift(); if (f) f(); };
		y.post = function (f) { y.queue.push(f); y.channel.port2.postMessage(0); };
		y.onRaf = function (t) {
			var now = performance.now();
			if (y.rafLastAt > 0 && now - y.rafLastAt < 100) y.rafInterval = 0.9 * y.rafInterval + 0.1 * (now - y.rafLastAt);
			y.rafLastAt = now;
			y.rafPending = false;
			var waiters = y.rafWaiters;
			y.rafWaiters = [];
			for (var i = 0; i < waiters.length; i++) waiters[i]();
		};
		y.askRaf = function () {
			if (y.rafPending || typeof requestAnimationFrame != 'function') return;
			y.rafPending = true;
			y.rafAskedAt = performance.now();
			requestAnimationFrame(y.onRaf);
		};
		// Calls `f` on the next animation frame, or after 50 ms if the page gets none (occluded window).
		y.waitRaf = function (f) {
			var done = false, timer = 0;
			var fire = function () { if (done) return; done = true; clearTimeout(timer); f(); };
			y.askRaf();
			y.rafWaiters.push(fire);
			timer = setTimeout(fire, 50);
		};
	}
	return Asyncify.handleSleep(function (wakeUp) {
		var start = performance.now();
		var wake = function () {
			y.idleMs += performance.now() - start;
			y.frames++;
			if (typeof callUserCallback == 'function') callUserCallback(wakeUp); else wakeUp();
		};
		if (typeof document != 'undefined' && document.hidden) {
			setTimeout(wake, 16);
			return;
		}
		if (typeof requestAnimationFrame != 'function') {
			setTimeout(wake, 0);
			return;
		}
		if (mode == 1) {
			y.waitRaf(wake);
			return;
		}
		if (y.rafPending && start - y.rafAskedAt > y.rafInterval) {
			y.waitRaf(wake); // let the browser render (and deliver input) before the next frame
			return;
		}
		y.askRaf();
		if (mode == 2 && intervalMs > 0) {
			var next = y.deadline + intervalMs;
			if (next < start) next = start;
			y.deadline = next;
			var step = function () {
				var left = next - performance.now();
				if (left < 1) wake(); else setTimeout(function () { y.post(step); }, left);
			};
			y.post(step);
			return;
		}
		y.post(wake);
	});
})js");
#endif

namespace devilution {

int refreshDelay;

namespace {

/** The single diagnostics instance; counters are bumped by the presenter and CreateBackBuffer. */
RendererDiagnostics Diagnostics;

} // namespace

RendererDiagnostics GetRendererDiagnostics()
{
	return Diagnostics;
}

void ResetRendererDiagnostics()
{
	Diagnostics = RendererDiagnostics {};
}

RendererDiagnostics &MutableRendererDiagnostics()
{
	return Diagnostics;
}

namespace {
// WebGPU requests complete asynchronously on native Dawn and in Emdawnwebgpu.
// Keep the transition logic independent of the callback ABI so callbacks can
// be queued by the application pump and safely ignored after a restart.
enum class WebGpuState {
	Idle,
	AdapterPending,
	AdapterReady,
	DevicePending,
	DeviceReady,
	PipelinePending,
	Ready,
	SurfaceZeroSize,
	Failed,
	Lost,
	ShuttingDown,
};

class WebGpuLifecycle final {
public:
	using Callback = std::function<void()>;

	uint64_t Start()
	{
		++generation_;
		state_ = WebGpuState::AdapterPending;
		callbacks_.clear();
		return generation_;
	}

	bool Queue(uint64_t generation, Callback callback)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (generation != generation_ || state_ == WebGpuState::ShuttingDown)
			return false;
		callbacks_.emplace_back(generation, std::move(callback));
		return true;
	}

	// Advance at most one callback per application frame. A WebGPU callback
	// must not turn device startup into an unbounded browser task.
	bool Pump()
	{
		std::pair<uint64_t, Callback> queued;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (callbacks_.empty()) return false;
			queued = std::move(callbacks_.front());
			callbacks_.pop_front();
		}
		if (queued.first != generation_ || state_ == WebGpuState::ShuttingDown)
			return true;
		queued.second();
		return true;
	}

	bool Transition(WebGpuState expected, WebGpuState next)
	{
		if (state_ != expected || state_ == WebGpuState::ShuttingDown)
			return false;
		state_ = next;
		return true;
	}

	void SetDrawableSize(int width, int height)
	{
		if (state_ == WebGpuState::ShuttingDown || state_ == WebGpuState::Failed)
			return;
		if (width <= 0 || height <= 0) {
			state_ = WebGpuState::SurfaceZeroSize;
		} else if (state_ == WebGpuState::SurfaceZeroSize) {
			state_ = WebGpuState::Ready;
		}
	}

	void Fail() { if (state_ != WebGpuState::ShuttingDown) state_ = WebGpuState::Failed; }
	void Lose() { if (state_ != WebGpuState::ShuttingDown) state_ = WebGpuState::Lost; }
	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		++generation_;
		callbacks_.clear();
		state_ = WebGpuState::ShuttingDown;
	}
	WebGpuState State() const { return state_; }
	uint64_t Generation() const { return generation_; }

private:
	WebGpuState state_ = WebGpuState::Idle;
	uint64_t generation_ = 0;
	std::mutex mutex_;
	std::deque<std::pair<uint64_t, Callback>> callbacks_;
};

// The binding contains only borrowed handles. Platform-specific surface
// creation remains behind this boundary: HWND, CAMetalLayer, X11/Wayland and
// the SDL browser canvas all use the same lifecycle and size rules.
struct WebGpuSurfaceBinding {
	void *nativeHandle = nullptr;
	int drawableWidth = 0;
	int drawableHeight = 0;
	bool zeroSized() const { return drawableWidth <= 0 || drawableHeight <= 0; }
};

class NullRenderer final : public Renderer {
public:
	explicit NullRenderer(RendererBackend backend) : backend_(backend) {}
	bool Initialize(const RendererConfig &config, const OutputSurface *surface) override
	{
		if (config.backend != backend_ || initialized_ || (backend_ == RendererBackend::WebGPU && surface == nullptr)) return false;
		initialized_ = true;
		return true;
	}
	bool BeginFrame() override { if (!initialized_ || inFrame_) return false; inFrame_ = true; return true; }
	Surface *FrameSurface() override { return nullptr; }
	bool Present() override { return initialized_ && inFrame_; }
	void EndFrame() override { inFrame_ = false; }
	void Shutdown() override { inFrame_ = false; initialized_ = false; }
private:
	RendererBackend backend_;
	bool initialized_ = false;
	bool inFrame_ = false;
};
} // namespace

namespace {
class LegacyRenderer final : public Renderer {
public:
	bool Initialize(const RendererConfig &config, const OutputSurface *) override
	{
		if (config.backend != RendererBackend::Legacy || initialized_)
			return false;
		dx_init();
		backBuffer_ = GlobalBackBuffer();
		initialized_ = true;
		return true;
	}
	bool BeginFrame() override
	{
		if (!initialized_ || inFrame_)
			return false;
		inFrame_ = true;
		return true;
	}
	Surface *FrameSurface() override { return inFrame_ ? &backBuffer_ : nullptr; }
	bool Present() override
	{
		if (!initialized_ || !inFrame_)
			return false;
		RenderPresent();
		return true;
	}
	void EndFrame() override { inFrame_ = false; }
	void Shutdown() override
	{
		if (initialized_)
			dx_cleanup();
		inFrame_ = false;
		initialized_ = false;
	}
private:
	Surface backBuffer_;
	bool initialized_ = false;
	bool inFrame_ = false;
};
} // namespace

std::unique_ptr<Renderer> CreateRenderer(RendererBackend backend)
{
	if (backend == RendererBackend::Legacy)
		return std::make_unique<LegacyRenderer>();
	return std::make_unique<NullRenderer>(backend);
}

SDL_Renderer *renderer;
#ifndef USE_SDL1
SDLTextureUniquePtr texture;
#endif

/** Currently active palette */
SDLPaletteUniquePtr Palette;

/** 24-bit renderer texture surface */
SDLSurfaceUniquePtr RendererTextureSurface;

/** 8-bit surface that we render to */
SDL_Surface *PalSurface;
namespace {
SDLSurfaceUniquePtr PinnedPalSurface;
} // namespace

/** Whether we render directly to the screen surface, i.e. `PalSurface == GetOutputSurface()` */
bool RenderDirectlyToOutputSurface;

namespace {

bool CanRenderDirectlyToOutputSurface()
{
#ifdef USE_SDL1
#ifdef SDL1_FORCE_DIRECT_RENDER
	return true;
#else
	auto *outputSurface = GetOutputSurface();
	return ((outputSurface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF
	    && outputSurface->w == gnScreenWidth && outputSurface->h == gnScreenHeight
	    && outputSurface->format->BitsPerPixel == 8);
#endif
#else // !USE_SDL1
	if (renderer != nullptr) return false;
	SDL_Surface *outputSurface = GetOutputSurface();
	// Assumes double-buffering is available.
	return outputSurface->w == static_cast<int>(gnScreenWidth)
	    && outputSurface->h == static_cast<int>(gnScreenHeight)
	    && SDLC_SURFACE_BITSPERPIXEL(outputSurface) == 8;
#endif
}

/**
 * @brief Limit FPS to avoid high CPU load, use when v-sync isn't available
 */
void LimitFrameRate()
{
	if (*GetOptions().Graphics.frameRateControl != FrameRateControl::CPUSleep)
		return;
	static uint32_t frameDeadline;
	const uint32_t tc = SDL_GetTicks() * 1000;
	uint32_t v = 0;
	if (frameDeadline > tc) {
		v = tc % refreshDelay;
		SDL_Delay((v / 1000) + 1); // ceil
	}
	frameDeadline = tc + v + refreshDelay;
}

#ifdef __EMSCRIPTEN__
/** Yields to the browser at the end of a frame, paced by Frame Rate Control (see dx_browser_yield_frame). */
void YieldFrameToBrowser(bool idle = false)
{
	int mode = 0;
	if (idle) {
		mode = 1; // nothing was drawn: wait for the next animation frame instead of spinning
	} else {
		switch (*GetOptions().Graphics.frameRateControl) {
		case FrameRateControl::VerticalSync: mode = 1; break;
		case FrameRateControl::CPUSleep: mode = 2; break;
		default: mode = 0; break;
		}
	}
	dx_browser_yield_frame(mode, refreshDelay / 1000.0);
}
#endif

/** Shown once when the first game starts: an HD art pack is installed but this run cannot draw it. */
std::string HdArtNotice;

#if !DEVILUTIONX_WEBGPU && !defined(__EMSCRIPTEN__)
/** Classic SDL builds have no HD renderer: tell a player who installed the HD pack why it does not show. */
void NoteHdArtWithoutWebGpu()
{
	if (!*GetOptions().Graphics.hdArt)
		return;
	const std::string directory = FindHdArtDirectory();
	if (directory.empty())
		return;
	LogWarn("HD art folder {} found, but this build has no WebGPU renderer: HD art needs the WebGPU build of DamnationX", directory);
	HdArtNotice = _("HD art found, but this build cannot show it. Use the WebGPU build of DamnationX for HD art.");
}
#endif

} // namespace

void ShowHdArtNoticeOnce()
{
	if (HdArtNotice.empty())
		return;
	EventPlrMsg(HdArtNotice, UiFlags::ColorRed);
	HdArtNotice.clear();
}

void dx_init()
{
#ifndef USE_SDL1
	SDL_RaiseWindow(ghMainWnd);
	SDL_ShowWindow(ghMainWnd);
#endif

	Palette = SDLWrap::AllocPalette();
	palette_init();
	CreateBackBuffer();
#if DEVILUTIONX_WEBGPU
	if (!WebGpuInitialize("#canvas", gnScreenWidth, gnScreenHeight)) {
#ifdef __EMSCRIPTEN__
		LogError("Unable to initialize the Emdawnwebgpu renderer");
		GetWebGpuPlatform().notifyState("renderer-failure", "WebGPU initialization failed. Check browser and GPU support, then retry.", true, true);
#else
		// Desktop: keep playing with the classic frame through an SDL renderer (RenderPresent's renderer path).
		LogError("Unable to initialize the Dawn WebGPU renderer; presenting with the SDL renderer (no HD art)");
		if (HdArtEnabled() && !WebGpuDisabledByCommandLine()) {
			LogWarn("HD art needs WebGPU, which needs a GPU and driver with Direct3D 12, Vulkan or Metal");
			HdArtNotice = _("HD art is off: WebGPU could not start. It needs a GPU with Direct3D 12, Vulkan or Metal; try updating the graphics driver.");
		}
		ClearHdLaunchFlags(); // the SDL renderer cannot draw HD art
		WebGpuShutdown();
		FallBackToSdlRenderer();
#endif
	}
	HdUiInstall(); // ?hdUi=1: hooks the UI draws before the first UI asset loads
#elif !defined(__EMSCRIPTEN__)
	NoteHdArtWithoutWebGpu();
#endif
}

Surface GlobalBackBuffer()
{
	return Surface(PalSurface, SDL_Rect { 0, 0, gnScreenWidth, gnScreenHeight });
}

void dx_cleanup()
{
#if DEVILUTIONX_WEBGPU
	WebGpuShutdown();
#endif
#ifndef USE_SDL1
	if (ghMainWnd != nullptr)
		SDL_HideWindow(ghMainWnd);
#endif

	gbuffer::Free();
	PalSurface = nullptr;
	PinnedPalSurface = nullptr;
	Palette = nullptr;
	RendererTextureSurface = nullptr;
#ifndef USE_SDL1
	texture = nullptr;
	FreeVirtualGamepadTextures();
	if (*GetOptions().Graphics.upscale)
		SDL_DestroyRenderer(renderer);
#endif
	SDL_DestroyWindow(ghMainWnd);
}

void CreateBackBuffer()
{
	if (CanRenderDirectlyToOutputSurface()) {
		Log("{}", "Will render directly to the SDL output surface");
		PalSurface = GetOutputSurface();
		RenderDirectlyToOutputSurface = true;
	} else {
		PinnedPalSurface = SDLWrap::CreateRGBSurfaceWithFormat(
		    /*flags=*/0,
		    /*width=*/gnScreenWidth,
		    /*height=*/gnScreenHeight,
		    /*depth=*/8,
		    SDL_PIXELFORMAT_INDEX8);
		PalSurface = PinnedPalSurface.get();
	}

#if defined(USE_SDL3)
	if (!SDL_SetSurfacePalette(PalSurface, Palette.get())) ErrSdl();
#elif !defined(USE_SDL1)
	// In SDL2, `PalSurface` points to the global `palette`.
	if (SDL_SetSurfacePalette(PalSurface, Palette.get()) < 0)
		ErrSdl();
#else
	// In SDL1, `PalSurface` owns its palette and we must update it every
	// time the global `palette` is changed. No need to do anything here as
	// the global `palette` doesn't have any colors set yet.
#endif

	// The G-buffer planes mirror the back buffer; the presenter's textures follow its size.
	gbuffer::Allocate(static_cast<const uint8_t *>(PalSurface->pixels), static_cast<uint16_t>(PalSurface->pitch),
	    static_cast<uint16_t>(gnScreenWidth), static_cast<uint16_t>(gnScreenHeight));
#if DEVILUTIONX_WEBGPU
	// A no-op before WebGpuInitialize (dx_init creates the back buffer first); a full texture + bind group rebuild afterwards.
	const bool presenterWasReady = WebGpuIsReady();
	if (!WebGpuResize(gnScreenWidth, gnScreenHeight) && presenterWasReady)
		LogError("WebGPU presenter could not follow the back buffer resize to {}x{}", gnScreenWidth, gnScreenHeight);
#endif
}

void BltFast(SDL_Rect *srcRect, SDL_Rect *dstRect)
{
	if (RenderDirectlyToOutputSurface)
		return;
	Blit(PalSurface, srcRect, dstRect);
}

void Blit(SDL_Surface *src, SDL_Rect *srcRect, SDL_Rect *dstRect)
{
	if (HeadlessMode)
		return;

	SDL_Surface *dst = GetOutputSurface();
#if defined(USE_SDL3)
	if (!SDL_BlitSurface(src, srcRect, dst, dstRect)) ErrSdl();
#elif !defined(USE_SDL1)
	if (SDL_BlitSurface(src, srcRect, dst, dstRect) < 0)
		ErrSdl();
#else
	if (!OutputRequiresScaling()) {
		if (SDL_BlitSurface(src, srcRect, dst, dstRect) < 0)
			ErrSdl();
		return;
	}

	SDL_Rect scaledDstRect;
	if (dstRect != NULL) {
		scaledDstRect = *dstRect;
		ScaleOutputRect(&scaledDstRect);
		dstRect = &scaledDstRect;
	}

	// Same pixel format: We can call BlitScaled directly.
	if (SDLBackport_PixelFormatFormatEq(src->format, dst->format)) {
		if (SDL_BlitScaled(src, srcRect, dst, dstRect) < 0)
			ErrSdl();
		return;
	}

	// If the surface has a color key, we must stretch first and can then call BlitSurface.
	if (SDL_HasColorKey(src)) {
		SDLSurfaceUniquePtr stretched = SDLWrap::CreateRGBSurface(SDL_SWSURFACE, dstRect->w, dstRect->h, src->format->BitsPerPixel,
		    src->format->Rmask, src->format->Gmask, src->format->BitsPerPixel, src->format->Amask);
		SDL_SetColorKey(stretched.get(), SDL_SRCCOLORKEY, src->format->colorkey);
		if (src->format->palette != NULL)
			SDL_SetPalette(stretched.get(), SDL_LOGPAL, src->format->palette->colors, 0, src->format->palette->ncolors);
		SDL_Rect stretched_rect = { 0, 0, dstRect->w, dstRect->h };
		if (SDL_SoftStretch(src, srcRect, stretched.get(), &stretched_rect) < 0
		    || SDL_BlitSurface(stretched.get(), &stretched_rect, dst, dstRect) < 0) {
			ErrSdl();
		}
		return;
	}

	// A surface with a non-output pixel format but without a color key needs scaling.
	// We can convert the format and then call BlitScaled.
	SDLSurfaceUniquePtr converted = SDLWrap::ConvertSurface(src, dst->format, 0);
	if (SDL_BlitScaled(converted.get(), srcRect, dst, dstRect) < 0)
		ErrSdl();
#endif
}

#if DEVILUTIONX_WEBGPU
std::array<float, 3> GetObjectRayTracingColor(_object_id type)
{
	switch (type) {
	case OBJ_STORYCANDLE:
	case OBJ_L5CANDLE:
	case OBJ_CANDLE1:
	case OBJ_CANDLE2:
	case OBJ_BOOKCANDLE:
		return { 1.0F, 0.62F, 0.22F };
	case OBJ_BCROSS:
	case OBJ_TBCROSS:
		return { 1.0F, 0.28F, 0.08F };
	case OBJ_L1LIGHT:
	case OBJ_SKFIRE:
	case OBJ_TORCHL:
	case OBJ_TORCHR:
	case OBJ_TORCHL2:
	case OBJ_TORCHR2:
		return { 1.0F, 0.42F, 0.12F };
	default:
		return { 0.95F, 0.86F, 0.70F };
	}
}

std::array<float, 3> GetMissileRayTracingColor(MissileID type)
{
	switch (GetMissileData(type).damageType()) {
	case DamageType::Fire:
		return { 1.0F, 0.28F, 0.06F };
	case DamageType::Lightning:
		return { 0.28F, 0.58F, 1.0F };
	case DamageType::Magic:
		return { 0.68F, 0.38F, 1.0F };
	case DamageType::Acid:
		return { 0.32F, 1.0F, 0.12F };
	case DamageType::Physical:
		return { 0.95F, 0.86F, 0.70F };
	}

	return { 0.95F, 0.86F, 0.70F };
}

/** Light height above the floor by owner: torches and candles sit high, everything else at chest height. */
float GetObjectLightHeight(_object_id type)
{
	switch (type) {
	case OBJ_TORCHL:
	case OBJ_TORCHR:
	case OBJ_TORCHL2:
	case OBJ_TORCHR2:
	case OBJ_L1LIGHT:
	case OBJ_STORYCANDLE:
	case OBJ_L5CANDLE:
	case OBJ_CANDLE1:
	case OBJ_CANDLE2:
	case OBJ_BOOKCANDLE:
		return 0.9F;
	default:
		return 0.5F;
	}
}

float GetMissileLightHeight(MissileID type)
{
	return type == MissileID::TownPortal ? 0.9F : 0.4F;
}

/** tile_geometry::DoorOpenFn: only called for cells with dObject != 0. */
bool IsDoorOpenAt(Point tile)
{
	const Object *object = FindObjectAtPosition(tile);
	// DOOR_OPEN == 1 (Source/objects.cpp:130; the enum is local to that file).
	return object != nullptr && object->isDoor() && object->_oVar4 == 1;
}

/** FNV-1a over the 256 logical palette entries; the emissive weights are rebuilt only when it changes. */
uint64_t HashPalette(const SDL_Color *palette)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < 256; ++i) {
		const uint8_t bytes[4] = { palette[i].r, palette[i].g, palette[i].b, palette[i].a };
		for (const uint8_t byte : bytes) {
			hash ^= byte;
			hash *= 0x100000001b3ULL;
		}
	}
	return hash;
}

const std::array<float, 256> &GetPaletteEmissive(const SDL_Color *logicalPalette, dungeon_type level)
{
	static std::array<float, 256> Emissive {};
	static uint64_t EmissiveHash = 0;
	static dungeon_type EmissiveLevel = DTYPE_NONE;
	static bool EmissiveValid = false;
	const uint64_t hash = HashPalette(logicalPalette);
	if (!EmissiveValid || hash != EmissiveHash || level != EmissiveLevel) {
		gbuffer::BuildPaletteEmissive(std::span<const SDL_Color, 256>(logicalPalette, 256), level, Emissive);
		EmissiveHash = hash;
		EmissiveLevel = level;
		EmissiveValid = true;
	}
	return Emissive;
}

const std::array<float, 16> &GetClassicAmbientCurve(const SDL_Color *palette)
{
	static std::array<float, 16> Curve {};
	static uint64_t CurveHash = 0;
	static const uint8_t *CurveTables = nullptr;
	static bool CurveValid = false;
	const uint64_t hash = HashPalette(palette);
	// LightTables is rebuilt per level (MakeLightTable); its first byte address is stable, so pair the hash with a level change.
	if (!CurveValid || hash != CurveHash || CurveTables != FullyDarkLightTable) {
		gbuffer::BuildClassicAmbientCurve(std::span<const SDL_Color, 256>(palette, 256), LightTables, Curve);
		CurveHash = hash;
		CurveTables = FullyDarkLightTable;
		CurveValid = true;
	}
	return Curve;
}

void FillHybridPalette(std::array<std::array<uint8_t, 4>, 256> &out)
{
#ifdef USE_SDL3
	const SDL_Palette *sdlPalette = SDL_GetSurfacePalette(PalSurface);
#else
	const SDL_Palette *sdlPalette = PalSurface->format->palette;
#endif
	if (sdlPalette == nullptr)
		return;
	const int count = std::min(sdlPalette->ncolors, 256);
	for (int i = 0; i < count; ++i) {
		const SDL_Color &color = sdlPalette->colors[i];
		out[i] = { color.r, color.g, color.b, color.a };
	}
}

const WebGpuLightingData &BuildWebGpuLightingData()
{
	static_assert(WebGpuOccluderGridWidth == MAXDUNX);
	static_assert(WebGpuOccluderGridHeight == MAXDUNY);

	// ~52 KB by value: filled in place and returned by reference. The occluder grid and every
	// scalar are rewritten on each call, so nothing from the previous frame leaks through.
	static WebGpuLightingData Result;
	static tile_geometry::OccluderVolume Volume;
	WebGpuLightingData &result = Result;
	result.enabled = false;
	result.lightCount = 0;
	result.hybrid.enabled = false;
	result.hybrid.lightCount = 0;
	result.hybrid.planes = {};
	result.hybrid.volume = nullptr;
	result.worldLightCount = 0;
	result.uiRects = {}; // only the open panels are set below
	// Menus, the title screen and level transitions have no player or dungeon to light.
	if (MyPlayer == nullptr || !gbRunGame)
		return result;

	result.viewportHeight = GetViewportHeight();
	result.debugOccluders = *GetOptions().Graphics.debugLightGeometry;
	result.highQuality = *GetOptions().Graphics.lightingQuality == LightingQuality::High;
	result.bloom = *GetOptions().Graphics.bloom;
	result.isTown = leveltype == DTYPE_TOWN;
	const auto setUiRect = [&](size_t index, const Rectangle &rect) {
		result.uiRects[index] = {
			static_cast<float>(rect.position.x),
			static_cast<float>(rect.position.y),
			static_cast<float>(rect.size.width),
			static_cast<float>(rect.size.height),
		};
	};
	setUiRect(0, GetMainPanel());
	if (IsLeftPanelOpen())
		setUiRect(1, GetLeftPanel());
	if (IsRightPanelOpen())
		setUiRect(2, GetRightPanel());
	if (SpellSelectFlag) {
		const std::optional<Rectangle> spellList = GetSpellListRectangle();
		if (spellList.has_value())
			setUiRect(3, *spellList);
	}
	if (qtextflag)
		setUiRect(4, GetQuestTextRectangle());
	else if (IsPlayerInStore())
		setUiRect(4, GetStoreTextRectangle());

	// GetScreenPosition returns the bottom-left of a tile's bounding box, while
	// the shader floors world coordinates relative to the diamond's top vertex.
	const Point unscaledOrigin = GetScreenPosition({ 0, 0 });
	Point origin = unscaledOrigin + Displacement { TILE_WIDTH / 2, -(TILE_HEIGHT - 1) };
	const Point xBasisPoint = GetScreenPosition({ 1, 0 });
	const Point yBasisPoint = GetScreenPosition({ 0, 1 });
	// World-pass pixels to the screen: the zoom (also fractional), its remainder and an open left panel.
	const HdWorldTransform view = GetViewTransform();
	const float zoomFactor = view.Scale();
	const float originX = view.ToScreenX(static_cast<float>(origin.x));
	const float originY = view.ToScreenY(static_cast<float>(origin.y));
	const float basisXX = static_cast<float>(xBasisPoint.x - unscaledOrigin.x) * zoomFactor;
	const float basisXY = static_cast<float>(yBasisPoint.x - unscaledOrigin.x) * zoomFactor;
	const float basisYX = static_cast<float>(xBasisPoint.y - unscaledOrigin.y) * zoomFactor;
	const float basisYY = static_cast<float>(yBasisPoint.y - unscaledOrigin.y) * zoomFactor;
	const float determinant = basisXX * basisYY - basisXY * basisYX;
	if (determinant == 0.0F)
		return result;
	result.screenOriginX = originX;
	result.screenOriginY = originY;
	result.screenToWorldXX = basisYY / determinant;
	result.screenToWorldXY = -basisXY / determinant;
	result.screenToWorldYX = -basisYX / determinant;
	result.screenToWorldYY = basisXX / determinant;

	for (size_t y = 0; y < WebGpuOccluderGridHeight; ++y) {
		for (size_t x = 0; x < WebGpuOccluderGridWidth; ++x) {
			const Point tile { static_cast<int>(x), static_cast<int>(y) };
			result.occluders[y * WebGpuOccluderGridWidth + x] = TileHasAny(tile, TileProperties::BlockLight) ? 1U : 0U;
		}
	}

	struct Candidate {
		WebGpuRayTracedLight light;
		uint32_t stableId;
		int priority;
		int relevance;
		float worldZ; // hybrid only
		// Hybrid only: the emitter tile is Visible (or the level is town). The legacy path dropped this filter
		// (its shader occludes emitters by line of sight; filtering made lights pop at vision boundaries), but the
		// hybrid pass keeps it as its no-reveal rule for hidden emitters.
		bool emitterVisible;
	};
	static std::array<Candidate, MAXLIGHTS + MAXOBJECTS> candidates;
	size_t candidateCount = 0;
	std::array<bool, MAXLIGHTS> objectLightIds {};
	std::array<std::array<float, 3>, MAXLIGHTS> missileLightColors {};
	std::array<float, MAXLIGHTS> missileLightHeights {};
	std::array<bool, MAXLIGHTS> missileLightIds {};
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		if (GetObjectLightRadius(object._otype) != 0 && object._oVar1 == 1 && object._olid >= 0 && object._olid < MAXLIGHTS)
			objectLightIds[object._olid] = true;
	}
	for (const Missile &missile : Missiles) {
		if (missile._mlid < 0 || missile._mlid >= MAXLIGHTS)
			continue;
		missileLightIds[missile._mlid] = true;
		missileLightColors[missile._mlid] = GetMissileRayTracingColor(missile._mitype);
		missileLightHeights[missile._mlid] = GetMissileLightHeight(missile._mitype);
	}

	for (int i = 0; i < ActiveLightCount; ++i) {
		const uint32_t lightId = ActiveLights[i];
		const Light &light = Lights[lightId];
		if (light.isInvalid || light.radius == 0 || objectLightIds[lightId] || !InDungeonBounds(light.position.tile))
			continue;
		const int deltaX = light.position.tile.x - ViewPosition.x;
		const int deltaY = light.position.tile.y - ViewPosition.y;
		const bool isPlayerLight = MyPlayer != nullptr && lightId == static_cast<uint32_t>(MyPlayer->lightId);
		const std::array<float, 3> color = missileLightIds[lightId]
		    ? missileLightColors[lightId]
		    : std::array<float, 3> { 0.95F, 0.86F, 0.70F };
		candidates[candidateCount++] = {
			{
			    static_cast<float>(light.position.tile.x) + static_cast<float>(light.position.offset.deltaX) / 8.0F + 0.5F,
			    static_cast<float>(light.position.tile.y) + static_cast<float>(light.position.offset.deltaY) / 8.0F + 0.5F,
			    static_cast<float>(light.radius) + 1.0F,
			    1.0F,
			    color[0],
			    color[1],
			    color[2],
			},
			lightId,
			isPlayerLight ? 0 : 1,
			deltaX * deltaX + deltaY * deltaY,
			missileLightIds[lightId] ? missileLightHeights[lightId] : 0.5F, // player / monster lights at chest height
			currlevel == 0 || HasAnyOf(dFlags[light.position.tile.x][light.position.tile.y], DungeonFlag::Visible),
		};
	}

	for (int i = 0; i < ActiveObjectCount; ++i) {
		const int objectId = ActiveObjects[i];
		const Object &object = Objects[objectId];
		const uint8_t radius = GetObjectLightRadius(object._otype);
		if (radius == 0 || !InDungeonBounds(object.position))
			continue;
		// The shader handles line-of-sight occlusion. Using the emitter tile's
		// visibility here made its complete colored light pop on or off whenever
		// the player crossed a vision-tile boundary.
		if (!IsObjectLightInRange(object, radius))
			continue;
		const int deltaX = object.position.x - ViewPosition.x;
		const int deltaY = object.position.y - ViewPosition.y;
		const std::array<float, 3> color = GetObjectRayTracingColor(object._otype);
		candidates[candidateCount++] = {
			{
			    static_cast<float>(object.position.x) + 0.5F,
			    static_cast<float>(object.position.y) + 0.5F,
			    static_cast<float>(radius) + 1.0F,
			    0.8F,
			    color[0],
			    color[1],
			    color[2],
			},
			static_cast<uint32_t>(MAXLIGHTS + objectId),
			1,
			deltaX * deltaX + deltaY * deltaY,
			GetObjectLightHeight(object._otype),
			currlevel == 0 || HasAnyOf(dFlags[object.position.x][object.position.y], DungeonFlag::Visible),
		};
	}
	std::stable_sort(candidates.begin(), candidates.begin() + candidateCount, [](const Candidate &a, const Candidate &b) {
		if (a.priority != b.priority)
			return a.priority < b.priority;
		if (a.relevance != b.relevance)
			return a.relevance < b.relevance;
		return a.stableId < b.stableId;
	});

	// First-person 3D pass: all lights (no viewport or visibility filter, the 3D shaders trace their own shadows).
	if (render3d::WorldPassActive()) {
		for (size_t i = 0; i < candidateCount && result.worldLightCount < WebGpuMaxHybridLights; ++i) {
			const Candidate &candidate = candidates[i];
			result.worldLights[result.worldLightCount++] = { candidate.light.worldX, candidate.light.worldY, candidate.worldZ, candidate.light.radius,
				candidate.light.intensity, candidate.light.red, candidate.light.green, candidate.light.blue };
		}
	}

	result.lightCount = static_cast<uint32_t>(std::min(candidateCount, WebGpuMaxRayTracedLights));
	for (uint32_t i = 0; i < result.lightCount; ++i)
		result.lights[i] = candidates[i].light;
	result.enabled = *GetOptions().Graphics.perPixelLighting
	    && *GetOptions().Graphics.rayTracedLighting
	    && (result.lightCount != 0 || result.debugOccluders);

	// ---- 2.5D hybrid pass. `enabled` is what the back buffer holds, not the option. ----
	WebGpuHybridData &hybrid = result.hybrid;
	hybrid.enabled = gbuffer::BackBufferIsHybrid();
	if (!hybrid.enabled)
		return result;

	const bool highQuality = *GetOptions().Graphics.lightingQuality == LightingQuality::High;
	hybrid.softShadows = highQuality;
	hybrid.fullResolution = highQuality;
	hybrid.frameHasUnlit = false;
#ifndef USE_SDL1
	hybrid.frameHasUnlit = ControlMode == ControlTypes::VirtualGamepad;
#endif
	auto debugView = static_cast<uint8_t>(*GetOptions().Graphics.hybridDebugView);
	if (debugView == static_cast<uint8_t>(GBufferDebugView::Off) && *GetOptions().Graphics.debugLightGeometry)
		debugView = static_cast<uint8_t>(GBufferDebugView::Volume); // the legacy occluder view maps to Volume
	hybrid.debugView = debugView;
	// Exactly Zoom()'s mapping (HdWorldTransform): quarters, first-row remainder, offsetX (left panel and remainder).
	const HdWorldTransform zoomView = GetViewTransform();
	hybrid.zoom = static_cast<uint32_t>(zoomView.quarters) | (static_cast<uint32_t>(-zoomView.offsetY) << 8);
	hybrid.zoomOffsetX = static_cast<uint32_t>(zoomView.quarters > 4 ? zoomView.offsetX : 0);
	hybrid.frameCounter = gbuffer::FrameCounter();
	hybrid.ambientScale = static_cast<float>(*GetOptions().Graphics.hybridAmbient) / 100.0F;
	hybrid.heightPixelsPerWorldUnit = static_cast<float>(TILE_HEIGHT);

	const size_t hybridBudget = highQuality ? WebGpuMaxHybridLights : WebGpuMaxHybridLights / 2; // 32 / 16
	// Lights that cannot reach a receiver of the viewport must not take a budget slot. Receivers lie in the viewport rectangle
	// with their ground up to 255 (zoomed) rows lower, so their world positions lie in the box around that screen
	// area; the shader nudges a receiver by receiverNudge, the margin below covers it with room to spare.
	const float reachBottom = static_cast<float>(std::min<int>(result.viewportHeight, gnScreenHeight)) + 255.0F * zoomFactor;
	float reachLoX = 1e9F;
	float reachLoY = 1e9F;
	float reachHiX = -1e9F;
	float reachHiY = -1e9F;
	for (int corner = 0; corner < 4; ++corner) {
		const float dx = ((corner & 1) != 0 ? static_cast<float>(gnScreenWidth) : 0.0F) - originX;
		const float dy = ((corner & 2) != 0 ? reachBottom : 0.0F) - originY;
		const float worldX = result.screenToWorldXX * dx + result.screenToWorldXY * dy;
		const float worldY = result.screenToWorldYX * dx + result.screenToWorldYY * dy;
		reachLoX = std::min(reachLoX, worldX);
		reachLoY = std::min(reachLoY, worldY);
		reachHiX = std::max(reachHiX, worldX);
		reachHiY = std::max(reachHiY, worldY);
	}
	const float reachMargin = hybrid.receiverNudge + 1.0F;
	reachLoX -= reachMargin;
	reachLoY -= reachMargin;
	reachHiX += reachMargin;
	reachHiY += reachMargin;
	const auto reachesViewport = [&](const WebGpuRayTracedLight &light) {
		const float qx = std::clamp(light.worldX, reachLoX, reachHiX) - light.worldX;
		const float qy = std::clamp(light.worldY, reachLoY, reachHiY) - light.worldY;
		return qx * qx + qy * qy < light.radius * light.radius;
	};
	// Same priority/relevance order as the legacy list, restricted to Visible emitters (no-reveal, see Candidate) that reach
	// the viewport.
	hybrid.lightCount = 0;
	for (size_t i = 0; i < candidateCount && hybrid.lightCount < hybridBudget; ++i) {
		const Candidate &candidate = candidates[i];
		if (!candidate.emitterVisible || !reachesViewport(candidate.light))
			continue;
		hybrid.lights[hybrid.lightCount++] = {
			candidate.light.worldX,
			candidate.light.worldY,
			candidate.worldZ,
			candidate.light.radius,
			candidate.light.intensity,
			candidate.light.red,
			candidate.light.green,
			candidate.light.blue,
		};
	}

	const std::array<gbuffer::NormalLutEntry, 32> &lut = gbuffer::NormalLut();
	for (size_t i = 0; i < WebGpuNormalLutSize; ++i)
		hybrid.normalLut[i] = { lut[i].x, lut[i].y, lut[i].z, lut[i].pad };

	const SDL_Color *framePalette = gbuffer::CurrentPalette();
	if (framePalette == nullptr)
		framePalette = logical_palette.data();
	hybrid.paletteEmissive = GetPaletteEmissive(framePalette, leveltype);
	hybrid.ambientCurve = GetClassicAmbientCurve(framePalette);
	FillHybridPalette(hybrid.palette);

	// Planes: aux/index for the viewport rows, base for the unzoomed rows, rounded up like Zoom()'s source.
	const int viewportRows = std::min<int>(result.viewportHeight, gnScreenHeight);
	const std::span<const uint8_t> auxRows = gbuffer::AuxRows(viewportRows);
	const std::span<const uint8_t> baseRows = gbuffer::BaseRows(viewportRows <= 0 ? 0 : ((viewportRows - 1 - zoomView.offsetY) * 4) / zoomView.quarters + 1);
	hybrid.planes.aux = auxRows.data();
	hybrid.planes.base = baseRows.data();
	hybrid.planes.index = static_cast<const uint8_t *>(PalSurface->pixels);
	hybrid.planes.pitch = static_cast<uint32_t>(PalSurface->pitch);
	hybrid.planes.rows = static_cast<uint32_t>(viewportRows);
	if (auxRows.empty() || baseRows.empty()) {
		hybrid.enabled = false;
		return result;
	}

	// Height volume: static bits are rebuilt when dPiece/dSpecial change, dynamic bits every frame; the presenter
	// uploads the texture only when `generation` moves.
	tile_geometry::UpdateOccluderVolume(Volume, IsDoorOpenAt);
	hybrid.volume = Volume.cells.data();
	hybrid.volumeGeneration = Volume.generation;
	return result;
}
#endif

void RenderPresent()
{
	if (HeadlessMode)
		return;
	HdGraphicsFrameTick();

	SDL_Surface *surface = GetOutputSurface();

	if (!gbActive) {
#ifdef __EMSCRIPTEN__
		// Just yield to browser when inactive instead of blocking
		YieldFrameToBrowser(/*idle=*/true);
#else
		LimitFrameRate();
#endif
		return;
	}

#if DEVILUTIONX_WEBGPU
	// The desktop client presents through an SDL renderer when Dawn did not start (dx_init); the browser never has one.
	if (renderer == nullptr) {
		const auto presentStart = std::chrono::steady_clock::now();
		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(surface);
			// The gamepad is drawn into the RGBA frame only: mark its pixels Unlit so the composite passes them through.
			const Surface backBuffer = GlobalBackBuffer();
			for (const SDL_Rect &rect : VirtualGamepadRects())
				gbuffer::StampUnlit(backBuffer, Rectangle { Point { rect.x, rect.y }, Size { rect.w, rect.h } });
		}
		static std::array<std::array<uint8_t, 4>, 256> FloorPalette {};
		FillHybridPalette(FloorPalette); // the HD floor tints the Cathedral atlas toward the level palette
		// The HD trackers diff the frame without a view recolour (RedBack while paused or dead): it is not a draw over the HD art.
		int trackedPitch = PalSurface->pitch;
		const uint8_t *tracked = HdLayersTrackedFrame(static_cast<const uint8_t *>(PalSurface->pixels), PalSurface->pitch, PalSurface->w, PalSurface->h, trackedPitch);
		WebGpuFinishCathedralFloor(tracked, trackedPitch, FloorPalette[0].data());
		HdTilesFinishFrame(tracked, trackedPitch, PalSurface->w, PalSurface->h);
		HdSpritesFinishFrame(tracked, trackedPitch, PalSurface->w, PalSurface->h);
		HdUiFinishFrame(tracked, trackedPitch, PalSurface->w, PalSurface->h);
		// First person: UI pixels drawn with the transparent index stay UI black (render3d/world_renderer.hpp).
		if (render3d::WorldPassActive())
			render3d::ResolveUiCoverage(static_cast<uint8_t *>(PalSurface->pixels), PalSurface->pitch, PalSurface->w, GetViewportHeight());
		const DrawableSize drawableSize = GetDrawableSize();
		const WebGpuLightingData &lighting = BuildWebGpuLightingData();
		if (!WebGpuPresent(surface, drawableSize.pixelWidth, drawableSize.pixelHeight, lighting))
			LogError("WebGPU frame presentation failed");
		gbuffer::EndFrame();
		// The CPU side of RenderPresent (stamping, lighting data, uploads and encoding).
		Diagnostics.cpuFrameTimeMicroseconds = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - presentStart).count());
#ifdef __EMSCRIPTEN__
		YieldFrameToBrowser();
#endif
		return;
	}
#endif

#ifndef USE_SDL1
	if (renderer != nullptr) {
#ifdef USE_SDL3
		if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255)) ErrSdl();
		if (!SDL_RenderClear(renderer)) ErrSdl();
		if (!SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch)) ErrSdl();
		if (!SDL_RenderTexture(renderer, texture.get(), nullptr, nullptr)) ErrSdl();
#else
		if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) <= -1) ErrSdl();
		if (SDL_RenderClear(renderer) <= -1) ErrSdl();
		if (SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch) <= -1) ErrSdl();
		if (SDL_RenderCopy(renderer, texture.get(), nullptr, nullptr) <= -1) ErrSdl();
#endif

		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(renderer);
		}
		SDL_RenderPresent(renderer);

#ifdef __EMSCRIPTEN__
		// Return to the browser so it presents the frame; the yield also does the Frame Rate Control pacing.
		YieldFrameToBrowser();
#else
		if (*GetOptions().Graphics.frameRateControl != FrameRateControl::VerticalSync) {
			LimitFrameRate();
		}
#endif
	} else {
		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(surface);
		}

#ifdef USE_SDL3
		if (!SDL_UpdateWindowSurface(ghMainWnd)) ErrSdl();
#else
		if (SDL_UpdateWindowSurface(ghMainWnd) <= -1) ErrSdl();
#endif

		if (RenderDirectlyToOutputSurface)
			PalSurface = GetOutputSurface();
		LimitFrameRate();
	}
#else
	if (SDL_Flip(surface) <= -1) {
		ErrSdl();
	}
	if (RenderDirectlyToOutputSurface)
		PalSurface = GetOutputSurface();
	LimitFrameRate();
#endif
}

} // namespace devilution
