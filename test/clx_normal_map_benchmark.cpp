/**
 * @file clx_normal_map_benchmark.cpp
 *
 * Benchmarks for the sprite normal-plane generator and cache.
 *
 * Counters: `packed_bytes` / `dense_bytes` / `packed_ratio` (row-span bytes vs w*h) per frame, and for GeneratePlane
 * `native_us` (measured per frame) plus `wasm_x3_estimate_us` (the x3 wasm scaling calibrates Pump against).
 */
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/clx_sprite.hpp"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/gbuffer.hpp"
#include "utils/clx_encode.hpp"
#include "utils/endian_write.hpp"

namespace devilution::clx_normal_map {
Scratch &SharedScratchForTesting();
} // namespace devilution::clx_normal_map

namespace devilution {
namespace {

constexpr uint8_t Transparent = 255;

const std::array<SDL_Color, 256> &GreyPalette()
{
	static const std::array<SDL_Color, 256> Palette = [] {
		std::array<SDL_Color, 256> palette {};
		for (size_t i = 0; i < palette.size(); ++i)
			palette[i] = SDL_Color { static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 255 };
		return palette;
	}();
	return Palette;
}

std::span<const SDL_Color, 256> PaletteSpan()
{
	return std::span<const SDL_Color, 256>(GreyPalette().data(), 256);
}

void LatchPalette()
{
	gbuffer::LatchInput in {};
	in.logicalPalette = GreyPalette().data();
	gbuffer::LatchForFrame(in);
}

/** Top-down 8-bit image of vertically stacked frames, encoded exactly like SurfaceToClx. */
struct Image {
	unsigned width;
	unsigned height;
	std::vector<uint8_t> pixels;
	Image(unsigned w, unsigned h)
	    : width(w)
	    , height(h)
	    , pixels(static_cast<size_t>(w) * h, Transparent)
	{
	}
	uint8_t &at(unsigned x, unsigned y) { return pixels[static_cast<size_t>(y) * width + x]; }
};

OwnedClxSpriteList EncodeClx(const Image &img, unsigned numFrames)
{
	std::vector<uint8_t> clxData(4 * (2 + static_cast<size_t>(numFrames)));
	WriteLE32(clxData.data(), numFrames);
	const unsigned frameHeight = img.height / numFrames;
	const uint8_t *dataPtr = img.pixels.data();
	for (unsigned frame = 1; frame <= numFrames; ++frame) {
		WriteLE32(&clxData[4 * static_cast<size_t>(frame)], static_cast<uint32_t>(clxData.size()));
		const size_t frameHeaderPos = clxData.size();
		clxData.resize(clxData.size() + ClxFrameHeaderSize);
		WriteLE16(&clxData[frameHeaderPos], ClxFrameHeaderSize);
		WriteLE16(&clxData[frameHeaderPos + 2], static_cast<uint16_t>(img.width));
		WriteLE16(&clxData[frameHeaderPos + 4], static_cast<uint16_t>(frameHeight));
		unsigned transparentRunWidth = 0;
		for (unsigned line = 0; line != frameHeight; ++line) {
			const uint8_t *src = &dataPtr[(frameHeight - (line + 1)) * img.width];
			unsigned solidRunWidth = 0;
			for (const uint8_t *srcEnd = src + img.width; src != srcEnd; ++src) {
				if (*src == Transparent) {
					if (solidRunWidth != 0) {
						AppendClxPixelsOrFillRun(src - transparentRunWidth - solidRunWidth, solidRunWidth, clxData);
						solidRunWidth = 0;
					}
					++transparentRunWidth;
				} else {
					AppendClxTransparentRun(transparentRunWidth, clxData);
					transparentRunWidth = 0;
					++solidRunWidth;
				}
			}
			if (solidRunWidth != 0)
				AppendClxPixelsOrFillRun(src - solidRunWidth, solidRunWidth, clxData);
		}
		AppendClxTransparentRun(transparentRunWidth, clxData);
		dataPtr += static_cast<size_t>(img.width) * frameHeight;
	}
	WriteLE32(&clxData[4 * (1 + static_cast<size_t>(numFrames))], static_cast<uint32_t>(clxData.size()));
	std::unique_ptr<uint8_t[]> data { new uint8_t[clxData.size()] };
	std::memcpy(data.get(), clxData.data(), clxData.size());
	return OwnedClxSpriteList { std::move(data) };
}

/**
 * Monster-like frames at `size` px: a textured body (torso ellipse + head disc + two legs) standing on a colour-0 shadow
 * ellipse, occupying roughly the proportion of the frame a Diablo monster does. `variant` perturbs the texture so that
 * frames never alias by content hash.
 */
Image MonsterLikeFrames(unsigned size, unsigned count)
{
	Image img(size, size * count);
	const float s = static_cast<float>(size);
	for (unsigned f = 0; f < count; ++f) {
		const unsigned base = f * size;
		for (unsigned y = 0; y < size; ++y) {
			for (unsigned x = 0; x < size; ++x) {
				const float px = static_cast<float>(x) + 0.5F;
				const float py = static_cast<float>(y) + 0.5F;
				const float cx = s * 0.5F;
				// Shadow ellipse on the feet rows.
				const float sdx = (px - cx) / (s * 0.30F);
				const float sdy = (py - s * 0.93F) / (s * 0.05F);
				bool shadow = sdx * sdx + sdy * sdy <= 1.0F;
				// Torso ellipse.
				const float tdx = (px - cx) / (s * 0.20F);
				const float tdy = (py - s * 0.55F) / (s * 0.24F);
				bool body = tdx * tdx + tdy * tdy <= 1.0F;
				// Head.
				const float hdx = (px - cx) / (s * 0.11F);
				const float hdy = (py - s * 0.24F) / (s * 0.11F);
				body = body || hdx * hdx + hdy * hdy <= 1.0F;
				// Legs.
				const bool legRows = py > s * 0.70F && py < s * 0.93F;
				body = body || (legRows && std::fabs(px - (cx - s * 0.10F)) < s * 0.05F) || (legRows && std::fabs(px - (cx + s * 0.10F)) < s * 0.05F);
				if (body) {
					const unsigned tex = ((x * 7 + y * 13 + f * 5) % 23) + ((x / 4 + y / 4) % 2) * 40;
					img.at(x, base + y) = static_cast<uint8_t>(60 + tex);
				} else if (shadow) {
					img.at(x, base + y) = 0;
				}
			}
		}
	}
	return img;
}

std::optional<OwnedClxSpriteList> LoadFixture(const char *name)
{
	const std::string path = std::string("test/fixtures/normal_map/") + name;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return std::nullopt;
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() < 12)
		return std::nullopt;
	std::unique_ptr<uint8_t[]> data { new uint8_t[bytes.size()] };
	std::memcpy(data.get(), bytes.data(), bytes.size());
	return OwnedClxSpriteList { std::move(data) };
}

void ReportPacking(benchmark::State &state, const clx_normal_map::Plane &plane)
{
	const double dense = static_cast<double>(plane.width) * plane.height;
	state.counters["packed_bytes"] = static_cast<double>(plane.bytes());
	state.counters["dense_bytes"] = dense;
	state.counters["packed_ratio"] = dense > 0.0 ? static_cast<double>(plane.bytes()) / dense : 0.0;
}

void RunGenerate(benchmark::State &state, ClxSprite sprite)
{
	std::vector<uint8_t> out;
	clx_normal_map::Options opts;
	clx_normal_map::Plane plane;
	double totalUs = 0.0;
	for (auto _ : state) {
		const auto t0 = std::chrono::steady_clock::now();
		plane = clx_normal_map::GeneratePlane(sprite, PaletteSpan(), opts, false, out, clx_normal_map::SharedScratchForTesting());
		const auto t1 = std::chrono::steady_clock::now();
		const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
		totalUs += us;
		state.SetIterationTime(us * 1e-6);
		benchmark::DoNotOptimize(plane);
	}
	const double perFrameUs = state.iterations() > 0 ? totalUs / static_cast<double>(state.iterations()) : 0.0;
	state.counters["native_us"] = perFrameUs;
	state.counters["wasm_x3_estimate_us"] = perFrameUs * 3.0;
	ReportPacking(state, plane);
	state.SetItemsProcessed(state.iterations());
}

// GeneratePlane on a monster-like frame at 64/96/128/160 px.
void BM_GeneratePlane(benchmark::State &state)
{
	const auto size = static_cast<unsigned>(state.range(0));
	const OwnedClxSpriteList list = EncodeClx(MonsterLikeFrames(size, 1), 1);
	RunGenerate(state, list[0]);
}
BENCHMARK(BM_GeneratePlane)->Arg(64)->Arg(96)->Arg(128)->Arg(160)->UseManualTime();

// GeneratePlane on the committed fixtures (the packed/dense ratio sizes the wasm budget from).
void BM_GeneratePlane_FixtureDisc(benchmark::State &state)
{
	std::optional<OwnedClxSpriteList> list = LoadFixture("disc.clx");
	if (!list) {
		state.SkipWithError("test/fixtures/normal_map/disc.clx missing");
		return;
	}
	RunGenerate(state, (*list)[0]);
}
BENCHMARK(BM_GeneratePlane_FixtureDisc)->UseManualTime();

void BM_GeneratePlane_FixtureRamp(benchmark::State &state)
{
	std::optional<OwnedClxSpriteList> list = LoadFixture("ramp.clx");
	if (!list) {
		state.SkipWithError("test/fixtures/normal_map/ramp.clx missing");
		return;
	}
	RunGenerate(state, (*list)[0]);
}
BENCHMARK(BM_GeneratePlane_FixtureRamp)->UseManualTime();

void BM_HashPixels(benchmark::State &state)
{
	const auto size = static_cast<unsigned>(state.range(0));
	const OwnedClxSpriteList list = EncodeClx(MonsterLikeFrames(size, 1), 1);
	const ClxSprite sprite = list[0];
	for (auto _ : state) {
		uint64_t h = clx_normal_map::HashPixels(sprite);
		benchmark::DoNotOptimize(h);
	}
	state.counters["pixel_data_bytes"] = static_cast<double>(sprite.pixelDataSize());
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * sprite.pixelDataSize());
}
BENCHMARK(BM_HashPixels)->Arg(128);

