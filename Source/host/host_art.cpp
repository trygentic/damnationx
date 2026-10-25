/**
 * @file host/host_art.cpp
 *
 * HostEntityArtSnapshot (contract v4): the classic sprite frame of every entity, decoded with the level palette and
 * its TRN into one RGBA atlas (the same decoding as the game's own 3D billboards, render3d/world_sprites.cpp), so a
 * host can draw entities it has no model for as billboards of the real art; and the clip choice for the pack's
 * skinned characters (the rules of render3d/skinned_renderer.cpp, without loading the model).
 *
 * HostEntityHdArtSnapshot: where the HD pack has 2D art for an entity (towners, monsters, player bodies), the clip frame
 * the Hd2D view would draw instead (engine/render/hd_sprites.hpp HdSpritePose), so the host's billboards can show it.
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <unordered_map>

#include "engine/palette.h"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_sprites_monsters.hpp"
#include "levels/gendung.h"
#include "nthread.h"
#include "monster.h"
#include "player.h"
#include "towners.h"
#include "utils/clx_decode.hpp"
#include "utils/str_cat.hpp"

namespace devilution::host {

namespace {

constexpr int AtlasSize = 2048;

struct SpriteKey {
	const uint8_t *data;
	const uint8_t *trn;
	bool operator==(const SpriteKey &) const = default;
};

struct SpriteKeyHash {
	size_t operator()(const SpriteKey &key) const
	{
		return std::hash<const void *>()(key.data) ^ (std::hash<const void *>()(key.trn) * 31);
	}
};

struct Rect {
	int x, y, w, h;
};

struct Atlas {
	std::vector<uint8_t> rgba;
	std::unordered_map<SpriteKey, Rect, SpriteKeyHash> rects;
	int shelfX = 0, shelfY = 0, shelfHeight = 0;
	uint64_t paletteHash = 0;
	uint32_t revision = 0;
	// Changed since the last snapshot (x0, y0 inclusive, x1, y1 exclusive).
	int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;

	void markDirty(int x, int y, int w, int h)
	{
		if (dirtyX1 == 0) {
			dirtyX0 = x, dirtyY0 = y, dirtyX1 = x + w, dirtyY1 = y + h;
		} else {
			dirtyX0 = std::min(dirtyX0, x), dirtyY0 = std::min(dirtyY0, y);
			dirtyX1 = std::max(dirtyX1, x + w), dirtyY1 = std::max(dirtyY1, y + h);
		}
		++revision;
	}

	void clear()
	{
		rgba.assign(static_cast<size_t>(AtlasSize) * AtlasSize * 4, 0);
		rects.clear();
		shelfX = shelfY = shelfHeight = 0;
		markDirty(0, 0, AtlasSize, AtlasSize);
	}

	bool allocate(int w, int h, Rect &rect)
	{
		if (w > AtlasSize || h > AtlasSize)
			return false;
		if (shelfX + w > AtlasSize) {
			shelfX = 0;
			shelfY += shelfHeight;
			shelfHeight = 0;
		}
		if (shelfY + h > AtlasSize)
			return false;
		rect = { shelfX, shelfY, w, h };
		shelfX += w;
		shelfHeight = std::max(shelfHeight, h);
		return true;
	}
};

Atlas TheAtlas;
std::vector<HostEntitySprite> Sprites;
std::vector<HostModelClip> Clips;
std::deque<std::string> ClipNames;
HostEntityArt Art {};
std::vector<HostEntityHdSprite> HdSprites;
HostEntityHdArt HdArt {};
/** Sheet file of each clip (index of hd/sprites.txt), resolved once; the strings stay put for the run. */
std::unordered_map<int, std::string> HdSheetPaths;

uint64_t LogicalPaletteHash()
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	for (const SDL_Color &c : logical_palette)
		hash = (hash ^ (static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16)) * 0x100000001B3ULL;
	return hash;
}

