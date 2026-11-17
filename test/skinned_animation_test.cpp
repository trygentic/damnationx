#include "render3d/skinned_animation.hpp"
#include <array>
#include <gtest/gtest.h>

namespace devilution::render3d {
namespace {
TEST(SkinnedAnimation, ClassAssetsAreIndependentAndPlatformSpecific)
{
	EXPECT_EQ(SkinnedAssetPath(SkinnedModelKind::Warrior, false), "hd/3d/warrior/warrior.glb");
	EXPECT_EQ(SkinnedAssetPath(SkinnedModelKind::Rogue, false), "hd/3d/rogue/rogue.glb");
	EXPECT_EQ(SkinnedAssetPath(SkinnedModelKind::Rogue, true), "hd/3d/rogue/rogue_1024.glb");
	EXPECT_NE(SkinnedAssetPath(SkinnedModelKind::Warrior, true), SkinnedAssetPath(SkinnedModelKind::Rogue, true));
	EXPECT_EQ(SkinnedAssetPath(SkinnedModelKind::Sorcerer, false), "hd/3d/sorcerer/sorcerer.glb");
	EXPECT_EQ(SkinnedAssetPath(SkinnedModelKind::Sorcerer, true), "hd/3d/sorcerer/sorcerer_1024.glb");
}

TEST(SkinnedAnimation, WipSorcererPlaysHisBaseClipsEmptyHandedUntilAFamilyArrives)
{
	SkinnedClipSet clips;
	clips.walk = 0;
	clips.attack = 1;
	clips.stand = 3;
	for (auto equipment : { SkinnedEquipmentKind::Unarmed, SkinnedEquipmentKind::Staff, SkinnedEquipmentKind::SwordShield, SkinnedEquipmentKind::Unsupported }) {
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Stand, false, equipment, false), 3);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Stand, true, equipment, false), 3);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Walk, false, equipment, false), 0);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Walk, true, equipment, false), 0);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Attack, false, equipment, true), 1);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Attack, true, equipment, false), -1);
		// Without their clips, these states keep the sprites.
		for (auto action : { SkinnedAnimationKind::Hit, SkinnedAnimationKind::Cast, SkinnedAnimationKind::Death, SkinnedAnimationKind::Block })
			EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, action, false, equipment, false), -1);
	}
	// The round-2 GLB still names its clips walk_rt / slash_rt: without the packaging renames he stays on sprites.
	EXPECT_EQ(ChooseSkinnedClip(SkinnedClipSet {}, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Walk, false, SkinnedEquipmentKind::Unarmed, false), -1);
}

TEST(SkinnedAnimation, WipSorcererRoutesActionsAndFamiliesStrictly)
{
	SkinnedClipSet clips;
	clips.walk = 0;
	clips.attack = 1;
	clips.stand = 3;
	clips.cast = 4;
	clips.townCast = 5;
	clips.hit = 6;
	clips.death = 7;
	clips.block = 8;
	clips.sword = { 20, 21, 22, 23, 24, -1, 25, -1, 26 };
	constexpr auto S = SkinnedModelKind::Sorcerer;
	constexpr auto Sword = SkinnedEquipmentKind::Sword;
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Stand, true, Sword, false), 22);
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Attack, false, Sword, true), 24); // no repeat: the attack again
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Block, false, Sword, false), 25);
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Block, true, Sword, false), -1);
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Hit, false, Sword, false), 26); // the family's own reaction
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Cast, false, Sword, false), 4); // else the shared cast
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Cast, true, Sword, false), 5);
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Death, false, Sword, false), 7);
	// A family the pack lacks plays his base clips, including the shared actions.
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Mace, false), 3);
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Hit, true, SkinnedEquipmentKind::Mace, false), 6);
	EXPECT_EQ(ChooseSkinnedClip(clips, S, SkinnedAnimationKind::Block, false, SkinnedEquipmentKind::MaceShield, false), 8);
}

TEST(SkinnedAnimation, WipSorcererPlaysStaffClipsWhenThePackHasThem)
{
	SkinnedClipSet clips;
	clips.walk = 0;
	clips.attack = 1;
	clips.stand = 3;
	clips.staff = { 10, 11, 12, 13, 14, 15 };
	constexpr auto Staff = SkinnedEquipmentKind::Staff;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Stand, false, Staff, false), 10);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Walk, false, Staff, false), 11);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Stand, true, Staff, false), 12);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Walk, true, Staff, false), 13);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Attack, false, Staff, false), 14);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Attack, false, Staff, true), 15);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Attack, true, Staff, false), -1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Cast, false, Staff, false), -1);
	// Other equipment stays empty-handed on his own clips.
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Sword, false), 3);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Sorcerer, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Sword, false), 1);
}

TEST(SkinnedAnimation, IncompleteRogueFallsBackPerStateAndEquipment)
{
	SkinnedClipSet clips;
	clips.attack = 0;
	clips.attackRepeat = 1;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Bow, false), 0);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Bow, true), 1);
	for (auto action : { SkinnedAnimationKind::Stand, SkinnedAnimationKind::Walk, SkinnedAnimationKind::Hit,
	         SkinnedAnimationKind::Cast, SkinnedAnimationKind::Death, SkinnedAnimationKind::Block })
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, action, false, SkinnedEquipmentKind::Bow, false), -1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, true, SkinnedEquipmentKind::Bow, false), -1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Unsupported, false), -1);
	// Adding a dungeon idle must not substitute it for the unimplemented town idle.
	clips.stand = 2;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Bow, false), 2);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, true, SkinnedEquipmentKind::Bow, false), -1);
	clips.attackRepeat = -1;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Bow, true), -1);
}

TEST(SkinnedAnimation, UnarmedLocomotionUsesSeparateClipsAndNeverSubstitutesBowAttack)
{
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.walk = 1;
	clips.townStand = 2;
	clips.townWalk = 3;
	clips.attack = 4;
	clips.attackRepeat = 5;
	clips.standUnarmed = 6;
	clips.walkUnarmed = 7;
	clips.townStandUnarmed = 8;
	clips.townWalkUnarmed = 9;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Bow, false), 0);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Unarmed, false), 6);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, false, SkinnedEquipmentKind::Unarmed, false), 7);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, true, SkinnedEquipmentKind::Unarmed, false), 8);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Unarmed, false), 9);
	for (auto action : { SkinnedAnimationKind::Attack, SkinnedAnimationKind::Hit, SkinnedAnimationKind::Cast,
	         SkinnedAnimationKind::Death, SkinnedAnimationKind::Block })
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, action, false, SkinnedEquipmentKind::Unarmed, false), -1);
	for (auto action : { SkinnedAnimationKind::Stand, SkinnedAnimationKind::Walk, SkinnedAnimationKind::Attack })
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, action, false, SkinnedEquipmentKind::Unsupported, false), -1);
	clips.townWalkUnarmed = -1;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Unarmed, false), -1);
}

