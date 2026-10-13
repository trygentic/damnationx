#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_sprites_internal.hpp"
#include "engine/render/hd_sprites_monsters.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets.hpp"
#include "engine/render/scrollrt.h"
#include "engine/surface.hpp"
#include "levels/gendung.h"
#include "nthread.h"
#include "player.h"
#include "render3d/skinned_renderer.hpp"
#include "tables/townerdat.hpp"
#include "towners.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

constexpr char IndexAsset[] = "hd\\sprites.txt";
/** Ground point of classic player/towner art relative to the tile draw position (measured: feet sit ~11 px above the diamond bottom). */
constexpr Displacement TileCentre { 32, -11 };

enum class IndexState : uint8_t { Unloaded, Loading, Loaded, Failed };

IndexState CurrentIndexState = IndexState::Unloaded;
hd_sprites::SpriteIndex Index;
hd_sprites::OcclusionTracker Occlusion;
hd_sprites::PixelRect OcclusionBounds;
std::vector<HdSpriteInstance> Instances;
std::vector<hd_sprites::PixelRect> TranslucentRects;
bool WorldPassEnabled = false;
bool WorldPassAllowed = false;
/** Screen pixels per world pixel of the recorded instances (HdSpritesApplyWorldTransform). */
float FrameScale = 1.0F;
/** Hover outline colour given to the instances recorded now (HdSpritesOutlineScope), -1 for none. */
int PendingOutline = -1;

void AcceptIndex(std::string_view text, std::string_view source)
{
	Index = hd_sprites::ParseSpriteIndex(text);
	for (const std::string &error : Index.errors)
		LogError("HD sprites ({}): {}", source, error);
	CurrentIndexState = Index.clips.empty() ? IndexState::Failed : IndexState::Loaded;
	Log("HD sprites: {} clips from {}", Index.clips.size(), source);
}

/** Loads hd/sprites.txt from the assets, or polls the browser fetch; true once the index is usable. */
bool EnsureIndex()
{
	switch (CurrentIndexState) {
	case IndexState::Loaded:
		return true;
	case IndexState::Failed:
		return false;
	case IndexState::Unloaded:
		if (FindAsset(IndexAsset).ok()) {
			auto asset = LoadAsset(IndexAsset);
			if (asset.has_value()) {
				AcceptIndex(static_cast<std::string_view>(*asset), "assets");
				return CurrentIndexState == IndexState::Loaded;
			}
		}
		CurrentIndexState = IndexState::Loading;
		[[fallthrough]];
	case IndexState::Loading: {
		std::string text;
		bool failed = false;
		if (HdSpritesGpuFetchIndex(text, failed)) {
			AcceptIndex(text, "hd/sprites.txt");
		} else if (failed) {
			LogError("HD sprites: hd/sprites.txt is unavailable; using classic sprites");
			CurrentIndexState = IndexState::Failed;
		}
		return CurrentIndexState == IndexState::Loaded;
	}
	}
	return false;
}

std::string_view PlayerClipName(player_graphic graphic, bool inTown)
{
	switch (graphic) {
	case player_graphic::Stand:
		return inTown ? "town_stand" : "stand";
	case player_graphic::Walk:
		return inTown ? "town_walk" : "walk";
	case player_graphic::Attack:
		return "attack";
	case player_graphic::Block:
		return "block";
	case player_graphic::Hit:
		return "hit";
	case player_graphic::Lightning:
	case player_graphic::Fire:
	case player_graphic::Magic:
		return "cast";
	case player_graphic::Death:
		return "death";
	}
	return {};
}

/** Classes whose baked appearance namespaces the HD renderer supports. */
std::string_view PlayerClassName(HeroClass heroClass)
{
	switch (heroClass) {
	case HeroClass::Warrior:
		return "warrior";
	case HeroClass::Rogue:
		return "rogue";
	case HeroClass::Sorcerer:
		return "sorcerer";
	default:
		return {};
	}
}

/** The pinned and prefetched body clips last handed to the GPU (see KeepPlayerClipsLoaded). */
hd_sprites::PlayerPreload LastPreload;
/** The monster clips the level keeps pinned (HdSpritesSetMonsterPinnedClips). */
std::vector<int> MonsterPinnedClips;

/** Pins the union of the player's and the monsters' pinned clips. */
void UpdatePinnedClips()
{
	std::vector<int> pinned = LastPreload.pinned;
	for (const int clip : MonsterPinnedClips) {
		if (std::find(pinned.begin(), pinned.end(), clip) == pinned.end())
			pinned.push_back(clip);
	}
	HdSpritesGpuSetPinnedClips(pinned);
}

