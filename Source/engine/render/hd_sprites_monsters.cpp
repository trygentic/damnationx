#include "engine/render/hd_sprites_monsters.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dead.h"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_sprites_internal.hpp"
#include "engine/render/hd_sprites_monster_keys.hpp"
#include "engine/render/scrollrt.h"
#include "engine/surface.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "player.h"
#include "tables/monstdat.h"
#include "utils/clx_decode.hpp"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

constexpr int NumMonsterGraphics = 6;
constexpr int NumDirections = 8;
constexpr int StandGraphic = static_cast<int>(MonsterGraphic::Stand);
constexpr int DeathGraphic = static_cast<int>(MonsterGraphic::Death);

/** Stone curse: the classic grey stone TRN, drawn unlit. */
constexpr HdSpriteTint StoneTint { { 0.9F, 0.9F, 0.92F }, 0.0F };
/** Infravision: the classic red infravision TRN, drawn unlit. */
constexpr HdSpriteTint InfravisionTint { { 1.15F, 0.2F, 0.12F }, 0.0F };

const uint8_t *FirstListData(const ClxSpriteListOrSheet &sprites)
{
	return sprites.isSheet() ? sprites.sheet()[0].data() : sprites.list().data();
}

/**
 * Which of the type's animations (and which direction of it) `current` is. Monsters do not store their current
 * graphic, and the shown direction may differ from `direction` (e.g. turning to face a player, or the free-form walk
 * facing), so the sprite list the AnimationInfo points at is matched against the type's lists. Single-list
 * animations match `fallbackDirection`.
 */
bool FindMonsterAnimation(const CMonster &type, const uint8_t *current, Direction fallbackDirection, int &graphic, int &direction)
{
	for (int g = 0; g < NumMonsterGraphics; ++g) {
		const AnimStruct &anim = type.anims[g];
		if (!anim.sprites || anim.frames <= 0)
			continue;
		const ClxSpriteListOrSheet &sprites = *anim.sprites;
		if (!sprites.isSheet()) {
			if (sprites.list().data() == current) {
				graphic = g;
				direction = static_cast<int>(fallbackDirection);
				return true;
			}
			continue;
		}
		const ClxSpriteSheet sheet = sprites.sheet();
		const int lists = std::min<int>(sheet.numLists(), NumDirections);
		for (int d = 0; d < lists; ++d) {
			if (sheet[static_cast<size_t>(d)].data() == current) {
				graphic = g;
				direction = d;
				return true;
			}
		}
	}
	return false;
}

// ---- Looks: which clips (graphics set + recolour) draw a monster, and the tint standing in for a missing recolour ----

/** A monster look as drawn: the clip key prefix (`monster/<look>/`, empty without HD art) and its tint. */
struct MonsterLook {
	std::string prefix;
	HdSpriteTint tint;
};

/** The palette indices of one classic sprite, counted for RecolourTint (fills count their width). */
void CountSpritePixels(ClxSprite sprite, std::array<uint32_t, 256> &histogram)
{
	const uint8_t *src = sprite.pixelData();
	uint32_t remaining = sprite.pixelDataSize();
	while (remaining != 0) {
		const uint8_t control = *src++;
		--remaining;
		if (!IsClxOpaque(control))
			continue;
		if (IsClxOpaqueFill(control)) {
			if (remaining == 0)
				break;
			histogram[*src++] += GetClxOpaqueFillWidth(control);
			--remaining;
			continue;
		}
		const uint32_t width = std::min<uint32_t>(GetClxOpaquePixelsWidth(control), remaining);
		for (uint32_t i = 0; i < width; ++i)
			++histogram[*src++];
		remaining -= width;
	}
}

/** The palette indices of the type's stand (or first) animation, all facings, first frame each. */
std::array<uint32_t, 256> TypeHistogram(const CMonster &type)
{
	std::array<uint32_t, 256> histogram {};
	for (int g = 0; g < NumMonsterGraphics; ++g) {
		const AnimStruct &anim = type.anims[g];
		if (!anim.sprites || anim.frames <= 0)
			continue;
		for (int d = 0; d < NumDirections; ++d) {
			const OptionalClxSpriteList list = anim.spritesForDirection(static_cast<Direction>(d));
			if (list && list->numSprites() > 0)
				CountSpritePixels((*list)[0], histogram);
			if (!anim.sprites->isSheet())
				break;
		}
		break;
	}
	return histogram;
}