/** Decodes a CLX frame into the atlas at `rect` (1 px transparent border), bottom-up rows as CLX stores them. */
void Decode(ClxSprite sprite, const uint8_t *trn, const Rect &rect)
{
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
			return; // transparent (shadow index)
		const SDL_Color &c = logical_palette[index];
		const size_t o = ((static_cast<size_t>(rect.y) + h - y) * AtlasSize + rect.x + px + 1) * 4;
		TheAtlas.rgba[o + 0] = c.r;
		TheAtlas.rgba[o + 1] = c.g;
		TheAtlas.rgba[o + 2] = c.b;
		TheAtlas.rgba[o + 3] = 255;
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

/** The atlas rectangle of a frame, decoding it when new; false when the atlas is full even after clearing it. */
bool Acquire(ClxSprite sprite, const uint8_t *trn, Rect &rect)
{
	const SpriteKey key { sprite.pixelData(), trn };
	if (const auto it = TheAtlas.rects.find(key); it != TheAtlas.rects.end()) {
		rect = it->second;
		return true;
	}
	const int w = sprite.width() + 2;
	const int h = sprite.height() + 2;
	if (!TheAtlas.allocate(w, h, rect)) {
		TheAtlas.clear(); // full: start over (a full upload for the host)
		if (!TheAtlas.allocate(w, h, rect))
			return false;
	}
	Decode(sprite, trn, rect);
	TheAtlas.rects.emplace(key, rect);
	TheAtlas.markDirty(rect.x, rect.y, rect.w, rect.h);
	return true;
}

// ---- skinned character clips (render3d/skinned_renderer.cpp's choice) ----

constexpr std::array<std::string_view, 9> FamilyNames { "unarmed", "unarmed_shield", "sword", "sword_shield", "bow", "axe", "mace", "mace_shield", "staff" };
constexpr std::array<uint8_t, 9> FamilyWithoutShield { 0, 0, 2, 2, 4, 5, 6, 6, 8 };

float AnimationProgress(const AnimationInfo &anim)
{
	if (anim.distributesFrames())
		return static_cast<float>(anim.getAnimationProgress()) / AnimationInfo::baseValueFraction;
	const int frames = std::max<int>(anim.numberOfFrames, 1);
	const int ticksPerFrame = std::max<int>(anim.ticksPerFrame, 1);
	const float subtick = anim.isPetrified ? 0.0F : static_cast<float>(ProgressToNextGameTick) / AnimationInfo::baseValueFraction;
	const float position = static_cast<float>(std::max<int>(anim.currentFrame, 0))
	    + (static_cast<float>(std::max<int>(anim.tickCounterOfCurrentFrame, 0)) + std::clamp(subtick, 0.0F, 1.0F)) / ticksPerFrame;
	return std::clamp(position / frames, 0.0F, 1.0F);
}

/** `<action>_<family>`, `<action>_<family without shield>`, `<action>` for each action in `actions`. */
std::string ClipCandidates(std::initializer_list<std::string_view> actions, size_t family)
{
	std::string out;
	const auto add = [&](std::string_view name) {
		if (!out.empty())
			out += ';';
		out += name;
	};
	for (const std::string_view action : actions) {
		add(StrCat(action, "_", FamilyNames[family]));
		if (const size_t base = FamilyWithoutShield[family]; base != family)
			add(StrCat(action, "_", FamilyNames[base]));
		add(action);
	}
	return out;
}

void AddClip(const HostEntity &entity, const Player &player)
{
	const bool inTown = leveltype == DTYPE_TOWN;
	const size_t family = std::min<size_t>(player._pgfxnum & 0xF, FamilyNames.size() - 1);
	HostModelClip clip {};
	clip.entityId = entity.id;
	clip.progress = AnimationProgress(player.AnimInfo);
	clip.hitProgress = -1.0F;
	std::string names;
	switch (player.getGraphic()) {
	case player_graphic::Stand:
		names = inTown ? ClipCandidates({ "town_stand", "stand" }, family) : ClipCandidates({ "stand" }, family);
		clip.loop = 1;
		break;
	case player_graphic::Walk:
		names = inTown ? ClipCandidates({ "town_walk", "walk" }, family) : ClipCandidates({ "walk" }, family);
		clip.loop = 1;
		break;
	case player_graphic::Attack: {
		names = ClipCandidates({ "attack" }, family);
		const float frames = static_cast<float>(std::max<int>(player.AnimInfo.numberOfFrames, 2));
		clip.hitProgress = std::clamp(static_cast<float>(player._pAFNum - 1), 1.0F, frames - 1.0F) / frames;
		break;
	}
	case player_graphic::Block:
		names = ClipCandidates({ "block", "stand" }, family);
		break;
	case player_graphic::Hit:
		names = ClipCandidates({ "hit", "stand" }, family);
		break;
	case player_graphic::Lightning:
	case player_graphic::Fire:
	case player_graphic::Magic:
		names = ClipCandidates({ "cast", "stand" }, family);
		break;
	case player_graphic::Death:
		names = ClipCandidates({ "death" }, family);
		break;
	}
	ClipNames.push_back(std::move(names));
	clip.clips = ClipNames.back().c_str();
	Clips.push_back(clip);
}

/** The HD clip frame the Hd2D view draws for `entity`, if the pack has one. */
bool HdPoseOf(const HostEntity &entity, HdSpritePose &pose)
{
	const uint32_t index = (entity.id & 0xFFFFFF) - 1;
	switch (entity.kind) {
	case HostEntityKind::Towner:
		return index < Towners.size() && HdSpritesTownerPose(Towners[index], pose);
	case HostEntityKind::Monster:
		return index < MaxMonsters && HdSpritesMonsterPose(Monsters[index], pose);
	case HostEntityKind::Player:
		return index < Players.size() && HdSpritesPlayerPose(Players[index], pose);
	default:
		return false;
	}
}

} // namespace