/**
 * Keeps the class's walk and idle clips loaded for the current level (hd_sprites::PlanPlayerPreload): pinned clips
 * never leave the texture cache, so a body always has a resident clip to fall back to. Prefetches are issued only when
 * the plan changes (level type, texture resolution): re-requesting evicted prefetches every frame would thrash.
 */
void KeepPlayerClipsLoaded(std::string_view prefix, bool inTown)
{
	hd_sprites::PlayerPreload preload = hd_sprites::PlanPlayerPreload(Index, prefix, inTown, HdSpritesGpuClipBytes, HdSpritesGpuTextureBudget() / 4);
	if (preload.pinned == LastPreload.pinned && preload.prefetched == LastPreload.prefetched)
		return;
	LastPreload = std::move(preload);
	UpdatePinnedClips();
	HdSpritesGpuPrefetchClips(LastPreload.prefetched);
}

std::string_view CowKey(const Towner &towner)
{
	for (const TownerDataEntry &entry : TownersDataEntries) {
		if (entry.type != TOWN_COW || entry.position != towner.position)
			continue;
		switch (entry.direction) {
		case Direction::SouthWest:
			return "towner/cow_sw/idle";
		case Direction::NorthWest:
			return "towner/cow_nw/idle";
		case Direction::North:
			return "towner/cow_n/idle";
		default:
			return {};
		}
	}
	return {};
}

std::string_view TownerKey(const Towner &towner)
{
	switch (towner._ttype) {
	case TOWN_SMITH:
		return "towner/griswold/idle";
	case TOWN_HEALER:
		return "towner/pepin/idle";
	case TOWN_STORY:
		return "towner/cain/idle";
	case TOWN_TAVERN:
		return "towner/ogden/idle";
	case TOWN_WITCH:
		return "towner/adria/idle";
	case TOWN_BMAID:
		return "towner/gillian/idle";
	case TOWN_PEGBOY:
		return "towner/wirt/idle";
	case TOWN_DRUNK:
		return "towner/farnham/idle";
	case TOWN_COW:
		return CowKey(towner);
	default:
		return {};
	}
}

float SubtickProgress(bool frozen)
{
	return frozen ? 0.0F : static_cast<float>(ProgressToNextGameTick) / AnimationInfo::baseValueFraction;
}

/** Common tail: places the clip cell on the pivot, snapshots the occlusion rectangle and records the instance. */
bool RecordInstanceAt(const Surface &out, int clipIndex, int frame, float pivotX, float pivotY, float anchorY, int lightTableIndex,
    bool dropShadow, bool selfLit, const HdSpriteTint &tint = {})
{
	const hd_sprites::ClipInfo &clip = Index.clips[clipIndex];
	const SDL_Surface &buffer = *out.surface;
	const auto *bufferPixels = static_cast<const uint8_t *>(buffer.pixels);
	// `out` is the viewport subregion of the back buffer: record in back-buffer coordinates.
	const Point origin { out.region.x, out.region.y };

	HdSpriteInstance instance;
	instance.clip = clipIndex;
	instance.frame = frame;
	instance.x = static_cast<float>(origin.x) + pivotX - clip.pivotX / 4.0F;
	instance.y = static_cast<float>(origin.y) + pivotY - clip.pivotY / 4.0F;
	instance.width = clip.cellWidth / 4.0F;
	instance.height = clip.cellHeight / 4.0F;
	instance.lightTableIndex = lightTableIndex;
	instance.anchorY = static_cast<float>(origin.y) + anchorY;
	instance.pivotY = static_cast<float>(origin.y) + pivotY;
	instance.dropShadow = dropShadow;
	instance.selfLit = selfLit;
	instance.tint = tint;
	instance.outlineColor = PendingOutline;

	// Snapshot the quad and its drop shadow: the shadow's parallelogram (HdSpriteShadow) spans from the quad's corners
	// to their images, so its bounds are the union of both corner sets.
	float minX = instance.x;
	float maxX = instance.x + instance.width;
	float minY = instance.y;
	float maxY = instance.y + instance.height;
	if (dropShadow) {
		for (const float y : { instance.y, instance.y + instance.height }) {
			const float height = instance.pivotY - y;
			const float shadowY = instance.pivotY - HdSpriteShadow::ShearY * height;
			minX = std::min(minX, instance.x - HdSpriteShadow::ShearX * height - HdSpriteShadow::BlurRadius);
			maxX = std::max(maxX, instance.x + instance.width - HdSpriteShadow::ShearX * height + HdSpriteShadow::BlurRadius);
			minY = std::min(minY, shadowY - HdSpriteShadow::BlurRadius);
			maxY = std::max(maxY, shadowY + HdSpriteShadow::BlurRadius);
		}
	}
	if (instance.outlineColor >= 0) {
		// The outline reaches past the quad (hd_sprites::HdOutline): snapshot it too, so walls in front hide it.
		minX -= hd_sprites::HdOutline::SnapshotPadding;
		minY -= hd_sprites::HdOutline::SnapshotPadding;
		maxX += hd_sprites::HdOutline::SnapshotPadding;
		maxY += hd_sprites::HdOutline::SnapshotPadding;
	}
	const int left = std::max(static_cast<int>(std::floor(minX)), origin.x);
	const int top = std::max(static_cast<int>(std::floor(minY)), origin.y);
	const int right = std::min(static_cast<int>(std::ceil(maxX)), origin.x + out.w());
	const int bottom = std::min(static_cast<int>(std::ceil(maxY)), origin.y + out.h());
	const int index = Occlusion.record(bufferPixels, buffer.pitch, { left, top, right - left, bottom - top });
	if (index < 0)
		return false;
	instance.clipRect = Occlusion.rect(static_cast<size_t>(index));
	Instances.push_back(instance);
	return true;
}