std::array<uint8_t, 768> PaletteRgb()
{
	std::array<uint8_t, 768> rgb;
	for (size_t i = 0; i < 256; ++i) {
		rgb[i * 3 + 0] = logical_palette[i].r;
		rgb[i * 3 + 1] = logical_palette[i].g;
		rgb[i * 3 + 2] = logical_palette[i].b;
	}
	return rgb;
}

/** The type's TRN file (monsters\<trnFile>.trn), or empty. */
std::vector<uint8_t> LoadTypeTrn(const CMonster &type)
{
	const std::string &trnFile = type.data().trnFile;
	if (trnFile.empty())
		return {};
	std::vector<uint8_t> trn(256);
	const std::string path = StrCat("monsters\\", trnFile, ".trn");
	if (!LoadFileInMemWithStatus<uint8_t>(path.c_str(), trn.data(), trn.size()).has_value())
		return {};
	std::replace(trn.begin(), trn.end(), uint8_t { 255 }, uint8_t { 0 }); // as InitMonsterTRN
	return trn;
}

/**
 * The look of a monster type, or of one of its unique monsters: the recolour the classic renderer applies (the
 * unique TRN, else the type's TRN, else none) when the pack has clips for it, else the nearest look with clips
 * (hd_sprites::ChooseMonsterLook) tinted towards the wanted colours (hd_sprites::RecolourTint, measured on the
 * classic sprite in memory, which already carries the type's TRN).
 */
MonsterLook ResolveLook(std::span<const std::string> looks, const CMonster &type, const Monster *unique)
{
	MonsterLook look;
	const std::string gfx = hd_sprites::MonsterGfxName(type.data().spritePath());
	const std::string typeVariant = type.data().trnFile.empty() ? std::string {} : hd_sprites::MonsterTrnVariant(type.data().trnFile);
	std::vector<std::string> wanted;
	const uint8_t *uniqueTrn = nullptr;
	if (unique != nullptr && unique->isUnique() && unique->uniqueMonsterTRN != nullptr) {
		const size_t uniqueIndex = static_cast<size_t>(unique->uniqueType);
		if (uniqueIndex < UniqueMonstersData.size()) {
			wanted.push_back(hd_sprites::MonsterUniqueVariant(UniqueMonstersData[uniqueIndex].mTrnName));
			uniqueTrn = unique->uniqueMonsterTRN.get();
		}
	}
	if (!typeVariant.empty())
		wanted.push_back(typeVariant);
	const hd_sprites::MonsterLookChoice choice = hd_sprites::ChooseMonsterLook(looks, gfx, wanted);
	look.prefix = choice.prefix();
	if (look.prefix.empty() || choice.exact)
		return look;

	// The sprite in memory is the type's look (its TRN applied at load). Wanted: that plus the unique TRN.
	// Reference: the colours the chosen clips were rendered with, the type's look when they are the type variant,
	// the untranslated set when they are the base look.
	const std::string_view chosenVariant = choice.look.size() > gfx.size() ? std::string_view(choice.look).substr(gfx.size() + 1) : std::string_view {};
	std::vector<uint8_t> reference;
	if (chosenVariant.empty() && !typeVariant.empty()) {
		const std::vector<uint8_t> typeTrn = LoadTypeTrn(type);
		if (typeTrn.size() == 256) {
			const std::array<uint8_t, 256> inverse = hd_sprites::InverseTrn(std::span<const uint8_t, 256>(typeTrn.data(), 256));
			reference.assign(inverse.begin(), inverse.end());
		}
	}
	const std::span<const uint8_t> wantedMap = uniqueTrn != nullptr ? std::span<const uint8_t>(uniqueTrn, 256) : std::span<const uint8_t> {};
	const std::array<uint32_t, 256> histogram = TypeHistogram(type);
	const std::array<uint8_t, 768> palette = PaletteRgb();
	look.tint.rgb = hd_sprites::RecolourTint(histogram, palette, wantedMap, reference);
	return look;
}