TEST(SkinnedAnimation, ExactBowPartNamesHideEquipmentAndPreserveBodyAndForelock)
{
	// Deliberately reorder the exported meshes and put the rigid quiver at bit 31.
	// Part bits select meshes, independently of the 120 skinned joints.
	constexpr std::array<std::string_view, 7> names {
		"Forelock", "Bow_Animated_String", "Body", "Arrow_Nocked_only",
		"Bow_Leather_Grip_and_Brass_Collars", "Bow_Flexible_Yew_Limbs", "Quiver_Back"
	};
	const auto findPart = [&](std::string_view name) {
		for (size_t i = 0; i < names.size(); ++i)
			if (names[i] == name) return i == 6 ? 31 : static_cast<int>(i);
		return -1;
	};
	const PartMask bowParts = RogueBowPartMask(findPart);
	const uint32_t expected = (1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 31);
	ASSERT_EQ(bowParts, expected);
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Bow, bowParts) & bowParts, bowParts);
	const PartMask unarmed = RogueVisibleParts(SkinnedEquipmentKind::Unarmed, bowParts);
	EXPECT_EQ(unarmed & bowParts, 0U);
	EXPECT_NE(unarmed & (1U << 0), 0U); // forelock
	EXPECT_NE(unarmed & (1U << 2), 0U); // body
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Unsupported, bowParts), 0U);
}

TEST(SkinnedAnimation, MissingOrOutOfRangeBowPartsDoNotHideOtherMeshes)
{
	EXPECT_EQ(RogueBowPartMask([](std::string_view) { return -1; }), 0U);
	EXPECT_EQ(RogueBowPartMask([](std::string_view) { return static_cast<int>(MaxSkinnedParts); }), 0U);
	const PartMask bowParts = RogueBowPartMask([](std::string_view name) {
		return name == "Quiver_Back" ? 7 : -1;
	});
	EXPECT_EQ(bowParts, PartMask { 1 } << 7);
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Unarmed, bowParts), ~(PartMask { 1 } << 7));
}

TEST(SkinnedAnimation, MissingEquipmentFamiliesDoNotUseUnrelatedClips)
{
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.walk = 1;
	clips.townStand = 2;
	clips.townWalk = 3;
	clips.attack = 4;
	clips.attackRepeat = 5;
	// Explicit body reactions and unarmed casts are supported; unsupported held
	// equipment and missing family casts/blocks still fall back to sprites.
	clips.hit = 10;
	clips.cast = 11;
	clips.death = 12;
	clips.block = 13;
	for (bool town : { false, true }) {
		for (auto equipment : { SkinnedEquipmentKind::Bow, SkinnedEquipmentKind::Unarmed, SkinnedEquipmentKind::Unsupported }) {
			for (auto action : { SkinnedAnimationKind::Hit, SkinnedAnimationKind::Cast, SkinnedAnimationKind::Death, SkinnedAnimationKind::Block }) {
				const int expected = action == SkinnedAnimationKind::Death ? clips.death
				    : action == SkinnedAnimationKind::Hit && equipment != SkinnedEquipmentKind::Unsupported ? clips.hit
				    : action == SkinnedAnimationKind::Cast && equipment == SkinnedEquipmentKind::Unarmed ? clips.cast
				                                                                                       : -1;
				EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, action, town, equipment, false), expected);
			}
		}
		for (auto action : { SkinnedAnimationKind::Stand, SkinnedAnimationKind::Walk, SkinnedAnimationKind::Attack })
			EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, action, town, SkinnedEquipmentKind::Unsupported, false), -1);
	}
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, false, SkinnedEquipmentKind::Bow, false), 1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, true, SkinnedEquipmentKind::Bow, false), 2);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Bow, false), 3);
	for (bool town : { false, true }) {
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, town, SkinnedEquipmentKind::Unarmed, false), -1);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, town, SkinnedEquipmentKind::Unarmed, false), -1);
	}
}

TEST(SkinnedAnimation, EachWeaponSelectsItsAuthoredFamilyAndShieldBlock)
{
	SkinnedClipSet clips;
	clips.sword = { 10, 11, 12, 13, 14, 15, -1 };
	clips.swordShield = { 20, 21, 22, 23, 24, 25, 26 };
	clips.mace = { 30, 31, 32, 33, 34, 35, -1 };
	clips.maceShield = { 40, 41, 42, 43, 44, 45, 46 };
	clips.axe = { 50, 51, 52, 53, 54, 55, -1 };
	clips.unarmedShield = { 60, 61, 62, 63, 64, 65, 66 };
	const std::array equipment { SkinnedEquipmentKind::Sword, SkinnedEquipmentKind::SwordShield,
		SkinnedEquipmentKind::Mace, SkinnedEquipmentKind::MaceShield, SkinnedEquipmentKind::Axe, SkinnedEquipmentKind::UnarmedShield };
	for (size_t i = 0; i < equipment.size(); ++i) {
		const int base = static_cast<int>(i + 1) * 10;
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, false, equipment[i], false), base);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, false, equipment[i], false), base + 1);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, true, equipment[i], false), base + 2);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, true, equipment[i], false), base + 3);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, equipment[i], false), base + 4);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, equipment[i], true), base + 5);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, true, equipment[i], false), -1);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Block, true, equipment[i], false), -1);
	}
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Block, false, SkinnedEquipmentKind::SwordShield, false), 26);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Block, false, SkinnedEquipmentKind::Sword, false), -1);
	clips.attackUnarmed = 70;
	clips.attackRepeatUnarmed = 71;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Unarmed, false), 70);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Unarmed, true), 71);
}

TEST(SkinnedAnimation, WeaponVisibilityKeepsOnlySelectedGearAndTownScabbard)
{
	RogueEquipmentParts parts { 0x1FU, 1U << 5, 1U << 6, 1U << 7, 1U << 8, 1U << 9, 1U << 10, true };
	const uint32_t bodyAndHair = (1U << 11) | (1U << 31);
	for (bool town : { false, true }) {
		for (auto equipment : { SkinnedEquipmentKind::Bow, SkinnedEquipmentKind::Unarmed, SkinnedEquipmentKind::UnarmedShield,
		         SkinnedEquipmentKind::Sword, SkinnedEquipmentKind::SwordShield, SkinnedEquipmentKind::Mace, SkinnedEquipmentKind::MaceShield, SkinnedEquipmentKind::Axe }) {
			EXPECT_TRUE(RogueEquipmentAvailable(equipment, parts, town));
			EXPECT_EQ(RogueVisibleParts(equipment, parts, town) & bodyAndHair, bodyAndHair);
		}
	}
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::SwordShield, parts, false) & parts.all(), parts.sword | parts.scabbard | parts.shield);
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::SwordShield, parts, true) & parts.all(), parts.swordStowed | parts.scabbard | parts.shield);
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::MaceShield, parts, true) & parts.all(), parts.mace | parts.shield);
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Axe, parts, false) & parts.all(), parts.axe);
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Unarmed, parts, false) & parts.all(), 0U);
	parts.shield = 0;
	EXPECT_FALSE(RogueEquipmentAvailable(SkinnedEquipmentKind::SwordShield, parts, false));
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::SwordShield, parts, false), 0U);
	EXPECT_TRUE(RogueEquipmentAvailable(SkinnedEquipmentKind::Sword, parts, false));
	parts.swordStowed = 0;
	EXPECT_FALSE(RogueEquipmentAvailable(SkinnedEquipmentKind::Sword, parts, true));
	parts.completeBow = false;
	EXPECT_FALSE(RogueEquipmentAvailable(SkinnedEquipmentKind::Bow, parts, false));
}