/** Players and towners: the pivot is a whole pixel; the classic frame's bottom row (card anchor) sits TileCentre.y below it. */
bool RecordInstance(const Surface &out, int clipIndex, int frame, Point pivot, int lightTableIndex, const HdSpriteTint &tint = {}, bool selfLit = false)
{
	return RecordInstanceAt(out, clipIndex, frame, static_cast<float>(pivot.x), static_cast<float>(pivot.y),
	    static_cast<float>(pivot.y - TileCentre.deltaY), lightTableIndex, /*dropShadow=*/true, selfLit, tint);
}

/**
 * The external clip a 3D warrior imposter of player `playerId` is drawn through (render3d/skinned_renderer.hpp): one
 * frame of the bake camera's cell, in the left half of a [colour | normal] texture the skinned renderer fills.
 */
int ImposterClip(uint8_t playerId)
{
	hd_sprites::ClipInfo clip;
	clip.key = StrCat("render3d/warrior/p", static_cast<int>(playerId));
	clip.frames = 2;
	clip.cols = 2;
	clip.cellWidth = render3d::kImposterCellSize;
	clip.cellHeight = render3d::kImposterCellSize;
	clip.pivotX = render3d::kImposterPivotX;
	clip.pivotY = render3d::kImposterPivotY;
	clip.loop = false;
	return HdSpritesAppendClip(clip);
}

} // namespace

HdSpritesOutlineScope::HdSpritesOutlineScope(int paletteIndex)
    : previous_(PendingOutline)
{
	PendingOutline = paletteIndex;
}

HdSpritesOutlineScope::~HdSpritesOutlineScope()
{
	PendingOutline = previous_;
}

void HdSpritesBeginWorldPass(const Surface &out, bool allowed)
{
	Instances.clear();
	TranslucentRects.clear();
	OcclusionBounds = {};
	FrameScale = 1.0F;
	Occlusion.reset(out.surface->w, out.surface->h);
	WorldPassAllowed = allowed && HdSpritesGpuAvailable();
	WorldPassEnabled = WorldPassAllowed && EnsureIndex();
	HdSpritesMonstersBeginPass();
	render3d::SkinnedBeginWorldPass();
}

void HdSpritesApplyWorldTransform(const HdWorldTransform &transform)
{
	if (transform.IsIdentity())
		return;
	FrameScale = transform.Scale();
	if (Instances.empty()) {
		TranslucentRects.clear();
		return;
	}
	Occlusion.remap(transform);
	for (size_t i = 0; i < Instances.size(); ++i) {
		HdSpriteInstance &instance = Instances[i];
		instance.x = transform.ToScreenX(instance.x);
		instance.y = transform.ToScreenY(instance.y);
		instance.width = transform.ToScreenLength(instance.width);
		instance.height = transform.ToScreenLength(instance.height);
		instance.pivotY = transform.ToScreenY(instance.pivotY);
		instance.anchorY = transform.ToScreenY(instance.anchorY);
		instance.clipRect = Occlusion.rect(i);
	}
	for (hd_sprites::PixelRect &rect : TranslucentRects) {
		const HdRect screen = transform.ToScreen({ rect.x, rect.y, rect.width, rect.height });
		rect = { screen.x, screen.y, screen.width, screen.height };
	}
}