/** Looks of the current level, resolved on first use (the palette and the TRNs are loaded by then). */
struct LookCache {
	size_t indexSize = static_cast<size_t>(-1);
	std::vector<std::string> looks; // hd_sprites::MonsterLooks of the index
	uint64_t level = 0;
	std::map<std::pair<size_t, int>, MonsterLook> resolved; // (level type index, unique type or -1)
} Looks;

uint64_t LevelKey()
{
	uint64_t key = (static_cast<uint64_t>(currlevel) << 8) | (setlevel ? 0x80U : 0U) | static_cast<uint8_t>(setlvlnum);
	key = key * 1315423911U + LevelMonsterTypeCount;
	for (size_t i = 0; i < LevelMonsterTypeCount; ++i)
		key = key * 1315423911U + static_cast<uint64_t>(LevelMonsterTypes[i].type);
	return key;
}

void SyncLookCache()
{
	const hd_sprites::SpriteIndex &index = HdSpritesIndex();
	if (Looks.indexSize != index.clips.size()) {
		Looks.looks = hd_sprites::MonsterLooks(index);
		Looks.indexSize = index.clips.size();
		Looks.resolved.clear();
	}
	if (const uint64_t level = LevelKey(); level != Looks.level) {
		Looks.level = level;
		Looks.resolved.clear();
	}
}

const MonsterLook &LookFor(size_t typeIndex, const Monster *unique)
{
	SyncLookCache();
	const bool isUnique = unique != nullptr && unique->isUnique() && unique->uniqueMonsterTRN != nullptr;
	const std::pair<size_t, int> key { typeIndex, isUnique ? static_cast<int>(unique->uniqueType) : -1 };
	const auto it = Looks.resolved.find(key);
	if (it != Looks.resolved.end())
		return it->second;
	return Looks.resolved.emplace(key, ResolveLook(Looks.looks, LevelMonsterTypes[typeIndex], isUnique ? unique : nullptr)).first->second;
}

// ---- Level preload ----

struct PreloadState {
	uint64_t signature = 0;
	bool active = false;
} Preload;

/**
 * Pins (and prefetches) the clips of the monster looks on the current level within the texture budget
 * (hd_sprites::PlanMonsterPreload), re-planned when the level, its monster types or the texture resolution change.
 */
void KeepMonsterClipsLoaded()
{
	if (leveltype == DTYPE_TOWN || LevelMonsterTypeCount == 0) {
		if (Preload.active) {
			HdSpritesSetMonsterPinnedClips({});
			Preload = {};
		}
		return;
	}
	SyncLookCache();
	const size_t budget = HdSpritesGpuTextureBudget();
	const size_t playerBytes = HdSpritesPlayerPinnedBytes();
	uint64_t signature = Looks.level * 2654435761U + HdSpritesGpuClipBytes(0);
	signature = signature * 2654435761U + playerBytes;
	signature = signature * 2654435761U + Looks.indexSize;
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.isUnique())
			signature = signature * 31U + static_cast<uint64_t>(monster.uniqueType) + 1;
	}
	if (Preload.active && signature == Preload.signature)
		return;
	Preload.signature = signature;
	Preload.active = true;

	// Looks in load order: the monster types and uniques nearest the player first, so the stand and walk clips of
	// what the player meets first are queued ahead of those of monsters across the level (the plan is action-major:
	// every look's stand clips, then every look's walk clips, ...).
	struct RankedLook {
		int distance;
		const MonsterLook *look;
	};
	std::vector<RankedLook> ranked;
	const Point playerTile = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : Point { 0, 0 };
	const auto distanceTo = [&](const Monster &monster) {
		return std::max(std::abs(monster.position.tile.x - playerTile.x), std::abs(monster.position.tile.y - playerTile.y));
	};
	for (size_t i = 0; i < LevelMonsterTypeCount; ++i) {
		int nearest = std::numeric_limits<int>::max();
		for (size_t m = 0; m < ActiveMonsterCount; ++m) {
			const Monster &monster = Monsters[ActiveMonsters[m]];
			if (monster.levelType == i)
				nearest = std::min(nearest, distanceTo(monster));
		}
		ranked.push_back({ nearest, &LookFor(i, nullptr) });
	}
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.isUnique())
			ranked.push_back({ distanceTo(monster), &LookFor(monster.levelType, &monster) });
	}
	std::stable_sort(ranked.begin(), ranked.end(), [](const RankedLook &a, const RankedLook &b) { return a.distance < b.distance; });
	std::vector<std::string> prefixes;
	for (const RankedLook &entry : ranked) {
		const std::string &prefix = entry.look->prefix;
		if (!prefix.empty() && std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end())
			prefixes.push_back(prefix);
	}
	// Pins may take 5/8 of the budget with the player's; prefetches fill up to 7/8, leaving room for what is drawn
	// on demand (objects, the remaining actions). Prefetches only load into free texture space
	// (HdSpritesGpuPrefetchClipsIfRoom): a prefetch that had to evict was itself evicted first by the next on-demand
	// load (it is never drawn before it is needed), so the Catacombs kept reloading 100+ MiB every few seconds.
	const size_t pinLimit = budget / 8 * 5;
	const size_t pinBudget = pinLimit > playerBytes ? pinLimit - playerBytes : 0;
	const size_t prefetchLimit = budget / 8 * 7;
	const size_t prefetchBudget = prefetchLimit > playerBytes + pinBudget ? prefetchLimit - playerBytes - pinBudget : 0;
	const hd_sprites::MonsterPreload plan = hd_sprites::PlanMonsterPreload(HdSpritesIndex(), prefixes, HdSpritesGpuClipBytes, pinBudget, prefetchBudget);
	HdSpritesSetMonsterPinnedClips(plan.pinned);
	HdSpritesGpuPrefetchClipsIfRoom(plan.prefetched);
	Log("HD sprites: {} monster looks on level {}: pinned {} clips ({} MiB), prefetching {} ({} MiB)", prefixes.size(), currlevel,
	    plan.pinned.size(), plan.pinnedBytes >> 20, plan.prefetched.size(), plan.prefetchedBytes >> 20);
}

