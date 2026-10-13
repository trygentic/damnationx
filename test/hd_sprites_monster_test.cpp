#include "engine/render/hd_sprites_core.hpp"
#include "engine/render/hd_sprites_monster_keys.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace devilution::hd_sprites {
namespace {

// MonsterGraphic order (monster.h): Stand, Walk, Attack, GotHit, Death, Special.
constexpr int Stand = 0;
constexpr int Walk = 1;
constexpr int Attack = 2;
constexpr int GotHit = 3;
constexpr int Death = 4;
constexpr int Special = 5;

// Direction order (direction.hpp): South, SouthWest, West, NorthWest, North, NorthEast, East, SouthEast.
constexpr int South = 0;
constexpr int NorthWest = 3;
constexpr int SouthEast = 7;

TEST(HdSpritesMonsterKeys, GfxNameIsTheGraphicsFolder)
{
	EXPECT_EQ(MonsterGfxName("zombie\\zombie"), "zombie");
	EXPECT_EQ(MonsterGfxName("falspear\\phall"), "falspear");
	EXPECT_EQ(MonsterGfxName("skelsd\\sklsr"), "skelsd");
	EXPECT_EQ(MonsterGfxName("FatC/FatC"), "fatc");
	EXPECT_EQ(MonsterGfxName("bat"), "bat");
	EXPECT_EQ(MonsterGfxName(""), "");
}

TEST(HdSpritesMonsterKeys, TrnVariantIsTheFileStem)
{
	EXPECT_EQ(MonsterTrnVariant("zombie\\bluered"), "bluered");
	EXPECT_EQ(MonsterTrnVariant("monsters/zombie/yellow.trn"), "yellow");
	EXPECT_EQ(MonsterTrnVariant("Scav\\ScavBr"), "scavbr");
	EXPECT_EQ(MonsterTrnVariant("grey"), "grey");
	EXPECT_EQ(MonsterTrnVariant(""), "");
	EXPECT_EQ(MonsterUniqueVariant("genrl"), "u_genrl");
	EXPECT_EQ(MonsterUniqueVariant("monsters\\monsters\\bsdb.trn"), "u_bsdb");
	EXPECT_EQ(MonsterUniqueVariant(""), "");
}

TEST(HdSpritesMonsterKeys, AnimNamesFollowMonsterGraphic)
{
	EXPECT_EQ(MonsterAnimName(Stand), "stand");
	EXPECT_EQ(MonsterAnimName(Walk), "walk");
	EXPECT_EQ(MonsterAnimName(Attack), "attack");
	EXPECT_EQ(MonsterAnimName(GotHit), "hit");
	EXPECT_EQ(MonsterAnimName(Death), "death");
	EXPECT_EQ(MonsterAnimName(Special), "special");
	EXPECT_EQ(MonsterAnimName(-1), "");
	EXPECT_EQ(MonsterAnimName(6), "");
}

TEST(HdSpritesMonsterKeys, ClipKeys)
{
	EXPECT_EQ(MonsterClipKey("zombie", "", Walk, South), "monster/zombie/walk/s");
	EXPECT_EQ(MonsterClipKey("zombie", "bluered", GotHit, NorthWest), "monster/zombie+bluered/hit/nw");
	EXPECT_EQ(MonsterClipKey("skelsd", "u_genrl", Special, SouthEast), "monster/skelsd+u_genrl/special/se");
	EXPECT_EQ(MonsterClipKey("", "", Walk, South), "");
	EXPECT_EQ(MonsterClipKey("zombie", "", 6, South), "");
	EXPECT_EQ(MonsterClipKey("zombie", "", Walk, 8), "");
}

TEST(HdSpritesMonsterKeys, ParsedIndexFindsMonsterKeys)
{
	const SpriteIndex index = ParseSpriteIndex(
	    "clip monster/zombie/walk/s sprites/monster_zombie_walk_s.png 72 27 149 256 77 255 1\n"
	    "clip monster/zombie+bluered/walk/s sprites/monster_zombie+bluered_walk_s.png 72 27 149 256 77 255 1\n");
	EXPECT_TRUE(index.errors.empty());
	EXPECT_GE(index.find(MonsterClipKey("zombie", "", Walk, South)), 0);
	EXPECT_GE(index.find(MonsterClipKey("zombie", "bluered", Walk, South)), 0);
	EXPECT_LT(index.find(MonsterClipKey("zombie", "grey", Walk, South)), 0);
}

TEST(HdSpritesMonsterLooks, LooksAreTheRenderedGfxSetsAndVariants)
{
	const SpriteIndex index = ParseSpriteIndex(
	    "clip monster/zombie/walk/s a.png 1 1 1 1 0 0 1\n"
	    "clip monster/zombie/stand/s a.png 1 1 1 1 0 0 1\n"
	    "clip monster/zombie+grey/walk/s a.png 1 1 1 1 0 0 1\n"
	    "clip monster/sking+white/walk/s a.png 1 1 1 1 0 0 1\n"
	    "clip warrior/walk/s a.png 1 1 1 1 0 0 1\n"
	    "clip monster//walk/s a.png 1 1 1 1 0 0 1\n");
	const std::vector<std::string> looks = MonsterLooks(index);
	EXPECT_EQ(looks, (std::vector<std::string> { "sking+white", "zombie", "zombie+grey" }));
}

TEST(HdSpritesMonsterLooks, MissingRecoloursFallBackInsteadOfClassic)
{
	const std::vector<std::string> looks { "bat", "bat+red", "falspear+fallent", "fatc", "sking+white", "zombie", "zombie+grey" }; // sorted, as MonsterLooks()
	const auto choose = [&looks](std::string_view gfx, std::vector<std::string> wanted) { return ChooseMonsterLook(looks, gfx, wanted); };

	// The exact recolour when it was rendered.
	MonsterLookChoice choice = choose("zombie", { "grey" });
	EXPECT_EQ(choice.look, "zombie+grey");
	EXPECT_TRUE(choice.exact);
	EXPECT_EQ(choice.prefix(), "monster/zombie+grey/");
	choice = choose("zombie", {});
	EXPECT_EQ(choice.look, "zombie");
	EXPECT_TRUE(choice.exact);

	// A unique monster's TRN without clips (Rotfeast on a Zombie) takes the type's look, tinted by the caller.
	choice = choose("zombie", { "u_eth" });
	EXPECT_EQ(choice.look, "zombie");
	EXPECT_FALSE(choice.exact);
	choice = choose("falspear", { "u_ptu", "fallent" }); // Pukerat the Unclean on a Fallen One
	EXPECT_EQ(choice.look, "falspear+fallent");
	EXPECT_FALSE(choice.exact);
	// The Butcher (unique TRN, base graphics) and the Skeleton King (unique TRN over the type's white TRN).
	EXPECT_EQ(choose("fatc", { "u_genrl" }).look, "fatc");
	EXPECT_EQ(choose("sking", { "u_genrl", "white" }).look, "sking+white");
	// A TRN variant the pack has no clips for (the Familiar's orange bat) draws the base look.
	choice = choose("bat", { "orange" });
	EXPECT_EQ(choice.look, "bat");
	EXPECT_FALSE(choice.exact);
	// Without a base look: any rendered variant of the set rather than classic.
	EXPECT_EQ(choose("sking", {}).look, "sking+white");
	EXPECT_EQ(choose("falspear", { "dark" }).look, "falspear+fallent");

	// Only a set without any clips stays classic.
	EXPECT_TRUE(choose("goatmace", {}).look.empty());
	EXPECT_TRUE(choose("goatmace", { "beige" }).prefix().empty());
	EXPECT_TRUE(choose("", { "grey" }).look.empty());
	// "bat" must not match the "bats" set (or "zombie" a "zombie2" look).
	const std::vector<std::string> other { "bats", "zombie2+x" };
	EXPECT_TRUE(ChooseMonsterLook(other, "bat", std::vector<std::string> {}).look.empty());
	EXPECT_TRUE(ChooseMonsterLook(other, "zombie", std::vector<std::string> {}).look.empty());
}

// The monster clips use the players' proportional mapping: HD frames = classic frames x ticks per frame x 3.
TEST(HdSpritesMonsterFrames, ProportionalMappingMatchesTheClassicTimeline)
{
	// Zombie stand: 11 frames x 4 ticks = 132 HD frames, looping.
	EXPECT_EQ(SelectFrame(132, true, 0, 11, 0, 0.0F, 4), 0);
	EXPECT_EQ(SelectFrame(132, true, 1, 11, 0, 0.0F, 4), 12);
	EXPECT_EQ(SelectFrame(132, true, 1, 11, 2, 0.5F, 4), 19);
	EXPECT_EQ(SelectFrame(132, true, 10, 11, 3, 0.99F, 4), 131);
	// Skeleton axe variants stand at 2..5 ticks per frame with one 12-frame clip: progress, not ticks, picks the frame.
	EXPECT_EQ(SelectFrame(72, true, 6, 12, 0, 0.0F, 2), 36);
	EXPECT_EQ(SelectFrame(72, true, 6, 12, 0, 0.0F, 5), 36);
	// Zombie death: 16 x 1 = 48, one-shot, clamps on the last frame (the corpse frame).
	EXPECT_EQ(SelectFrame(48, false, 15, 16, 0, 1.0F, 1), 47);
	EXPECT_EQ(SelectFrame(48, false, 16, 16, 0, 0.0F, 1), 47);
	// Fallen walk: 11 x 1 = 33.
	EXPECT_EQ(SelectFrame(33, true, 5, 11, 0, 0.5F, 1), 16);
}

} // namespace
} // namespace devilution::hd_sprites
