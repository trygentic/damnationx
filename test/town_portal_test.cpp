/**
 * @file town_portal_test.cpp
 *
 * The permanent portal to the Cathedral beside the town fountain (Gameplay option "Cathedral Portal").
 */
#include <gtest/gtest.h>

#include "control/control.hpp"
#include "cursor.h"
#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "levels/trigs.h"
#include "multi.h"
#include "options.h"
#include "player.h"
#include "quests.h"
#include "tables/townerdat.hpp"
#include "utils/paths.h"

namespace devilution {
namespace {

class TownPortalTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		paths::SetAssetsPath(paths::BasePath() + "/assets/");
		Players.resize(1);
		MyPlayerId = 0;
		MyPlayer = &Players[MyPlayerId];
		*MyPlayer = {};
		leveltype = DTYPE_TOWN;
		currlevel = 0;
		setlevel = false;
		gbIsSpawn = false;
		gbIsHellfire = false;
		gbIsMultiplayer = false;
		GetOptions().Gameplay.cathedralPortal.SetValue(true);
	}

	void TearDown() override
	{
		gbIsMultiplayer = false;
		GetOptions().Gameplay.cathedralPortal.SetValue(true);
	}

	static const TriggerStruct *FindTrigger(Point position)
	{
		for (int i = 0; i < numtrigs; i++) {
			if (trigs[i].position == position)
				return &trigs[i];
		}
		return nullptr;
	}
};

TEST_F(TownPortalTest, OnByDefault)
{
	EXPECT_TRUE(*GetOptions().Gameplay.cathedralPortal);
}

TEST_F(TownPortalTest, TriggerBesideTheFountainLeadsToLevel1)
{
	InitTownTriggers();
	const TriggerStruct *portal = FindTrigger(TownCathedralPortalPosition);
	ASSERT_NE(portal, nullptr);
	EXPECT_EQ(portal->_tmsg, WM_DIABNEXTLVL);

	// The Cathedral stairs are still there too.
	const TriggerStruct *stairs = FindTrigger({ 25, 29 });
	ASSERT_NE(stairs, nullptr);
	EXPECT_EQ(stairs->_tmsg, WM_DIABNEXTLVL);
}

TEST_F(TownPortalTest, OptionOffRemovesTheTrigger)
{
	GetOptions().Gameplay.cathedralPortal.SetValue(false);
	InitTownTriggers();
	EXPECT_EQ(FindTrigger(TownCathedralPortalPosition), nullptr);
	EXPECT_NE(FindTrigger({ 25, 29 }), nullptr);
}

TEST_F(TownPortalTest, MultiplayerKeepsEveryTriggerAndThePortal)
{
	// Multiplayer opens every town warp, so this is the fullest town trigger table.
	gbIsMultiplayer = true;
	InitTownTriggers();
	EXPECT_LE(numtrigs, MAXTRIGGERS);
	const TriggerStruct *portal = FindTrigger(TownCathedralPortalPosition);
	ASSERT_NE(portal, nullptr);
	EXPECT_EQ(portal->_tmsg, WM_DIABNEXTLVL);
}

TEST_F(TownPortalTest, PortalIsNotOnATownerOrASpawnTile)
{
	LoadTownerData();
	for (const TownerDataEntry &towner : TownersDataEntries) {
		EXPECT_GE(Point(TownCathedralPortalPosition).WalkingDistance(towner.position), 2) << towner.name;
	}
	// Multiplayer new-game spawn tiles (multi.cpp) and the town portal drop spots (portal.cpp).
	for (const Point spawn : { Point { 75, 68 }, Point { 77, 70 }, Point { 75, 70 }, Point { 77, 68 }, Point { 76, 69 },
	         Point { 75, 69 }, Point { 76, 68 }, Point { 77, 69 }, Point { 76, 70 }, Point { 57, 40 }, Point { 59, 40 },
	         Point { 61, 40 }, Point { 63, 40 } }) {
		EXPECT_NE(TownCathedralPortalPosition, spawn);
	}
}

TEST_F(TownPortalTest, HoverTextNamesTheCathedral)
{
	InitTownTriggers();
	cursPosition = TownCathedralPortalPosition + Displacement { -1, -1 }; // over the portal's body
	CheckTrigForce();
	EXPECT_TRUE(trigflag);
	EXPECT_EQ(cursPosition, TownCathedralPortalPosition);
	EXPECT_NE(InfoString.str().find("Portal to"), std::string_view::npos) << InfoString.str();
	EXPECT_NE(InfoString.str().find("Cathedral"), std::string_view::npos) << InfoString.str();
}

TEST_F(TownPortalTest, StandingOnThePortalEntersLevel1)
{
	InitTownTriggers();
	Player &player = *MyPlayer;
	player.plractive = true;
	player.setLevel(0);
	player._pmode = PM_STAND;
	player.position.tile = TownCathedralPortalPosition;
	player.position.future = TownCathedralPortalPosition;
	player.position.old = TownCathedralPortalPosition;

	CheckTriggers();

	EXPECT_EQ(player._pmode, PM_NEWLVL);
	EXPECT_EQ(player.plrlevel, 1);
	EXPECT_FALSE(player.plrIsOnSetLevel);
}

TEST_F(TownPortalTest, StandingNextToThePortalDoesNothing)
{
	InitTownTriggers();
	Player &player = *MyPlayer;
	player.plractive = true;
	player.setLevel(0);
	player._pmode = PM_STAND;
	player.position.tile = TownCathedralPortalPosition + Displacement { 1, 0 };

	CheckTriggers();

	EXPECT_EQ(player._pmode, PM_STAND);
	EXPECT_EQ(player.plrlevel, 0);
}

TEST_F(TownPortalTest, PortalTileIsWalkableBesideTheFountain)
{
	LoadCoreArchives();
	LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
	LoadQuestData();
	InitQuests();
	pMegaTiles = LoadFileInMem<MegaTile>("levels\\towndata\\town.til");
	ASSERT_NE(pMegaTiles, nullptr);
	ASSERT_TRUE(LoadLevelSOLData().has_value());
	CreateTown(ENTRY_MAIN);
	// The portal and the tiles around it can be walked on, so it can be reached from every side.
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const Point tile = TownCathedralPortalPosition + Displacement { dx, dy };
			EXPECT_FALSE(TileHasAny(tile, TileProperties::Solid)) << tile.x << "," << tile.y;
		}
	}
	// The fountain (CleanTownFountain repaints the 2x2 tiles at 60,70) is close by.
	EXPECT_LE(Point(TownCathedralPortalPosition).WalkingDistance(Point { 61, 70 }), 4);
	pMegaTiles = nullptr;
}

} // namespace
} // namespace devilution
