/**
 * @file render3d/world_sprites.cpp
 *
 * Per-frame billboards of the 3D world (see world_sprites.hpp).
 */
#include "render3d/world_sprites.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "dead.h"
#include "engine/clx_sprite.hpp"
#include "engine/freeform_movement.hpp"
#include "engine/trn.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "render3d/fpv_contract.hpp"
#include "render3d/world_renderer.hpp"
#include "towners.h"
#include "utils/clx_decode.hpp"

namespace devilution::render3d {

namespace {

constexpr float kPixel = 1.0F / kSpritePixelsPerUnit;
constexpr float kMaxDistance = 28.0F;

// ---- Sprite atlas cache ----------------------------------------------------------------------------------

struct SpriteKey {
	const uint8_t *pixels;
	const uint8_t *trn;
	bool operator==(const SpriteKey &o) const { return pixels == o.pixels && trn == o.trn; }
};

struct SpriteKeyHash {
	size_t operator()(const SpriteKey &k) const noexcept
	{
		uint64_t h = reinterpret_cast<uintptr_t>(k.pixels) * 0x9E3779B97F4A7C15ULL;
		h ^= reinterpret_cast<uintptr_t>(k.trn) + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
		return static_cast<size_t>(h);
	}
};

/** Rect of a cached sprite including its 1 px transparent border. */
struct SpriteRect {
	int x = 0;
	int y = 0;
	int width = 0;  // padded
	int height = 0; // padded
};

struct SpriteCache {
	std::unordered_map<SpriteKey, SpriteRect, SpriteKeyHash> rects;
	int shelfX = 0;
	int shelfY = 0;
	int shelfHeight = 0;
	uint64_t paletteHash = 0;

	void Clear()
	{
		rects.clear();
		shelfX = shelfY = shelfHeight = 0;
	}

