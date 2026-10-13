#pragma once
/**
 * @file render3d/world_sprites.hpp
 *
 * Billboards of the 3D world: monsters, towners, other players, items, objects without a 3D model, missiles and
 * floor-aligned corpses, plus textured boxes for sarcophagi and chests. CLX frames are decoded with the level palette
 * (and the actor's TRN) into a sprite atlas cached across frames.
 */

#include <cstdint>
#include <vector>

#include "render3d/math3d.hpp"
#include "render3d/world_level.hpp"

namespace devilution::render3d {

constexpr int kSpriteAtlasSize = 2048;

/** A pending sprite atlas upload (RGBA8 rows, `width * 4` bytes each). */
struct SpriteUpload {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;
};

struct BillboardFrame {
	std::vector<WorldVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<SpriteUpload> uploads;
	/** Additive glow quads (missiles), drawn after the opaque billboards with the glow pipeline. */
	std::vector<WorldVertex> glowVertices;
	std::vector<uint32_t> glowIndices;
};

struct CameraView {
	Vec3 position;
	float yaw = 0.0F;
};

/** Fills `out` with this frame's billboards (vertices, indices, atlas uploads). */
void BuildBillboards(const CameraView &camera, const LevelGeometry &level, bool (*skipPlayer)(int playerId), BillboardFrame &out);

/** Forgets every cached sprite (device loss: the atlas texture is gone). */
void ResetSpriteCache();

} // namespace devilution::render3d