TEST(SkinnedAnimation, RogueTownStowsMaceAxeAndBowWhenExported)
{
	const auto findPart = [](std::string_view name) {
		constexpr std::array<std::string_view, 15> names { "Body", "Shield_LEFT", "Mace_RIGHT", "Sword_RIGHT", "Quiver_Back", "Scabbard_SIDE",
			"Sword_Town_Stowed", "Bow_Flexible_Yew_Limbs", "Bow_Leather_Grip_and_Brass_Collars", "Bow_Animated_String", "Arrow_Nocked_only",
			"Axe_TWOHANDED", "Mace_Town_Stowed", "Axe_Town_Stowed", "Bow_Town_Stowed" };
		const auto it = std::find(names.begin(), names.end(), name);
		return it == names.end() ? -1 : static_cast<int>(it - names.begin());
	};
	const RogueEquipmentParts parts = FindRogueEquipmentParts(findPart);
	ASSERT_NE(parts.maceStowed, 0U);
	ASSERT_NE(parts.axeStowed, 0U);
	ASSERT_NE(parts.bowStowed, 0U);
	EXPECT_EQ(parts.all() & 1U, 0U); // body
	const PartMask stowed = parts.maceStowed | parts.axeStowed | parts.bowStowed;
	for (bool town : { false, true }) {
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Mace, parts, town) & parts.all(), town ? parts.maceStowed : parts.mace);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::MaceShield, parts, town) & parts.all(), (town ? parts.maceStowed : parts.mace) | parts.shield);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Axe, parts, town) & parts.all(), town ? parts.axeStowed : parts.axe);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Bow, parts, town) & parts.all(), town ? parts.bowStowed | parts.quiver : parts.bow);
		// Stowed meshes never leak into other sets.
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Unarmed, parts, town) & stowed, 0U);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::SwordShield, parts, town) & stowed, 0U);
		EXPECT_NE(RogueVisibleParts(SkinnedEquipmentKind::Bow, parts, town) & 1U, 0U);
	}
}

TEST(SkinnedAnimation, MeleeDamageRepeatBoundaryAndRecoveryRespectSimulationFrames)
{
	// Sword, mace, kick and shield punch share action frame 10 out of 18.
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(9, 10, 18, false, .90F, .45F, .50F), .45F);
	EXPECT_NEAR(RogueAttackSampleTime(10, 10, 18, false, .90F, .45F, .50F), .50F, 1e-6F);
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(10, 10, 18, true, .90F, .45F, .50F), .50F);
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(18, 10, 18, true, .90F, .45F, .50F), .90F);
	// The two-handed axe has a later hit and a longer natural stop recovery.
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(12, 13, 22, false, 1.10F, .60F, .65F), .60F);
	EXPECT_NEAR(RogueAttackSampleTime(13, 13, 22, false, 1.10F, .60F, .65F), .65F, 1e-6F);
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(13, 13, 22, true, 1.10F, .60F, .65F), .65F);
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(22, 13, 22, true, 1.10F, .60F, .65F), 1.10F);
	EXPECT_FLOAT_EQ(RogueAttackSampleTime(100, 13, 22, true, 1.10F, .60F, .65F), 1.10F);
}

TEST(SkinnedAnimation, WarriorTownWalkSeparatesFreeHandsAndShieldCarriesWithLegacyFallback)
{
	SkinnedClipSet clips;
	clips.stand = 10;
	clips.walk = 11;
	clips.townStand = 12;
	clips.townWalk = 13;
	clips.attack = 14;
	const std::array equipment { SkinnedEquipmentKind::Unarmed, SkinnedEquipmentKind::UnarmedShield,
		SkinnedEquipmentKind::Sword, SkinnedEquipmentKind::SwordShield };
	const auto choose = [&](SkinnedEquipmentKind kind, SkinnedAnimationKind action, bool town) {
		return ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, action, town, kind, false);
	};
	for (auto kind : equipment)
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Walk, true), 13);
	const std::array optional { &clips.townWalkUnarmed, &clips.unarmedShield.townWalk,
		&clips.sword.townWalk, &clips.swordShield.townWalk };
	for (size_t i = 0; i < optional.size(); ++i)
		*optional[i] = 20 + static_cast<int>(i);
	for (size_t i = 0; i < equipment.size(); ++i) {
		const auto kind = equipment[i];
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Walk, true), 20 + static_cast<int>(i));
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Walk, false), 11);
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Stand, true), 12);
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Attack, false), 14);
		*optional[i] = -1;
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Walk, true), 13);
		clips.townWalk = -1;
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Walk, true), 11);
		clips.walk = -1;
		EXPECT_EQ(choose(kind, SkinnedAnimationKind::Walk, true), 10);
		clips.walk = 11;
		clips.townWalk = 13;
		*optional[i] = 20 + static_cast<int>(i);
	}
}

TEST(SkinnedAnimation, WarriorKeepsExistingFallbackAndTownSelection)
{
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.walk = 1;
	clips.attack = 2;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Stand, true, SkinnedEquipmentKind::Unsupported, false), 0);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Unsupported, false), 1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Hit, false, SkinnedEquipmentKind::Unsupported, false), 0);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Unsupported, true), 2);
	clips.townStand = 3;
	clips.townWalk = 4;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Stand, true, SkinnedEquipmentKind::Unsupported, false), 3);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Unsupported, false), 4);
}

TEST(SkinnedAnimation, WarriorBowUsesAuthoredClipsAndRejectsUnsupportedStates)
{
 SkinnedClipSet clips;
 clips.stand=0; clips.walk=1; clips.attack=2; clips.hit=3; clips.cast=4; clips.death=5;
 clips.bow={10,11,12,13,14,15,-1};
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Stand,false,SkinnedEquipmentKind::Bow,false),10);
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Walk,false,SkinnedEquipmentKind::Bow,false),11);
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Stand,true,SkinnedEquipmentKind::Bow,false),12);
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Walk,true,SkinnedEquipmentKind::Bow,false),13);
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Attack,false,SkinnedEquipmentKind::Bow,false),14);
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Attack,false,SkinnedEquipmentKind::Bow,true),15);
 for(auto action:{SkinnedAnimationKind::Hit,SkinnedAnimationKind::Cast,SkinnedAnimationKind::Death,SkinnedAnimationKind::Block})
  EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,action,false,SkinnedEquipmentKind::Bow,false),-1);
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Attack,true,SkinnedEquipmentKind::Bow,false),-1);
 clips.bow={};
 EXPECT_EQ(ChooseSkinnedClip(clips,SkinnedModelKind::Warrior,SkinnedAnimationKind::Stand,false,SkinnedEquipmentKind::Bow,false),-1);
}

TEST(SkinnedAnimation, WarriorBowVisibilityAndAvailability)
{
 WarriorEquipmentParts parts;
 parts.swordRight=2; parts.shield=4; parts.swordStowed=8; parts.scabbard=16;
 parts.maceRight=32; parts.maceStowed=64; parts.axeRight=128; parts.axeStowed=256;
 EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Bow,parts,false));
 parts.bowDrawn=512; parts.quiver=1024; parts.bowStowed=2048; parts.completeBow=true;
 for(bool town:{false,true}) {
  EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Bow,parts,town));
  EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Bow,parts,town,false)&parts.all(),(town?2048U:512U)|1024U);
  EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Bow,parts,town,true)&parts.all(),0U);
  EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Axe,parts,town,false)&(parts.bowDrawn|parts.quiver|parts.bowStowed),0U);
 }
 parts.bowStowed=0;
 EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Bow,parts,true));
 EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Bow,parts,false));
 parts.completeBow=false;
 EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Bow,parts,false));
}

