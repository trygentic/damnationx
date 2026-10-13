/**
 * @file hd_items_core.hpp
 *
 * Engine-independent parts of the opt-in HD dropped items: the
 * hd/items.txt clip keys and the choice of HD clip and frame for an item on the ground, resting or still dropping.
 *
 * Nothing here touches game state, SDL or WebGPU so it is covered by native unit tests (test/hd_items_test.cpp).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "engine/render/hd_sprites_core.hpp"

namespace devilution::hd_sprites {

/** What the item on the ground is doing. */
enum class ItemClipState : uint8_t {
	/** Thrown onto the ground: the classic drop ("flip") animation is playing. */
	Drop,
	/** Lying on the ground on the last frame of its drop animation. */
	Rest,
};

/** `drop` / `rest`. */
std::string_view ItemStateName(ItemClipState state);

/**
 * Clip key `item/<cel>/<state>` or, with a variant, `item/<cel>/<state>_<variant>`. <cel> is the item's drop CEL file
 * name (items\<cel>.cel: goldflip, fbttle, swrdflip, ...); variants distinguish looks sharing one CEL (gold pile sizes).
 */
std::string ItemClipKey(std::string_view cel, std::string_view variant, ItemClipState state);

/** Pile size of a gold item by its cursor: `small`, `medium` or `large` (the inventory graphic the value selects). */
std::string_view GoldPileVariant(bool small, bool large);

/** The item's clip: the variant's key first, then the plain key; -1 when neither exists (the item stays classic). */
int FindItemClip(const SpriteIndex &index, std::string_view cel, std::string_view variant, ItemClipState state);

/** Clips kept streaming ahead of any drop: the gold piles and the healing / mana potions (most frequent drops). */
bool IsWarmItemClip(std::string_view key);

/** The classic drop animation of one item, as its AnimationInfo shows it. */
struct ItemAnimState {
	int currentFrame = 0;
	int numberOfFrames = 1;
	int tick = 0;
	int ticksPerFrame = 1;
	/** Progress to the next game tick in [0, 1]. */
	float subtick = 0.0F;
	/** AnimationInfo::distributesFrames(): use `progress` (0..1 through the whole animation) instead. */
	bool distributed = false;
	float progress = 0.0F;
};

/**
 * @brief Frame of the one-shot drop clip for the classic animation's position (proportional, contract section 2
 * formula), clamped so the clip holds its last frame (the landed item, equal to the rest clip) at the end.
 */
int SelectItemDropFrame(const ClipInfo &clip, const ItemAnimState &state);

/** Frame of the rest clip: 0 for a still, a 60 FPS loop over game time (`elapsedMs`) for an animated one (a glint). */
int SelectItemRestFrame(const ClipInfo &clip, uint32_t elapsedMs);

} // namespace devilution::hd_sprites