const HostEntityHdArt &BuildEntityHdArt(bool inGame)
{
	HdSprites.clear();
	if (inGame && (InstalledPacks() & kPackHd2D) != 0 && HdGraphicsEnabled()) {
		const hd_sprites::SpriteIndex &index = HdSpritesIndex();
		for (const HostEntity &entity : SnapshotEntities()) {
			if ((entity.flags & kEntityReplacedByMesh) != 0)
				continue;
			HdSpritePose pose;
			if (!HdPoseOf(entity, pose) || pose.clip < 0 || static_cast<size_t>(pose.clip) >= index.clips.size())
				continue;
			const hd_sprites::ClipInfo &clip = index.clips[static_cast<size_t>(pose.clip)];
			if (clip.cols <= 0 || clip.frames <= 0 || clip.png.empty())
				continue;
			auto path = HdSheetPaths.find(pose.clip);
			if (path == HdSheetPaths.end()) {
				std::string asset = clip.png;
				std::replace(asset.begin(), asset.end(), '\\', '/');
				if (!asset.starts_with("hd/"))
					asset.insert(0, "hd/");
				path = HdSheetPaths.emplace(pose.clip, ResolvePackPath(asset)).first;
			}
			constexpr float Scale = 4.0F; // hd/sprites.txt sizes are 4x classic pixels (CONTRACT.md v1)
			// HostEntitySprite puts the ground 16 classic pixels above the frame's bottom row; the clip pivot (the feet) sits
			// HdSpritesPivotAboveClassicBottom above it, as in the Hd2D view.
			constexpr float GroundAbovePivot = (16 - HdSpritesPivotAboveClassicBottom) * Scale;
			const int frame = std::clamp(pose.frame, 0, clip.frames - 1);
			HostEntityHdSprite sprite {};
			sprite.entityId = entity.id;
			sprite.clip = static_cast<uint32_t>(pose.clip);
			sprite.srcX = static_cast<float>((frame % clip.cols) * clip.cellWidth);
			sprite.srcY = static_cast<float>((frame / clip.cols) * clip.cellHeight);
			sprite.srcW = static_cast<float>(clip.cellWidth);
			sprite.srcH = static_cast<float>(clip.cellHeight);
			sprite.sheetWidth = static_cast<uint32_t>(clip.sheetWidth());
			sprite.sheetHeight = static_cast<uint32_t>(clip.sheetHeight());
			sprite.anchorX = static_cast<float>(clip.pivotX);
			sprite.anchorY = static_cast<float>(clip.pivotY) - GroundAbovePivot;
			sprite.scale = Scale;
			sprite.tint[0] = pose.tint.rgb[0];
			sprite.tint[1] = pose.tint.rgb[1];
			sprite.tint[2] = pose.tint.rgb[2];
			sprite.saturation = pose.tint.saturation;
			sprite.flags = pose.selfLit ? kHdSelfLit : 0;
			sprite.path = path->second.c_str();
			HdSprites.push_back(sprite);
		}
	}
	HdArt.sprites = HdSprites.empty() ? nullptr : HdSprites.data();
	HdArt.spriteCount = static_cast<uint32_t>(HdSprites.size());
	return HdArt;
}

