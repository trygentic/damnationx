/**
 * @file host/host_snapshot.cpp
 *
 * HostSnapshot: the level geometry (render3d::LevelGeometry, the same CPU mesh and atlas the WebGPU 3D view uses),
 * the drawable entities with their interpolated positions, the game's light sources, the UI layer and the queued
 * sounds. Runs on the host thread while the game thread is parked (host_runtime.cpp).
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_map>

#include "cursor.h"
#include "engine/freeform_movement.hpp"
#include "engine/path.h"
#include "engine/render/ui_scale.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "nthread.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "qol/itemlabels.h"
#include "render3d/world_level.hpp"
#include "tables/misdat.h"
#include "tables/itemdat.h"
#include "engine/trn.hpp"
#include "towners.h"

namespace devilution::host {

static_assert(sizeof(HostWorldVertex) == sizeof(render3d::WorldVertex));
static_assert(offsetof(HostWorldVertex, u) == offsetof(render3d::WorldVertex, u));
static_assert(offsetof(HostWorldVertex, nx) == offsetof(render3d::WorldVertex, nx));
static_assert(offsetof(HostWorldVertex, flags) == offsetof(render3d::WorldVertex, flags));

namespace {

render3d::LevelGeometry Level;
std::vector<HostEntity> Entities;
std::vector<HostLight> FrameLights;
std::vector<HostSoundEvent> FrameSounds;
std::vector<EntitySpriteSource> SpriteSources;

void RecordSprite(const HostEntity &entity, ClxSprite sprite, const uint8_t *trn)
{
	if (sprite.width() == 0 || sprite.height() == 0)
		return;
	SpriteSources.push_back({ entity.id, sprite, trn });
}
HostFrame Frame {};
/** Snapshots taken since the last HostTick; sounds go to the first one only. */
bool SoundsDelivered = false;

/** Stable ids for missiles (they live in a std::list and have no index of their own). */
std::unordered_map<const Missile *, uint32_t> MissileIds;
uint32_t NextMissileId = 0;

struct WorldPosition {
	float x;
	float z;
};

WorldPosition FeetOf(const ::devilution::ActorPosition &position, bool walking, Direction dir, const AnimationInfo &anim)
{
	const SubTileDisplacement offset = position.GetDrawnSubTileOffset(walking, dir, anim);
	return {
		static_cast<float>(position.tile.x) + 0.5F + static_cast<float>(offset.deltaX) / static_cast<float>(SubTileUnits),
		static_cast<float>(position.tile.y) + 0.5F + static_cast<float>(offset.deltaY) / static_cast<float>(SubTileUnits),
	};
}

float AnimProgress(const AnimationInfo &anim)
{
	const int ticks = std::max<int>(anim.ticksPerFrame, 1);
	const float progress = (static_cast<float>(anim.tickCounterOfCurrentFrame) + static_cast<float>(ProgressToNextGameTick) / AnimationInfo::baseValueFraction) / ticks;
	return std::clamp(progress, 0.0F, 1.0F);
}

uint16_t ClampU16(int value)
{
	return static_cast<uint16_t>(std::clamp(value, 0, 0xFFFF));
}

uint16_t LightRadiusOf(int lightId)
{
	if (lightId < 0 || lightId >= MAXLIGHTS)
		return 0;
	return ::devilution::Lights[lightId].radius;
}

void FillCommon(HostEntity &entity, HostEntityKind kind, uint32_t index, WorldPosition position, float height)
{
	entity = {};
	entity.id = MakeEntityId(kind, index);
	entity.kind = kind;
	entity.x = position.x;
	entity.y = render3d::FloorHeightOf(Level, position.x, position.z) + height;
	entity.z = position.z;
}