	bool Allocate(int width, int height, SpriteRect &rect)
	{
		if (width > kSpriteAtlasSize || height > kSpriteAtlasSize)
			return false;
		if (shelfX + width > kSpriteAtlasSize) {
			shelfX = 0;
			shelfY += shelfHeight;
			shelfHeight = 0;
		}
		if (shelfY + height > kSpriteAtlasSize)
			return false;
		rect = { shelfX, shelfY, width, height };
		shelfX += width;
		shelfHeight = std::max(shelfHeight, height);
		return true;
	}
};

SpriteCache Cache;

void DecodeClx(ClxSprite sprite, const uint8_t *trn, std::vector<uint8_t> &rgba, int paddedWidth)
{
	const auto &palette = CurrentArtPalette();
	const uint32_t w = sprite.width();
	const uint32_t h = sprite.height();
	const uint8_t *src = sprite.pixelData();
	const uint8_t *const end = src + sprite.pixelDataSize();
	uint32_t x = 0;
	uint32_t y = 0; // from the bottom
	const auto put = [&](uint32_t px, uint8_t index) {
		if (px >= w || y >= h)
			return;
		if (trn != nullptr)
			index = trn[index];
		if (index == 0)
			return; // shadow / transparent
		const auto &c = palette[index];
		const size_t o = (static_cast<size_t>(h - y) * paddedWidth + px + 1) * 4; // row h - y: 1 px top border, bottom-up rows
		rgba[o + 0] = c[0];
		rgba[o + 1] = c[1];
		rgba[o + 2] = c[2];
		rgba[o + 3] = 255;
	};
	while (src < end && y < h) {
		const uint8_t control = *src++;
		if (!IsClxOpaque(control)) {
			x += control;
			while (x >= w && w != 0) {
				x -= w;
				++y;
			}
			continue;
		}
		if (IsClxOpaqueFill(control)) {
			const uint32_t n = GetClxOpaqueFillWidth(control);
			if (src >= end)
				break;
			const uint8_t color = *src++;
			for (uint32_t k = 0; k < n; ++k)
				put(x + k, color);
			x += n;
		} else {
			const uint32_t n = GetClxOpaquePixelsWidth(control);
			const auto avail = static_cast<uint32_t>(std::min<ptrdiff_t>(n, end - src));
			for (uint32_t k = 0; k < avail; ++k)
				put(x + k, src[k]);
			src += avail;
			x += n;
		}
		while (x >= w && w != 0) {
			x -= w;
			++y;
		}
	}
}

/** Returns the atlas rect of a sprite (uploading it when new) or nullopt when the atlas is full this frame. */
std::optional<SpriteRect> AcquireSprite(ClxSprite sprite, const uint8_t *trn, BillboardFrame &frame)
{
	const SpriteKey key { sprite.pixelData(), trn };
	if (const auto it = Cache.rects.find(key); it != Cache.rects.end())
		return it->second;
	const int paddedWidth = sprite.width() + 2;
	const int paddedHeight = sprite.height() + 2;
	SpriteRect rect;
	if (!Cache.Allocate(paddedWidth, paddedHeight, rect))
		return std::nullopt;
	SpriteUpload upload;
	upload.x = rect.x;
	upload.y = rect.y;
	upload.width = paddedWidth;
	upload.height = paddedHeight;
	upload.rgba.assign(static_cast<size_t>(paddedWidth) * paddedHeight * 4, 0);
	DecodeClx(sprite, trn, upload.rgba, paddedWidth);
	frame.uploads.push_back(std::move(upload));
	Cache.rects.emplace(key, rect);
	return rect;
}

// ---- Geometry helpers ------------------------------------------------------------------------------------

struct Basis {
	Vec3 right;   // screen right in the xz plane
	Vec3 forward; // camera forward in the xz plane
};

void PushQuad(BillboardFrame &frame, const Vec3 (&p)[4], const SpriteRect &rect, const Vec3 &normal, uint32_t flags)
{
	const float inv = 1.0F / static_cast<float>(kSpriteAtlasSize);
	const float u0 = static_cast<float>(rect.x) * inv;
	const float v0 = static_cast<float>(rect.y) * inv;
	const float u1 = static_cast<float>(rect.x + rect.width) * inv;
	const float v1 = static_cast<float>(rect.y + rect.height) * inv;
	const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
	const auto base = static_cast<uint32_t>(frame.vertices.size());
	for (int i = 0; i < 4; ++i)
		frame.vertices.push_back({ p[i].x, p[i].y, p[i].z, uv[i][0], uv[i][1], normal.x, normal.y, normal.z, flags });
	frame.indices.insert(frame.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

/**
 * Upright camera-facing quad for a classic sprite whose bottom row is drawn at the tile's bottom vertex (16 px below
 * the tile centre, where `feet` is), horizontally centred.
 */
void AddUpright(BillboardFrame &frame, const Basis &basis, const Vec3 &feet, ClxSprite sprite, const uint8_t *trn, uint32_t flags,
    float lift = 0.0F)
{
	const std::optional<SpriteRect> rect = AcquireSprite(sprite, trn, frame);
	if (!rect)
		return;
	const float halfWidth = static_cast<float>(rect->width) * 0.5F * kPixel;
	const float bottom = feet.y + lift - (16.0F + 1.0F) * kPixel;
	const float top = bottom + static_cast<float>(rect->height) * kPixel;
	const Vec3 centre { feet.x, 0.0F, feet.z };
	const Vec3 l = centre - basis.right * halfWidth;
	const Vec3 r = centre + basis.right * halfWidth;
	const Vec3 quad[4] = {
		{ l.x, top, l.z },
		{ r.x, top, r.z },
		{ r.x, bottom, r.z },
		{ l.x, bottom, l.z },
	};
	PushQuad(frame, quad, *rect, -basis.forward, PackVertexFlags(flags | kVertexSpriteAtlas | kVertexBillboard));
}

/** Corpse/blood lying on the floor: the iso art unsquashed onto the ground, its top row away from the camera. */
void AddFlat(BillboardFrame &frame, const Basis &basis, const Vec3 &feet, ClxSprite sprite, const uint8_t *trn)
{
	const std::optional<SpriteRect> rect = AcquireSprite(sprite, trn, frame);
	if (!rect)
		return;
	constexpr float Unsquash = 1.6F;
	const float halfWidth = static_cast<float>(rect->width) * 0.5F * kPixel;
	const float nearDepth = 17.0F * kPixel * Unsquash;
	const float farDepth = (static_cast<float>(rect->height) - 17.0F) * kPixel * Unsquash;
	const Vec3 centre { feet.x, 0.012F, feet.z };
	const Vec3 nearC = centre - basis.forward * nearDepth;
	const Vec3 farC = centre + basis.forward * farDepth;
	const Vec3 quad[4] = {
		farC - basis.right * halfWidth,
		farC + basis.right * halfWidth,
		nearC + basis.right * halfWidth,
		nearC - basis.right * halfWidth,
	};
	PushQuad(frame, quad, *rect, { 0.0F, 1.0F, 0.0F }, PackVertexFlags(kVertexSpriteAtlas));
}

/** Box with the sprite on its four sides and its top band on the lid (sarcophagi, chests). */
void AddSpriteBox(BillboardFrame &frame, const Vec3 &centre, float halfX, float halfZ, float height, ClxSprite sprite, uint32_t flags)
{
	const std::optional<SpriteRect> rect = AcquireSprite(sprite, nullptr, frame);
	if (!rect)
		return;
	// Use the lower middle part of the sprite (the body of the object) for the sides.
	SpriteRect side = *rect;
	side.y += rect->height / 3;
	side.height = rect->height - rect->height / 3;
	SpriteRect lid = *rect;
	lid.y += rect->height / 6;
	lid.height = std::max(2, rect->height / 3);
	const float x0 = centre.x - halfX;
	const float x1 = centre.x + halfX;
	const float z0 = centre.z - halfZ;
	const float z1 = centre.z + halfZ;
	const uint32_t packed = PackVertexFlags(flags | kVertexSpriteAtlas);
	const Vec3 faces[4][4] = {
		{ { x0, height, z1 }, { x1, height, z1 }, { x1, 0, z1 }, { x0, 0, z1 } },
		{ { x1, height, z0 }, { x0, height, z0 }, { x0, 0, z0 }, { x1, 0, z0 } },
		{ { x1, height, z1 }, { x1, height, z0 }, { x1, 0, z0 }, { x1, 0, z1 } },
		{ { x0, height, z0 }, { x0, height, z1 }, { x0, 0, z1 }, { x0, 0, z0 } },
	};
	const Vec3 normals[4] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
	for (int i = 0; i < 4; ++i)
		PushQuad(frame, faces[i], side, normals[i], packed);
	const Vec3 top[4] = { { x0, height, z0 }, { x1, height, z0 }, { x1, height, z1 }, { x0, height, z1 } };
	PushQuad(frame, top, lid, { 0, 1, 0 }, packed);
}

// ---- Directions --------------------------------------------------------------------------------------------

/** Direction index (South = 0 ... SouthEast = 7) of a tile-space vector. */
int DirectionIndexOf(float dx, float dz)
{
	const float yaw = std::atan2(dx, dz); // South (1, 1) -> 45 deg, each index step -45 deg
	const float steps = (0.78539816F - yaw) / 0.78539816F;
	int index = static_cast<int>(std::lround(steps));
	index %= 8;
	if (index < 0)
		index += 8;
	return index;
}

/** The classic sprite direction that shows an actor facing `facing` to a camera at `camera`. */
Direction ViewDirection(Direction facing, const Vec3 &actor, const Vec3 &camera)
{
	if (facing == Direction::NoDirection)
		return facing;
	const int view = DirectionIndexOf(camera.x - actor.x, camera.z - actor.z);
	const int index = ((static_cast<int>(facing) - view) % 8 + 8) % 8;
	return static_cast<Direction>(index);
}

Vec3 ActorFeet(const ActorPosition &position, bool walking, Direction dir, const AnimationInfo &anim)
{
	const SubTileDisplacement offset = position.GetDrawnSubTileOffset(walking, dir, anim);
	return {
		static_cast<float>(position.tile.x) + 0.5F + static_cast<float>(offset.deltaX) / static_cast<float>(SubTileUnits),
		0.0F,
		static_cast<float>(position.tile.y) + 0.5F + static_cast<float>(offset.deltaY) / static_cast<float>(SubTileUnits),
	};
}

bool InRange(const Vec3 &p, const CameraView &camera)
{
	const float dx = p.x - camera.position.x;
	const float dz = p.z - camera.position.z;
	return dx * dx + dz * dz < kMaxDistance * kMaxDistance;
}

uint32_t OutlineFlag(fpv::TargetKind kind, int id)
{
	const fpv::CrosshairTarget &target = fpv::GetCrosshairTarget();
	return target.kind == kind && target.id == id && target.inRange ? kVertexOutline : 0U;
}

void AddMonsters(const CameraView &camera, const Basis &basis, BillboardFrame &frame)
{
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const int id = static_cast<int>(ActiveMonsters[i]);
		const Monster &monster = Monsters[id];
		if ((monster.flags & MFLAG_HIDDEN) != 0 || !monster.animInfo.sprites)
			continue;
		if (!InDungeonBounds(monster.position.tile) || !IsTileLit(monster.position.tile))
			continue;
		const Vec3 feet = ActorFeet(monster.position, monster.isWalking(), monster.direction, monster.animInfo);
		if (!InRange(feet, camera))
			continue;
		ClxSprite sprite = monster.animInfo.currentSprite();
		// Re-pick the direction for the camera: find which animation/direction list the monster is playing.
		const uint8_t *current = monster.animInfo.sprites->data();
		const int frameIndex = monster.animInfo.getFrameToUseForRendering();
		bool found = false;
		for (int g = 0; g < 6 && !found; ++g) {
			const AnimStruct &anim = monster.type().getAnimData(static_cast<MonsterGraphic>(g));
			if (!anim.sprites)
				continue;
			for (int d = 0; d < 8 && !found; ++d) {
				const OptionalClxSpriteList list = anim.spritesForDirection(static_cast<Direction>(d));
				if (!list || list->data() != current)
					continue;
				found = true;
				const Direction viewDir = ViewDirection(static_cast<Direction>(d), feet, camera.position);
				const OptionalClxSpriteList viewList = anim.spritesForDirection(viewDir);
				if (viewList && frameIndex >= 0 && static_cast<uint32_t>(frameIndex) < viewList->numSprites())
					sprite = (*viewList)[frameIndex];
			}
		}
		const uint8_t *trn = nullptr;
		if (monster.isUnique())
			trn = monster.uniqueMonsterTRN.get();
		if (monster.mode == MonsterMode::Petrified)
			trn = GetStoneTRN();
		AddUpright(frame, basis, feet, sprite, trn, OutlineFlag(fpv::TargetKind::Monster, id));
	}
}

void AddTowners(const CameraView &camera, const Basis &basis, BillboardFrame &frame)
{
	for (size_t i = 0; i < Towners.size(); ++i) {
		const Towner &towner = Towners[i];
		if (!towner.anim)
			continue;
		const Vec3 feet { static_cast<float>(towner.position.x) + 0.5F, 0.0F, static_cast<float>(towner.position.y) + 0.5F };
		if (!InRange(feet, camera) || towner._tAnimFrame >= towner.anim->numSprites())
			continue;
		AddUpright(frame, basis, feet, towner.currentSprite(), nullptr, OutlineFlag(fpv::TargetKind::Towner, static_cast<int>(i)));
	}
}

void AddPlayers(const CameraView &camera, const Basis &basis, bool (*skipPlayer)(int), BillboardFrame &frame)
{
	for (size_t i = 0; i < Players.size(); ++i) {
		const Player &player = Players[i];
		if (!player.plractive || !player.isOnActiveLevel() || &player == MyPlayer || !player.AnimInfo.sprites)
			continue;
		if (skipPlayer != nullptr && skipPlayer(static_cast<int>(i)))
			continue;
		if (leveltype != DTYPE_TOWN && !IsTileLit(player.position.tile))
			continue;
		const Vec3 feet = ActorFeet(player.position, player.isWalking(), player._pdir, player.AnimInfo);
		if (!InRange(feet, camera))
			continue;
		ClxSprite sprite = player.currentSprite();
		const uint8_t *current = player.AnimInfo.sprites->data();
		const int frameIndex = player.AnimInfo.getFrameToUseForRendering();
		bool found = false;
		for (size_t g = 0; g < player.AnimationData.size() && !found; ++g) {
			const PlayerAnimationData &data = player.AnimationData[g];
			if (!data.sprites)
				continue;
			for (int d = 0; d < 8 && !found; ++d) {
				const ClxSpriteList list = data.spritesForDirection(static_cast<Direction>(d));
				if (list.data() != current)
					continue;
				found = true;
				const ClxSpriteList viewList = data.spritesForDirection(ViewDirection(static_cast<Direction>(d), feet, camera.position));
				if (frameIndex >= 0 && static_cast<uint32_t>(frameIndex) < viewList.numSprites())
					sprite = viewList[frameIndex];
			}
		}
		AddUpright(frame, basis, feet, sprite, nullptr, OutlineFlag(fpv::TargetKind::Player, static_cast<int>(i)));
	}
}

void AddItems(const CameraView &camera, const Basis &basis, BillboardFrame &frame)
{
	for (int i = 0; i < ActiveItemCount; ++i) {
		const int id = ActiveItems[i];
		const Item &item = Items[id];
		if (!item.AnimInfo.sprites || !InDungeonBounds(item.position))
			continue;
		const Vec3 feet { static_cast<float>(item.position.x) + 0.5F, 0.0F, static_cast<float>(item.position.y) + 0.5F };
		if (!InRange(feet, camera))
			continue;
		AddUpright(frame, basis, feet, item.AnimInfo.currentSprite(), nullptr, OutlineFlag(fpv::TargetKind::Item, id));
	}
}

bool IsBoxObject(_object_id type)
{
	return IsAnyOf(type, OBJ_SARC, OBJ_L5SARC, OBJ_CHEST1, OBJ_CHEST2, OBJ_CHEST3, OBJ_TCHEST1, OBJ_TCHEST2, OBJ_TCHEST3);
}

void AddObjects(const CameraView &camera, const Basis &basis, const LevelGeometry &level, BillboardFrame &frame)
{
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		if (object.isDoor() || !object._oAnimData || object._oAnimFrame == 0 || object._oAnimFrame > object._oAnimData->numSprites())
			continue;
		if (!InDungeonBounds(object.position))
			continue;
		Vec3 feet { static_cast<float>(object.position.x) + 0.5F, 0.0F, static_cast<float>(object.position.y) + 0.5F };
		if (!InRange(feet, camera))
			continue;
		const ClxSprite sprite = object.currentSprite();
		const uint32_t flags = (object.applyLighting ? 0U : kVertexUnlit) | OutlineFlag(fpv::TargetKind::Object, id);
		if (IsBoxObject(object._otype)) {
			const bool sarcophagus = IsAnyOf(object._otype, OBJ_SARC, OBJ_L5SARC);
			// Same boxes as the fpv crosshair picking (chest 0.6 x 0.45 x 0.6, sarcophagus 0.9 x 0.55 x 0.9).
			AddSpriteBox(frame, feet, sarcophagus ? 0.45F : 0.3F, sarcophagus ? 0.45F : 0.3F, sarcophagus ? 0.55F : 0.45F, sprite, flags);
			continue;
		}
		// Objects standing in a wall cell (torches, wall decorations) are pulled out toward the open neighbour.
		if (OccluderTopAt(level, object.position.x, object.position.y) > 0.5F) {
			for (const auto &[dx, dy] : { std::pair { 1, 0 }, std::pair { 0, 1 }, std::pair { -1, 0 }, std::pair { 0, -1 } }) {
				if (OccluderTopAt(level, object.position.x + dx, object.position.y + dy) <= 0.0F) {
					feet.x += 0.56F * static_cast<float>(dx);
					feet.z += 0.56F * static_cast<float>(dy);
					break;
				}
			}
		}
		AddUpright(frame, basis, feet, sprite, nullptr, flags);
	}
}

/** Additive glow colour of a missile (fire and lightning family), or zero for none. */
Vec3 MissileGlowColor(const Missile &missile)
{
	switch (GetMissileData(missile._mitype).damageType()) {
	case DamageType::Fire:
		return { 0.55F, 0.22F, 0.05F };
	case DamageType::Lightning:
		return { 0.18F, 0.32F, 0.6F };
	case DamageType::Magic:
		return { 0.3F, 0.16F, 0.45F };
	default:
		return { 0.0F, 0.0F, 0.0F };
	}
}

/** Camera-facing additive glow quad (uv 0..1 over the quad, colour in the normal; see glowFragment). */
void AddGlow(BillboardFrame &frame, const Basis &basis, const Vec3 &centre, float radius, const Vec3 &color)
{
	const Vec3 up { 0.0F, radius, 0.0F };
	const Vec3 right = basis.right * radius;
	const Vec3 p[4] = { centre - right + up, centre + right + up, centre + right - up, centre - right - up };
	const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	const auto base = static_cast<uint32_t>(frame.glowVertices.size());
	for (int i = 0; i < 4; ++i)
		frame.glowVertices.push_back({ p[i].x, p[i].y, p[i].z, uv[i][0], uv[i][1], color.x, color.y, color.z, 0 });
	frame.glowIndices.insert(frame.glowIndices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

void AddMissiles(const CameraView &camera, const Basis &basis, BillboardFrame &frame)
{
	for (const Missile &missile : Missiles) {
		if (!missile._miDrawFlag || !missile._miAnimData || missile._miAnimFrame <= 0
		    || static_cast<uint32_t>(missile._miAnimFrame) > missile._miAnimData->numSprites())
			continue;
		// The render position scrollrt interpolated between the 20 Hz game ticks this frame (UpdateMissilesRendererData),
		// as a pixel offset from the tile's draw position (tile x + 1 = 32 px right, 16 down).
		const Point tile = missile.position.tileForRendering;
		const float sx = static_cast<float>(missile.position.offsetForRendering.deltaX);
		const float sy = static_cast<float>(missile.position.offsetForRendering.deltaY);
		const Vec3 feet {
			static_cast<float>(tile.x) + 0.5F + (sx / 32.0F + sy / 16.0F) * 0.5F,
			0.0F,
			static_cast<float>(tile.y) + 0.5F + (sy / 16.0F - sx / 32.0F) * 0.5F,
		};
		if (!InRange(feet, camera))
			continue;
		const ClxSprite sprite = (*missile._miAnimData)[missile._miAnimFrame - 1];
		const uint8_t *trn = missile._miUniqTrans != 0 && missile._misource >= 0 ? Monsters[missile._misource].uniqueMonsterTRN.get() : nullptr;
		// Flying projectiles travel at chest height (their iso art is drawn on the ground line).
		const bool moving = missile.position.velocity.deltaX != 0 || missile.position.velocity.deltaY != 0;
		const float centre = (static_cast<float>(sprite.height()) * 0.5F - 17.0F) * kPixel;
		float lift = moving ? 0.42F - centre : 0.0F;
		Vec3 at = feet;
		// The local player's projectiles leave from the off-hand / crosshair area in front of the camera and blend into
		// their simulated path over the first two tiles.
		if (moving && missile._micaster == TARGET_MONSTERS && missile._misource == static_cast<int>(MyPlayerId)) {
			const float dx = feet.x - camera.position.x;
			const float dz = feet.z - camera.position.z;
			const float t = std::clamp(std::sqrt(dx * dx + dz * dz) / 2.0F, 0.0F, 1.0F);
			const Vec3 hand = camera.position + basis.forward * 0.45F - basis.right * 0.2F;
			at = { hand.x + (feet.x - hand.x) * t, 0.0F, hand.z + (feet.z - hand.z) * t };
			const float handLift = fpv::kEyeHeight - 0.16F - centre;
			lift = handLift + (lift - handLift) * t;
		}
		AddUpright(frame, basis, at, sprite, trn, missile._miLightFlag ? 0U : kVertexUnlit, lift);
		const Vec3 glow = MissileGlowColor(missile);
		if (glow.x + glow.y + glow.z > 0.0F && !missile._miLightFlag) {
			// The sprite's centre: its quad spans lift + (-17 px .. height - 17 px) above the feet.
			const float size = std::clamp(static_cast<float>(sprite.width()) * kPixel * 0.9F, 0.35F, 1.1F);
			AddGlow(frame, basis, { at.x, std::max(0.1F, lift + centre), at.z }, size, glow);
		}
	}
}

void AddCorpses(const CameraView &camera, const Basis &basis, BillboardFrame &frame)
{
	const int cx = static_cast<int>(camera.position.x);
	const int cz = static_cast<int>(camera.position.z);
	const int range = static_cast<int>(kMaxDistance);
	for (int y = std::max(0, cz - range); y < std::min(MAXDUNY, cz + range); ++y) {
		for (int x = std::max(0, cx - range); x < std::min(MAXDUNX, cx + range); ++x) {
			const int8_t corpseValue = dCorpse[x][y];
			if (corpseValue == 0)
				continue;
			const int index = (corpseValue & 0x1F) - 1;
			if (index < 0 || index >= MaxCorpses)
				continue;
			const Corpse &corpse = Corpses[index];
			if (!corpse.sprites)
				continue;
			const ClxSpriteList list = corpse.spritesForDirection(static_cast<Direction>((corpseValue >> 5) & 7));
			if (corpse.frame < 0 || static_cast<uint32_t>(corpse.frame) >= list.numSprites())
				continue;
			const uint8_t *trn = corpse.translationPaletteIndex != 0 ? Monsters[corpse.translationPaletteIndex - 1].uniqueMonsterTRN.get() : nullptr;
			AddFlat(frame, basis, { static_cast<float>(x) + 0.5F, 0.0F, static_cast<float>(y) + 0.5F }, list[corpse.frame], trn);
		}
	}
}

} // namespace

void ResetSpriteCache()
{
	Cache.Clear();
}

void BuildBillboards(const CameraView &camera, const LevelGeometry &level, bool (*skipPlayer)(int), BillboardFrame &out)
{
	out.vertices.clear();
	out.indices.clear();
	out.uploads.clear();
	out.glowVertices.clear();
	out.glowIndices.clear();
	if (Cache.paletteHash != CurrentArtPaletteHash() && leveltype != DTYPE_CAVES && leveltype != DTYPE_HELL
	    && leveltype != DTYPE_NEST && leveltype != DTYPE_CRYPT) {
		Cache.Clear();
		Cache.paletteHash = CurrentArtPaletteHash();
	}
	const Vec3 forward { std::sin(camera.yaw), 0.0F, std::cos(camera.yaw) };
	const Basis basis { { -forward.z, 0.0F, forward.x }, forward };

	// Two attempts: when the atlas fills up mid-frame, start it over and rebuild every billboard once.
	for (int attempt = 0; attempt < 2; ++attempt) {
		const size_t before = Cache.rects.size();
		AddCorpses(camera, basis, out);
		if (leveltype == DTYPE_TOWN)
			AddTowners(camera, basis, out);
		else
			AddMonsters(camera, basis, out);
		AddPlayers(camera, basis, skipPlayer, out);
		AddObjects(camera, basis, level, out);
		AddItems(camera, basis, out);
		AddMissiles(camera, basis, out);
		const bool full = Cache.shelfY + Cache.shelfHeight >= kSpriteAtlasSize - 256;
		(void)before;
		if (!full || attempt == 1)
			break;
		Cache.Clear();
		out.vertices.clear();
		out.indices.clear();
		out.uploads.clear();
		out.glowVertices.clear();
		out.glowIndices.clear();
	}
}

} // namespace devilution::render3d
