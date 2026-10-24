#include "render3d/placement_runtime.hpp"
#include "host/host_hooks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <magic_enum/magic_enum.hpp>
#include <picosha2.h>

#include "config.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_geometry.hpp"
#include "objects.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"

#if DEVILUTIONX_WEBGPU
#include "engine/render/hd_sprites_loader.hpp"
#endif

namespace devilution::render3d {
namespace {

/**
 * Placement directories, lowest tier first. Each tier's file for the level type is optional; present files are merged
 * in this order (MergePlacementFiles), so the HD (Patreon) pack overrides the free pack by section id without either
 * pack overwriting the other's files when both are unzipped into hd/.
 */
constexpr std::array<std::string_view, 3> kPlacementDirs { "hd/3d/placements/", "hd/3d/free/placements/", "hd/3d/hd/placements/" };

enum class Fetch : uint8_t { Idle, Loading, Ready, Failed };

struct CachedMesh {
	Fetch state = Fetch::Idle;
	uint32_t id = 0;
	std::string expectedSha256;
	std::unique_ptr<StaticMesh> mesh;
};

struct TierFile {
	std::string name;
	Fetch state = Fetch::Idle;
	std::string text;
};

struct State {
	int leveltype = -2; // the level type the files below belong to
	std::array<TierFile, kPlacementDirs.size()> tiers;
	bool merged = false;
	std::optional<PlacementFile> file;
	/** Use state of the objects the rules look at (opened chests, door states): a change re-resolves the level. */
	uint64_t objectStateHash = 0;
	/** By asset path (shared across level types: town and dungeon files may reuse a mesh). */
	std::map<std::string, CachedMesh> meshes;
	LevelPlacements current;
} S;

/** Never reused, even across ReleasePlacementMeshes, so the renderer's per-mesh GPU buffers cannot go stale. */
uint32_t NextMeshId = 1;

/** Starts or advances a fetch of `path`; Ready fills `bytes`. Optional: a missing file is not an error. */
Fetch FetchFile(const std::string &path, std::string &bytes)
{
#if DEVILUTIONX_WEBGPU
	bool failed = false;
	if (hd_loader::FetchText(path.c_str(), bytes, failed, /*optional=*/true))
		return Fetch::Ready;
	return failed ? Fetch::Failed : Fetch::Loading;
#else
	// The 3D world (and with it the placements) needs the WebGPU presenter, or an embedding host (host/host_hd.cpp).
	bool failed = false;
	if (host::HdFetchText(path.c_str(), bytes, failed, /*optional=*/true))
		return Fetch::Ready;
	return Fetch::Failed;
#endif
}

std::string Sha256Hex(std::string_view bytes)
{
	return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

/** Advances the mesh fetch of `mesh` (its file path is `path`); true when it just finished (loaded or failed). */
bool PollMesh(const std::string &path, CachedMesh &mesh)
{
	if (mesh.state == Fetch::Ready || mesh.state == Fetch::Failed)
		return false;
	std::string bytes;
	const Fetch fetch = FetchFile(path, bytes);
	if (fetch == Fetch::Loading) {
		mesh.state = Fetch::Loading;
		return false;
	}
	if (fetch == Fetch::Failed) {
		LogWarn("3D placements: mesh {} is not in the HD pack; its placements keep the tile geometry", path);
		mesh.state = Fetch::Failed;
		return true;
	}
	if (!mesh.expectedSha256.empty()) {
		const std::string actual = Sha256Hex(bytes);
		if (actual != mesh.expectedSha256) {
			LogError("3D placements: {} has sha256 {}, the placement file pins {}; not drawn", path, actual, mesh.expectedSha256);
			mesh.state = Fetch::Failed;
			return true;
		}
	}
	auto loaded = std::make_unique<StaticMesh>();
	std::string error;
	if (!LoadStaticMesh(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()), *loaded, error)) {
		LogError("3D placements: {}: {}", path, error);
		mesh.state = Fetch::Failed;
		return true;
	}
	Log("3D placements: loaded {} ({} triangles)", path, loaded->indices.size() / 3);
	mesh.mesh = std::move(loaded);
	mesh.state = Fetch::Ready;
	return true;
}

uint64_t ObjectStateHash()
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		const uint64_t value = static_cast<uint64_t>(ActiveObjects[i]) << 24 | static_cast<uint64_t>(static_cast<uint8_t>(object._otype)) << 16
		    | static_cast<uint64_t>(object.selectionRegion) << 8 | static_cast<uint64_t>(object._oVar4 & 0xFF);
		hash = (hash ^ value) * 0x100000001B3ULL;
	}
	return hash;
}

int ObjectTypeOf(std::string_view name);