void AddPlayers()
{
	for (size_t i = 0; i < Players.size(); ++i) {
		const Player &player = Players[i];
		if (!player.plractive || !player.isOnActiveLevel())
			continue;
		HostEntity &entity = Entities.emplace_back();
		FillCommon(entity, HostEntityKind::Player, static_cast<uint32_t>(i), FeetOf(player.position, player.isWalking(), player._pdir, player.AnimInfo), 0.0F);
		entity.facing = static_cast<uint8_t>(player._pdir);
		entity.typeId = static_cast<uint32_t>(player._pClass);
		entity.variant = player._pgfxnum;
		// Which animation (player_graphic) is playing: the sprite list the animation points at.
		if (player.AnimInfo.sprites) {
			const uint8_t *current = player.AnimInfo.sprites->data();
			for (size_t g = 0; g < player.AnimationData.size(); ++g) {
				const PlayerAnimationData &data = player.AnimationData[g];
				if (!data.sprites)
					continue;
				for (int d = 0; d < 8; ++d) {
					if (data.spritesForDirection(static_cast<Direction>(d)).data() == current) {
						entity.anim = static_cast<uint16_t>(g);
						entity.facing = static_cast<uint8_t>(d);
					}
				}
			}
		}
		entity.frame = ClampU16(player.AnimInfo.currentFrame);
		entity.frameCount = ClampU16(player.AnimInfo.numberOfFrames);
		entity.animProgress = AnimProgress(player.AnimInfo);
		entity.lightRadius = ClampU16(player._pLightRad);
		if (&player == MyPlayer)
			entity.flags |= kEntityLocalPlayer;
		if (PlayerUnderCursor == &player)
			entity.flags |= kEntityHighlighted;
		if (player._pHitPoints >> 6 <= 0 || player._pmode == PM_DEATH)
			entity.flags |= kEntityDead;
		if (player.AnimInfo.sprites)
			RecordSprite(entity, player.currentSprite(), nullptr);
	}
}

void AddMonsters()
{
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (!InDungeonBounds(monster.position.tile))
			continue;
		HostEntity &entity = Entities.emplace_back();
		FillCommon(entity, HostEntityKind::Monster, id, FeetOf(monster.position, monster.isWalking(), monster.direction, monster.animInfo), 0.0F);
		entity.facing = static_cast<uint8_t>(monster.direction);
		entity.typeId = static_cast<uint32_t>(monster.type().type);
		entity.variant = monster.isUnique() ? static_cast<uint32_t>(monster.uniqueType) + 1 : 0;
		if (monster.animInfo.sprites) {
			const uint8_t *current = monster.animInfo.sprites->data();
			for (int g = 0; g < 6; ++g) {
				const AnimStruct &anim = monster.type().getAnimData(static_cast<MonsterGraphic>(g));
				if (!anim.sprites)
					continue;
				for (int d = 0; d < 8; ++d) {
					const OptionalClxSpriteList list = anim.spritesForDirection(static_cast<Direction>(d));
					if (list && list->data() == current)
						entity.anim = static_cast<uint16_t>(g);
				}
			}
		}
		entity.frame = ClampU16(monster.animInfo.currentFrame);
		entity.frameCount = ClampU16(monster.animInfo.numberOfFrames);
		entity.animProgress = AnimProgress(monster.animInfo);
		if (monster.lightId != NO_LIGHT)
			entity.lightRadius = LightRadiusOf(monster.lightId);
		if (pcursmonst == static_cast<int>(id) && leveltype != DTYPE_TOWN)
			entity.flags |= kEntityHighlighted;
		if (monster.hitPoints >> 6 <= 0 || monster.mode == MonsterMode::Death)
			entity.flags |= kEntityDead;
		// Hidden (burrowed, invisible) or in the dark: the classic view would not draw it.
		if ((monster.flags & MFLAG_HIDDEN) != 0 || (leveltype != DTYPE_TOWN && !IsTileLit(monster.position.tile)))
			entity.flags |= kEntityInvisible;
		if (monster.animInfo.sprites) {
			const uint8_t *trn = monster.isUnique() ? monster.uniqueMonsterTRN.get() : nullptr;
			if (monster.mode == MonsterMode::Petrified)
				trn = GetStoneTRN();
			RecordSprite(entity, monster.animInfo.currentSprite(), trn);
		}
	}
}

void AddTowners()
{
	for (size_t i = 0; i < Towners.size(); ++i) {
		const Towner &towner = Towners[i];
		HostEntity &entity = Entities.emplace_back();
		FillCommon(entity, HostEntityKind::Towner, static_cast<uint32_t>(i),
		    { static_cast<float>(towner.position.x) + 0.5F, static_cast<float>(towner.position.y) + 0.5F }, 0.0F);
		entity.typeId = static_cast<uint32_t>(towner._ttype);
		entity.frame = towner._tAnimFrame;
		entity.frameCount = towner.anim ? ClampU16(static_cast<int>(towner.anim->numSprites())) : 0;
		if (leveltype == DTYPE_TOWN && pcursmonst == static_cast<int>(i))
			entity.flags |= kEntityHighlighted;
		if (!towner.anim)
			entity.flags |= kEntityInvisible;
		else if (towner._tAnimFrame < towner.anim->numSprites())
			RecordSprite(entity, towner.currentSprite(), nullptr);
	}
}