TEST(SkinnedAnimation, WarriorBowReleaseAndRepeatFollowActionFrames)
{
 for(int hit:{9,11,13}) {
  EXPECT_FLOAT_EQ(WarriorBowSampleTime(0,hit,20,false,.8F),0.F);
  EXPECT_FLOAT_EQ(WarriorBowSampleTime(hit-1,hit,20,false,.8F),.5F);
  EXPECT_FLOAT_EQ(WarriorBowSampleTime(hit-1,hit,20,true,.8F),.5F);
  EXPECT_FLOAT_EQ(WarriorBowSampleTime(hit,hit,20,true,.8F),.55F);
  EXPECT_FLOAT_EQ(WarriorBowSampleTime(20,hit,20,true,.8F),.8F);
 }
 EXPECT_NEAR(WarriorBowSampleTime(8.5F,9,20,true,.8F),.525F,1e-6F);
}

TEST(SkinnedAnimation, WarriorAxeUsesTwoHandedClipsAndStowedTownClips)
{
 SkinnedClipSet clips;
 clips.stand = 0; clips.walk = 1; clips.attack = 2;
 clips.axe = { 10, 11, 12, 13, 14, 15, -1 };
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Axe, false), 10);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, false, SkinnedEquipmentKind::Axe, false), 11);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Stand, true, SkinnedEquipmentKind::Axe, false), 12);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Axe, false), 13);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Axe, false), 14);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Axe, true), 15);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, true, SkinnedEquipmentKind::Axe, false), -1);
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Block, false, SkinnedEquipmentKind::Axe, false), -1);
 clips.axe = {};
 EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Axe, false), -1);
}

TEST(SkinnedAnimation, WarriorAxeVisibilityAndLegacyAvailability)
{
 WarriorEquipmentParts parts;
 parts.swordRight=2; parts.shield=4; parts.swordStowed=8; parts.scabbard=16;
 parts.maceRight=32; parts.maceStowed=64;
 EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Axe, parts, false));
 parts.axeRight=128; parts.axeStowed=256;
 for (bool town : { false, true }) {
  EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Axe, parts, town));
  EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Axe,parts,town,false)&parts.all(), town?256U:128U);
  EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Axe,parts,town,true)&parts.all(),0U);
  EXPECT_NE(WarriorVisibleParts(SkinnedEquipmentKind::Axe,parts,town,false)&1U,0U);
 }
 parts.axeStowed=0;
 EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Axe,parts,true));
 EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Axe,parts,false));
}

TEST(SkinnedAnimation, WarriorStaffUsesItsOwnClipsAndParts)
{
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.axe = { 10, 11, 12, 13, 14, 15, -1 };
	clips.staff = { 20, 21, 22, 23, 24, 25, -1 };
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Staff, false), 23);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Staff, true), 25);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Block, false, SkinnedEquipmentKind::Staff, false), -1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::Staff, false), 20); // the Rogue has her own staff clips

	WarriorEquipmentParts parts;
	parts.axeRight = 2;
	parts.axeStowed = 4;
	EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Staff, parts, false)); // older pack: keep the sprite
	parts.staffRight = 8;
	parts.staffStowed = 16;
	for (bool town : { false, true }) {
		EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Staff, parts, town));
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Staff, parts, town, false) & parts.all(), town ? 16U : 8U);
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Staff, parts, town, true) & parts.all(), 0U);
	}	// staffActionFrame 11 of 16: the sweep lands at 0.5 s and a held attack joins at 0.55 s.
	EXPECT_FLOAT_EQ(WarriorStaffSampleTime(10, 11, 16, false, .80F), .50F);
	EXPECT_FLOAT_EQ(WarriorStaffSampleTime(11, 11, 16, true, .80F), .55F);
	EXPECT_FLOAT_EQ(WarriorStaffSampleTime(16, 11, 16, false, .80F), .80F);
}

TEST(SkinnedAnimation, WarriorFamilyHitsPlayWhenThePackHasThemElseTheGenericHit)
{
	const auto choose = [](const SkinnedClipSet &clips, bool town, SkinnedEquipmentKind equipment, SkinnedModelKind kind = SkinnedModelKind::Warrior) {
		return ChooseSkinnedClip(clips, kind, SkinnedAnimationKind::Hit, town, equipment, false);
	};
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.walk = 1;
	clips.hit = 2;
	clips.townHit = 3; // the Warrior's generic town hit is not used: town plays the generic hit
	clips.axe = { 10, 11, 12, 13, 14, 15, -1 };
	clips.staff = { 20, 21, 22, 23, 24, 25, -1 };
	clips.twoHanded = { 30, 31, 32, 33, 34, 35, -1 };
	clips.bow = { 40, 41, 42, 43, 44, 45, -1 };
	// Older packs: the generic hit (the stand without one); the bow keeps its sprite.
	for (bool town : { false, true }) {
		for (auto equipment : { SkinnedEquipmentKind::Axe, SkinnedEquipmentKind::Staff, SkinnedEquipmentKind::TwoHanded })
			EXPECT_EQ(choose(clips, town, equipment), 2);
		EXPECT_EQ(choose(clips, town, SkinnedEquipmentKind::Bow), -1);
	}
	SkinnedClipSet noGeneric = clips;
	noGeneric.hit = -1;
	EXPECT_EQ(choose(noGeneric, false, SkinnedEquipmentKind::Staff), 0);

	// hit_<family>: in the dungeon; town keeps the generic hit until the pack has town_hit_<family>.
	clips.axe.hit = 16;
	clips.staff.hit = 26;
	clips.twoHanded.hit = 36;
	clips.bow.hit = 46;
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Axe), 16);
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Staff), 26);
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::TwoHanded), 36);
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Bow), 46);
	for (auto equipment : { SkinnedEquipmentKind::Axe, SkinnedEquipmentKind::Staff, SkinnedEquipmentKind::TwoHanded })
		EXPECT_EQ(choose(clips, true, equipment), 2);
	EXPECT_EQ(choose(clips, true, SkinnedEquipmentKind::Bow), -1);
	clips.staff.townHit = 27;
	clips.bow.townHit = 47;
	EXPECT_EQ(choose(clips, true, SkinnedEquipmentKind::Staff), 27);
	EXPECT_EQ(choose(clips, true, SkinnedEquipmentKind::Bow), 47);
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Staff), 26);
	// Other families and other actions are unchanged.
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Mace), 2);
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Sword), 2);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Cast, false, SkinnedEquipmentKind::Bow, false), -1);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Staff, false), 24);
	// The Rogue and the Sorcerer already chose their families' hits.
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Staff, SkinnedModelKind::Rogue), 26);
	EXPECT_EQ(choose(clips, true, SkinnedEquipmentKind::Staff, SkinnedModelKind::Rogue), 27);
	EXPECT_EQ(choose(clips, false, SkinnedEquipmentKind::Axe, SkinnedModelKind::Sorcerer), 16);
	EXPECT_EQ(choose(clips, true, SkinnedEquipmentKind::Axe, SkinnedModelKind::Sorcerer), 16);
}

