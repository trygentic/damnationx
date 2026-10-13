#pragma once
/**
 * @file render3d/world_level.hpp
 *
 * CPU side of the 3D world: static level geometry (floors, ceilings, walls, pillars, doors, arches, boulders) built
 * from dPiece/SOLData and textured from the level's own CEL art, plus the shadow/occluder grid the world shaders
 * trace their shadow rays through. Rebuilt when the level changes (dPiece/dSpecial/door state/palette hash or
 * render3d::InvalidateLevel()).
 */

#include <array>
#include <cstdint>
#include <vector>

#include "levels/gendung_defs.hpp"
#include "render3d/math3d.hpp"

namespace devilution::render3d {

/** Vertex of every world draw (level mesh, billboards, boxes): 40 bytes, matches WorldVertexIn in world_shaders.hpp. */
struct WorldVertex {
	float px, py, pz;
	float u, v;       // normalized texture coordinates in the atlas named by the flags
	float nx, ny, nz; // face normal (billboards: ignored, the shader faces the camera)
	uint32_t flags;   // WorldVertexFlag bits | albedo scale << 8 (128 = 1.0)
	uint32_t reserved = 0;
};
static_assert(sizeof(WorldVertex) == 40);

enum WorldVertexFlag : uint32_t {
	kVertexSpriteAtlas = 1U << 0, // sample the sprite atlas instead of the level atlas
	kVertexUnlit = 1U << 1,       // full bright (self-lit missiles, unlit objects)
	kVertexOutline = 1U << 2,     // red outline around the alpha-tested sprite (crosshair target)
	kVertexBillboard = 1U << 3,   // lit with a camera-facing normal
	kVertexWater = 1U << 4,       // river surface: moonlit ripple sheen
	kVertexSoft = 1U << 5,        // biased toward the low-frequency mips (ceilings)
};

constexpr uint32_t PackVertexFlags(uint32_t flags, float albedoScale = 1.0F)
{
	const float scaled = albedoScale * 128.0F;
	const uint32_t s = scaled <= 0.0F ? 0U : (scaled >= 255.0F ? 255U : static_cast<uint32_t>(scaled));
	return flags | (s << 8);
}

constexpr int kGridSize = MAXDUNX; // 112
static_assert(MAXDUNX == MAXDUNY);

/** Occluder cell: bits 0..15 top height * 1000 (world units), bits 16..31 bottom height * 1000; top 0 = open. */
constexpr uint32_t PackOccluder(float bottom, float top)
{
	const auto q = [](float v) -> uint32_t {
		const float s = v * 1000.0F;
		return s <= 0.0F ? 0U : (s >= 65535.0F ? 65535U : static_cast<uint32_t>(s));
	};
	return q(top) | (q(bottom) << 16);
}

struct LevelGeometry {
	std::vector<WorldVertex> vertices;
	std::vector<uint32_t> indices;
	uint32_t atlasWidth = 0;
	uint32_t atlasHeight = 0;
	std::vector<uint8_t> atlasRgba; // atlasWidth * atlasHeight * 4
	/** Mip levels 1.. of the atlas (each half the size of the previous), box-filtered per 2 x 2 block. */
	std::vector<std::vector<uint8_t>> atlasMips;
	/** Stairs per cell: 0 none, +1 up, -1 down; stairsDir is the index of the climb direction (+x, -x, +z, -z). */
	std::array<int8_t, kGridSize * kGridSize> stairs {};
	std::array<uint8_t, kGridSize * kGridSize> stairsDir {};
	std::array<uint32_t, kGridSize * kGridSize> occluders {}; // [y * 112 + x]
	bool isTown = false;
	float wallHeight = 1.0F;
	/** Bumped on every rebuild; the GPU side re-uploads when it changes. */
	uint32_t generation = 0;
};

/** Rebuilds `geometry` when the level state hash changed (or after InvalidateLevel). Returns true when rebuilt. */
bool UpdateLevelGeometry(LevelGeometry &geometry);

/** Walkable floor height under world point (x, z): 0 except on stairs, which ramp along their steps. */
float FloorHeightOf(const LevelGeometry &geometry, float x, float z);

/** Height of a solid cell's occluder in world units (0 when open). For picking/collision helpers. */
float OccluderTopAt(const LevelGeometry &geometry, int x, int y);

/** Quantized sRGB palette (the level's logical palette) used to decode CEL/CLX art. */
const std::array<std::array<uint8_t, 4>, 256> &CurrentArtPalette();
/** Hash of CurrentArtPalette(); changes when the level palette changes. */
uint64_t CurrentArtPaletteHash();
/** Re-reads the logical palette. Call once per frame before decoding. */
void RefreshArtPalette();

} // namespace devilution::render3d