// Pump over a prewarm queue of 64 monster-like 128 px frames (per-frame cost through the cache: hash + generate + carve).
void BM_Pump(benchmark::State &state)
{
	constexpr unsigned Frames = 64;
	LatchPalette();
	clx_normal_map::SetOptions(clx_normal_map::Options {});
	clx_normal_map::SetCacheBudget(16 * 1024 * 1024);
	const OwnedClxSpriteList list = EncodeClx(MonsterLikeFrames(128, Frames), Frames);
	size_t generated = 0;
	for (auto _ : state) {
		state.PauseTiming();
		clx_normal_map::ClearNormalPlaneCache();
		clx_normal_map::EnqueuePrewarm(ClxSpriteList { list });
		state.ResumeTiming();
		generated += clx_normal_map::Pump(1000000000U);
	}
	state.SetItemsProcessed(static_cast<int64_t>(generated));
	state.counters["resident_bytes"] = static_cast<double>(clx_normal_map::GetStats().bytes);
	state.counters["bytes_per_frame"] = static_cast<double>(clx_normal_map::GetStats().bytes) / Frames;
	clx_normal_map::ClearNormalPlaneCache();
}
BENCHMARK(BM_Pump);

// The public lookup on a warm cache: the per-sprite cost DrawTileContent pays (<= 40 ns).
void BM_GetOrBuildNormalPlane_Warm(benchmark::State &state)
{
	constexpr unsigned Frames = 64;
	LatchPalette();
	clx_normal_map::SetOptions(clx_normal_map::Options {});
	clx_normal_map::SetCacheBudget(16 * 1024 * 1024);
	clx_normal_map::SetBudget(1000, 1000000);
	const OwnedClxSpriteList list = EncodeClx(MonsterLikeFrames(128, Frames), Frames);
	std::vector<ClxSprite> sprites;
	sprites.reserve(Frames);
	for (unsigned i = 0; i < Frames; ++i)
		sprites.push_back(list[i]);
	for (unsigned i = 0; i < Frames; ++i)
		(void)clx_normal_map::GetOrBuildNormalPlane(sprites[i], false);
	clx_normal_map::ResetFrameStats();
	unsigned i = 0;
	for (auto _ : state) {
		clx_normal_map::Plane plane = clx_normal_map::GetOrBuildNormalPlane(sprites[i], false);
		benchmark::DoNotOptimize(plane);
		i = (i + 1) & (Frames - 1);
	}
	state.SetItemsProcessed(state.iterations());
	const clx_normal_map::Stats s = clx_normal_map::GetStats();
	state.counters["hits"] = static_cast<double>(s.hits);
	state.counters["misses"] = static_cast<double>(s.misses);
	clx_normal_map::ClearNormalPlaneCache();
}
BENCHMARK(BM_GetOrBuildNormalPlane_Warm);

// Same lookup, one sprite hammered (best case: LRU head, no relink).
void BM_GetOrBuildNormalPlane_WarmSingle(benchmark::State &state)
{
	LatchPalette();
	clx_normal_map::SetOptions(clx_normal_map::Options {});
	clx_normal_map::SetCacheBudget(16 * 1024 * 1024);
	const OwnedClxSpriteList list = EncodeClx(MonsterLikeFrames(128, 1), 1);
	const ClxSprite sprite = list[0];
	(void)clx_normal_map::GetOrBuildNormalPlane(sprite, false);
	for (auto _ : state) {
		clx_normal_map::Plane plane = clx_normal_map::GetOrBuildNormalPlane(sprite, false);
		benchmark::DoNotOptimize(plane);
	}
	state.SetItemsProcessed(state.iterations());
	clx_normal_map::ClearNormalPlaneCache();
}
BENCHMARK(BM_GetOrBuildNormalPlane_WarmSingle);

} // namespace
} // namespace devilution