const HostEntityArt &BuildEntityArt(bool inGame)
{
	Sprites.clear();
	Clips.clear();
	ClipNames.clear();
	if (TheAtlas.rgba.empty())
		TheAtlas.clear();
	if (inGame) {
		if (const uint64_t hash = LogicalPaletteHash(); hash != TheAtlas.paletteHash) {
			// The level palette changed (level change, palette effects): every decoded frame is stale.
			TheAtlas.paletteHash = hash;
			TheAtlas.clear();
		}
		for (const EntitySpriteSource &source : SnapshotSprites()) {
			Rect rect;
			if (!Acquire(source.sprite, source.trn, rect))
				continue;
			HostEntitySprite sprite {};
			sprite.entityId = source.entityId;
			sprite.srcX = static_cast<float>(rect.x + 1);
			sprite.srcY = static_cast<float>(rect.y + 1);
			sprite.srcW = static_cast<float>(rect.w - 2);
			sprite.srcH = static_cast<float>(rect.h - 2);
			sprite.width = source.sprite.width();
			sprite.height = source.sprite.height();
			sprite.anchorX = static_cast<int16_t>(sprite.width / 2);
			sprite.anchorY = static_cast<int16_t>(static_cast<int>(sprite.height) - 16);
			Sprites.push_back(sprite);
		}
		for (const HostEntity &entity : SnapshotEntities()) {
			if (entity.kind != HostEntityKind::Player || !HasCharacterModel(static_cast<int>(entity.typeId)))
				continue;
			const uint32_t index = (entity.id & 0xFFFFFF) - 1;
			if (index < Players.size())
				AddClip(entity, Players[index]);
		}
	}

	HostSpriteAtlas &atlas = Art.atlas;
	atlas.revision = TheAtlas.revision;
	atlas.width = AtlasSize;
	atlas.height = AtlasSize;
	atlas.dirtyX = static_cast<uint32_t>(TheAtlas.dirtyX0);
	atlas.dirtyY = static_cast<uint32_t>(TheAtlas.dirtyY0);
	atlas.dirtyW = static_cast<uint32_t>(TheAtlas.dirtyX1 - TheAtlas.dirtyX0);
	atlas.dirtyH = static_cast<uint32_t>(TheAtlas.dirtyY1 - TheAtlas.dirtyY0);
	atlas.rgba = TheAtlas.rgba.data();
	TheAtlas.dirtyX0 = TheAtlas.dirtyY0 = TheAtlas.dirtyX1 = TheAtlas.dirtyY1 = 0;
	Art.sprites = Sprites.empty() ? nullptr : Sprites.data();
	Art.spriteCount = static_cast<uint32_t>(Sprites.size());
	Art.clips = Clips.empty() ? nullptr : Clips.data();
	Art.clipCount = static_cast<uint32_t>(Clips.size());
	return Art;
}

} // namespace devilution::host
