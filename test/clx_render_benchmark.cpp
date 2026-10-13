#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

#include "engine/clx_sprite.hpp"
#include "engine/displacement.hpp"
#include "engine/load_clx.hpp"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/surface.hpp"
#include "utils/clx_decode.hpp"
#include "utils/log.hpp"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace {

struct ReferenceSceneCounters {
	uint64_t frames = 0;
	uint64_t atlasBytes = 0;
	uint64_t uploadBytes = 0;
	uint64_t cacheEvictions = 0;
};

// Keep the scene fixed: release-gate runners can associate these counters with
// the JSON benchmark record without depending on host-specific asset ordering.
ReferenceSceneCounters RunReferenceScene(const OwnedClxSpriteList &sprites, const Surface &out, uint32_t frames)
{
	ReferenceSceneCounters counters;
	counters.frames = frames;
	counters.atlasBytes = sprites.dataSize();
	for (uint32_t frame = 0; frame < frames; ++frame) {
		for (size_t i = 0; i < sprites.numSprites(); ++i)
			RenderClxSprite(out, sprites[i], Point { static_cast<int>((i * 100 + frame) % 640), static_cast<int>((i * 60 + frame) % 480) });
		counters.uploadBytes += sprites.dataSize();
	}
	return counters;
}

SDLSurfaceUniquePtr CreateBenchmarkSurface()
{
	SDLSurfaceUniquePtr sdlSurface = SDLWrap::CreateRGBSurfaceWithFormat(
	    /*flags=*/0, /*width=*/640, /*height=*/480, /*depth=*/8, SDL_PIXELFORMAT_INDEX8);
	if (sdlSurface == nullptr) {
		LogError("Failed to create SDL Surface: {}", SDL_GetError());
		exit(1);
	}
	return sdlSurface;
}

void RecordSceneCounters(benchmark::State &state, const ReferenceSceneCounters &counters, const OwnedClxSpriteList &sprites, size_t itemsPerIteration)
{
	state.counters["atlas_bytes"] = benchmark::Counter(counters.atlasBytes, benchmark::Counter::kAvgIterations);
	state.counters["upload_bytes"] = benchmark::Counter(counters.uploadBytes, benchmark::Counter::kAvgIterations);
	state.counters["cache_evictions"] = benchmark::Counter(counters.cacheEvictions, benchmark::Counter::kAvgIterations);
	state.SetBytesProcessed(state.iterations() * sprites.dataSize());
	state.SetItemsProcessed(state.iterations() * itemsPerIteration);
}

// ---- Hybrid lighting capture ----

/** Row-span packed plane built from a decoded frame: the shape the normal-plane generator produces, minus the normals. */
struct BenchmarkPlane {
	const uint8_t *key = nullptr; // sprite.pixelData()
	std::vector<uint8_t> data;
	std::vector<uint32_t> rowOffset;
	std::vector<uint16_t> rowX0;
	uint16_t width = 0;
	uint16_t height = 0;

	[[nodiscard]] clx_normal_map::Plane view() const
	{
		clx_normal_map::Plane p;
		p.data = data.data();
		p.rowOffset = rowOffset.data();
		p.rowX0 = rowX0.data();
		p.width = width;
		p.height = height;
		return p;
	}
};

// Decodes a CLX frame to rows bottom-up (row 0 = feet); -1 = transparent.
std::vector<int16_t> DecodeClxBottomUp(ClxSprite sprite)
{
	const int w = sprite.width();
	const int h = sprite.height();
	std::vector<int16_t> pixels(static_cast<size_t>(w) * h, -1);
	const uint8_t *src = sprite.pixelData();
	const uint8_t *const end = src + sprite.pixelDataSize();
	int x = 0;
	int row = 0;
	while (src < end && row < h) {
		const uint8_t control = *src++;
		if (!IsClxOpaque(control)) {
			x += control;
		} else if (IsClxOpaqueFill(control)) {
			const uint8_t n = GetClxOpaqueFillWidth(control);
			const uint8_t color = *src++;
			for (uint8_t i = 0; i < n && x + i < w; ++i)
				pixels[static_cast<size_t>(row) * w + x + i] = color;
			x += n;
		} else {
			const uint8_t n = GetClxOpaquePixelsWidth(control);
			for (uint8_t i = 0; i < n && x + i < w; ++i)
				pixels[static_cast<size_t>(row) * w + x + i] = src[i];
			src += n;
			x += n;
		}
		while (x >= w) {
			x -= w;
			++row;
		}
	}
	return pixels;
}