void AddObjects()
{
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		if (!InDungeonBounds(object.position))
			continue;
		HostEntity &entity = Entities.emplace_back();
		FillCommon(entity, HostEntityKind::Object, static_cast<uint32_t>(id),
		    { static_cast<float>(object.position.x) + 0.5F, static_cast<float>(object.position.y) + 0.5F }, 0.0F);
		entity.typeId = static_cast<uint32_t>(object._otype);
		// Classic chests jump directly between closed/open sprite poses; their
		// frame number also encodes an art variant, not opening animation time.
		if (object.IsChest())
			entity.anim = object.canInteractWith() ? 0 : 2;
		if (object.IsBroken())
			entity.flags |= kEntityDead;
		entity.frame = ClampU16(static_cast<int>(object._oAnimFrame));
		entity.frameCount = ClampU16(static_cast<int>(object._oAnimLen));
		if (object._olid >= 0 && object._olid < MAXLIGHTS)
			entity.lightRadius = LightRadiusOf(object._olid);
		if (ObjectUnderCursor == &object)
			entity.flags |= kEntityHighlighted;
		if (!object._oAnimData)
			entity.flags |= kEntityInvisible;
		if (ObjectReplacedByMesh(id))
			entity.flags |= kEntityReplacedByMesh;
		if (object._oAnimData && object._oAnimFrame >= 1 && object._oAnimFrame <= object._oAnimData->numSprites())
			RecordSprite(entity, object.currentSprite(), nullptr);
	}
}

void AddItems()
{
	for (uint8_t i = 0; i < ActiveItemCount; ++i) {
		const int id = ActiveItems[i];
		const Item &item = Items[id];
		if (!InDungeonBounds(item.position))
			continue;
		HostEntity &entity = Entities.emplace_back();
		FillCommon(entity, HostEntityKind::Item, static_cast<uint32_t>(id),
		    { static_cast<float>(item.position.x) + 0.5F, static_cast<float>(item.position.y) + 0.5F }, 0.0F);
		entity.typeId = item._iCurs;
		entity.variant = static_cast<uint32_t>(item._iMagical);
		entity.frame = ClampU16(item.AnimInfo.currentFrame);
		entity.frameCount = ClampU16(item.AnimInfo.numberOfFrames);
		entity.animProgress = AnimProgress(item.AnimInfo);
		if (pcursitem == id)
			entity.flags |= kEntityHighlighted;
		if (item.AnimInfo.sprites)
			RecordSprite(entity, item.AnimInfo.currentSprite(), nullptr);
	}
}

void AddMissiles()
{
	std::unordered_map<const Missile *, uint32_t> alive;
	for (const Missile &missile : Missiles) {
		uint32_t index;
		if (const auto it = MissileIds.find(&missile); it != MissileIds.end()) {
			index = it->second;
		} else {
			index = NextMissileId;
			NextMissileId = (NextMissileId + 1) % 0xFFFFFF; // index + 1 must stay within 24 bits
		}
		alive.emplace(&missile, index);
		// The render position scrollrt interpolated between the game ticks (UpdateMissilesRendererData), a pixel
		// offset from the tile's isometric draw position (x + 1 = 32 px right, 16 px down).
		const Point tile = missile.position.tileForRendering;
		const float sx = static_cast<float>(missile.position.offsetForRendering.deltaX);
		const float sy = static_cast<float>(missile.position.offsetForRendering.deltaY);
		const WorldPosition position {
			static_cast<float>(tile.x) + 0.5F + (sx / 32.0F + sy / 16.0F) * 0.5F,
			static_cast<float>(tile.y) + 0.5F + (sy / 16.0F - sx / 32.0F) * 0.5F,
		};
		const bool moving = missile.position.velocity.deltaX != 0 || missile.position.velocity.deltaY != 0;
		HostEntity &entity = Entities.emplace_back();
		// Flying projectiles at chest height (as the WebGPU 3D view draws them), ground effects on the floor.
		FillCommon(entity, HostEntityKind::Missile, index, position, moving ? 0.5F : 0.0F);
		entity.typeId = static_cast<uint32_t>(missile._mitype);
		entity.facing = static_cast<uint8_t>(missile.getFrameGroupRaw() & 7);
		entity.anim = ClampU16(missile.getFrameGroupRaw());
		entity.frame = ClampU16(missile._miAnimFrame - 1);
		entity.frameCount = ClampU16(missile._miAnimLen);
		entity.lightRadius = LightRadiusOf(missile._mlid);
		if (!missile._miDrawFlag)
			entity.flags |= kEntityInvisible;
		if (missile._miAnimData && missile._miAnimFrame >= 1 && static_cast<uint32_t>(missile._miAnimFrame) <= missile._miAnimData->numSprites()) {
			const uint8_t *trn = missile._miUniqTrans != 0 && missile._misource >= 0 ? Monsters[missile._misource].uniqueMonsterTRN.get() : nullptr;
			RecordSprite(entity, (*missile._miAnimData)[missile._miAnimFrame - 1], trn);
		}
	}
	MissileIds = std::move(alive);
}