/** Parses and merges the tier files once all of them finished; false while one is still loading. */
bool MergeTiers()
{
	for (const TierFile &tier : S.tiers) {
		if (tier.state == Fetch::Idle || tier.state == Fetch::Loading)
			return false;
	}
	S.merged = true;
	std::optional<PlacementFile> merged;
	for (const TierFile &tier : S.tiers) {
		if (tier.state != Fetch::Ready)
			continue;
		PlacementParseResult parsed = ParsePlacementFile(tier.text);
		if (!parsed.file) {
			LogError("3D placements: {} rejected: {}", tier.name, parsed.error);
			continue;
		}
		Log("3D placements: {}: {} meshes, {} instances, {} rules", tier.name, parsed.file->meshes.size(), parsed.file->instances.size(),
		    parsed.file->rules.size());
		if (merged)
			MergePlacementFiles(*merged, *parsed.file);
		else
			merged = std::move(parsed.file);
	}
	for (TierFile &tier : S.tiers)
		tier.text.clear();
	if (!merged)
		return true;
	for (PlacementRule &rule : merged->rules) {
		rule.objectTypes.clear();
		for (const std::string &name : rule.objectNames) {
			const int type = ObjectTypeOf(name);
			if (type < 0)
				LogWarn("3D placements: rule {} names unknown object {}", rule.id, name);
			else
				rule.objectTypes.push_back(type);
		}
	}
	for (const PlacementMesh &mesh : merged->meshes) {
		CachedMesh &cached = S.meshes[mesh.file];
		if (cached.id == 0)
			cached.id = NextMeshId++;
		if (cached.expectedSha256 != mesh.sha256 && cached.state != Fetch::Loading) {
			// Pinned differently than when it was cached: check the bytes again.
			cached.expectedSha256 = mesh.sha256;
			cached.state = Fetch::Idle;
			cached.mesh.reset();
		}
	}
	S.file = std::move(merged);
	return true;
}

int ObjectTypeOf(std::string_view name)
{
	const std::optional<_object_id> type = magic_enum::enum_cast<_object_id>(name);
	return type ? static_cast<int>(*type) : -1;
}

bool InGrid(int x, int y) { return x >= 0 && y >= 0 && x < kPlacementGridSize && y < kPlacementGridSize; }

} // namespace

bool PollLevelPlacements()
{
	const int type = static_cast<int>(leveltype);
	if (type != S.leveltype) {
		S.leveltype = type;
		S.file.reset();
		S.merged = false;
		const std::string_view name = PlacementFileNameForLevelType(type);
		for (size_t i = 0; i < S.tiers.size(); ++i) {
			S.tiers[i] = {};
			if (name.empty())
				S.tiers[i].state = Fetch::Failed;
			else
				S.tiers[i].name = std::string(kPlacementDirs[i]) + std::string(name);
		}
	}
	bool changed = false;
	if (!S.merged) {
		for (TierFile &tier : S.tiers) {
			if (tier.state == Fetch::Idle || tier.state == Fetch::Loading)
				tier.state = FetchFile(tier.name, tier.text); // a missing tier file is normal: that pack has none
		}
		changed = MergeTiers() && S.file.has_value();
	}
	if (S.file) {
		for (const PlacementMesh &mesh : S.file->meshes)
			changed |= PollMesh(mesh.file, S.meshes[mesh.file]);
		// Opening a chest or a door switches show = unopened / closed rules.
		const uint64_t objects = ObjectStateHash();
		if (objects != S.objectStateHash) {
			S.objectStateHash = objects;
			changed |= std::any_of(S.file->rules.begin(), S.file->rules.end(), [](const PlacementRule &rule) { return rule.show != PlacementShow::Always; });
		}
	}
	return changed;
}

