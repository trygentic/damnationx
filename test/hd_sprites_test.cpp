#include "engine/render/hd_sprites_core.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace devilution::hd_sprites {
namespace {

TEST(HdSpritesIndex, ParsesClipsCommentsAndErrors)
{
	const SpriteIndex index = ParseSpriteIndex(
	    "# DamnationX HD sprites\n"
	    "clip warrior/walk/s sprites/warrior_walk_s.png 24 20 198 272 115 268 1\r\n"
	    "\n"
	    "clip towner/cain/idle sprites/towner_cain_idle.png 225 34 119 251 62 248 1 # trailing comment\n"
	    "clip warrior/death/s sprites/warrior_death_s.png 120 14 284 429 165 267 0\n"
	    "clip broken sprites/x.png 10 2 3\n"
	    "clip warrior/walk/s sprites/dup.png 24 20 198 272 115 268 1\n"
	    "clip zero sprites/z.png 0 1 1 1 0 0 0\n"
	    "floor 1 2 3\n");
	ASSERT_EQ(index.clips.size(), 3U);
	EXPECT_EQ(index.errors.size(), 4U);

	const int walk = index.find("warrior/walk/s");
	ASSERT_GE(walk, 0);
	const ClipInfo &clip = index.clips[walk];
	EXPECT_EQ(clip.png, "sprites/warrior_walk_s.png");
	EXPECT_EQ(clip.frames, 24);
	EXPECT_EQ(clip.cols, 20);
	EXPECT_EQ(clip.cellWidth, 198);
	EXPECT_EQ(clip.cellHeight, 272);
	EXPECT_EQ(clip.pivotX, 115);
	EXPECT_EQ(clip.pivotY, 268);
	EXPECT_TRUE(clip.loop);
	EXPECT_EQ(clip.rows(), 2);
	EXPECT_EQ(clip.sheetWidth(), 3960);
	EXPECT_EQ(clip.sheetHeight(), 544);

	const int death = index.find("warrior/death/s");
	ASSERT_GE(death, 0);
	EXPECT_FALSE(index.clips[death].loop);
	EXPECT_EQ(index.clips[death].sheetHeight(), 9 * 429);
	EXPECT_EQ(index.find("towner/cain/idle"), 1);
	EXPECT_EQ(index.find("missing"), -1);
}

TEST(HdSpritesFrame, ContractExamples)
{
	// walk: 8 frames x 1 tick = 24 HD frames, 3 per game tick.
	EXPECT_EQ(SelectFrame(24, true, 0, 8, 0, 0.0F, 1), 0);
	EXPECT_EQ(SelectFrame(24, true, 0, 8, 0, 0.34F, 1), 1);
	EXPECT_EQ(SelectFrame(24, true, 0, 8, 0, 0.67F, 1), 2);
	EXPECT_EQ(SelectFrame(24, true, 7, 8, 0, 0.99F, 1), 23);
	// stand: 10 frames x 4 ticks = 120.
	EXPECT_EQ(SelectFrame(120, true, 5, 10, 2, 0.5F, 4), 67);
	// town_stand: 20 x 4 = 240; frame 19, tick 3, subtick ~1 is the last frame.
	EXPECT_EQ(SelectFrame(240, true, 19, 20, 3, 0.999F, 4), 239);
}

TEST(HdSpritesFrame, LoopWrapsAndOneShotClamps)
{
	// A tick due before the frame advanced (subtick 1 on the last tick of the last frame) wraps a loop...
	EXPECT_EQ(SelectFrame(24, true, 7, 8, 0, 1.0F, 1), 0);
	// ...and holds the last frame of a one-shot clip.
	EXPECT_EQ(SelectFrame(120, false, 19, 20, 1, 1.0F, 2), 119);
	EXPECT_EQ(SelectFrame(120, false, 25, 20, 0, 0.0F, 2), 119);
	EXPECT_EQ(SelectFrame(48, false, -1, 16, 0, 0.0F, 1), 0);
	// Faster attacks (skipped classic frames) keep the proportional mapping.
	EXPECT_EQ(SelectFrame(48, false, 6, 12, 0, 0.0F, 1), 24);
	// Defensive inputs.
	EXPECT_EQ(SelectFrame(0, true, 3, 8, 0, 0.0F, 1), 0);
	EXPECT_EQ(SelectFrame(24, true, 3, 0, 0, 0.0F, 1), 0);
	EXPECT_EQ(SelectFrame(24, true, 2, 8, 0, 0.0F, 0), 6);
}

TEST(HdSpritesFrame, Towners)
{
	// Griswold: 16 frames x delay 3 = 144; displayed frame 13 (after animOrder), tick 2 + 0.5.
	EXPECT_EQ(SelectFrame(144, true, 13, 16, 2, 0.5F, 3), 13 * 9 + 7);
}

TEST(HdSpritesFrame, Progress)
{
	EXPECT_EQ(SelectFrameFromProgress(48, false, 0.0F), 0);
	EXPECT_EQ(SelectFrameFromProgress(48, false, 0.5F), 24);
	EXPECT_EQ(SelectFrameFromProgress(48, false, 1.0F), 47);
	EXPECT_EQ(SelectFrameFromProgress(24, true, 1.0F), 0);
	EXPECT_EQ(SelectFrameFromProgress(24, true, -0.5F), 0);
}

TEST(HdSpritesFrame, DirectionSuffixes)
{
	EXPECT_EQ(DirectionSuffix(0), "s");
	EXPECT_EQ(DirectionSuffix(3), "nw");
	EXPECT_EQ(DirectionSuffix(7), "se");
	EXPECT_TRUE(DirectionSuffix(8).empty());
	EXPECT_TRUE(DirectionSuffix(-1).empty());
}

TEST(HdSpritesFrame, ImageRequestCarriesTheIndexedGridSize)
{
	// The mixamo walk rebuild changed warrior/walk/s from a 24 x 1 grid of 166 x 228 cells to 23 x 2 of 171 x 247. A
	// browser that served the old PNG from its HTTP cache next to the new index sliced it with the new cells and the
	// walk flipped between pieces of unrelated frames; the request now carries the grid size the index promises.
	const SpriteIndex index = ParseSpriteIndex("clip warrior/walk/s sprites/warrior_walk_s.png 24 23 171 247 102 227 1\n");
	ASSERT_EQ(index.clips.size(), 1U);
	const ClipInfo &clip = index.clips[0];

	const ImageRequest full = ClipImageRequest(clip, 1);
	EXPECT_EQ(full.width, 0); // decoded at its own size
	EXPECT_EQ(full.height, 0);
	EXPECT_EQ(full.expectedWidth, 23 * 171);
	EXPECT_EQ(full.expectedHeight, 2 * 247);

	const ImageRequest half = ClipImageRequest(clip, 2);
	EXPECT_EQ(half.width, (23 * 171 + 1) / 2); // rounds up like the texture
	EXPECT_EQ(half.height, 247);
	EXPECT_EQ(half.expectedWidth, 23 * 171); // checked before the downscale
	EXPECT_EQ(half.expectedHeight, 2 * 247);

	const ImageRequest quarter = ClipImageRequest(clip, 4);
	EXPECT_EQ(quarter.width, (23 * 171 + 3) / 4);
	EXPECT_EQ(quarter.height, (2 * 247 + 3) / 4);

	// The stale grid (3984 x 228) is not what the index describes.
	EXPECT_NE(full.expectedWidth, 3984);
	EXPECT_NE(full.expectedHeight, 228);
}

TEST(HdSpritesLru, EvictsLeastRecentlyUsedButNeverTheCurrentFrame)
{
	TextureLru lru(100);
	lru.insert(1, 40, 1);
	lru.insert(2, 40, 2);
	lru.insert(3, 40, 3);
	EXPECT_EQ(lru.totalBytes(), 120U);
	lru.touch(1, 4);
	// Clip 2 is the oldest; frame 4 uses clip 1.
	EXPECT_EQ(lru.evict(4), std::vector<int> { 2 });
	EXPECT_EQ(lru.totalBytes(), 80U);
	EXPECT_FALSE(lru.contains(2));

	// A frame that needs more than the budget keeps everything it uses.
	lru.insert(4, 90, 5);
	lru.touch(1, 5);
	lru.touch(3, 5);
	EXPECT_TRUE(lru.evict(5).empty());
	EXPECT_EQ(lru.totalBytes(), 170U);
	// Next frame only uses clip 4: the others go, oldest first.
	lru.touch(4, 6);
	EXPECT_EQ(lru.evict(6), (std::vector<int> { 1, 3 }));
	EXPECT_EQ(lru.totalBytes(), 90U);

	lru.insert(4, 10, 7); // resize
	EXPECT_EQ(lru.totalBytes(), 10U);
	lru.erase(4);
	EXPECT_EQ(lru.totalBytes(), 0U);
	EXPECT_EQ(lru.size(), 0U);
}

TEST(HdSpritesLru, PinnedEntriesAreNeverEvicted)
{
	TextureLru lru(100);
	lru.setPinned(1, true); // pins may precede the insert (a pinned clip that is still loading)
	lru.insert(1, 60, 1);
	lru.insert(2, 60, 2);
	EXPECT_TRUE(lru.pinned(1));
	// Clip 1 is the oldest but pinned: clip 2 goes although it is newer.
	EXPECT_EQ(lru.evict(3), std::vector<int> { 2 });
	EXPECT_TRUE(lru.contains(1));

	// Pinned bytes alone over the budget: nothing evictable, nothing evicted.
	lru.insert(3, 70, 3);
	lru.setPinned(3, true);
	EXPECT_TRUE(lru.evict(4).empty());
	EXPECT_EQ(lru.totalBytes(), 130U);

	// Unpinning makes an entry evictable again; clear() forgets entries but keeps the pins.
	lru.setPinned(1, false);
	EXPECT_EQ(lru.evict(4), std::vector<int> { 1 });
	lru.clear();
	EXPECT_EQ(lru.totalBytes(), 0U);
	EXPECT_TRUE(lru.pinned(3));
	EXPECT_FALSE(lru.pinned(1));
}

/** Index with the warrior's walk clips in all 8 facings plus the given extra clips. */
SpriteIndex BodyIndex(std::initializer_list<std::string_view> extra)
{
	std::string text;
	const auto add = [&text](std::string_view key) { text += "clip warrior/" + std::string(key) + " x.png 24 24 100 200 50 190 1\n"; };
	for (const std::string_view direction : { "s", "sw", "w", "nw", "n", "ne", "e", "se" })
		add("walk/" + std::string(direction));
	for (const std::string_view key : extra)
		add(key);
	return ParseSpriteIndex(text);
}

TEST(HdSpritesBody, CandidatesPreferSameFacingThenNeighbours)
{
	const std::vector<BodyClipCandidate> stand = BodyClipCandidates("town_stand", 0, /*inTown=*/true);
	ASSERT_GE(stand.size(), 4U);
	EXPECT_EQ(stand[0], (BodyClipCandidate { "town_stand", 0, true }));
	EXPECT_EQ(stand[1], (BodyClipCandidate { "stand", 0, true })); // the other idle variant keeps animating
	EXPECT_EQ(stand[2], (BodyClipCandidate { "walk", 0, false }));
	EXPECT_EQ(stand[3], (BodyClipCandidate { "town_stand", 1, true }));
	EXPECT_EQ(stand[4], (BodyClipCandidate { "walk", 1, false }));
	EXPECT_EQ(stand[5], (BodyClipCandidate { "town_stand", 7, true })); // -1 wraps around the ring

	const std::vector<BodyClipCandidate> attack = BodyClipCandidates("attack", 6, /*inTown=*/false);
	EXPECT_EQ(attack[0], (BodyClipCandidate { "attack", 6, true }));
	EXPECT_EQ(attack[1], (BodyClipCandidate { "stand", 6, false }));
	EXPECT_EQ(attack[2], (BodyClipCandidate { "town_stand", 6, false }));
	EXPECT_EQ(attack[3], (BodyClipCandidate { "walk", 6, false }));
	EXPECT_EQ(attack[4], (BodyClipCandidate { "attack", 7, true }));

	const std::vector<BodyClipCandidate> walk = BodyClipCandidates("walk", 3, false);
	EXPECT_EQ(walk[0], (BodyClipCandidate { "walk", 3, true }));
	EXPECT_EQ(walk[3], (BodyClipCandidate { "walk", 4, true }));
	// Every facing appears for the walk clip exactly once: the last resort always covers the whole ring.
	int walks = 0;
	for (const BodyClipCandidate &candidate : walk)
		walks += candidate.action == "walk" ? 1 : 0;
	EXPECT_EQ(walks, 8);

	EXPECT_TRUE(BodyClipCandidates("walk", 8, false).empty());
	EXPECT_TRUE(BodyClipCandidates("", 0, false).empty());
}

TEST(HdSpritesAppearance, DecodesEquipmentAndSharedAnimations)
{
	constexpr std::string_view armors[] = { "light", "medium", "heavy" };
	constexpr std::string_view weapons[] = { "unarmed", "unarmed_shield", "sword", "sword_shield", "bow", "axe", "mace", "mace_shield", "staff" };
	for (unsigned armor = 0; armor < 3; ++armor) {
		for (unsigned weapon = 0; weapon < 9; ++weapon) {
			const auto graphics = static_cast<uint8_t>((armor << 4) | weapon);
			EXPECT_EQ(PlayerAppearancePrefix("warrior", graphics, "walk", false),
			    "warrior/" + std::string(armors[armor]) + "/" + std::string(weapons[weapon]) + "/");
			EXPECT_EQ(PlayerAppearancePrefix("rogue", graphics, "death", false),
			    "rogue/" + std::string(armors[armor]) + "/unarmed/");
		}
	}
	EXPECT_EQ(PlayerAppearancePrefix("sorcerer", 0x27, "cast", true), "sorcerer/heavy/unarmed_shield/");
	EXPECT_EQ(PlayerAppearancePrefix("warrior", 0x16, "cast", true), "warrior/medium/unarmed/");
	EXPECT_EQ(PlayerAppearancePrefix("warrior", 0x16, "cast", false), "warrior/medium/mace/");
	EXPECT_TRUE(PlayerAppearancePrefix("warrior", 0x30, "walk", false).empty());
	EXPECT_TRUE(PlayerAppearancePrefix("warrior", 0x09, "walk", false).empty());
}

TEST(HdSpritesAppearance, MissingEquipmentDoesNotBorrowLegacySwordAndShield)
{
	const SpriteIndex legacy = BodyIndex({});
	EXPECT_EQ(ResolvePlayerClipPrefix(legacy, "warrior", 0x03, "walk", false), "warrior/");
	for (const uint8_t graphics : { 0x00, 0x02, 0x04, 0x13, 0x23 })
		EXPECT_TRUE(ResolvePlayerClipPrefix(legacy, "warrior", graphics, "walk", false).empty());
	EXPECT_TRUE(ResolvePlayerClipPrefix(legacy, "rogue", 0x03, "walk", false).empty());
	const SpriteIndex modern = ParseSpriteIndex(
	    "clip warrior/walk/n old.png 24 6 64 96 32 90 1\n"
	    "clip warrior/light/sword_shield/walk/n new.png 24 6 64 96 32 90 1\n"
	    "clip warrior/heavy/axe/walk/n axe.png 24 6 64 96 32 90 1\n");
	EXPECT_EQ(ResolvePlayerClipPrefix(modern, "warrior", 0x03, "walk", false), "warrior/light/sword_shield/");
	EXPECT_EQ(ResolvePlayerClipPrefix(modern, "warrior", 0x25, "walk", false), "warrior/heavy/axe/");
	const auto resident = [](int) { return ClipStatus::Resident; };
	const BodyClipChoice missingNorthEast = ChooseBodyClip(modern, "warrior/heavy/axe/", "walk", 5, false, resident, false);
	EXPECT_EQ(missingNorthEast.clip, -1);
	EXPECT_FALSE(missingNorthEast.hide);
	const BodyClipChoice north = ChooseBodyClip(modern, "warrior/heavy/axe/", "walk", 4, false, resident, false);
	EXPECT_EQ(north.clip, modern.find("warrior/heavy/axe/walk/n"));
	const auto loading = [](int) { return ClipStatus::Loading; };
	EXPECT_TRUE(ChooseBodyClip(modern, "warrior/heavy/axe/", "walk", 4, false, loading, false).hide);
	EXPECT_FALSE(ChooseBodyClip(modern, "warrior/heavy/axe/", "walk", 5, false, loading, false).hide);
}

TEST(HdSpritesBody, ChoosesNearestResidentClipNeverClassicWhileArtLoads)
{
	const SpriteIndex index = BodyIndex({ "town_stand/s", "town_stand/e", "attack/s" });
	const int walkS = index.find("warrior/walk/s");
	const int walkSw = index.find("warrior/walk/sw");
	const int standS = index.find("warrior/town_stand/s");
	const int standE = index.find("warrior/town_stand/e");
	const int attackS = index.find("warrior/attack/s");
	ASSERT_GE(walkS, 0);
	ASSERT_GE(attackS, 0);

	std::vector<ClipStatus> status(index.clips.size(), ClipStatus::Loading);
	const auto query = [&status](int clip) { return status[clip]; };

	// Nothing resident yet: draw nothing, never the classic body.
	BodyClipChoice choice = ChooseBodyClip(index, "warrior/", "town_stand", 0, true, query);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_TRUE(choice.hide);

	// The pinned walk clip of a neighbouring facing is the last resort.
	status[walkSw] = ClipStatus::Resident;
	choice = ChooseBodyClip(index, "warrior/", "town_stand", 0, true, query);
	EXPECT_EQ(choice.clip, walkSw);
	EXPECT_FALSE(choice.animate);
	EXPECT_FALSE(choice.exact);

	// Same facing beats a neighbour: the walk clip stands still in for the idle clip that is still streaming.
	status[walkS] = ClipStatus::Resident;
	choice = ChooseBodyClip(index, "warrior/", "town_stand", 0, true, query);
	EXPECT_EQ(choice.clip, walkS);
	EXPECT_FALSE(choice.animate);

	// The wanted clip itself once it is resident.
	status[standS] = ClipStatus::Resident;
	choice = ChooseBodyClip(index, "warrior/", "town_stand", 0, true, query);
	EXPECT_EQ(choice.clip, standS);
	EXPECT_TRUE(choice.animate);
	EXPECT_TRUE(choice.exact);

	// An attack whose clip streams in shows the idle pose of the same facing.
	choice = ChooseBodyClip(index, "warrior/", "attack", 0, true, query);
	EXPECT_EQ(choice.clip, standS);
	EXPECT_FALSE(choice.animate);
	status[attackS] = ClipStatus::Resident;
	choice = ChooseBodyClip(index, "warrior/", "attack", 0, true, query);
	EXPECT_EQ(choice.clip, attackS);
	EXPECT_TRUE(choice.exact);

	// A missing clip (no "attack/e" in the index) falls back the same way.
	status[standE] = ClipStatus::Resident;
	choice = ChooseBodyClip(index, "warrior/", "attack", 6, true, query);
	EXPECT_EQ(choice.clip, standE);
	EXPECT_FALSE(choice.exact);
}

TEST(HdSpritesBody, ClassicOnlyWithoutUsableArt)
{
	const SpriteIndex index = BodyIndex({});
	std::vector<ClipStatus> status(index.clips.size(), ClipStatus::Unavailable);
	const auto query = [&status](int clip) { return status[clip]; };
	// Every clip failed to load: there is nothing HD to wait for.
	BodyClipChoice choice = ChooseBodyClip(index, "warrior/", "walk", 2, false, query);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_FALSE(choice.hide);
	// A class without clips.
	choice = ChooseBodyClip(index, "rogue/", "walk", 2, false, query);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_FALSE(choice.hide);
	// One failed clip does not matter while another one loads.
	status[index.find("warrior/walk/n")] = ClipStatus::Loading;
	choice = ChooseBodyClip(index, "warrior/", "walk", 2, false, query);
	EXPECT_TRUE(choice.hide);
}

TEST(HdSpritesBody, PreloadPinsWalkAndIdleClipsThatFit)
{
	const SpriteIndex index = BodyIndex({ "stand/s", "stand/n", "town_stand/s", "town_stand/n", "attack/s" });
	const auto bytes = [](int) -> size_t { return 10; };

	PlayerPreload town = PlanPlayerPreload(index, "warrior/", /*inTown=*/true, bytes, /*idlePinBudget=*/20);
	ASSERT_EQ(town.pinned.size(), 10U); // 8 walk + 2 town idle
	EXPECT_TRUE(town.prefetched.empty());
	for (int i = 0; i < 8; ++i)
		EXPECT_TRUE(index.clips[town.pinned[i]].key.starts_with("warrior/walk/"));
	EXPECT_EQ(index.clips[town.pinned[8]].key, "warrior/town_stand/s");
	EXPECT_EQ(index.clips[town.pinned[9]].key, "warrior/town_stand/n");

	// Idle clips over the pin budget are only prefetched; the walk clips stay pinned.
	PlayerPreload dungeon = PlanPlayerPreload(index, "warrior/", /*inTown=*/false, bytes, /*idlePinBudget=*/19);
	EXPECT_EQ(dungeon.pinned.size(), 8U);
	ASSERT_EQ(dungeon.prefetched.size(), 2U);
	EXPECT_EQ(index.clips[dungeon.prefetched[0]].key, "warrior/stand/s");

	EXPECT_TRUE(PlanPlayerPreload(index, "rogue/", true, bytes, 1000).pinned.empty());
}

TEST(HdSpritesBody, TownWalkFallsBackToAnimatedWalk)
{
	const std::vector<BodyClipCandidate> townWalk = BodyClipCandidates("town_walk", 0, /*inTown=*/true);
	ASSERT_GE(townWalk.size(), 4U);
	EXPECT_EQ(townWalk[0], (BodyClipCandidate { "town_walk", 0, true }));
	EXPECT_EQ(townWalk[3], (BodyClipCandidate { "walk", 0, true }));

	const SpriteIndex index = BodyIndex({ "town_walk/s", "walk/n" });
	const auto bytes = [](int) -> size_t { return 10; };
	PlayerPreload town = PlanPlayerPreload(index, "warrior/", /*inTown=*/true, bytes, 0);
	ASSERT_EQ(town.pinned.size(), 8U);
	EXPECT_EQ(index.clips[town.pinned[0]].key, "warrior/town_walk/s");
	EXPECT_EQ(index.clips[town.pinned[4]].key, "warrior/walk/n");
}

TEST(HdSpritesBody, TownWalkUsesOnlyAvailableExactFacings)
{
	const SpriteIndex index = BodyIndex({ "town_walk/s" });
	EXPECT_EQ(PlayerActionForLocation(index, "warrior/", "walk", 0, true), "town_walk");
	EXPECT_EQ(PlayerActionForLocation(index, "warrior/", "walk", 0, false), "walk");
	EXPECT_EQ(PlayerActionForLocation(index, "warrior/", "walk", 4, true), "walk");
	EXPECT_EQ(PlayerActionForLocation(index, "rogue/", "walk", 0, true), "walk");
	EXPECT_EQ(PlayerActionForLocation(index, "warrior/", "attack", 0, true), "attack");
	EXPECT_EQ(PlayerActionForLocation(index, "warrior/", "walk", 8, true), "walk");

	const auto resident = [](int) { return ClipStatus::Resident; };
	const BodyClipChoice choice = ChooseBodyClip(index, "warrior/", "town_walk", 4, true, resident, false);
	ASSERT_GE(choice.clip, 0);
	EXPECT_EQ(index.clips[choice.clip].key, "warrior/walk/n");
	EXPECT_TRUE(choice.animate);

	const auto bytes = [](int) -> size_t { return 10; };
	const PlayerPreload town = PlanPlayerPreload(index, "warrior/", true, bytes, 1000);
	ASSERT_EQ(town.pinned.size(), 8U);
	EXPECT_EQ(index.clips[town.pinned[0]].key, "warrior/town_walk/s");
	const PlayerPreload dungeon = PlanPlayerPreload(index, "warrior/", false, bytes, 1000);
	EXPECT_EQ(index.clips[dungeon.pinned[0]].key, "warrior/walk/s");
}

TEST(HdSpritesFrame, ReversedPlaybackRunsBackwardsSmoothly)
{
	// A 15-frame special played backwards (a Hidden fading in) mapped onto 45 HD frames, 1 tick per frame.
	EXPECT_EQ(SelectFrameReversed(45, false, 14, 15, 0, 0.0F, 1), 44); // start: the last frame
	EXPECT_EQ(SelectFrameReversed(45, false, 14, 15, 0, 0.5F, 1), 43);
	EXPECT_EQ(SelectFrameReversed(45, false, 7, 15, 0, 0.0F, 1), 24);
	EXPECT_EQ(SelectFrameReversed(45, false, 7, 15, 0, 0.99F, 1), 21);
	EXPECT_EQ(SelectFrameReversed(45, false, 0, 15, 0, 0.99F, 1), 0);
	// Within one classic frame the HD frame only ever decreases (forward selection would jump back up).
	int previous = 1000;
	for (int frame = 14; frame >= 0; --frame) {
		for (int tick = 0; tick < 2; ++tick) {
			for (const float subtick : { 0.0F, 0.5F, 0.99F }) {
				const int hd = SelectFrameReversed(90, false, frame, 15, tick, subtick, 2);
				EXPECT_LE(hd, previous);
				previous = hd;
			}
		}
	}
	// Out-of-range input stays inside the clip.
	EXPECT_EQ(SelectFrameReversed(45, false, 20, 15, 0, 0.0F, 1), 44);
	EXPECT_EQ(SelectFrameReversed(45, false, -3, 15, 5, 1.0F, 1), 0);
	EXPECT_EQ(SelectFrameReversed(0, false, 3, 15, 0, 0.0F, 1), 0);
}

/** Index with `monster/zombie/<action>/<dir>` clips for the given keys. */
SpriteIndex MonsterIndex(std::initializer_list<std::string_view> keys)
{
	std::string text;
	for (const std::string_view key : keys)
		text += "clip monster/zombie/" + std::string(key) + " x.png 24 24 100 200 50 190 0\n";
	return ParseSpriteIndex(text);
}

TEST(HdSpritesMonsterBody, CandidatesPreferStandInsOfTheSameFacing)
{
	const std::vector<BodyClipCandidate> attack = MonsterClipCandidates("attack", 2);
	ASSERT_GE(attack.size(), 6U);
	EXPECT_EQ(attack[0], (BodyClipCandidate { "attack", 2, true }));
	EXPECT_EQ(attack[1], (BodyClipCandidate { "stand", 2, false }));
	EXPECT_EQ(attack[2], (BodyClipCandidate { "walk", 2, false }));
	EXPECT_EQ(attack[3], (BodyClipCandidate { "attack", 3, true }));
	EXPECT_EQ(attack[4], (BodyClipCandidate { "stand", 3, false }));
	EXPECT_EQ(attack[5], (BodyClipCandidate { "walk", 3, false }));
	EXPECT_EQ(attack[6], (BodyClipCandidate { "attack", 1, true }));
	EXPECT_EQ(attack.size(), 24U); // three actions x the whole ring

	const std::vector<BodyClipCandidate> stand = MonsterClipCandidates("stand", 0);
	EXPECT_EQ(stand[0], (BodyClipCandidate { "stand", 0, true }));
	EXPECT_EQ(stand[1], (BodyClipCandidate { "walk", 0, false }));
	EXPECT_EQ(stand[2], (BodyClipCandidate { "stand", 1, true }));
	EXPECT_EQ(stand.size(), 16U);

	const std::vector<BodyClipCandidate> walk = MonsterClipCandidates("walk", 7);
	EXPECT_EQ(walk[1], (BodyClipCandidate { "stand", 7, false }));
	EXPECT_EQ(walk[2], (BodyClipCandidate { "walk", 0, true })); // +1 wraps around the ring
	EXPECT_EQ(walk[3], (BodyClipCandidate { "stand", 0, false }));

	// A death only stands in with other facings of the death: never a monster standing up while it dies.
	const std::vector<BodyClipCandidate> death = MonsterClipCandidates("death", 4);
	ASSERT_EQ(death.size(), 8U);
	for (const BodyClipCandidate &candidate : death) {
		EXPECT_EQ(candidate.action, "death");
		EXPECT_TRUE(candidate.animate);
	}
	EXPECT_EQ(death[1].direction, 5);
	EXPECT_EQ(death[2].direction, 3);

	EXPECT_TRUE(MonsterClipCandidates("attack", 8).empty());
	EXPECT_TRUE(MonsterClipCandidates("", 0).empty());
}

TEST(HdSpritesMonsterBody, NeverClassicWhileArtLoads)
{
	const SpriteIndex index = MonsterIndex({ "stand/s", "stand/sw", "walk/s", "attack/s", "death/s", "death/sw" });
	std::vector<ClipStatus> status(index.clips.size(), ClipStatus::Loading);
	const auto query = [&status](int clip) { return status[clip]; };
	const auto choose = [&](std::string_view action, int direction) {
		const std::vector<BodyClipCandidate> candidates = MonsterClipCandidates(action, direction);
		return ChooseClip(index, "monster/zombie/", candidates, query);
	};

	// Nothing resident yet: hidden, not classic.
	BodyClipChoice choice = choose("attack", 0);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_TRUE(choice.hide);

	// A neighbouring facing's stand pose stands in for an attack that streams in.
	status[index.find("monster/zombie/stand/sw")] = ClipStatus::Resident;
	choice = choose("attack", 0);
	EXPECT_EQ(choice.clip, index.find("monster/zombie/stand/sw"));
	EXPECT_FALSE(choice.animate);
	EXPECT_FALSE(choice.exact);

	// The same facing's stand beats it; the attack itself beats both.
	status[index.find("monster/zombie/stand/s")] = ClipStatus::Resident;
	EXPECT_EQ(choose("attack", 0).clip, index.find("monster/zombie/stand/s"));
	status[index.find("monster/zombie/attack/s")] = ClipStatus::Resident;
	choice = choose("attack", 0);
	EXPECT_EQ(choice.clip, index.find("monster/zombie/attack/s"));
	EXPECT_TRUE(choice.exact);
	EXPECT_TRUE(choice.animate);

	// A missing facing of the pack (no attack/w) uses the nearest one.
	EXPECT_EQ(choose("attack", 2).clip, index.find("monster/zombie/stand/sw"));

	// A dying monster waits for a death clip (any facing) instead of standing.
	choice = choose("death", 0);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_TRUE(choice.hide);
	status[index.find("monster/zombie/death/sw")] = ClipStatus::Resident;
	choice = choose("death", 0);
	EXPECT_EQ(choice.clip, index.find("monster/zombie/death/sw"));
	EXPECT_TRUE(choice.animate);

	// Classic only when nothing will ever load.
	std::fill(status.begin(), status.end(), ClipStatus::Unavailable);
	choice = choose("walk", 0);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_FALSE(choice.hide);
	choice = ChooseClip(index, "monster/bat/", MonsterClipCandidates("walk", 0), query);
	EXPECT_EQ(choice.clip, -1);
	EXPECT_FALSE(choice.hide);
}

TEST(HdSpritesMonsterBody, PreloadPinsStandsFirstWithinTheBudget)
{
	std::string text;
	for (const std::string_view look : { "zombie", "bat" }) {
		for (const std::string_view action : { "stand", "walk", "attack", "hit", "death", "special" }) {
			for (const std::string_view direction : { "s", "sw", "w", "nw", "n", "ne", "e", "se" })
				text += "clip monster/" + std::string(look) + "/" + std::string(action) + "/" + std::string(direction) + " x.png 4 4 10 10 5 9 0\n";
		}
	}
	const SpriteIndex index = ParseSpriteIndex(text);
	const auto bytes = [](int) -> size_t { return 10; }; // 80 per look and action
	const std::vector<std::string> prefixes { "monster/zombie/", "monster/bat/", "monster/zombie/" };

	// Room for both looks' stand and walk groups pinned, then death and attack prefetched.
	MonsterPreload plan = PlanMonsterPreload(index, prefixes, bytes, /*pinBudget=*/320, /*prefetchBudget=*/320);
	ASSERT_EQ(plan.pinned.size(), 32U);
	EXPECT_EQ(plan.pinnedBytes, 320U);
	EXPECT_EQ(index.clips[plan.pinned[0]].key, "monster/zombie/stand/s");
	EXPECT_EQ(index.clips[plan.pinned[8]].key, "monster/bat/stand/s");
	EXPECT_EQ(index.clips[plan.pinned[16]].key, "monster/zombie/walk/s");
	ASSERT_EQ(plan.prefetched.size(), 32U);
	EXPECT_EQ(index.clips[plan.prefetched[0]].key, "monster/zombie/death/s");
	EXPECT_EQ(index.clips[plan.prefetched[16]].key, "monster/zombie/attack/s");

	// A budget below one group pins nothing; groups that do not fit are skipped, smaller later ones still fit.
	plan = PlanMonsterPreload(index, prefixes, bytes, 79, 0);
	EXPECT_TRUE(plan.pinned.empty());
	EXPECT_TRUE(plan.prefetched.empty());
	plan = PlanMonsterPreload(index, prefixes, bytes, 1000, 1000);
	EXPECT_EQ(plan.pinned.size() + plan.prefetched.size(), 96U); // every clip exactly once (duplicate prefix ignored)

	// Unknown looks plan nothing.
	const std::vector<std::string> none { "monster/goat/" };
	EXPECT_TRUE(PlanMonsterPreload(index, none, bytes, 1000, 1000).pinned.empty());
}

TEST(HdSpritesMonsterTint, RecolourTintCarriesATrnOverAsAMultiplier)
{
	std::array<uint8_t, 768> palette {};
	const auto set = [&palette](size_t index, uint8_t r, uint8_t g, uint8_t b) {
		palette[index * 3] = r;
		palette[index * 3 + 1] = g;
		palette[index * 3 + 2] = b;
	};
	set(10, 100, 100, 100); // grey
	set(20, 200, 50, 50);   // red
	set(30, 50, 50, 200);   // blue
	std::array<uint32_t, 256> histogram {};
	histogram[10] = 3;
	histogram[20] = 1;

	// Identity maps: no change.
	std::array<float, 3> tint = RecolourTint(histogram, palette, {}, {});
	EXPECT_FLOAT_EQ(tint[0], 1.0F);
	EXPECT_FLOAT_EQ(tint[2], 1.0F);

	// A TRN turning the red pixels blue: red goes down, blue up.
	std::array<uint8_t, 256> trn;
	for (size_t i = 0; i < 256; ++i)
		trn[i] = static_cast<uint8_t>(i);
	trn[20] = 30;
	tint = RecolourTint(histogram, palette, trn, {});
	EXPECT_FLOAT_EQ(tint[0], (300.0F + 50.0F) / (300.0F + 200.0F));
	EXPECT_FLOAT_EQ(tint[1], 1.0F);
	EXPECT_FLOAT_EQ(tint[2], (300.0F + 200.0F) / (300.0F + 50.0F));

	// The same measured against a reference look that is already blue: nothing to do.
	tint = RecolourTint(histogram, palette, trn, trn);
	EXPECT_FLOAT_EQ(tint[0], 1.0F);

	// Empty histogram (or a black reference) leaves the channels alone; extreme ratios are clamped.
	tint = RecolourTint(std::array<uint32_t, 256> {}, palette, trn, {});
	EXPECT_FLOAT_EQ(tint[0], 1.0F);
	set(40, 1, 1, 1);
	std::array<uint32_t, 256> dark {};
	dark[40] = 1;
	std::array<uint8_t, 256> toWhite = trn;
	toWhite[40] = 41;
	set(41, 255, 255, 255);
	tint = RecolourTint(dark, palette, toWhite, {});
	EXPECT_FLOAT_EQ(tint[0], 8.0F);
}

TEST(HdSpritesMonsterTint, InverseTrnUndoesARangeShift)
{
	std::array<uint8_t, 256> trn;
	for (size_t i = 0; i < 256; ++i)
		trn[i] = static_cast<uint8_t>(i);
	for (size_t i = 0; i < 16; ++i)
		trn[0xC0 + i] = static_cast<uint8_t>(0xA0 + i); // shift one ramp onto another
	trn[5] = 0xA3;                                      // and a second index onto an already mapped one
	const std::array<uint8_t, 256> inverse = InverseTrn(trn);
	EXPECT_EQ(inverse[0xA0], 0xC0);
	EXPECT_EQ(inverse[0xAF], 0xCF);
	EXPECT_EQ(inverse[0xA3], 5); // the lowest source wins
	EXPECT_EQ(inverse[0x10], 0x10);
	EXPECT_EQ(inverse[0xC0], 0xC0); // nothing maps to it: unchanged
}

class OcclusionTest : public ::testing::Test {
protected:
	static constexpr int Width = 16;
	static constexpr int Height = 12;
	std::vector<uint8_t> pixels = std::vector<uint8_t>(Width * Height, 7);