float HdSpritesFrameScale()
{
	return FrameScale;
}

bool HdSpritesWorldPassAllowed()
{
	return WorldPassAllowed;
}

bool HdSpritesIndexSettled()
{
	return CurrentIndexState == IndexState::Loaded || CurrentIndexState == IndexState::Failed;
}

int HdSpritesAppendClip(const hd_sprites::ClipInfo &clip)
{
	if (const int existing = Index.find(clip.key); existing >= 0)
		return existing;
	const int index = static_cast<int>(Index.clips.size());
	Index.byKey.emplace(clip.key, index);
	Index.clips.push_back(clip);
	return index;
}

bool HdSpritesRecord(const Surface &out, int clip, int frame, float pivotX, float pivotY, float anchorY, int lightTableIndex,
    bool dropShadow, bool selfLit)
{
	if (!WorldPassAllowed || clip < 0 || static_cast<size_t>(clip) >= Index.clips.size())
		return false;
	return RecordInstanceAt(out, clip, frame, pivotX, pivotY, anchorY, lightTableIndex, dropShadow, selfLit);
}

bool HdSpritesDrawPlayer(const Surface &out, const Player &player, Point targetBufferPosition, int lightTableIndex)
{
	const std::string_view className = PlayerClassName(player._pClass);
	if (!WorldPassEnabled) {
		// hd/sprites.txt is still being fetched (the first frames in game): the body is about to be HD, so draw none
		// rather than flashing the classic one. A failed index or a pass without HD sprites keeps the classic body.
		return WorldPassAllowed && CurrentIndexState == IndexState::Loading && player._pClass == HeroClass::Warrior;
	}
	// The skinned 3D warrior (option "3D Warrior Model"), rendered this frame as an imposter through an external clip;
	// the prerendered clips below stay the fallback when the model cannot draw this player.
	if (render3d::SkinnedPlayerWanted(player)) {
		const int clip = ImposterClip(player.getId());
		if (clip >= 0 && render3d::SkinnedRequestImposter(player, clip)) {
			Point pivot = targetBufferPosition + TileCentre;
			pivot += player.position.GetDrawnScreenOffset(player.isWalking(), player._pdir, player.AnimInfo);
			RecordInstance(out, clip, 0, pivot, lightTableIndex);
			return true;
		}
	}
	const bool inTown = leveltype == DTYPE_TOWN;
	const std::string_view baseAction = PlayerClipName(player.getGraphic(), inTown);
	const std::string prefix = hd_sprites::ResolvePlayerClipPrefix(Index, className, player._pgfxnum, baseAction, inTown);
	if (prefix.empty())
		return false;
	KeepPlayerClipsLoaded(prefix, inTown);

	// Free-form movement: the sprite faces along the drawn movement (tempDirection while walking).
	const int direction = static_cast<int>(player.getSpriteDirection());
	const std::string_view action = hd_sprites::PlayerActionForLocation(Index, prefix, baseAction, direction, inTown);
	const hd_sprites::BodyClipChoice choice = hd_sprites::ChooseBodyClip(Index, prefix, action, direction, inTown, HdSpritesGpuClipStatus,
	    /*allowNeighbourDirections=*/prefix == "warrior/");
	if (!choice.exact) {
		// The wanted clip is missing or still streaming: load it ahead of everything else.
		if (const int wanted = Index.find(StrCat(prefix, action, "/", hd_sprites::DirectionSuffix(direction))); wanted >= 0)
			HdSpritesGpuRequestClipUrgent(wanted);
	}
	if (choice.clip < 0)
		return choice.hide; // hide: HD art is on its way, draw no body this frame rather than the classic one

	const hd_sprites::ClipInfo &clip = Index.clips[choice.clip];
	const AnimationInfo &anim = player.AnimInfo;
	int frame = 0;
	if (choice.animate) {
		frame = anim.distributesFrames()
		    ? hd_sprites::SelectFrameFromProgress(clip.frames, clip.loop, static_cast<float>(anim.getAnimationProgress()) / AnimationInfo::baseValueFraction)
		    : hd_sprites::SelectFrame(clip.frames, clip.loop, anim.currentFrame, anim.numberOfFrames, anim.tickCounterOfCurrentFrame,
		          SubtickProgress(anim.isPetrified), anim.ticksPerFrame);
	}

	Point pivot = targetBufferPosition + TileCentre;
	// Same drawn (free-form, sub-tile) offset as the classic body; it also applies while standing.
	pivot += player.position.GetDrawnScreenOffset(player.isWalking(), player._pdir, anim);
	// A body whose quad lies entirely outside the viewport records nothing; it is still an HD body (nothing to show).
	RecordInstance(out, choice.clip, frame, pivot, lightTableIndex);
	return true;
}

