#pragma once
/**
 * @file render3d/placement_runtime.hpp
 *
 * Loads the static mesh placements of the current level (render3d/placement.hpp, docs/design/static-mesh-placement.md)
 * from the HD art pack: hd/3d/placements/<leveltype>.ini and the .glb files it names. Main thread only.
 *
 * Lifecycle, driven by render3d::UpdateLevelGeometry (world_level.cpp):
 *  1. PollLevelPlacements() every frame: (re)starts the fetches when the level type changes and advances them (the
 *     browser fetch is asynchronous). Returns true when a file or mesh finished loading, so the level is rebuilt.
 *  2. ResolveLevelPlacements() on every level rebuild: matches the file against the live level (tiles, objects) and
 *     keeps the placements whose mesh loaded. Placements whose mesh is missing, malformed or fails its sha256 are
 *     dropped, and the tile geometry they would have hidden stays.
 *  3. The level builder asks PlacementHidesTile() per cell and ApplyPlacementOccluders() writes the shadow grid; the
 *     billboard builder asks PlacementHidesObject(); the static mesh renderer draws CurrentPlacements().
 *
 * Visual only: never touches collision, pathing, picking, triggers, saves or the RNG.
 */

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render3d/placement.hpp"
#include "render3d/static_mesh.hpp"

namespace devilution::render3d {

struct LoadedPlacement {
	ResolvedPlacement placement;
	/** The mesh (owned by the runtime's cache, alive until ReleasePlacementMeshes). */
	const StaticMesh *mesh = nullptr;
	/** Stable id of the mesh in the cache, for the renderer's GPU buffers. */
	uint32_t meshId = 0;
	/** The mesh's asset path ("hd/3d/..."), for an embedding host that draws it itself (Source/host/). */
	std::string file;
	skm::Vec3 worldMin;
	skm::Vec3 worldMax;
};

struct LevelPlacements {
	/** Bumped whenever `items` changes. */
	uint32_t generation = 0;
	std::vector<LoadedPlacement> items;
	/** 1 for cells whose tile-built geometry above the ground is replaced ([y * 112 + x]). */
	std::array<uint8_t, kPlacementGridSize * kPlacementGridSize> hiddenTiles {};
	/** Sorted indices of objects whose billboard (doors: built closed panel) is replaced. */
	std::vector<int> hiddenObjects;
	/** 1 for arch cells whose built lintel is replaced ([y * 112 + x]). */
	std::array<uint8_t, kPlacementGridSize * kPlacementGridSize> hiddenLintels {};
};

/** Advances loading for the current level type; true when something finished loading and the level should be rebuilt. */
bool PollLevelPlacements();

/** Re-resolves the placements against the live level (call while rebuilding the level geometry). */
void ResolveLevelPlacements();

/** The [environment] lighting overrides of the current level type's placement file (empty when it has none). */
const PlacementEnvironment &CurrentPlacementEnvironment();

/** The placements resolved by the last ResolveLevelPlacements. */
const LevelPlacements &CurrentPlacements();

/** True when a loaded placement replaces the geometry of tile (x, y) above the ground (the floor stays). */
bool PlacementHidesTile(int x, int y);

/** True when an arch rule with hide = self replaces the built lintel of cell (x, y). */
bool PlacementHidesLintel(int x, int y);

/** True when a loaded placement replaces the billboard of Objects[objectIndex] (doors: the built closed panel). */
bool PlacementHidesObject(int objectIndex);

/**
 * Writes the occluder bands of the loaded placements into the 112 x 112 grid (world_level's PackOccluder layout,
 * [y * 112 + x]): each covered cell blocks at least [0, top]; existing taller bands are kept.
 */
void ApplyPlacementOccluders(std::array<uint32_t, kPlacementGridSize * kPlacementGridSize> &occluders);

/** Forgets every cached file and mesh (device loss / shutdown; the next poll fetches again). */
void ReleasePlacementMeshes();

} // namespace devilution::render3d
