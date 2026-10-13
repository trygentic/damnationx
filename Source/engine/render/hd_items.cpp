#include "engine/render/hd_items.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/animationinfo.h"
#include "engine/assets.hpp"
#include "engine/render/hd_items_core.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/surface.hpp"
#include "items.h"
#include "nthread.h"
#include "utils/log.hpp"
#include "utils/timer.hpp"

namespace devilution {
namespace {

constexpr char ItemIndexAsset[] = "hd\\items.txt";
constexpr char ItemIndexUrl[] = "hd/items.txt";

enum class IndexState : uint8_t { Unloaded, Loading, Loaded, Failed };

IndexState CurrentIndexState = IndexState::Unloaded;
hd_sprites::SpriteIndex ItemIndex;
/** ItemIndex::clips -> HD sprite index (HdSpritesAppendClip). */
std::vector<int> GlobalClip;
/** Clips of the most frequent drops (hd_sprites::IsWarmItemClip), kept streaming in every world pass. */
std::vector<int> WarmClips;

void AcceptIndex(std::string_view text, std::string_view source)
{
	ItemIndex = hd_sprites::ParseSpriteIndex(text);
	for (const std::string &error : ItemIndex.errors)
		LogError("HD items ({}): {}", source, error);
	GlobalClip.clear();
	for (const hd_sprites::ClipInfo &clip : ItemIndex.clips)
		GlobalClip.push_back(HdSpritesAppendClip(clip));
	CurrentIndexState = ItemIndex.clips.empty() ? IndexState::Failed : IndexState::Loaded;
	Log("HD items: {} clips from {}", ItemIndex.clips.size(), source);
	WarmClips.clear();
	for (size_t i = 0; i < ItemIndex.clips.size(); ++i) {
		if (hd_sprites::IsWarmItemClip(ItemIndex.clips[i].key))
			WarmClips.push_back(GlobalClip[i]);
	}
}

/** Loads hd/items.txt (assets, else fetched next to the page) once the sprite index has settled. */
bool EnsureItemIndex()
{
	switch (CurrentIndexState) {
	case IndexState::Loaded:
		return true;
	case IndexState::Failed:
		return false;
	case IndexState::Unloaded:
	case IndexState::Loading:
		break;
	}
	// Item clips are appended to the sprite index: wait until hd/sprites.txt can no longer replace it.
	if (!HdSpritesIndexSettled())
		return false;
	if (CurrentIndexState == IndexState::Unloaded) {
		if (FindAsset(ItemIndexAsset).ok()) {
			auto asset = LoadAsset(ItemIndexAsset);
			if (asset.has_value()) {
				AcceptIndex(static_cast<std::string_view>(*asset), "assets");
				return CurrentIndexState == IndexState::Loaded;
			}
		}
		CurrentIndexState = IndexState::Loading;
	}
	std::string text;
	bool failed = false;
	if (HdSpritesGpuFetchText(ItemIndexUrl, text, failed)) {
		AcceptIndex(text, ItemIndexUrl);
	} else if (failed) {
		Log("HD items: hd/items.txt is unavailable; items stay classic");
		CurrentIndexState = IndexState::Failed;
	}
	return CurrentIndexState == IndexState::Loaded;
}

std::string_view ItemVariant(const Item &item)
{
	if (item._itype != ItemType::Gold)
		return {};
	return hd_sprites::GoldPileVariant(item._iCurs == ICURS_GOLD_SMALL, item._iCurs == ICURS_GOLD_LARGE);
}

} // namespace

void HdSpritesKeepItemClipsWarm()
{
	if (!HdSpritesWorldPassAllowed() || !EnsureItemIndex())
		return;
	// A drop lasts half a second: a clip that only starts streaming when the item is thrown would show it classic.
	// Requests for resident or queued clips are no-ops.
	HdSpritesGpuPrefetchClips(WarmClips);
}

bool HdSpritesDrawItem(const Surface &out, const Item &item, Point spriteBottomLeft, int spriteWidth, int lightTableIndex)
{
	// The magic rock animates on its stand forever; it has no drop/rest split and stays classic.
	if (item._iCurs == ICURS_MAGIC_ROCK || !HdSpritesWorldPassAllowed() || !EnsureItemIndex())
		return false;
	const std::string_view cel = ItemDropCelName(item);
	const hd_sprites::ItemClipState state = item._iAnimFlag ? hd_sprites::ItemClipState::Drop : hd_sprites::ItemClipState::Rest;
	const std::string_view variant = ItemVariant(item);
	const int local = hd_sprites::FindItemClip(ItemIndex, cel, variant, state);
	if (state == hd_sprites::ItemClipState::Drop) {
		// The item lands soon: stream its rest clip meanwhile, or it would show classic for a moment after landing.
		if (const int rest = hd_sprites::FindItemClip(ItemIndex, cel, variant, hd_sprites::ItemClipState::Rest); rest >= 0) {
			const int restClip = GlobalClip[rest];
			HdSpritesGpuPrefetchClips(std::span<const int>(&restClip, 1));
		}
	}
	if (local < 0)
		return false;
	const int clip = GlobalClip[local];
	if (!HdSpritesGpuRequestClip(clip))
		return false;

	const hd_sprites::ClipInfo &info = ItemIndex.clips[local];
	int frame = 0;
	if (state == hd_sprites::ItemClipState::Drop) {
		const AnimationInfo &anim = item.AnimInfo;
		const hd_sprites::ItemAnimState animState {
			.currentFrame = anim.currentFrame,
			.numberOfFrames = anim.numberOfFrames,
			.tick = anim.tickCounterOfCurrentFrame,
			.ticksPerFrame = anim.ticksPerFrame,
			.subtick = static_cast<float>(ProgressToNextGameTick) / AnimationInfo::baseValueFraction,
			.distributed = anim.distributesFrames(),
			.progress = static_cast<float>(anim.getAnimationProgress()) / AnimationInfo::baseValueFraction,
		};
		frame = hd_sprites::SelectItemDropFrame(info, animState);
	} else {
		frame = hd_sprites::SelectItemRestFrame(info, GetMillisecondsSinceStartup());
	}

	// Item clips pivot on canvas pixel (W/2, H-1) of the 4x classic frame: its bottom centre (as objects).
	const float pivotX = static_cast<float>(spriteBottomLeft.x) + spriteWidth / 2.0F;
	const float pivotY = static_cast<float>(spriteBottomLeft.y) + 0.75F;
	return HdSpritesRecord(out, clip, frame, pivotX, pivotY, static_cast<float>(spriteBottomLeft.y), lightTableIndex,
	    /*dropShadow=*/false, /*selfLit=*/false);
}

} // namespace devilution