/**
 * The resident clip that draws `action` in `direction` for `look` (hd_sprites::MonsterClipCandidates). Unless it is
 * the wanted clip, the best clip the pack has (the wanted one, or the first stand-in when the pack lacks it) is loaded
 * ahead of everything else while a stand-in draws (or nothing, while none is resident yet).
 */
hd_sprites::BodyClipChoice ChooseMonsterClip(const MonsterLook &look, std::string_view action, int direction)
{
	const hd_sprites::SpriteIndex &index = HdSpritesIndex();
	const std::vector<hd_sprites::BodyClipCandidate> candidates = hd_sprites::MonsterClipCandidates(action, direction);
	const hd_sprites::BodyClipChoice choice = hd_sprites::ChooseClip(index, look.prefix, candidates, HdSpritesGpuClipStatus);
	if (!choice.exact) {
		for (const hd_sprites::BodyClipCandidate &candidate : candidates) {
			const int clip = index.find(StrCat(look.prefix, candidate.action, "/", hd_sprites::DirectionSuffix(candidate.direction)));
			if (clip < 0 || (clip != choice.clip && HdSpritesGpuClipStatus(clip) == hd_sprites::ClipStatus::Unavailable))
				continue;
			if (clip != choice.clip)
				HdSpritesGpuRequestClipUrgent(clip);
			break;
		}
	}
	return choice;
}

} // namespace

void HdSpritesMonstersBeginPass()
{
	if (HdSpritesWorldPassEnabled())
		KeepMonsterClipsLoaded();
}

