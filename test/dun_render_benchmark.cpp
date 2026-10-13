#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <benchmark/benchmark.h>

#include "engine/assets.hpp"
#include "engine/clx_sprite.hpp"
#include "engine/displacement.hpp"
#include "engine/lighting_defs.hpp"
#include "engine/load_file.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "lighting.h"
#include "options.h"
#include "utils/log.hpp"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace {

SDLSurfaceUniquePtr SdlSurface;
ankerl::unordered_dense::map<TileType, std::vector<LevelCelBlock>> Tiles;
std::unique_ptr<std::byte[]> BmDunCelData;
uint_fast8_t BmMicroTileLen;
std::vector<uint8_t> AuxPlane;
std::vector<uint8_t> BasePlane;

void InitOnce()
{
	[[maybe_unused]] static const bool GlobalInitDone = []() {
		LoadCoreArchives();
		LoadGameArchives();
		if (!HaveMainData()) {
			LogError("This benchmark needs spawn.mpq or diabdat.mpq");
			exit(1);
		}

		leveltype = DTYPE_CATHEDRAL;
		BmDunCelData = LoadFileInMem("levels\\l1data\\l1.cel");
		SetDungeonMicros(BmDunCelData, BmMicroTileLen);
		MakeLightTable();

		SdlSurface = SDLWrap::CreateRGBSurfaceWithFormat(
		    /*flags=*/0, /*width=*/640, /*height=*/480, /*depth=*/8, SDL_PIXELFORMAT_INDEX8);
		if (SdlSurface == nullptr) {
			LogError("Failed to create SDL Surface: {}", SDL_GetError());
			exit(1);
		}

		for (size_t i = 0; i < 700; ++i) {
			for (size_t j = 0; j < 10; ++j) {
				if (const LevelCelBlock levelCelBlock = DPieceMicros[i].mt[j]; levelCelBlock.hasValue()) {
					if ((j == 0 || j == 1) && levelCelBlock.type() == TileType::TransparentSquare) {
						// This could actually be re-encoded foliage, which is a triangle followed by TransparentSquare.
						// Simply skip it.
						continue;
					}
					Tiles[levelCelBlock.type()].push_back(levelCelBlock);
				}
			}
		}
		GetOptions().Graphics.perPixelLighting.SetValue(false);
		return true;
	}();
}

struct LightSpec {
	const uint8_t *tbl;
	/** Render with the G-buffer world pass capturing (LightType::GBufferLit / GBuffer leaves). */
	bool capture;
};

/**
 * Wires sentinel-free planes over the benchmark surface and a wall tile context for the
 * duration of a capture-mode run (without going through gbuffer::Allocate/SetTileContext).
 */
class CaptureScope {
public:
	CaptureScope(bool capture, const Surface &out, Point position)
	{
		if (!capture) return;
		const size_t size = static_cast<size_t>(out.pitch()) * out.h();
		AuxPlane.assign(size, gbuffer::UnlitByte);
		BasePlane.assign(size, 0);
		gbuffer::CurrentPlanes.aux = AuxPlane.data();
		gbuffer::CurrentPlanes.base = BasePlane.data();
		gbuffer::CurrentPlanes.outBase = out.begin();
		gbuffer::CurrentPlanes.pitch = out.pitch();
		gbuffer::CurrentPlanes.width = static_cast<uint16_t>(out.w());
		gbuffer::CurrentPlanes.height = static_cast<uint16_t>(out.h());
		gbuffer::CurrentPlanes.pitchRecip40 = ((uint64_t { 1 } << 40) + out.pitch() - 1) / out.pitch();
		gbuffer::CurrentTileAux = gbuffer::PackAux(gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft);
		gbuffer::CurrentTileAuxBlended = gbuffer::PackAux(gbuffer::Kind::WallBlended, gbuffer::NormalWallLeft);
		gbuffer::BuildBaseRow(gbuffer::CurrentTileBaseRow, position.y, gbuffer::FootKind::SW);
		gbuffer::CurrentTileLeftX = static_cast<int16_t>(position.x);
		gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	}

	~CaptureScope()
	{
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		gbuffer::CurrentPlanes = gbuffer::Planes {};
	}
};

void RunForTileMaskLight(benchmark::State &state, TileType tileType, MaskType maskType, LightSpec light, Point position)
{
	const Surface out = Surface(SdlSurface.get());
	const Lightmap lightmap(/*outBuffer=*/nullptr, /*lightmapBuffer=*/ {}, /*pitch=*/1, LightTables, FullyLitLightTable, FullyDarkLightTable);
	const std::span<const LevelCelBlock> tiles = Tiles[tileType];
	// A pixel inside the frame for both the full and the clipped position.
	const Point probe { std::max(position.x, 0) + 5, std::max(position.y - 5, 5) };
	const CaptureScope capture(light.capture, out, position);
	for (auto _ : state) {
		for (const LevelCelBlock &levelCelBlock : tiles) {
			RenderTile(out, lightmap, position, BmDunCelData.get(), levelCelBlock, maskType, light.tbl);
		}
		FlushTileRenderCommands();
		uint8_t color = out[probe];
		benchmark::DoNotOptimize(color);
	}
	state.SetItemsProcessed(state.iterations() * tiles.size());
}