/** Colour of a light by what it belongs to (the WebGPU lighting uses the same families). */
void LightColor(const Missile *missile, bool object, uint8_t &r, uint8_t &g, uint8_t &b)
{
	if (missile != nullptr) {
		switch (GetMissileData(missile->_mitype).damageType()) {
		case DamageType::Fire:
			r = 255, g = 140, b = 50;
			return;
		case DamageType::Lightning:
			r = 120, g = 170, b = 255;
			return;
		case DamageType::Magic:
			r = 190, g = 120, b = 255;
			return;
		default:
			break;
		}
	}
	if (object) {
		r = 255, g = 170, b = 90; // torches, candles, braziers
		return;
	}
	r = 242, g = 219, b = 178;
}

void AddLights()
{
	// Who each light belongs to.
	std::array<uint32_t, MAXLIGHTS> owner {};
	std::array<const Missile *, MAXLIGHTS> missileOf {};
	std::array<bool, MAXLIGHTS> objectLight {};
	for (size_t i = 0; i < Players.size(); ++i) {
		const Player &player = Players[i];
		if (player.plractive && player.isOnActiveLevel() && player.lightId >= 0 && player.lightId < MAXLIGHTS)
			owner[player.lightId] = MakeEntityId(HostEntityKind::Player, static_cast<uint32_t>(i));
	}
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.lightId != NO_LIGHT && monster.lightId >= 0 && monster.lightId < MAXLIGHTS)
			owner[monster.lightId] = MakeEntityId(HostEntityKind::Monster, ActiveMonsters[i]);
	}
	for (const Missile &missile : Missiles) {
		if (missile._mlid < 0 || missile._mlid >= MAXLIGHTS)
			continue;
		missileOf[missile._mlid] = &missile;
		if (const auto it = MissileIds.find(&missile); it != MissileIds.end())
			owner[missile._mlid] = MakeEntityId(HostEntityKind::Missile, it->second);
	}
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		const int lightId = object._olid;
		// Non-emitting objects keep the default _olid == 0, and extinguished sources can keep a recycled id.
		// They must not turn the player's moving light into an orange object light (or a flame emitter).
		if (lightId < 0 || lightId >= MAXLIGHTS || owner[lightId] != 0 || object._oDelFlag)
			continue;
		const bool emitting = object._otype == OBJ_FLAMEHOLE
		    ? object._oVar4 != 0
		    : GetObjectLightRadius(object._otype) != 0 && object._oVar1 == 1;
		const Light &light = ::devilution::Lights[lightId];
		if (!emitting || light.isInvalid || light.radius == 0 || light.position.tile != object.position)
			continue;
		objectLight[lightId] = true;
		owner[lightId] = MakeEntityId(HostEntityKind::Object, static_cast<uint32_t>(ActiveObjects[i]));
	}

	for (int i = 0; i < ActiveLightCount; ++i) {
		const uint8_t lightId = ActiveLights[i];
		const Light &light = ::devilution::Lights[lightId];
		if (light.isInvalid || light.radius == 0 || !InDungeonBounds(light.position.tile))
			continue;
		HostLight &out = FrameLights.emplace_back();
		out = {};
		out.x = static_cast<float>(light.position.tile.x) + static_cast<float>(light.position.offset.deltaX) / 8.0F + 0.5F;
		out.z = static_cast<float>(light.position.tile.y) + static_cast<float>(light.position.offset.deltaY) / 8.0F + 0.5F;
		out.y = render3d::FloorHeightOf(Level, out.x, out.z) + (missileOf[lightId] != nullptr ? 0.5F : 0.6F);
		out.radius = static_cast<float>(light.radius);
		LightColor(missileOf[lightId], objectLight[lightId], out.r, out.g, out.b);
		out.entityId = owner[lightId];
		out.flags = owner[lightId] != 0 ? 1 : 0;
	}
}