TEST(SkinnedAnimation, WarriorSwordVariantsReplaceTheSwordAndScabbard)
{
	WarriorEquipmentParts parts;
	parts.swordRight = 2;
	parts.swordStowed = 4;
	parts.scabbard = 8;
	parts.frog = 16;
	parts.chape = 32;
	parts.shield = 64;
	const auto visible = [&](SkinnedEquipmentKind kind, bool town, WarriorSwordVariant variant) {
		return WarriorVisibleParts(kind, parts, town, false, { variant }) & parts.all();
	};
	// A pack without the dagger meshes keeps drawing the plain sword.
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false, WarriorSwordVariant::Dagger), 2U | 8U | 16U | 32U);
	parts.swordVariants[0] = { 128, 256, 2048 }; // dagger, with its empty dungeon sheath
	parts.swordVariants[2] = { 512, 1024 };      // scimitar, exported without one
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false, WarriorSwordVariant::Dagger), 128U | 2048U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, true, WarriorSwordVariant::Dagger), 256U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::SwordShield, false, WarriorSwordVariant::Scimitar), 512U | 64U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false, WarriorSwordVariant::Falchion), 2U | 8U | 16U | 32U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false, WarriorSwordVariant::Sword), 2U | 8U | 16U | 32U);
	// Variant meshes never leak into other weapons.
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false, WarriorSwordVariant::Dagger), 0U);
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Sword, parts, false, true, { WarriorSwordVariant::Dagger }) & parts.all(), 0U);
}

TEST(SkinnedAnimation, WarriorShieldVariantsReplaceTheDefaultShieldAboveBit32)
{
	WarriorEquipmentParts parts;
	parts.swordRight = 2;
	parts.shield = 4;
	parts.shieldVariants[2] = PartMask { 1 } << 40; // tower, past the first 32 mesh nodes
	const auto visible = [&](SkinnedEquipmentKind kind, bool town, WarriorShieldVariant shield) {
		return WarriorVisibleParts(kind, parts, town, false, { WarriorSwordVariant::Sword, shield }) & parts.all();
	};
	for (bool town : { false, true }) {
		EXPECT_EQ(visible(SkinnedEquipmentKind::UnarmedShield, town, WarriorShieldVariant::Tower), PartMask { 1 } << 40);
		EXPECT_EQ(visible(SkinnedEquipmentKind::UnarmedShield, town, WarriorShieldVariant::Kite), 4U); // not in this pack
		EXPECT_EQ(visible(SkinnedEquipmentKind::UnarmedShield, town, WarriorShieldVariant::Shield), 4U);
	}
	EXPECT_EQ(visible(SkinnedEquipmentKind::SwordShield, false, WarriorShieldVariant::Tower) & (PartMask { 1 } << 40), PartMask { 1 } << 40);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false, WarriorShieldVariant::Tower), 2U);
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::UnarmedShield, parts, false, true, { WarriorSwordVariant::Sword, WarriorShieldVariant::Tower }) & parts.all(), 0U);
}

TEST(SkinnedAnimation, EquipmentPagesGroupMapsByBaseColourImage)
{
	// Images: 0 body, 1-3 page A (base, normal, mr), 4-6 page B, 7 a normal map shared by an untextured material.
	const std::vector<SkinnedMaterialImages> materials {
		{ 0, -1, -1 }, // body
		{ 1, 2, 3 },   // page A
		{ 4, 5, 6 },   // page B
		{ 1, -1, 3 },  // page A again, metallic-roughness only
		{ -1, 5, -1 }, // untextured, shares page B's normal map
		{ -1, -1, -1 } // untextured factors
	};
	std::vector<SkinnedMaterialPage> assigned;
	const std::vector<SkinnedEquipmentPage> pages = AssignEquipmentPages(materials, 0, assigned);
	ASSERT_EQ(pages.size(), 2U);
	EXPECT_EQ(pages[0].base, 1);
	EXPECT_EQ(pages[0].normal, 2);
	EXPECT_EQ(pages[0].mr, 3);
	EXPECT_EQ(pages[1].base, 4);
	EXPECT_EQ(pages[1].normal, 5);
	EXPECT_EQ(assigned[0].page, -1);
	EXPECT_TRUE(assigned[1].base && assigned[1].normal && assigned[1].mr);
	EXPECT_EQ(assigned[2].page, 1);
	EXPECT_EQ(assigned[3].page, 0);
	EXPECT_FALSE(assigned[3].normal);
	EXPECT_TRUE(assigned[3].mr);
	EXPECT_EQ(assigned[4].page, 1);
	EXPECT_FALSE(assigned[4].base);
	EXPECT_TRUE(assigned[4].normal);
	EXPECT_EQ(assigned[5].page, -1);

	// A single-sheet model keeps one page, as before.
	const std::vector<SkinnedEquipmentPage> one = AssignEquipmentPages({ { 0, -1, -1 }, { 1, 2, 3 }, { 1, 2, 3 } }, 0, assigned);
	EXPECT_EQ(one.size(), 1U);
	EXPECT_EQ(assigned[2].page, 0);
}

TEST(SkinnedAnimation, WarriorMorningStarReplacesTheMace)
{
	WarriorEquipmentParts parts;
	parts.maceRight = 2;
	parts.maceStowed = 4;
	parts.shield = 8;
	const auto visible = [&](SkinnedEquipmentKind kind, bool town, bool star) {
		return WarriorVisibleParts(kind, parts, town, false, { WarriorSwordVariant::Sword, WarriorShieldVariant::Shield, static_cast<uint8_t>(star ? 1 : 0) }) & parts.all();
	};
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false, true), 2U); // pack without the morning star keeps the mace
	parts.maceVariants[0] = { 16, 32 }; // morning star
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false, true), 16U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, true, true), 32U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::MaceShield, false, true), 16U | 8U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false, false), 2U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false, true) & (16U | 32U), 0U);
}

TEST(SkinnedAnimation, RogueBattleAxeReplacesTheAxeAndNeverShowsOtherwise)
{
	constexpr std::array<std::string_view, 6> names { "Body", "Axe_TWOHANDED", "Axe_Town_Stowed", "Battle_Axe_TWOHANDED", "Battle_Axe_Town_Stowed", "Shield_LEFT" };
	const auto findPart = [&](std::string_view name) {
		const auto it = std::find(names.begin(), names.end(), name);
		return it == names.end() ? -1 : static_cast<int>(it - names.begin());
	};
	const RogueEquipmentParts parts = FindRogueEquipmentParts(findPart);
	const PartMask battle = parts.axeVariants[0].held | parts.axeVariants[0].stowed;
	ASSERT_NE(parts.axeVariants[0].held, 0U);
	for (bool town : { false, true }) {
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Axe, parts, town, { WarriorSwordVariant::Sword, WarriorShieldVariant::Shield, 0, 0, 1 }) & parts.all(), town ? parts.axeVariants[0].stowed : parts.axeVariants[0].held);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Axe, parts, town) & parts.all(), town ? parts.axeStowed : parts.axe);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Unarmed, parts, town) & battle, 0U);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::UnarmedShield, parts, town, { WarriorSwordVariant::Sword, WarriorShieldVariant::Shield, 0, 0, 1 }) & battle, 0U);
	}
	EXPECT_NE(RogueVisibleParts(SkinnedEquipmentKind::Unarmed, parts, false) & 1U, 0U); // body
}