using GetLightSpecFn = LightSpec();

LightSpec FullyLit() { return { LightTables[0].data(), /*capture=*/false }; }
LightSpec FullyDark() { return { LightTables.back().data(), /*capture=*/false }; }
LightSpec PartiallyLit() { return { LightTables[5].data(), /*capture=*/false }; }
LightSpec GBufferLit() { return { LightTables[0].data(), /*capture=*/true }; }
LightSpec GBuffer() { return { LightTables[5].data(), /*capture=*/true }; }

constexpr Point FullPosition { 320, 240 };
/** Left- and top-clipped: exercises the ClipLeftAndVertical paths. */
constexpr Point ClippedPosition { -10, 20 };

template <TileType TileT, MaskType MaskT, GetLightSpecFn GetLightSpecFnT>
void Render(benchmark::State &state)
{
	InitOnce();
	RunForTileMaskLight(state, TileT, MaskT, GetLightSpecFnT(), FullPosition);
}

template <TileType TileT, MaskType MaskT, GetLightSpecFn GetLightSpecFnT>
void RenderClipped(benchmark::State &state)
{
	InitOnce();
	RunForTileMaskLight(state, TileT, MaskT, GetLightSpecFnT(), ClippedPosition);
}

// Define aliases in order to have shorter benchmark names.
constexpr auto LeftTriangle = TileType::LeftTriangle;
constexpr auto RightTriangle = TileType::RightTriangle;
constexpr auto TransparentSquare = TileType::TransparentSquare;
constexpr auto Square = TileType::Square;
constexpr auto LeftTrapezoid = TileType::LeftTrapezoid;
constexpr auto RightTrapezoid = TileType::RightTrapezoid;
constexpr auto Transparent = MaskType::Transparent;
constexpr auto Solid = MaskType::Solid;
constexpr auto Left = MaskType::Left;
constexpr auto Right = MaskType::Right;

#define DEFINE_FOR_TILE_AND_MASK_TYPE(TILE_TYPE, MASK_TYPE)                 \
	BENCHMARK_TEMPLATE(Render, TILE_TYPE, MASK_TYPE, FullyLit);             \
	BENCHMARK_TEMPLATE(Render, TILE_TYPE, MASK_TYPE, FullyDark);            \
	BENCHMARK_TEMPLATE(Render, TILE_TYPE, MASK_TYPE, PartiallyLit);         \
	BENCHMARK_TEMPLATE(Render, TILE_TYPE, MASK_TYPE, GBufferLit);           \
	BENCHMARK_TEMPLATE(Render, TILE_TYPE, MASK_TYPE, GBuffer);              \
	BENCHMARK_TEMPLATE(RenderClipped, TILE_TYPE, MASK_TYPE, FullyLit);      \
	BENCHMARK_TEMPLATE(RenderClipped, TILE_TYPE, MASK_TYPE, FullyDark);     \
	BENCHMARK_TEMPLATE(RenderClipped, TILE_TYPE, MASK_TYPE, PartiallyLit);  \
	BENCHMARK_TEMPLATE(RenderClipped, TILE_TYPE, MASK_TYPE, GBufferLit);    \
	BENCHMARK_TEMPLATE(RenderClipped, TILE_TYPE, MASK_TYPE, GBuffer);

#define DEFINE_FOR_TILE_TYPE(TILE_TYPE)             \
	DEFINE_FOR_TILE_AND_MASK_TYPE(TILE_TYPE, Solid) \
	DEFINE_FOR_TILE_AND_MASK_TYPE(TILE_TYPE, Transparent)

DEFINE_FOR_TILE_TYPE(LeftTriangle)
DEFINE_FOR_TILE_TYPE(RightTriangle)
DEFINE_FOR_TILE_TYPE(TransparentSquare)
DEFINE_FOR_TILE_TYPE(Square)
DEFINE_FOR_TILE_TYPE(LeftTrapezoid)
DEFINE_FOR_TILE_TYPE(RightTrapezoid)
// The prefix masks are only valid for TransparentSquare and the matching trapezoid.
DEFINE_FOR_TILE_AND_MASK_TYPE(TransparentSquare, Left)
DEFINE_FOR_TILE_AND_MASK_TYPE(TransparentSquare, Right)
DEFINE_FOR_TILE_AND_MASK_TYPE(LeftTrapezoid, Left)
DEFINE_FOR_TILE_AND_MASK_TYPE(RightTrapezoid, Right)

template <bool Capture>
void BM_RenderBlackTile(benchmark::State &state)
{
	InitOnce();
	const Surface out = Surface(SdlSurface.get());
	const CaptureScope capture(Capture, out, FullPosition);
	for (auto _ : state) {
		world_draw_black_tile(out, 320, 240);
		uint8_t color = out[Point { 310, 200 }];
		benchmark::DoNotOptimize(color);
	}
	state.SetItemsProcessed(state.iterations());
}
BENCHMARK_TEMPLATE(BM_RenderBlackTile, false);
BENCHMARK_TEMPLATE(BM_RenderBlackTile, true);

} // namespace
} // namespace devilution