void ResolveLevelPlacements()
{
	LevelPlacements &current = S.current;
	const bool hadItems = !current.items.empty();
	current.items.clear();
	current.hiddenTiles.fill(0);
	current.hiddenObjects.clear();
	current.hiddenLintels.fill(0);
	if (!S.file || static_cast<int>(leveltype) != S.leveltype) {
		if (hadItems)
			++current.generation;
		return;
	}

	PlacementLevel level;
	level.currlevel = currlevel;
	level.setLevel = setlevel ? static_cast<int>(setlvlnum) : 0;
	level.piece = [](int x, int y) { return static_cast<uint16_t>(dPiece[x][y]); };
	std::array<uint8_t, kPlacementGridSize * kPlacementGridSize> doorCells {};
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		PlacementLevel::Object entry;
		entry.index = ActiveObjects[i];
		entry.type = static_cast<int>(object._otype);
		entry.x = object.position.x;
		entry.y = object.position.y;
		entry.used = object.selectionRegion == SelectionRegion::None;
		entry.door = object.isDoor();
		entry.doorOpen = entry.door && object._oVar4 != 0; // world_level.cpp: _oVar4 0 = DOOR_CLOSED
		entry.leftDoor = IsAnyOf(object._otype, OBJ_L1LDOOR, OBJ_L2LDOOR, OBJ_L3LDOOR, OBJ_L5LDOOR);
		if (entry.door && InGrid(entry.x, entry.y))
			doorCells[static_cast<size_t>(entry.y) * kPlacementGridSize + entry.x] = 1;
		level.objects.push_back(entry);
	}
	// Same notion of solid as the level builder (world_level.cpp BuildDungeon): door cells are passages.
	level.solid = [&doorCells](int x, int y) {
		return TileHasAny({ x, y }, TileProperties::Solid) && doorCells[static_cast<size_t>(y) * kPlacementGridSize + x] == 0;
	};
	level.arch = [](int x, int y) { return tile_geometry::ArchFacing(leveltype, dSpecial[x][y]); };

	std::vector<PlacementRect> rects;
	for (const ResolvedPlacement &placement : ResolvePlacements(*S.file, level)) {
		const PlacementMesh &meshInfo = S.file->meshes[static_cast<size_t>(placement.mesh)];
		const auto it = S.meshes.find(meshInfo.file);
		if (it == S.meshes.end() || it->second.state != Fetch::Ready || it->second.mesh == nullptr)
			continue; // not (yet) loaded: the tile geometry stays
		LoadedPlacement loaded;
		loaded.placement = placement;
		loaded.mesh = it->second.mesh.get();
		loaded.meshId = it->second.id;
		loaded.file = meshInfo.file;
		TransformBounds(*loaded.mesh, placement.model, loaded.worldMin, loaded.worldMax);
		rects.clear();
		AppendHiddenRects(*S.file, placement, rects);
		for (const PlacementRect &rect : rects) {
			for (int y = rect.y0; y <= rect.y1; ++y)
				for (int x = rect.x0; x <= rect.x1; ++x)
					current.hiddenTiles[static_cast<size_t>(y) * kPlacementGridSize + x] = 1;
		}
		if (placement.fromRule && S.file->rules[static_cast<size_t>(placement.source)].hideSelf) {
			if (placement.objectIndex >= 0)
				current.hiddenObjects.push_back(placement.objectIndex);
			if (placement.arch)
				current.hiddenLintels[static_cast<size_t>(placement.tileY) * kPlacementGridSize + placement.tileX] = 1;
		}
		current.items.push_back(loaded);
	}
	std::sort(current.hiddenObjects.begin(), current.hiddenObjects.end());
	if (hadItems || !current.items.empty())
		++current.generation;
}

const PlacementEnvironment &CurrentPlacementEnvironment()
{
	static const PlacementEnvironment None;
	return S.file && static_cast<int>(leveltype) == S.leveltype ? S.file->environment : None;
}

const LevelPlacements &CurrentPlacements()
{
	return S.current;
}

bool PlacementHidesTile(int x, int y)
{
	return InGrid(x, y) && S.current.hiddenTiles[static_cast<size_t>(y) * kPlacementGridSize + x] != 0;
}

bool PlacementHidesLintel(int x, int y)
{
	return InGrid(x, y) && S.current.hiddenLintels[static_cast<size_t>(y) * kPlacementGridSize + x] != 0;
}

bool PlacementHidesObject(int objectIndex)
{
	return std::binary_search(S.current.hiddenObjects.begin(), S.current.hiddenObjects.end(), objectIndex);
}

void ApplyPlacementOccluders(std::array<uint32_t, kPlacementGridSize * kPlacementGridSize> &occluders)
{
	for (const LoadedPlacement &loaded : S.current.items) {
		const ResolvedPlacement &placement = loaded.placement;
		if (placement.occluder == PlacementOccluder::None)
			continue;
		const float top = placement.occluder == PlacementOccluder::Fixed ? placement.occluderTop : loaded.worldMax.y;
		if (!(top > 0.0F))
			continue;
		// Cells whose centre lies inside the footprint: a thin prop (a fence post) does not shade a whole tile row.
		const int x0 = std::max(0, static_cast<int>(std::ceil(loaded.worldMin.x - 0.5F)));
		const int x1 = std::min(kPlacementGridSize - 1, static_cast<int>(std::floor(loaded.worldMax.x - 0.5F)));
		const int y0 = std::max(0, static_cast<int>(std::ceil(loaded.worldMin.z - 0.5F)));
		const int y1 = std::min(kPlacementGridSize - 1, static_cast<int>(std::floor(loaded.worldMax.z - 0.5F)));
		const uint32_t quantizedTop = static_cast<uint32_t>(std::min(top * 1000.0F, 65535.0F));
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				uint32_t &cell = occluders[static_cast<size_t>(y) * kPlacementGridSize + x];
				// Band [0, top] (bottom bits 16..31 = 0); a cell already blocking from the ground keeps the taller top.
				const uint32_t existingTop = cell & 0xFFFFU;
				const uint32_t existingBottom = cell >> 16;
				if (existingTop == 0 || existingBottom != 0 || existingTop < quantizedTop)
					cell = std::max(quantizedTop, existingBottom == 0 ? existingTop : 0U);
			}
		}
	}
}

void ReleasePlacementMeshes()
{
	const uint32_t generation = S.current.generation;
	S = {};
	S.current.generation = generation + 1;
}

} // namespace devilution::render3d