TEST(SkinnedAnimation, RogueStaffAndItemVariantsSwapTheirFamiliesMeshes)
{
	constexpr std::array<std::string_view, 16> names { "Body", "Sword_RIGHT", "Sword_Town_Stowed", "Scabbard_SIDE", "Shield_LEFT", "Mace_RIGHT",
		"Mace_Town_Stowed", "Staff_RIGHT", "Staff_Town_Stowed", "Dagger_RIGHT", "Dagger_Town_Stowed", "Dagger_Sheath_Empty",
		"Morning_Star_RIGHT", "Morning_Star_Town_Stowed", "Kite_Shield_LEFT", "Axe_TWOHANDED" };
	const auto findPart = [&](std::string_view name) {
		const auto it = std::find(names.begin(), names.end(), name);
		return it == names.end() ? -1 : static_cast<int>(it - names.begin());
	};
	const RogueEquipmentParts parts = FindRogueEquipmentParts(findPart);
	const auto bit = [&](std::string_view name) { return PartMask { 1 } << findPart(name); };
	const auto visible = [&](SkinnedEquipmentKind kind, bool town, SkinnedItemLook item) { return RogueVisibleParts(kind, parts, town, item) & parts.all(); };

	// Staff: in hand in the dungeon, stowed in town, with its own clips.
	EXPECT_EQ(visible(SkinnedEquipmentKind::Staff, false, {}), bit("Staff_RIGHT"));
	EXPECT_EQ(visible(SkinnedEquipmentKind::Staff, true, {}), bit("Staff_Town_Stowed"));
	SkinnedClipSet clips;
	clips.staff = { 20, 21, 22, 23, 24, 25, -1 };
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Staff, true), 25);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Staff, false), 23);

	// Dagger replaces sword and scabbard; kite replaces the shield.
	const SkinnedItemLook dagger { WarriorSwordVariant::Dagger, WarriorShieldVariant::Kite };
	EXPECT_EQ(visible(SkinnedEquipmentKind::SwordShield, false, dagger), bit("Dagger_RIGHT") | bit("Dagger_Sheath_Empty") | bit("Kite_Shield_LEFT"));
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, true, dagger), bit("Dagger_Town_Stowed"));
	// Missing variants (Falchion, Tower) keep the defaults.
	const SkinnedItemLook missing { WarriorSwordVariant::Falchion, WarriorShieldVariant::Tower };
	EXPECT_EQ(visible(SkinnedEquipmentKind::SwordShield, false, missing), bit("Sword_RIGHT") | bit("Scabbard_SIDE") | bit("Shield_LEFT"));

	// Morning star swaps the mace, in hand and stowed.
	SkinnedItemLook star;
	star.mace = 1;
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false, star), bit("Morning_Star_RIGHT"));
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, true, star), bit("Morning_Star_Town_Stowed"));
	EXPECT_EQ(visible(SkinnedEquipmentKind::Unarmed, false, star), 0U);
}

TEST(SkinnedAnimation, EverySwordVariantNameMapsToItsOwnSlot)
{
	static_assert(WarriorSwordVariantNames.size() == static_cast<size_t>(WarriorSwordVariant::BastardSword));
	WarriorEquipmentParts parts;
	parts.swordRight = 1U << 1;
	for (size_t i = 0; i < WarriorSwordVariantNames.size(); ++i)
		parts.swordVariants[i] = { PartMask { 1 } << (10 + 3 * i), PartMask { 1 } << (11 + 3 * i), PartMask { 1 } << (12 + 3 * i) };
	const auto visible = [&](WarriorSwordVariant variant) { return WarriorVisibleParts(SkinnedEquipmentKind::Sword, parts, false, false, { variant }) & parts.all(); };
	EXPECT_EQ(visible(WarriorSwordVariant::BastardSword), (PartMask { 1 } << 37) | (PartMask { 1 } << 39)); // slot 9: right + empty sheath
	EXPECT_EQ(visible(WarriorSwordVariant::ShortSword), (PartMask { 1 } << 19) | (PartMask { 1 } << 21));
}

TEST(SkinnedAnimation, PartMasksReach128PartsAndSplitIntoShaderWords)
{
	static_assert(MaxSkinnedParts == 128);
	EXPECT_EQ(PartMask { 1 } << 0, PartMask(1, 0));
	EXPECT_EQ(PartMask { 1 } << 63, PartMask(uint64_t { 1 } << 63, 0));
	EXPECT_EQ(PartMask { 1 } << 64, PartMask(0, 1));
	EXPECT_EQ(PartMask { 1 } << 127, PartMask(0, uint64_t { 1 } << 63));
	EXPECT_EQ(PartMask { 1 } << 128, PartMask {});
	const PartMask mask = (PartMask { 1 } << 5) | (PartMask { 1 } << 40) | (PartMask { 1 } << 70) | (PartMask { 1 } << 100);
	EXPECT_EQ(mask.word(0), 1U << 5);
	EXPECT_EQ(mask.word(1), 1U << 8);
	EXPECT_EQ(mask.word(2), 1U << 6);
	EXPECT_EQ(mask.word(3), 1U << 4);
	EXPECT_EQ((~mask & mask), PartMask {});

	// A shield variant at part 100 still replaces the default shield.
	WarriorEquipmentParts parts;
	parts.shield = 4;
	parts.shieldVariants[1] = PartMask { 1 } << 100; // kite
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::UnarmedShield, parts, false, false, { WarriorSwordVariant::Sword, WarriorShieldVariant::Kite }) & parts.all(),
	    PartMask { 1 } << 100);
}

TEST(SkinnedAnimation, MaceStaffAndAxeFamiliesSwapTheirOwnVariants)
{
	WarriorEquipmentParts parts;
	parts.maceRight = 2;
	parts.maceStowed = 4;
	parts.staffRight = 8;
	parts.staffStowed = 16;
	parts.axeRight = 32;
	parts.axeStowed = 64;
	parts.maceVariants[1] = { 128, 256 };   // club
	parts.staffVariants[2] = { 512, 1024 }; // war staff
	parts.axeVariants[4] = { 2048, 4096 };  // great axe
	SkinnedItemLook item;
	item.mace = 2;
	item.staff = 3;
	item.axe = 5;
	const auto visible = [&](SkinnedEquipmentKind kind, bool town) { return WarriorVisibleParts(kind, parts, town, false, item) & parts.all(); };
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false), 128U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, true), 256U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Staff, false), 512U);
	EXPECT_EQ(visible(SkinnedEquipmentKind::Axe, true), 4096U);
	// A variant the pack lacks (spiked club) keeps the default mace.
	item.mace = 3;
	EXPECT_EQ(visible(SkinnedEquipmentKind::Mace, false), 2U);
	// Variant meshes never show for other families.
	EXPECT_EQ(visible(SkinnedEquipmentKind::Sword, false) & (128U | 512U | 2048U), 0U);
	// The stowed-only mask leaves the held variants visible in first person.
	EXPECT_EQ(HeldVariantMask(parts.maceVariants, true), 256U);
}

TEST(SkinnedAnimation, WarriorAxeImpactAndRepeatJoinFollowActionFrames)
{
 for(int actionFrame : { 9, 11, 13 }) {
  EXPECT_FLOAT_EQ(WarriorAxeSampleTime(0,actionFrame,20,false,1.F),0.F);
  EXPECT_FLOAT_EQ(WarriorAxeSampleTime(actionFrame-1,actionFrame,20,false,1.F),.45F);
  EXPECT_FLOAT_EQ(WarriorAxeSampleTime(actionFrame-1,actionFrame,20,true,1.F),.45F);
  EXPECT_FLOAT_EQ(WarriorAxeSampleTime(actionFrame,actionFrame,20,true,1.F),.5F);
  EXPECT_FLOAT_EQ(WarriorAxeSampleTime(20,actionFrame,20,true,1.F),1.F);
 }
 EXPECT_NEAR(WarriorAxeSampleTime(8.5F,9,20,true,1.F),.475F,1e-6F);
}

