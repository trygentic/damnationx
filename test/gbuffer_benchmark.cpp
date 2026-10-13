#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/rectangle.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/surface.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {
namespace {

std::array<SDL_Color, 256> BenchPalette {};

/** An 8-bit back buffer mirrored by planes, latched into a hybrid frame. */
struct HybridFrame {
	OwnedSurface surface;

	HybridFrame(int width, int height, int viewportRows)
	    : surface(width, height)
	{
		gbuffer::Allocate(surface.begin(), surface.pitch(), static_cast<uint16_t>(width), static_cast<uint16_t>(height));
		gbuffer::LatchForFrame({ /*optionEnabled=*/true, /*presenterReady=*/true, /*headless=*/false, /*gameRunning=*/true, DTYPE_CATHEDRAL, BenchPalette.data() });
		gbuffer::BeginWorldPass(viewportRows);
	}

	~HybridFrame()
	{
		gbuffer::EndFrame();
		gbuffer::Free();
	}
};

// The per-frame plane A clear at the default 640x480 layout (viewport = 352 rows above the main panel). Target: < 50 us.
void BM_BeginWorldPassClear_640x352(benchmark::State &state)
{
	HybridFrame frame(640, 480, 352);
	for (auto _ : state) {
		gbuffer::BeginWorldPass(352);
		benchmark::DoNotOptimize(gbuffer::CurrentPlanes.aux[0]);
		benchmark::ClobberMemory();
	}
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * gbuffer::CurrentPlanes.pitch * 352);
}

void BM_BeginWorldPassClear_1280x576(benchmark::State &state)
{
	HybridFrame frame(1280, 720, 576);
	for (auto _ : state) {
		gbuffer::BeginWorldPass(576);
		benchmark::DoNotOptimize(gbuffer::CurrentPlanes.aux[0]);
		benchmark::ClobberMemory();
	}
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * gbuffer::CurrentPlanes.pitch * 576);
}

// A panel-sized rect stamp (StampUnlit mode), e.g. FillRect of a 200x20 text background.
void BM_StampUnlit_Rect200x20(benchmark::State &state)
{
	HybridFrame frame(640, 480, 480);
	gbuffer::EndWorldPass();
	const Rectangle rect { { 100, 100 }, { 200, 20 } };
	for (auto _ : state) {
		gbuffer::StampUnlit(frame.surface, rect);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations());
}

// A one-row stamp (the UnsafeDrawHorizontalLine chokepoint), clipped at the right edge.
void BM_StampUnlit_Row(benchmark::State &state)
{
	HybridFrame frame(640, 480, 480);
	gbuffer::EndWorldPass();
	const Surface sub = frame.surface.subregion(320, 352, 320, 128);
	const Rectangle rect { { 10, 5 }, { 400, 1 } };
	for (auto _ : state) {
		gbuffer::StampUnlit(sub, rect);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations());
}

// Plane A pixel doubling for the zoomed 640x480 world view.
void BM_ZoomInPlace_640x480(benchmark::State &state)
{
	HybridFrame frame(640, 480, 480);
	for (auto _ : state) {
		gbuffer::ZoomInPlace(640, 0, 480);
		benchmark::DoNotOptimize(gbuffer::CurrentPlanes.aux[0]);
		benchmark::ClobberMemory();
	}
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 640 * 480);
}

void BM_BuildBaseRow(benchmark::State &state)
{
	std::array<uint8_t, 32> row {};
	int anchor = 200;
	for (auto _ : state) {
		gbuffer::BuildBaseRow(row, anchor++, gbuffer::FootKind::SW);
		benchmark::DoNotOptimize(row);
	}
	state.SetItemsProcessed(state.iterations());
}

void BM_SetTileContext(benchmark::State &state)
{
	int anchor = 200;
	for (auto _ : state) {
		gbuffer::SetTileContext(gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft, anchor++, -12, gbuffer::FootKind::SW);
		benchmark::DoNotOptimize(gbuffer::CurrentTileBaseRow);
		benchmark::DoNotOptimize(gbuffer::CurrentTileAuxBlended);
	}
	state.SetItemsProcessed(state.iterations());
}

constexpr size_t RowOfBatch = 4096;

std::vector<uint32_t> RandomOffsets(uint32_t pitch, uint32_t height)
{
	std::mt19937 rng(42);
	std::uniform_int_distribution<uint32_t> dist(0, pitch * height - 1);
	std::vector<uint32_t> offsets(RowOfBatch);
	for (uint32_t &off : offsets)
		off = dist(rng);
	return offsets;
}

// The per-run row recovery the blitters pay: reciprocal multiply + shift ...
void BM_RowOf_Recip40(benchmark::State &state)
{
	const uint32_t pitch = static_cast<uint32_t>(state.range(0));
	gbuffer::Planes planes;
	planes.pitch = static_cast<uint16_t>(pitch);
	planes.pitchRecip40 = ((uint64_t { 1 } << 40) + pitch - 1) / pitch;
	const std::vector<uint32_t> offsets = RandomOffsets(pitch, 1080);
	for (auto _ : state) {
		uint32_t sum = 0;
		for (const uint32_t off : offsets)
			sum += planes.rowOf(off);
		benchmark::DoNotOptimize(sum);
	}
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * RowOfBatch);
}

// ... versus a 32-bit division by the run-time pitch.
void BM_RowOf_Div32(benchmark::State &state)
{
	const uint32_t pitch = static_cast<uint32_t>(state.range(0));
	const std::vector<uint32_t> offsets = RandomOffsets(pitch, 1080);
	for (auto _ : state) {
		uint32_t divisor = pitch;
		benchmark::DoNotOptimize(divisor); // keep it a run-time value: no strength reduction to a multiply
		uint32_t sum = 0;
		for (const uint32_t off : offsets)
			sum += off / divisor;
		benchmark::DoNotOptimize(sum);
	}
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * RowOfBatch);
}

BENCHMARK(BM_BeginWorldPassClear_640x352);
BENCHMARK(BM_BeginWorldPassClear_1280x576);
BENCHMARK(BM_StampUnlit_Rect200x20);
BENCHMARK(BM_StampUnlit_Row);
BENCHMARK(BM_ZoomInPlace_640x480);
BENCHMARK(BM_BuildBaseRow);
BENCHMARK(BM_SetTileContext);
BENCHMARK(BM_RowOf_Recip40)->Arg(640)->Arg(1366)->Arg(1920);
BENCHMARK(BM_RowOf_Div32)->Arg(640)->Arg(1366)->Arg(1920);

} // namespace
} // namespace devilution