BenchmarkPlane BuildBenchmarkPlane(ClxSprite sprite)
{
	BenchmarkPlane plane;
	plane.key = sprite.pixelData();
	plane.width = sprite.width();
	plane.height = sprite.height();
	const int w = plane.width;
	const std::vector<int16_t> pixels = DecodeClxBottomUp(sprite);
	plane.rowOffset.push_back(0);
	for (int r = 0; r < plane.height; ++r) {
		int x0 = w;
		int x1 = 0;
		for (int x = 0; x < w; ++x) {
			if (pixels[static_cast<size_t>(r) * w + x] < 0)
				continue;
			x0 = std::min(x0, x);
			x1 = x + 1;
		}
		if (x1 <= x0) {
			x0 = 0;
			x1 = 0;
		}
		plane.rowX0.push_back(static_cast<uint16_t>(x0));
		for (int x = x0; x < x1; ++x) {
			const int16_t c = pixels[static_cast<size_t>(r) * w + x];
			plane.data.push_back(c < 0 ? 0 : c == 0 ? gbuffer::ShadowByte : gbuffer::PackAux(gbuffer::Kind::Card, static_cast<uint8_t>(3 + (x * 5 + r * 3) % 29)));
		}
		plane.rowOffset.push_back(static_cast<uint32_t>(plane.data.size()));
	}
	return plane;
}

std::vector<BenchmarkPlane> ProviderPlanes;

// Pointer-keyed lookup over the handful of benchmark frames (mirrors the cost class of the generator's warm lookup).
clx_normal_map::Plane ProvideBenchmarkPlane(ClxSprite sprite, bool /*mirrored*/)
{
	const uint8_t *key = sprite.pixelData();
	for (const BenchmarkPlane &plane : ProviderPlanes) {
		if (plane.key == key)
			return plane.view();
	}
	return clx_normal_map::Plane {};
}

/** Back buffer + planes mirrored into gbuffer::CurrentPlanes with the requested mode; restores the globals on destruction. */
struct HybridScene {
	SDLSurfaceUniquePtr sdlSurface = CreateBenchmarkSurface();
	Surface out = Surface(sdlSurface.get());
	std::vector<uint8_t> aux;
	std::vector<uint8_t> base;

	HybridScene(gbuffer::Mode mode, gbuffer::Kind cardKind, const OwnedClxSpriteList *planesFor)
	    : aux(static_cast<size_t>(out.pitch()) * out.h(), gbuffer::UnlitByte)
	    , base(static_cast<size_t>(out.pitch()) * out.h(), 0)
	{
		gbuffer::Planes &planes = gbuffer::CurrentPlanes;
		planes.aux = aux.data();
		planes.base = base.data();
		planes.outBase = out.begin();
		planes.pitch = out.pitch();
		planes.width = static_cast<uint16_t>(out.w());
		planes.height = static_cast<uint16_t>(out.h());
		planes.pitchRecip40 = ((static_cast<uint64_t>(1) << 40) + planes.pitch - 1) / planes.pitch;
		gbuffer::CurrentCardKind = cardKind;
		ProviderPlanes.clear();
		if (planesFor != nullptr) {
			for (size_t i = 0; i < planesFor->numSprites(); ++i)
				ProviderPlanes.push_back(BuildBenchmarkPlane((*planesFor)[i]));
			clx_normal_map::SetNormalPlaneProviderForTesting(ProvideBenchmarkPlane);
		}
		gbuffer::CurrentMode = mode;
	}
	~HybridScene()
	{
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		gbuffer::CurrentCardKind = gbuffer::Kind::Card;
		gbuffer::CurrentPlanes = gbuffer::Planes {};
		clx_normal_map::SetNormalPlaneProviderForTesting(nullptr);
		ProviderPlanes.clear();
	}

	[[nodiscard]] uint8_t auxProbe() const { return aux[static_cast<size_t>(120) * out.pitch() + 120]; }
};

void BM_RenderSmallClx(benchmark::State &state)
{
	const SDLSurfaceUniquePtr sdl_surface = CreateBenchmarkSurface();
	const Surface out = Surface(sdl_surface.get());
	const OwnedClxSpriteList sprites = LoadClx("data\\resistance.clx");

	const size_t numSprites = sprites.numSprites();
	ReferenceSceneCounters counters;
	for (auto _ : state) {
		counters = RunReferenceScene(sprites, out, 1);
		uint8_t color = out[Point { 120, 120 }];
		benchmark::DoNotOptimize(color);
	}
	RecordSceneCounters(state, counters, sprites, numSprites);
}