TEST(SkinnedAnimation, WarriorMaceSelectsEquipmentClipsAndSharedStowedTownWalk)
{
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.walk = 1;
	clips.attack = 2;
	clips.townStand = 3;
	clips.townWalk = 4;
	clips.mace = { 10, 11, 12, 13, 14, 15, -1 };
	clips.maceShield = { 20, 21, -1, -1, 24, 25, -1 };
	for (auto equipment : { SkinnedEquipmentKind::Mace, SkinnedEquipmentKind::MaceShield }) {
		const bool shield = equipment == SkinnedEquipmentKind::MaceShield;
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Stand, false, equipment, false), shield ? 20 : 10);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, false, equipment, false), shield ? 21 : 11);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Stand, true, equipment, false), 12);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, true, equipment, false), 13);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, equipment, false), shield ? 24 : 14);
		EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, equipment, true), shield ? 25 : 15);
	}
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::SwordShield, true), 2);
	clips.mace = {};
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::Mace, false), 4);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::Mace, true), 2);
}

TEST(SkinnedAnimation, WarriorMaceVisibilityStowsWeaponAndHidesSwordSheath)
{
	WarriorEquipmentParts parts;
	parts.swordRight = 1U << 1;
	parts.shield = 1U << 2;
	parts.swordStowed = 1U << 3;
	parts.scabbard = 1U << 4;
	parts.frog = 1U << 5;
	parts.chape = 1U << 6;
	parts.maceRight = 1U << 7;
	parts.maceStowed = 1U << 8;
	for (bool town : { false, true }) {
		const PartMask mace = town ? parts.maceStowed : parts.maceRight;
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Mace, parts, town, false) & parts.all(), mace);
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::MaceShield, parts, town, false) & parts.all(), mace | parts.shield);
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::SwordShield, parts, town, false) & parts.all(), (town ? parts.swordStowed : parts.swordRight) | parts.scabbard | parts.frog | parts.chape | parts.shield);
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Unarmed, parts, town, false) & parts.all(), 0U);
		EXPECT_NE(WarriorVisibleParts(SkinnedEquipmentKind::Mace, parts, town, false) & (1U << 0), 0U); // body
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::MaceShield, parts, town, true) & parts.all(), 0U);
	}
}

TEST(SkinnedAnimation, OlderWarriorPacksWithoutMaceMeshesKeepSpriteFallback)
{
	WarriorEquipmentParts parts;
	parts.swordRight = 1;
	parts.shield = 2;
	EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Mace, parts, false));
	EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::MaceShield, parts, true));
	EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::SwordShield, parts, false));
	parts.maceRight = 4;
	EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::MaceShield, parts, false));
	EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Mace, parts, true));
	parts.maceStowed = 8;
	EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::MaceShield, parts, true));
	parts.shield = 0;
	EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::MaceShield, parts, false));
	EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Mace, parts, false));
}

TEST(SkinnedAnimation, WarriorMaceImpactAndRepeatJoinFollowActionFrames)
{
	for (int actionFrame : { 7, 9, 12 }) {
		EXPECT_FLOAT_EQ(WarriorMaceSampleTime(0, actionFrame, 16, false, .8F), 0);
		EXPECT_FLOAT_EQ(WarriorMaceSampleTime(actionFrame - 1, actionFrame, 16, false, .8F), .4F);
		EXPECT_FLOAT_EQ(WarriorMaceSampleTime(actionFrame - 1, actionFrame, 16, true, .8F), .4F);
		EXPECT_FLOAT_EQ(WarriorMaceSampleTime(actionFrame, actionFrame, 16, true, .8F), .45F);
		EXPECT_FLOAT_EQ(WarriorMaceSampleTime(16, actionFrame, 16, true, .8F), .8F);
	}
	EXPECT_NEAR(WarriorMaceSampleTime(8.5F, 9, 16, true, .8F), .425F, 1e-6F);
	EXPECT_FLOAT_EQ(WarriorMaceSampleTime(100, 9, 16, true, .8F), .8F);
}

TEST(SkinnedAnimation, MissingAssetAndDisabledOptionNeverHideSpriteFallback)
{
	EXPECT_TRUE(SkinnedPoseAvailable(true, true, false, 0));
	EXPECT_FALSE(SkinnedPoseAvailable(false, true, false, 0));
	EXPECT_FALSE(SkinnedPoseAvailable(true, false, false, 0));
	EXPECT_FALSE(SkinnedPoseAvailable(true, true, true, 0));
	EXPECT_FALSE(SkinnedPoseAvailable(true, true, false, -1));
}

TEST(SkinnedAnimation, BowReleaseAndQueuedJoinUseGameplayFrames)
{
	EXPECT_FLOAT_EQ(RogueBowSampleTime(0, 7, 12, false, .60F), 0);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(6, 7, 12, false, .60F), .30F);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(7, 7, 12, false, .60F), .35F);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(12, 7, 12, false, .60F), .60F);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(0, 7, 12, true, .60F), 0);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(6, 7, 12, true, .60F), .30F);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(7, 7, 12, true, .60F), .35F);
	EXPECT_NEAR(RogueBowSampleTime(6.5F, 7, 12, true, .60F), .325F, 1e-6F);
	// Continuing after the release samples the authored hold tail, never wraps into a phantom second shot.
	EXPECT_FLOAT_EQ(RogueBowSampleTime(12, 7, 12, true, .60F), .60F);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(100, 7, 12, true, .60F), .60F);
	EXPECT_FLOAT_EQ(RogueBowSampleTime(-1, 7, 12, true, .60F), 0);
}