void UpdateLevel()
{
	render3d::RefreshArtPalette();
	render3d::UpdateLevelGeometry(Level);
	HostLevel &level = Frame.level;
	level.revision = Level.generation;
	level.levelType = static_cast<uint8_t>(leveltype);
	level.currentLevel = currlevel;
	level.setLevel = setlevel ? static_cast<uint8_t>(setlvlnum) : 0;
	level.vertices = Level.vertices.empty() ? nullptr : reinterpret_cast<const HostWorldVertex *>(Level.vertices.data());
	level.vertexCount = static_cast<uint32_t>(Level.vertices.size());
	level.indices = Level.indices.empty() ? nullptr : Level.indices.data();
	level.indexCount = static_cast<uint32_t>(Level.indices.size());
	level.atlasRgba = Level.atlasRgba.empty() ? nullptr : Level.atlasRgba.data();
	level.atlasWidth = Level.atlasWidth;
	level.atlasHeight = Level.atlasHeight;
}

} // namespace

const std::vector<HostEntity> &SnapshotEntities()
{
	return Entities;
}

const std::vector<EntitySpriteSource> &SnapshotSprites()
{
	return SpriteSources;
}

void ResetSnapshot()
{
	SoundsDelivered = false;
}

namespace {

/** HostSetWorldView: the host camera as UI-layer pixels from world points (column-major), when it sent one. */
std::optional<std::array<float, 16>> WorldView;

} // namespace

void HostSetWorldView(const float *screenFromWorld)
{
	if (screenFromWorld == nullptr) {
		WorldView = std::nullopt;
		return;
	}
	WorldView.emplace();
	std::copy_n(screenFromWorld, 16, WorldView->begin());
}

void QueueWorldItemLabels(int width, int height)
{
	if (!WorldView || !IsHighlightingLabelsEnabled())
		return;
	const std::array<float, 16> &m = *WorldView;
	for (uint8_t i = 0; i < ActiveItemCount; ++i) {
		const int id = ActiveItems[i];
		const Item &item = Items[id];
		// As the isometric pass: items it does not leave in the dark, once their drop animation has landed.
		if (!InDungeonBounds(item.position) || (leveltype != DTYPE_TOWN && !IsTileLit(item.position))
		    || !(item.AnimInfo.isLastFrame() || item._iCurs == ICURS_MAGIC_ROCK))
			continue;
		// The label sits half a tile above the item, where the classic one rises above its sprite.
		const float x = static_cast<float>(item.position.x) + 0.5F;
		const float z = static_cast<float>(item.position.y) + 0.5F;
		const float y = render3d::FloorHeightOf(Level, x, z) + 0.5F;
		const float w = m[3] * x + m[7] * y + m[11] * z + m[15];
		if (w <= 1e-4F)
			continue;
		const float sx = (m[0] * x + m[4] * y + m[8] * z + m[12]) / w;
		const float sy = (m[1] * x + m[5] * y + m[9] * z + m[13]) / w;
		if (!(sx >= 0 && sx < static_cast<float>(width) && sy >= 0 && sy < static_cast<float>(height)))
			continue;
		AddItemToLabelQueueAt(id, ScreenToUi({ static_cast<int>(sx), static_cast<int>(sy) }));
	}
}