void BM_RenderLargeClx(benchmark::State &state)
{
	const SDLSurfaceUniquePtr sdl_surface = CreateBenchmarkSurface();
	const Surface out = Surface(sdl_surface.get());
	const OwnedClxSpriteList sprites = LoadClx("ui_art\\dvl_lrpopup.clx");

	ReferenceSceneCounters counters;
	for (auto _ : state) {
		counters = RunReferenceScene(sprites, out, 1);
		uint8_t color = out[Point { 120, 120 }];
		benchmark::DoNotOptimize(color);
	}
	RecordSceneCounters(state, counters, sprites, 1);
}

// Capture with a row-span plane per frame (BlitWithGBuffer): target <= 1.6x BM_RenderSmallClx per run.
void BM_RenderSmallClx_Capture(benchmark::State &state)
{
	const OwnedClxSpriteList sprites = LoadClx("data\\resistance.clx");
	HybridScene scene(gbuffer::Mode::CaptureWorld, gbuffer::Kind::Card, &sprites);

	ReferenceSceneCounters counters;
	for (auto _ : state) {
		counters = RunReferenceScene(sprites, scene.out, 1);
		uint8_t color = scene.out[Point { 120, 120 }];
		benchmark::DoNotOptimize(color);
		uint8_t aux = scene.auxProbe();
		benchmark::DoNotOptimize(aux);
	}
	RecordSceneCounters(state, counters, sprites, sprites.numSprites());
	state.counters["plane_bytes"] = benchmark::Counter(static_cast<double>(ProviderPlanes.empty() ? 0 : ProviderPlanes[0].data.size()));
}

void BM_RenderLargeClx_Capture(benchmark::State &state)
{
	const OwnedClxSpriteList sprites = LoadClx("ui_art\\dvl_lrpopup.clx");
	HybridScene scene(gbuffer::Mode::CaptureWorld, gbuffer::Kind::Card, &sprites);

	ReferenceSceneCounters counters;
	for (auto _ : state) {
		counters = RunReferenceScene(sprites, scene.out, 1);
		uint8_t color = scene.out[Point { 120, 120 }];
		benchmark::DoNotOptimize(color);
		uint8_t aux = scene.auxProbe();
		benchmark::DoNotOptimize(aux);
	}
	RecordSceneCounters(state, counters, sprites, 1);
	state.counters["plane_bytes"] = benchmark::Counter(static_cast<double>(ProviderPlanes.empty() ? 0 : ProviderPlanes[0].data.size()));
}

// Capture with constant aux/base per run (BlitWithConstAux, CardEmissive): target <= 1.3x per run.
void BM_RenderSmallClx_CaptureConstAux(benchmark::State &state)
{
	const OwnedClxSpriteList sprites = LoadClx("data\\resistance.clx");
	HybridScene scene(gbuffer::Mode::CaptureWorld, gbuffer::Kind::CardEmissive, nullptr);

	ReferenceSceneCounters counters;
	for (auto _ : state) {
		counters = RunReferenceScene(sprites, scene.out, 1);
		uint8_t color = scene.out[Point { 120, 120 }];
		benchmark::DoNotOptimize(color);
		uint8_t aux = scene.auxProbe();
		benchmark::DoNotOptimize(aux);
	}
	RecordSceneCounters(state, counters, sprites, sprites.numSprites());
}

// UI stamping (BlitStampUnlit): one memset per run on plane A.
void BM_RenderSmallClx_StampUnlit(benchmark::State &state)
{
	const OwnedClxSpriteList sprites = LoadClx("data\\resistance.clx");
	HybridScene scene(gbuffer::Mode::StampUnlit, gbuffer::Kind::Card, nullptr);

	ReferenceSceneCounters counters;
	for (auto _ : state) {
		counters = RunReferenceScene(sprites, scene.out, 1);
		uint8_t color = scene.out[Point { 120, 120 }];
		benchmark::DoNotOptimize(color);
		uint8_t aux = scene.auxProbe();
		benchmark::DoNotOptimize(aux);
	}
	RecordSceneCounters(state, counters, sprites, sprites.numSprites());
}

BENCHMARK(BM_RenderSmallClx);
BENCHMARK(BM_RenderLargeClx);
BENCHMARK(BM_RenderSmallClx_Capture);
BENCHMARK(BM_RenderLargeClx_Capture);
BENCHMARK(BM_RenderSmallClx_CaptureConstAux);
BENCHMARK(BM_RenderSmallClx_StampUnlit);

} // namespace
} // namespace devilution
