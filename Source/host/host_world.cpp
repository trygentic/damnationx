/**
 * @file host/host_world.cpp
 *
 * HostWorldSnapshot (contract v3): what the 3D packs add to the World3D view. The static mesh placements of the
 * current level come from render3d's placement runtime (hd/3d/placements/<leveltype>.ini of the base, FREE and HD
 * tiers merged with HD winning, meshes sha256-checked; render3d::UpdateLevelGeometry, which HostSnapshot runs, loads
 * them and leaves the tiles they replace out of the level mesh). Model hints name the skinned 3D characters of the pack.
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>

#include "player.h"
#include "render3d/placement_runtime.hpp"
#include "utils/file_util.h"

namespace devilution::host {

namespace {

std::vector<HostPlacement> Placements;
std::vector<HostModelHint> Hints;
std::deque<std::string> Strings; // cleared with every snapshot: pointers live until the next HostTick
HostWorldFrame Frame {};

const char *Keep(std::string text)
{
	Strings.push_back(std::move(text));
	return Strings.back().c_str();
}

/** The skinned model of a hero class in the pack ("hd/3d/<class>/<class>.glb"), or empty. Checked once per class. */
const std::string &CharacterModel(HeroClass heroClass)
{
	static std::array<std::optional<std::string>, 8> Cache;
	const auto slot = static_cast<size_t>(heroClass);
	static const std::string None;
	if (slot >= Cache.size())
		return None;
	if (!Cache[slot]) {
		const char *name = nullptr;
		switch (heroClass) {
		case HeroClass::Warrior: name = "warrior"; break;
		case HeroClass::Rogue: name = "rogue"; break;
		case HeroClass::Sorcerer: name = "sorcerer"; break;
		case HeroClass::Monk: name = "monk"; break;
		case HeroClass::Bard: name = "bard"; break;
		case HeroClass::Barbarian: name = "barbarian"; break;
		default: break;
		}
		std::string asset;
		if (name != nullptr && !HdDirectory().empty()) {
			const std::string candidate = std::string("hd/3d/") + name + "/" + name + ".glb";
			if (FileExists(ResolvePackPath(candidate)))
				asset = candidate;
		}
		Cache[slot] = std::move(asset);
	}
	return *Cache[slot];
}

} // namespace

bool HasCharacterModel(int heroClass)
{
	return heroClass >= 0 && !CharacterModel(static_cast<HeroClass>(heroClass)).empty();
}

bool ObjectReplacedByMesh(int objectIndex)
{
	return render3d::PlacementHidesObject(objectIndex);
}

const HostWorldFrame &BuildWorldSnapshot(bool inGame, const std::vector<HostEntity> &entities)
{
	Placements.clear();
	Hints.clear();
	Strings.clear();
	Frame = {};
	if (inGame) {
		const render3d::LevelPlacements &level = render3d::CurrentPlacements();
		Frame.placementRevision = level.generation;
		for (const render3d::LoadedPlacement &loaded : level.items) {
			HostPlacement placement {};
			placement.meshId = loaded.meshId;
			placement.tileX = loaded.placement.tileX;
			placement.tileY = loaded.placement.tileY;
			placement.objectEntityId = loaded.placement.objectIndex >= 0
			    ? MakeEntityId(HostEntityKind::Object, static_cast<uint32_t>(loaded.placement.objectIndex))
			    : 0;
			for (size_t i = 0; i < 16; ++i)
				placement.model[i] = loaded.placement.model.m[i];
			placement.boundsMin[0] = loaded.worldMin.x;
			placement.boundsMin[1] = loaded.worldMin.y;
			placement.boundsMin[2] = loaded.worldMin.z;
			placement.boundsMax[0] = loaded.worldMax.x;
			placement.boundsMax[1] = loaded.worldMax.y;
			placement.boundsMax[2] = loaded.worldMax.z;
			std::string asset = loaded.file;
			std::replace(asset.begin(), asset.end(), '\\', '/');
			placement.assetPath = Keep(asset);
			placement.path = Keep(ResolvePackPath(asset));
			Placements.push_back(placement);
		}

		const render3d::PlacementEnvironment &environment = render3d::CurrentPlacementEnvironment();
		HostEnvironment &out = Frame.environment;
		if (environment.ambient) {
			out.setMask |= 1;
			out.ambient = *environment.ambient;
		}
		if (environment.exposure) {
			out.setMask |= 2;
			out.exposure = *environment.exposure;
		}
		if (environment.moonDirection) {
			out.setMask |= 4;
			out.moonDirection[0] = environment.moonDirection->x;
			out.moonDirection[1] = environment.moonDirection->y;
			out.moonDirection[2] = environment.moonDirection->z;
		}
		if (environment.moonStrength) {
			out.setMask |= 8;
			out.moonStrength = *environment.moonStrength;
		}

		for (const HostEntity &entity : entities) {
			if (entity.kind != HostEntityKind::Player)
				continue;
			const std::string &model = CharacterModel(static_cast<HeroClass>(entity.typeId));
			if (model.empty())
				continue;
			HostModelHint hint {};
			hint.entityId = entity.id;
			hint.pack = kPackCharacters3D;
			hint.assetPath = Keep(model);
			hint.path = Keep(ResolvePackPath(model));
			Hints.push_back(hint);
		}
	}
	Frame.placementCount = static_cast<uint32_t>(Placements.size());
	Frame.placements = Placements.empty() ? nullptr : Placements.data();
	Frame.modelHintCount = static_cast<uint32_t>(Hints.size());
	Frame.modelHints = Hints.empty() ? nullptr : Hints.data();
	return Frame;
}

} // namespace devilution::host