const HostFrame &BuildSnapshot()
{
	Frame.apiVersion = kHostApiVersion;
	Frame.gameTick = TotalTicksRun();

	Entities.clear();
	FrameLights.clear();
	SpriteSources.clear();
	if (InGame()) {
		UpdateLevel();
		AddPlayers();
		AddMonsters();
		if (leveltype == DTYPE_TOWN)
			AddTowners();
		AddObjects();
		AddItems();
		AddMissiles();
		AddLights();

		const Player &player = *MyPlayer;
		const WorldPosition target = FeetOf(player.position, player.isWalking(), player._pdir, player.AnimInfo);
		Frame.camera.targetX = target.x;
		Frame.camera.targetY = render3d::FloorHeightOf(Level, target.x, target.z);
		Frame.camera.targetZ = target.z;
		Frame.camera.viewMode = static_cast<uint8_t>(*GetOptions().Graphics.cameraMode);
	}
	Frame.entities = Entities.empty() ? nullptr : Entities.data();
	Frame.entityCount = static_cast<uint32_t>(Entities.size());
	Frame.lights = FrameLights.empty() ? nullptr : FrameLights.data();
	Frame.lightCount = static_cast<uint32_t>(FrameLights.size());

	HostUiLayer &ui = Frame.ui;
	ui.revision = UiLayer.revision;
	ui.width = UiLayer.width;
	ui.height = UiLayer.height;
	ui.rgba = UiLayer.rgba.empty() ? nullptr : UiLayer.rgba.data();
	ui.dirtyX = UiLayer.dirtyX0;
	ui.dirtyY = UiLayer.dirtyY0;
	ui.dirtyW = UiLayer.dirtyX1 - UiLayer.dirtyX0;
	ui.dirtyH = UiLayer.dirtyY1 - UiLayer.dirtyY0;
	// The next snapshot reports only what changes after this one.
	UiLayer.dirtyX0 = UiLayer.dirtyY0 = UiLayer.dirtyX1 = UiLayer.dirtyY1 = 0;

	if (!SoundsDelivered) {
		TakeSounds(FrameSounds);
		SoundsDelivered = true;
		Frame.soundCount = static_cast<uint32_t>(FrameSounds.size());
	} else {
		Frame.soundCount = 0;
	}
	Frame.sounds = FrameSounds.empty() ? nullptr : FrameSounds.data();
	return Frame;
}

const char *HostPlayerEquipmentName(uint32_t entityId, uint32_t slot)
{
	static std::string name;
	name.clear();
	const uint32_t index = (entityId & 0xFFFFFF) - 1;
	if ((entityId >> 24) != static_cast<uint32_t>(HostEntityKind::Player) || index >= Players.size() || slot > 1 || !Players[index].plractive)
		return name.c_str();
	for (const inv_body_loc hand : { INVLOC_HAND_LEFT, INVLOC_HAND_RIGHT }) {
		const Item &item = Players[index].InvBody[hand];
		if (item.isEmpty() || (item._itype == ItemType::Shield) != (slot == 1)
		    || item.IDidx < 0 || static_cast<size_t>(item.IDidx) >= AllItemsList.size())
			continue;
		name = AllItemsList[static_cast<size_t>(item.IDidx)].iName;
		if (name == "Quarter Staff")
			name = "Short Staff";
		std::replace(name.begin(), name.end(), ' ', '_');
		std::replace(name.begin(), name.end(), '-', '_');
		break;
	}
	return name.c_str();
}

uint32_t HostReachableObject(uint32_t typeId)
{
	if (!InGame() || MyPlayer == nullptr || !MyPlayer->plractive)
		return 0;
	uint32_t nearest = 0;
	int shortest = static_cast<int>(MaxPathLengthPlayer) + 1;
	for (int active = 0; active < ActiveObjectCount; ++active) {
		const int index = ActiveObjects[active];
		const Object &object = Objects[index];
		if (!object.canInteractWith() || object.IsBroken())
			continue;
		// UINT32_MAX requests a closed door, useful when a scripted acceptance run needs to open a route.
		if (typeId == UINT32_MAX ? !object.isDoor() || object._oVar4 != 0 : static_cast<uint32_t>(object._otype) != typeId)
			continue;
		int8_t path[MaxPathLengthPlayer];
		const int length = FindPath(CanStep, [](Point tile) { return PosOkPlayer(*MyPlayer, tile); },
		    MyPlayer->position.future, object.position, path, MaxPathLengthPlayer);
		if (length > 0 && length < shortest) {
			shortest = length;
			nearest = MakeEntityId(HostEntityKind::Object, static_cast<uint32_t>(index));
		}
	}
	return nearest;
}

} // namespace devilution::host