bool HdSpritesDrawMonster(const Surface &out, const Monster &monster, Point targetBufferPosition, bool infraVision, int lightTableIndex)
{
	if (!monster.animInfo.sprites)
		return false;
	if (!HdSpritesWorldPassEnabled()) {
		// hd/sprites.txt is still being fetched (the first frames in game): hide rather than flash the classic body.
		return HdSpritesIndexLoading();
	}
	const MonsterLook &look = LookFor(monster.levelType, &monster);
	if (look.prefix.empty())
		return false; // no HD art for this graphics set: the classic sprite is the only option

	const CMonster &type = monster.type();
	const AnimationInfo &anim = monster.animInfo;
	int graphic = StandGraphic;
	int direction = static_cast<int>(monster.direction);
	// An animation that matches none of the type's lists (never seen) still draws the HD stand pose.
	const bool matched = FindMonsterAnimation(type, anim.sprites->data(), monster.direction, graphic, direction);
	const std::string_view action = hd_sprites::MonsterAnimName(graphic);
	const hd_sprites::BodyClipChoice choice = ChooseMonsterClip(look, action, direction);
	if (choice.clip < 0)
		return choice.hide; // hide: HD art is on its way; otherwise every clip failed to load (classic)

	const hd_sprites::ClipInfo &clip = HdSpritesIndex().clips[choice.clip];
	// Frozen (no processAnimation this tick): stone curse, or a monster waiting in its special pose.
	const bool frozen = anim.isPetrified || (monster.flags & MFLAG_ALLOW_SPECIAL) != 0;
	int frame = 0;
	if (matched && choice.animate) {
		if (anim.distributesFrames()) {
			frame = hd_sprites::SelectFrameFromProgress(clip.frames, clip.loop, static_cast<float>(anim.getAnimationProgress()) / AnimationInfo::baseValueFraction);
		} else if ((monster.flags & MFLAG_LOCK_ANIMATION) != 0) {
			frame = hd_sprites::SelectFrameReversed(clip.frames, clip.loop, anim.currentFrame, anim.numberOfFrames, anim.tickCounterOfCurrentFrame,
			    HdSpritesSubtickProgress(frozen), anim.ticksPerFrame);
		} else {
			frame = hd_sprites::SelectFrame(clip.frames, clip.loop, anim.currentFrame, anim.numberOfFrames, anim.tickCounterOfCurrentFrame,
			    HdSpritesSubtickProgress(frozen), anim.ticksPerFrame);
		}
	}

	// The classic TRN draws (stone curse, infravision) are unlit: so are their HD stand-ins.
	HdSpriteTint tint = look.tint;
	bool selfLit = false;
	if (monster.mode == MonsterMode::Petrified) {
		tint = StoneTint;
		selfLit = true;
	}
	if (infraVision) {
		tint = InfravisionTint;
		selfLit = true;
	}
	Point pivot = HdSpritesGroundPoint(targetBufferPosition);
	// Same drawn (free-form, sub-tile) offset as the classic body; it also applies while standing.
	pivot += monster.position.GetDrawnScreenOffset(monster.isWalking(), monster.direction, anim);
	// A body whose quad lies entirely outside the viewport records nothing; it is still an HD body (nothing to show).
	HdSpritesRecordInstance(out, choice.clip, frame, pivot, selfLit ? 0 : lightTableIndex, tint, selfLit);
	return true;
}

bool HdSpritesDrawCorpse(const Surface &out, const Corpse &corpse, Direction direction, Point targetBufferPosition, int lightTableIndex)
{
	if (!corpse.sprites)
		return false;
	if (!HdSpritesWorldPassEnabled())
		return HdSpritesIndexLoading();
	const uint8_t *corpseData = FirstListData(*corpse.sprites);
	const Monster *unique = corpse.translationPaletteIndex != 0 && corpse.translationPaletteIndex <= MaxMonsters
	    ? &Monsters[corpse.translationPaletteIndex - 1]
	    : nullptr;
	for (size_t i = 0; i < LevelMonsterTypeCount; ++i) {
		const CMonster &type = LevelMonsterTypes[i];
		const AnimStruct &death = type.getAnimData(MonsterGraphic::Death);
		if (!death.sprites || FirstListData(*death.sprites) != corpseData)
			continue;
		const MonsterLook &look = LookFor(i, unique);
		if (look.prefix.empty())
			return false;
		const int dir = death.sprites->isSheet() ? static_cast<int>(direction) : static_cast<int>(Direction::South);
		const hd_sprites::BodyClipChoice choice = ChooseMonsterClip(look, hd_sprites::MonsterAnimName(DeathGraphic), dir);
		if (choice.clip < 0)
			return choice.hide;
		const hd_sprites::ClipInfo &clip = HdSpritesIndex().clips[choice.clip];
		HdSpritesRecordInstance(out, choice.clip, clip.frames - 1, HdSpritesGroundPoint(targetBufferPosition), lightTableIndex, look.tint, false);
		return true;
	}
	return false;
}

} // namespace devilution