bool HdSpritesDrawTowner(const Surface &out, const Towner &towner, Point targetBufferPosition)
{
	if (!WorldPassEnabled)
		return false;
	const std::string_view key = TownerKey(towner);
	if (key.empty())
		return false;
	const int clipIndex = Index.find(key);
	if (clipIndex < 0 || !HdSpritesGpuRequestClip(clipIndex))
		return false;
	const hd_sprites::ClipInfo &clip = Index.clips[clipIndex];
	// Towner clips hold frames x delay x 3 HD frames indexed by the displayed frame (after animOrder).
	const int frame = hd_sprites::SelectFrame(clip.frames, clip.loop, towner._tAnimFrame, towner._tAnimLen, towner._tAnimCnt,
	    SubtickProgress(false), towner._tAnimDelay);
	return RecordInstance(out, clipIndex, frame, targetBufferPosition + TileCentre, 0);
}

void HdSpritesMarkTranslucent(const Surface &out, Point bottomLeft, int width, int height)
{
	// Only writes after an instance can hide it; before the first one the wall is part of every snapshot.
	if (Instances.empty())
		return;
	TranslucentRects.push_back({ out.region.x + bottomLeft.x, out.region.y + bottomLeft.y - height + 1, width, height });
}

void HdSpritesInvalidate()
{
	Instances.clear();
	TranslucentRects.clear();
	OcclusionBounds = {};
	Occlusion.reset(Occlusion.width(), Occlusion.height());
}

void HdSpritesFinishFrame(const uint8_t *pixels, int pitch, int width, int height)
{
	OcclusionBounds = {};
	if (Instances.empty())
		return;
	if (pixels == nullptr || width != Occlusion.width() || height != Occlusion.height()) {
		HdSpritesInvalidate();
		return;
	}
	Occlusion.resolve(pixels, pitch, TranslucentRects, OcclusionBounds);
}

std::span<const HdSpriteInstance> HdSpritesFrameInstances()
{
	return Instances;
}

const hd_sprites::OcclusionTracker &HdSpritesOcclusion()
{
	return Occlusion;
}

hd_sprites::PixelRect HdSpritesOcclusionBounds()
{
	return OcclusionBounds;
}

const hd_sprites::SpriteIndex &HdSpritesIndex()
{
	return Index;
}

// ---- Shared with the other HD sprite kinds (hd_sprites_internal.hpp) ----

bool HdSpritesWorldPassEnabled()
{
	return WorldPassEnabled;
}

float HdSpritesSubtickProgress(bool frozen)
{
	return SubtickProgress(frozen);
}

Point HdSpritesGroundPoint(Point targetBufferPosition)
{
	return targetBufferPosition + TileCentre;
}

bool HdSpritesIndexLoading()
{
	return WorldPassAllowed && CurrentIndexState == IndexState::Loading;
}

bool HdSpritesRecordInstance(const Surface &out, int clipIndex, int frame, Point pivot, int lightTableIndex)
{
	return RecordInstance(out, clipIndex, frame, pivot, lightTableIndex);
}

bool HdSpritesRecordInstance(const Surface &out, int clipIndex, int frame, Point pivot, int lightTableIndex, const HdSpriteTint &tint, bool selfLit)
{
	return RecordInstance(out, clipIndex, frame, pivot, lightTableIndex, tint, selfLit);
}

void HdSpritesSetMonsterPinnedClips(std::span<const int> clips)
{
	if (std::equal(clips.begin(), clips.end(), MonsterPinnedClips.begin(), MonsterPinnedClips.end()))
		return;
	MonsterPinnedClips.assign(clips.begin(), clips.end());
	UpdatePinnedClips();
}

size_t HdSpritesPlayerPinnedBytes()
{
	size_t bytes = 0;
	for (const int clip : LastPreload.pinned)
		bytes += HdSpritesGpuClipBytes(clip);
	return bytes;
}

} // namespace devilution