	void Fill(int x, int y, int w, int h, uint8_t value)
	{
		for (int yy = y; yy < y + h; ++yy)
			for (int xx = x; xx < x + w; ++xx)
				pixels[yy * Width + xx] = value;
	}

	[[nodiscard]] static uint16_t At(const OcclusionTracker &tracker, int x, int y)
	{
		return tracker.plane()[y * Width + x];
	}
};

TEST_F(OcclusionTest, LaterWritesHideEarlierInstances)
{
	OcclusionTracker tracker;
	tracker.reset(Width, Height);
	// Instance 0 records over the floor; a wall is drawn; instance 1 records; a pillar is drawn last.
	ASSERT_EQ(tracker.record(pixels.data(), Width, { 2, 2, 8, 8 }), 0);
	Fill(4, 2, 3, 8, 20); // wall in front of instance 0 only
	ASSERT_EQ(tracker.record(pixels.data(), Width, { 5, 1, 8, 8 }), 1);
	Fill(11, 1, 2, 8, 30); // pillar in front of both (only overlaps instance 1)

	PixelRect bounds;
	tracker.resolve(pixels.data(), Width, bounds);
	EXPECT_EQ(bounds.x, 2);
	EXPECT_EQ(bounds.y, 1);
	EXPECT_EQ(bounds.width, 11);
	EXPECT_EQ(bounds.height, 9);

	EXPECT_EQ(At(tracker, 3, 5), 0);  // untouched floor: both visible
	EXPECT_EQ(At(tracker, 5, 5), 1);  // wall: hides instance 0 (plane > 0), not instance 1 (plane == 1)
	EXPECT_EQ(At(tracker, 11, 5), 2); // pillar: hides instance 1 (and 0 where it overlaps)
	EXPECT_EQ(At(tracker, 8, 5), 0);
	EXPECT_TRUE(tracker.visible(0));
	EXPECT_TRUE(tracker.visible(1));
}

TEST_F(OcclusionTest, ClipsToBufferAndRejectsEmpty)
{
	OcclusionTracker tracker;
	tracker.reset(Width, Height);
	EXPECT_EQ(tracker.record(pixels.data(), Width, { -4, -4, 3, 3 }), -1);
	ASSERT_EQ(tracker.record(pixels.data(), Width, { -4, 8, 10, 10 }), 0);
	const PixelRect rect = tracker.rect(0);
	EXPECT_EQ(rect.x, 0);
	EXPECT_EQ(rect.y, 8);
	EXPECT_EQ(rect.width, 6);
	EXPECT_EQ(rect.height, 4);
}

TEST_F(OcclusionTest, ClosesSameIndexHolesAndDropsReplacedFrames)
{
	OcclusionTracker tracker;
	tracker.reset(Width, Height);
	Fill(0, 0, Width, Height, 7);
	pixels[5 * Width + 5] = 9; // the floor pixel the wall will happen to repeat
	ASSERT_EQ(tracker.record(pixels.data(), Width, { 2, 2, 8, 8 }), 0);
	ASSERT_EQ(tracker.record(pixels.data(), Width, { 0, 0, 4, 4 }), 1);
	Fill(3, 3, 5, 5, 9); // wall writes index 9 everywhere, including over the 9 already there
	Fill(0, 0, 4, 4, 1); // a panel replaces instance 1's rectangle completely

	PixelRect bounds;
	tracker.resolve(pixels.data(), Width, bounds);
	EXPECT_EQ(At(tracker, 5, 5), 1); // hole closed from the neighbours (monotone: max of the neighbour pair minimum)
	EXPECT_EQ(At(tracker, 4, 4), 1);
	EXPECT_TRUE(tracker.visible(0));
	EXPECT_FALSE(tracker.visible(1));

	tracker.reset(Width, Height);
	EXPECT_EQ(tracker.count(), 0U);
}

TEST_F(OcclusionTest, SeeThroughWallsMarkTranslucentPixels)
{
	OcclusionTracker tracker;
	tracker.reset(Width, Height);
	ASSERT_EQ(tracker.record(pixels.data(), Width, { 2, 2, 8, 8 }), 0);
	// A see-through wall dithered over the whole instance (every other pixel), then an opaque pillar.
	for (int y = 2; y < 10; ++y)
		for (int x = 2 + (y & 1); x < 10; x += 2)
			pixels[y * Width + x] = 20;
	Fill(8, 2, 2, 8, 30);
	const PixelRect wall { 2, 2, 6, 8 };

	PixelRect bounds;
	tracker.resolve(pixels.data(), Width, std::span<const PixelRect>(&wall, 1), bounds);
	constexpr uint16_t Translucent = OcclusionTracker::TranslucentBit;
	EXPECT_EQ(At(tracker, 3, 3), Translucent | 1); // written by the wall
	EXPECT_EQ(At(tracker, 5, 4), Translucent | 1); // dither gap closed from its neighbours, still see-through
	EXPECT_EQ(At(tracker, 9, 5), 1);               // pillar: opaque
	EXPECT_EQ(At(tracker, 1, 5), 0);
	// Nearly every pixel changed, but the see-through ones do not count: the instance stays drawn.
	EXPECT_TRUE(tracker.visible(0));
}

TEST(HdOutlineTest, RadiusIsOneWorldPixelAtEveryZoom)
{
	// Zoom levels 1x..4x in quarter steps: the classic 1-pixel outline is upscaled with the world pass.
	for (int quarters = 4; quarters <= 16; ++quarters) {
		const float scale = static_cast<float>(quarters) / 4.0F;
		EXPECT_FLOAT_EQ(OutlineRadius(scale), scale) << quarters;
	}
	EXPECT_FLOAT_EQ(OutlineRadius(0.5F), 1.0F); // never thinner than one back-buffer pixel
}

TEST(HdOutlineTest, QuadPaddingStaysInsideTheSnapshot)
{
	// The occlusion snapshot is padded in world-pass pixels and scaled with the world pass; the outline quad must not
	// reach past it, or its outer texels would read occlusion data of other pixels (and be clipped).
	for (int quarters = 4; quarters <= 16; ++quarters) {
		const float scale = static_cast<float>(quarters) / 4.0F;
		EXPECT_GT(OutlineQuadPadding(scale), OutlineRadius(scale)) << quarters;
		EXPECT_LE(OutlineQuadPadding(scale), HdOutline::SnapshotPadding * scale) << quarters;
	}
}

} // namespace
} // namespace devilution::hd_sprites
