#include "engine/render/hd_items_core.hpp"

#include <algorithm>

namespace devilution::hd_sprites {

std::string_view ItemStateName(ItemClipState state)
{
	return state == ItemClipState::Drop ? "drop" : "rest";
}

std::string ItemClipKey(std::string_view cel, std::string_view variant, ItemClipState state)
{
	std::string key = "item/";
	key.append(cel);
	key.push_back('/');
	key.append(ItemStateName(state));
	if (!variant.empty()) {
		key.push_back('_');
		key.append(variant);
	}
	return key;
}

std::string_view GoldPileVariant(bool small, bool large)
{
	if (large)
		return "large";
	return small ? "small" : "medium";
}

int FindItemClip(const SpriteIndex &index, std::string_view cel, std::string_view variant, ItemClipState state)
{
	if (cel.empty())
		return -1;
	if (!variant.empty()) {
		const int clip = index.find(ItemClipKey(cel, variant, state));
		if (clip >= 0)
			return clip;
	}
	return index.find(ItemClipKey(cel, {}, state));
}

bool IsWarmItemClip(std::string_view key)
{
	for (const std::string_view prefix : { "item/goldflip/", "item/fbttle/", "item/fbttledb/" }) {
		if (key.starts_with(prefix))
			return true;
	}
	return false;
}

int SelectItemDropFrame(const ClipInfo &clip, const ItemAnimState &state)
{
	if (state.distributed)
		return SelectFrameFromProgress(clip.frames, /*loop=*/false, state.progress);
	return SelectFrame(clip.frames, /*loop=*/false, state.currentFrame, std::max(state.numberOfFrames, 1), state.tick,
	    state.subtick, std::max(state.ticksPerFrame, 1));
}

int SelectItemRestFrame(const ClipInfo &clip, uint32_t elapsedMs)
{
	if (clip.frames <= 1 || !clip.loop)
		return 0;
	// 60 FPS clips.
	return static_cast<int>((static_cast<uint64_t>(elapsedMs) * 60 / 1000) % static_cast<uint64_t>(clip.frames));
}

} // namespace devilution::hd_sprites
