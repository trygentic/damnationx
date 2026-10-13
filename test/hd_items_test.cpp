#include "engine/render/hd_items_core.hpp"

#include <gtest/gtest.h>

namespace devilution::hd_sprites {
namespace {

constexpr char SampleIndex[] = R"(# DamnationX HD items
clip item/goldflip/drop_small items/goldflip_drop_small.png 30 6 260 520 130 519 0
clip item/goldflip/rest_small items/goldflip_rest_small.png 1 1 260 140 130 139 0
clip item/goldflip/drop_large items/goldflip_drop_large.png 30 6 260 520 130 519 0
clip item/goldflip/rest_large items/goldflip_rest_large.png 1 1 260 160 130 159 0
clip item/goldflip/rest items/goldflip_rest.png 1 1 260 150 130 149 0
clip item/fbttle/drop items/fbttle_drop.png 48 8 120 520 60 519 0
clip item/fbttle/rest items/fbttle_rest.png 1 1 120 120 60 119 0
clip item/ring/rest items/ring_rest.png 12 6 60 60 30 59 1
)";

TEST(HdItemsKeys, BuildsStateAndVariantKeys)
{
	EXPECT_EQ(ItemClipKey("goldflip", "small", ItemClipState::Drop), "item/goldflip/drop_small");
	EXPECT_EQ(ItemClipKey("goldflip", "", ItemClipState::Rest), "item/goldflip/rest");
	EXPECT_EQ(ItemClipKey("swrdflip", {}, ItemClipState::Drop), "item/swrdflip/drop");
	EXPECT_EQ(GoldPileVariant(/*small=*/true, /*large=*/false), "small");
	EXPECT_EQ(GoldPileVariant(false, false), "medium");
	EXPECT_EQ(GoldPileVariant(false, true), "large");
}

TEST(HdItemsKeys, PrefersVariantThenFallsBackToPlainKey)
{
	const SpriteIndex index = ParseSpriteIndex(SampleIndex);
	ASSERT_TRUE(index.errors.empty());
	EXPECT_EQ(index.clips[FindItemClip(index, "goldflip", "small", ItemClipState::Drop)].key, "item/goldflip/drop_small");
	EXPECT_EQ(index.clips[FindItemClip(index, "goldflip", "large", ItemClipState::Rest)].key, "item/goldflip/rest_large");
	// No medium pile: the plain rest clip; no plain or medium drop clip: classic.
	EXPECT_EQ(index.clips[FindItemClip(index, "goldflip", "medium", ItemClipState::Rest)].key, "item/goldflip/rest");
	EXPECT_EQ(FindItemClip(index, "goldflip", "medium", ItemClipState::Drop), -1);
	EXPECT_EQ(index.clips[FindItemClip(index, "fbttle", "", ItemClipState::Drop)].key, "item/fbttle/drop");
	// Rest art without drop art: the drop stays classic.
	EXPECT_EQ(FindItemClip(index, "ring", "", ItemClipState::Drop), -1);
	EXPECT_EQ(FindItemClip(index, "swrdflip", "", ItemClipState::Rest), -1);
	EXPECT_EQ(FindItemClip(index, "", "", ItemClipState::Rest), -1);
}

TEST(HdItemsKeys, WarmClipsAreGoldAndCommonPotions)
{
	EXPECT_TRUE(IsWarmItemClip("item/goldflip/drop_large"));
	EXPECT_TRUE(IsWarmItemClip("item/fbttle/rest"));
	EXPECT_TRUE(IsWarmItemClip("item/fbttledb/drop"));
	EXPECT_FALSE(IsWarmItemClip("item/fbttlebr/drop"));
	EXPECT_FALSE(IsWarmItemClip("item/swrdflip/drop"));
}

TEST(HdItemsFrames, DropFollowsTheClassicAnimationProportionally)
{
	// Gold: 10 CEL frames x 1 tick -> 30 HD frames.
	ClipInfo clip;
	clip.frames = 30;
	ItemAnimState state { .currentFrame = 0, .numberOfFrames = 10, .tick = 0, .ticksPerFrame = 1 };
	EXPECT_EQ(SelectItemDropFrame(clip, state), 0);
	state.subtick = 0.5F;
	EXPECT_EQ(SelectItemDropFrame(clip, state), 1);
	state.currentFrame = 4;
	state.subtick = 0.0F;
	EXPECT_EQ(SelectItemDropFrame(clip, state), 12);
	// The classic animation stops on its last frame: the clip holds its final (landed) frame.
	state.currentFrame = 9;
	state.subtick = 1.0F;
	EXPECT_EQ(SelectItemDropFrame(clip, state), 29);
	state.currentFrame = 12;
	EXPECT_EQ(SelectItemDropFrame(clip, state), 29);
	// Potions: 16 frames -> 48 HD frames.
	clip.frames = 48;
	state = { .currentFrame = 8, .numberOfFrames = 16, .tick = 0, .ticksPerFrame = 1 };
	EXPECT_EQ(SelectItemDropFrame(clip, state), 24);
}

TEST(HdItemsFrames, DistributedDropUsesProgress)
{
	ClipInfo clip;
	clip.frames = 30;
	const ItemAnimState state { .currentFrame = 3, .numberOfFrames = 10, .distributed = true, .progress = 0.5F };
	EXPECT_EQ(SelectItemDropFrame(clip, state), 15);
}

TEST(HdItemsFrames, RestIsStillOrLoopsAtSixtyFps)
{
	ClipInfo still;
	still.frames = 1;
	EXPECT_EQ(SelectItemRestFrame(still, 12345), 0);
	ClipInfo oneShot;
	oneShot.frames = 30;
	EXPECT_EQ(SelectItemRestFrame(oneShot, 12345), 0);
	ClipInfo glint;
	glint.frames = 12;
	glint.loop = true;
	EXPECT_EQ(SelectItemRestFrame(glint, 0), 0);
	EXPECT_EQ(SelectItemRestFrame(glint, 100), 6);
	EXPECT_EQ(SelectItemRestFrame(glint, 200), 0);
}

} // namespace
} // namespace devilution::hd_sprites