TEST(SkinnedAnimation, JointMasksCoverThe256EntryBudget)
{
	// The Sorcerer's spring chains plus the 55-joint bow rig need more than 128 entries.
	static_assert(MaxSkinnedJoints == 256);
	static_assert(SkinnedJointMaskWords == 8);
	std::array<uint32_t, SkinnedJointMaskWords> mask {};
	for (size_t entry : { 0U, 31U, 32U, 119U, 127U, 128U, 168U, 255U })
		SetSkinnedJointBit(mask, entry);
	EXPECT_EQ(mask[0], 0x80000001U);
	EXPECT_EQ(mask[1], 0x00000001U);
	EXPECT_EQ(mask[3], 0x80800000U);
	EXPECT_EQ(mask[4], 0x00000001U);
	EXPECT_EQ(mask[5], 0x00000100U);
	EXPECT_EQ(mask[7], 0x80000000U);
	auto before = mask;
	SetSkinnedJointBit(mask, 256);
	EXPECT_EQ(mask, before);
}
TEST(SkinnedAnimation, TwoHandersUseTheirOwnClipsAndMeshesOrKeepTheSprite)
{
	SkinnedClipSet clips;
	clips.stand = 0;
	clips.walk = 1;
	clips.attack = 2;
	clips.axe = { 10, 11, 12, 13, 14, 15, -1 };
	clips.twoHanded = { 30, 31, 12, 13, 34, 35, -1 }; // town carry reuses the axe's back-carry clips
	for (auto kind : { SkinnedModelKind::Warrior, SkinnedModelKind::Rogue }) {
		EXPECT_EQ(ChooseSkinnedClip(clips, kind, SkinnedAnimationKind::Stand, false, SkinnedEquipmentKind::TwoHanded, false), 30);
		EXPECT_EQ(ChooseSkinnedClip(clips, kind, SkinnedAnimationKind::Walk, true, SkinnedEquipmentKind::TwoHanded, false), 13);
		EXPECT_EQ(ChooseSkinnedClip(clips, kind, SkinnedAnimationKind::Attack, false, SkinnedEquipmentKind::TwoHanded, true), 35);
		EXPECT_EQ(ChooseSkinnedClip(clips, kind, SkinnedAnimationKind::Block, false, SkinnedEquipmentKind::TwoHanded, false), -1);
	}
	clips.cast = 5;
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Cast, false, SkinnedEquipmentKind::TwoHanded, false), 5);
	clips.twoHanded.cast = 36; // its own cast keeps the long weapon out of the leg and floor
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Cast, false, SkinnedEquipmentKind::TwoHanded, false), 36);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Warrior, SkinnedAnimationKind::Cast, false, SkinnedEquipmentKind::Axe, false), 5);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Cast, false, SkinnedEquipmentKind::TwoHanded, false), 36);
	EXPECT_EQ(ChooseSkinnedClip(clips, SkinnedModelKind::Rogue, SkinnedAnimationKind::Cast, false, SkinnedEquipmentKind::Axe, false), -1);

	constexpr std::array<std::string_view, 9> names { "Body", "Sword_RIGHT", "Mace_RIGHT", "Two_Handed_Sword_RIGHT", "Two_Handed_Sword_Town_Stowed",
		"Two_Handed_Sword_Sheath_Empty", "Maul_RIGHT", "Maul_Town_Stowed", "Great_Sword_RIGHT" };
	const auto findPart = [&](std::string_view name) {
		const auto it = std::find(names.begin(), names.end(), name);
		return it == names.end() ? -1 : static_cast<int>(it - names.begin());
	};
	const auto bit = [&](std::string_view name) { return PartMask { 1 } << findPart(name); };
	const RogueEquipmentParts rogue = FindRogueEquipmentParts(findPart);
	WarriorEquipmentParts warrior;
	warrior.swordRight = bit("Sword_RIGHT");
	warrior.maceRight = bit("Mace_RIGHT");
	for (size_t i = 0; i < TwoHandedVariantNames.size(); ++i) {
		const auto &own = rogue.twoHandedVariants[i];
		warrior.twoHandedVariants[i] = { own.right, own.stowed, own.sheath };
	}

	SkinnedItemLook sword;
	sword.twoHanded = 1;
	SkinnedItemLook maul;
	maul.twoHanded = 3;
	SkinnedItemLook greatSword;
	greatSword.twoHanded = 2; // no town mesh in this pack
	for (bool town : { false, true }) {
		const PartMask swordParts = town ? bit("Two_Handed_Sword_Town_Stowed") : bit("Two_Handed_Sword_RIGHT") | bit("Two_Handed_Sword_Sheath_Empty");
		EXPECT_TRUE(WarriorEquipmentAvailable(SkinnedEquipmentKind::TwoHanded, warrior, town, sword));
		EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::TwoHanded, warrior, town, false, sword) & warrior.all(), swordParts);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::TwoHanded, rogue, town, sword) & rogue.all(), swordParts);
		EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::TwoHanded, rogue, town, maul) & rogue.all(), bit(town ? "Maul_Town_Stowed" : "Maul_RIGHT"));
		EXPECT_EQ(WarriorEquipmentAvailable(SkinnedEquipmentKind::TwoHanded, warrior, town, greatSword), !town);
		EXPECT_EQ(RogueEquipmentAvailable(SkinnedEquipmentKind::TwoHanded, rogue, town, greatSword), !town);
		// An item or pack without its own mesh keeps the sprite instead of a one-handed socket.
		EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::TwoHanded, warrior, town, {}));
		EXPECT_FALSE(WarriorEquipmentAvailable(SkinnedEquipmentKind::Unsupported, warrior, town));
		EXPECT_FALSE(RogueEquipmentAvailable(SkinnedEquipmentKind::TwoHanded, rogue, town, {}));
	}
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::TwoHanded, warrior, false, true, sword) & warrior.all(), 0U);
}


TEST(SkinnedAnimation, BowVariantsSwapMeshesAndOverlayTheirFlexOnlyOnBowAttacks)
{
	constexpr std::array<std::string_view, 9> names { "Body", "Bow_Flexible_Yew_Limbs", "Bow_Leather_Grip_and_Brass_Collars", "Bow_Animated_String",
		"Arrow_Nocked_only", "Quiver_Back", "Bow_Town_Stowed", "Long_War_Bow_LEFT", "Long_War_Bow_Town_Stowed" };
	const auto findPart = [&](std::string_view name) {
		const auto it = std::find(names.begin(), names.end(), name);
		return it == names.end() ? -1 : static_cast<int>(it - names.begin());
	};
	const auto bit = [&](std::string_view name) { return PartMask { 1 } << findPart(name); };
	RogueEquipmentParts rogue = FindRogueEquipmentParts(findPart);
	SkinnedItemLook longWar;
	longWar.bow = 7;
	const PartMask defaultBow = bit("Bow_Flexible_Yew_Limbs") | bit("Bow_Leather_Grip_and_Brass_Collars") | bit("Bow_Animated_String") | bit("Arrow_Nocked_only") | bit("Quiver_Back");
	// Without both flex overlays the variant would bend wrongly: keep the default bow.
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Bow, rogue, false, longWar) & rogue.all(), defaultBow);
	rogue.bowReady[6] = true;
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Bow, rogue, false, longWar) & rogue.all(), bit("Long_War_Bow_LEFT") | bit("Arrow_Nocked_only") | bit("Quiver_Back"));
	EXPECT_EQ(RogueVisibleParts(SkinnedEquipmentKind::Bow, rogue, true, longWar) & rogue.all(), bit("Long_War_Bow_Town_Stowed") | bit("Quiver_Back"));

	WarriorEquipmentParts warrior;
	warrior.bowDrawn = bit("Bow_Flexible_Yew_Limbs");
	warrior.bowStowed = bit("Bow_Town_Stowed");
	warrior.quiver = bit("Quiver_Back");
	warrior.bowArrow = bit("Arrow_Nocked_only");
	warrior.bowVariants = rogue.bowVariants;
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Bow, warrior, false, false, longWar) & warrior.all(), bit("Bow_Flexible_Yew_Limbs") | bit("Quiver_Back"));
	warrior.bowReady[6] = true;
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Bow, warrior, false, false, longWar) & warrior.all(), bit("Long_War_Bow_LEFT") | bit("Arrow_Nocked_only") | bit("Quiver_Back"));
	EXPECT_EQ(WarriorVisibleParts(SkinnedEquipmentKind::Bow, warrior, true, false, longWar) & warrior.all(), bit("Long_War_Bow_Town_Stowed") | bit("Quiver_Back"));

	SkinnedClipSet clips;
	clips.attack = 2;
	clips.attackRepeat = 3;
	clips.bow = { 10, 11, 12, 13, 14, 15, -1 };
	clips.bowFlex[6] = { 40, 41 };
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Warrior, 14, 7), 40);
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Warrior, 15, 7), 41);
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Warrior, 10, 7), -1); // never on stand, walk or town
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Rogue, 2, 7), 40);
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Rogue, 3, 7), 41);
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Rogue, 2, 0), -1);
	EXPECT_EQ(BowFlexOverlay(clips, SkinnedModelKind::Rogue, 2, 1), -1);
}

} // namespace
} // namespace devilution::render3d
