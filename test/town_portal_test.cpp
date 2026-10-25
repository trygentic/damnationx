/**
 * @file town_portal_test.cpp
 *
 * The free section connection is a native missile/warp, never a second invisible town trigger.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "engine/assets.hpp"
#include "engine/load_file.hpp"
#include "headless_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "levels/trigs.h"
#include "missiles.h"
#include "multi.h"
#include "options.h"
#include "player.h"
#include "portal.h"
#include "quests.h"
#include "tables/misdat.h"
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
		MyPlayer->setLevel(0);
		leveltype = DTYPE_TOWN;
		currlevel = 0;
		setlevel = false;
		gbIsSpawn = false;
		gbIsHellfire = false;
		gbIsMultiplayer = false;
		previousHeadless = HeadlessMode;
		HeadlessMode = true;
		sgGameInitInfo.bPermanentCathedralPortal = 1;
		GetOptions().Gameplay.cathedralPortal.SetValue(true);
		Missiles.clear();
		InitPortals();
		LoadMissileData();
	}

	void TearDown() override
	{
		Missiles.clear();
		InitPortals();
		HeadlessMode = previousHeadless;
		gbIsMultiplayer = false;
		GetOptions().Gameplay.cathedralPortal.SetValue(true);
		sgGameInitInfo.bPermanentCathedralPortal = 0;
	}

	static const TriggerStruct *FindTrigger(Point position)
	{
		for (int i = 0; i < numtrigs; i++) {
			if (trigs[i].position == position)
				return &trigs[i];
		}
		return nullptr;
	}

private:
	bool previousHeadless = false;
};

// Synthetic geometry keeps these regressions independent of proprietary game data.
class PortalGeometryGuard {
public:
	PortalGeometryGuard()
	{
		std::memcpy(pieces.data(), dPiece, sizeof(dPiece));
		std::memcpy(flags.data(), dFlags, sizeof(dFlags));
		std::memset(dFlags, 0, sizeof(dFlags));
		std::memcpy(players.data(), dPlayer, sizeof(dPlayer));
		std::memcpy(monsters.data(), dMonster, sizeof(dMonster));
		std::memcpy(objects.data(), dObject, sizeof(dObject));
		std::memcpy(triggers.data(), trigs, sizeof(trigs));
		std::memset(dPlayer, 0, sizeof(dPlayer));
		std::memset(dMonster, 0, sizeof(dMonster));
		std::memset(dObject, 0, sizeof(dObject));
		SOLData[0] = TileProperties::Solid | TileProperties::BlockMissile;
		SOLData[1] = {};
	}
	~PortalGeometryGuard()
	{
		std::memcpy(dPiece, pieces.data(), sizeof(dPiece));
		std::memcpy(dFlags, flags.data(), sizeof(dFlags));
		std::memcpy(dPlayer, players.data(), sizeof(dPlayer));
		std::memcpy(dMonster, monsters.data(), sizeof(dMonster));
		std::memcpy(dObject, objects.data(), sizeof(dObject));
		std::memcpy(trigs, triggers.data(), sizeof(trigs));
		numtrigs = triggerCount;
		SOLData[0] = solid0;
		SOLData[1] = solid1;
	}
private:
	std::array<std::byte, sizeof(dPiece)> pieces;
	std::array<std::byte, sizeof(dFlags)> flags;
	std::array<std::byte, sizeof(dPlayer)> players;
	std::array<std::byte, sizeof(dMonster)> monsters;
	std::array<std::byte, sizeof(dObject)> objects;
	std::array<std::byte, sizeof(trigs)> triggers;
	int triggerCount = numtrigs;
	TileProperties solid0 = SOLData[0];
	TileProperties solid1 = SOLData[1];
};

TEST_F(TownPortalTest, DungeonEndpointAvoidsStairArrivalAndPrefersScreenLeft)
{
	PortalGeometryGuard geometry;
	leveltype = DTYPE_CATHEDRAL;
	currlevel = 1;
	for (const Point stairs : { Point { 30, 30 }, Point { 55, 72 }, Point { 80, 40 } }) {
		SCOPED_TRACE(stairs.x);
		for (auto &column : dPiece)
			for (auto &tile : column)
				tile = 1;
		numtrigs = 1;
		trigs[0].position = stairs;
		trigs[0]._tmsg = WM_DIABPREVLVL;
		ViewPosition = stairs + Displacement { 0, 2 };
		const Point portal = CathedralPortalPosition();
		EXPECT_EQ(portal, stairs + Displacement(-6, 6));
		EXPECT_GE(portal.WalkingDistance(stairs), 6);
		EXPECT_GE(portal.WalkingDistance(ViewPosition), 4);
		// Neither player occupancy nor the current camera may move a shared endpoint.
		dPlayer[portal.x][portal.y] = 1;
		ViewPosition = { 20, 20 };
		EXPECT_EQ(CathedralPortalPosition(), portal);
		dPlayer[portal.x][portal.y] = 0;
		SetCurrentPortal(CathedralPortalIndex);
		GetPortalLvlPos();
		EXPECT_GE(ViewPosition.WalkingDistance(portal), 2);
		EXPECT_GE(ViewPosition.WalkingDistance(stairs), 4);
	}
}

TEST_F(TownPortalTest, DungeonEndpointFindsReachableFloorWhenLeftSideIsBlocked)
{
	PortalGeometryGuard geometry;
	leveltype = DTYPE_CATHEDRAL;
	currlevel = 1;
	const Point stairs { 50, 50 };
	numtrigs = 1;
	trigs[0].position = stairs;
	trigs[0]._tmsg = WM_DIABPREVLVL;
	for (auto &column : dPiece)
		for (auto &tile : column)
			tile = 0;
	// The only connected room runs to screen-right; an isolated left-hand room
	// must not win just because it is closest to the preferred position.
	for (int x = 50; x <= 68; ++x)
		for (int y = 48; y <= 52; ++y)
			dPiece[x][y] = 1;
	for (int x = 41; x <= 45; ++x)
		for (int y = 54; y <= 58; ++y)
			dPiece[x][y] = 1;
	ViewPosition = stairs + Displacement { 1, 0 };
	const Point portal = CathedralPortalPosition();
	EXPECT_GE(portal.x, 56);
	EXPECT_GE(portal.WalkingDistance(stairs), 6);
	EXPECT_FALSE(TileHasAny(portal, TileProperties::Solid | TileProperties::BlockMissile));
}

TEST_F(TownPortalTest, GeneratedCathedralEntrancesKeepPortalAwayAcrossSeeds)
{
	LoadCoreArchives();
	LoadGameArchives();
	if (!HaveMainData())
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
	leveltype = DTYPE_CATHEDRAL;
	currlevel = 1;
	MyPlayer->pOriginalCathedral = true;
	LoadQuestData();
	InitQuests();
	pMegaTiles = LoadFileInMem<MegaTile>("levels\\l1data\\l1.til");
	ASSERT_NE(pMegaTiles, nullptr);
	ASSERT_TRUE(LoadLevelSOLData().has_value());
	for (uint32_t seed : { 2588U, 743271966U, 401921334U, 536340718U, 1U, 42U, 12345U, 0xDEADBEEFU }) {
		SCOPED_TRACE(seed);
		LevelSeeds[1] = std::nullopt;
		CreateDungeon(seed, ENTRY_MAIN);
		InitL1Triggers();
		Freeupstairs();
		const Point arrival = ViewPosition;
		const Point portal = CathedralPortalPosition();
		EXPECT_GE(portal.WalkingDistance(arrival), 3);
		EXPECT_FALSE(TileHasAny(portal, TileProperties::Solid | TileProperties::BlockMissile));
		EXPECT_FALSE(TileContainsSetPiece(portal));
		for (int i = 0; i < numtrigs; ++i)
			EXPECT_GE(portal.WalkingDistance(trigs[i].position), 6);
		SetCurrentPortal(CathedralPortalIndex);
		GetPortalLvlPos();
		EXPECT_GE(ViewPosition.WalkingDistance(portal), 2);
		EXPECT_FALSE(TileHasAny(ViewPosition, TileProperties::Solid));
	}
	pMegaTiles = nullptr;
}

TEST_F(TownPortalTest, GameplayOptionDefaultsOn)
{
	const GameplayOptions defaults;
	EXPECT_TRUE(*defaults.cathedralPortal);
}

TEST_F(TownPortalTest, SharedHostSelectionOverridesDifferentLocalOptions)
{
	// A joining client's local preferences cannot add/remove a simulation entity.
	GetOptions().Gameplay.cathedralPortal.SetValue(false);
	EXPECT_TRUE(IsCathedralPortalEnabled());
	sgGameInitInfo.bPermanentCathedralPortal = 0;
	GetOptions().Gameplay.cathedralPortal.SetValue(true);
	EXPECT_FALSE(IsCathedralPortalEnabled());
}

TEST_F(TownPortalTest, SharedConnectionExistsOnlyOnTownAndFourSectionEntrances)
{
	EXPECT_TRUE(IsCathedralPortalOnActiveLevel());
	for (int level = 1; level <= 24; ++level) {
		currlevel = level;
		leveltype = GetLevelType(level);
		EXPECT_EQ(IsCathedralPortalOnActiveLevel(), level == 1 || level == 5 || level == 9 || level == 13) << level;
	}
	currlevel = 1;
	setlevel = true;
	EXPECT_FALSE(IsCathedralPortalOnActiveLevel());
}

TEST_F(TownPortalTest, LastVisitedSectionTracksActualPlayerLevelsAndLowerReplacesHigher)
{
	for (const uint8_t level : { 1, 4, 5, 8, 9, 12, 13, 16, 6, 2 }) {
		MyPlayer->setLevel(level);
		EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), 1 + ((level - 1) / 4) * 4) << static_cast<int>(level);
	}
	MyPlayer->setLevel(0);
	EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), 1);
	MyPlayer->setLevel(14);
	MyPlayer->setLevel(0);
	EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), 13);
	MyPlayer->setLevel(SL_SKELKING);
	EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), 13);
	for (uint8_t level = 17; level <= 24; ++level) {
		MyPlayer->setLevel(level);
		EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), 13);
	}
}

TEST_F(TownPortalTest, SharedMultiplayerFeatureCannotBeChangedByLocalOptionApi)
{
	gbIsMultiplayer = true;
	EXPECT_FALSE(SetFreeTownPortalEnabled(false));
	EXPECT_TRUE(IsFreeTownPortalEnabled());
	EXPECT_TRUE(*GetOptions().Gameplay.cathedralPortal);
}

TEST_F(TownPortalTest, NativeWarpRoutesEachSectionAndPreservesPlayerCastSlot)
{
	SetPortalStats(0, true, { 40, 40 }, 7, DTYPE_CATACOMBS, false);
	for (const uint8_t visited : { 4, 8, 12, 16, 6, 2 }) {
		MyPlayer->setLevel(visited);
		MyPlayer->setLevel(0);
		currlevel = 0;
		leveltype = DTYPE_TOWN;
		const uint8_t expected = FreeTownPortalSectionForLevel(visited);
		SetCurrentPortal(FreeTownPortalWarp(FreeTownPortalTargetLevel(*MyPlayer)));
		GetPortalLevel();
		EXPECT_EQ(currlevel, expected);
		EXPECT_EQ(leveltype, GetLevelType(expected));
		EXPECT_TRUE(MyPlayer->isOnLevel(expected));
		GetPortalLevel();
		EXPECT_EQ(currlevel, 0);
		EXPECT_TRUE(MyPlayer->isOnLevel(0));
		EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), expected);
		EXPECT_TRUE(Portals[0].open);
		EXPECT_EQ(Portals[0].level, 7);
		EXPECT_EQ(Portals[0].position, Point(40, 40));
	}
}

TEST_F(TownPortalTest, LegacyBareNativeIndexUsesTheRememberedSection)
{
	MyPlayer->setLevel(8);
	MyPlayer->setLevel(0);
	SetCurrentPortal(CathedralPortalIndex);
	GetPortalLevel();
	EXPECT_EQ(currlevel, 5);
}

TEST_F(TownPortalTest, WarpPayloadAcceptsOnlyDedicatedCanonicalSections)
{
	for (const uint8_t section : { 1, 5, 9, 13 }) {
		const uint16_t payload = FreeTownPortalWarp(section);
		EXPECT_TRUE(IsFreeTownPortalWarp(payload));
		EXPECT_EQ(FreeTownPortalWarpDestination(payload, 1), section);
		EXPECT_EQ(payload & 0xFF, CathedralPortalIndex);
	}
	for (const size_t invalid : { 0U, 1U, 2U, 3U, 5U, 0x204U, 0x1104U, 0xFFFFU, 0x10104U })
		EXPECT_FALSE(IsFreeTownPortalWarp(invalid)) << invalid;
}

TEST_F(TownPortalTest, TaggedPersistenceCoversAllSectionsAndRejectsOldOrInvalidBytes)
{
	for (const uint8_t section : { 1, 5, 9, 13 }) {
		const auto state = EncodeFreeTownPortalState(section);
		EXPECT_EQ(state, (FreeTownPortalSaveState { 'F', 'P', 1, section }));
		EXPECT_EQ(DecodeFreeTownPortalState(state, 0, false), section);
	}
	for (const auto invalid : { FreeTownPortalSaveState {}, FreeTownPortalSaveState { 'F', 'P', 2, 9 },
	         FreeTownPortalSaveState { 'F', 'P', 1, 7 }, FreeTownPortalSaveState { 0xFF, 0xFF, 0xFF, 0xFF } }) {
		EXPECT_EQ(DecodeFreeTownPortalState(invalid, 0, false), 1);
		EXPECT_EQ(DecodeFreeTownPortalState(invalid, 14, false), 13);
		EXPECT_EQ(DecodeFreeTownPortalState(invalid, 14, true), 1);
	}
}

TEST_F(TownPortalTest, TownHasNoDuplicateInvisibleStairTrigger)
{
	InitTownTriggers();
	EXPECT_EQ(FindTrigger(TownCathedralPortalPosition), nullptr);
	EXPECT_EQ(FindTrigger({ 63, 68 }), nullptr); // Removed historical decoration/trigger location.
	const TriggerStruct *stairs = FindTrigger({ 25, 29 });
	ASSERT_NE(stairs, nullptr);
	EXPECT_EQ(stairs->_tmsg, WM_DIABNEXTLVL);
}

TEST_F(TownPortalTest, MultiplayerKeepsOrdinaryTownTriggers)
{
	gbIsMultiplayer = true;
	InitTownTriggers();
	EXPECT_LE(numtrigs, MAXTRIGGERS);
	EXPECT_EQ(FindTrigger(TownCathedralPortalPosition), nullptr);
	EXPECT_NE(FindTrigger({ 25, 29 }), nullptr);
	EXPECT_NE(FindTrigger({ 49, 21 }), nullptr);
	EXPECT_NE(FindTrigger({ 17, 69 }), nullptr);
	EXPECT_NE(FindTrigger({ 41, 80 }), nullptr);
}

TEST_F(TownPortalTest, SyncCreatesTheVisibleNativeMissileAtTheAuthoritativeEndpoint)
{
	EXPECT_EQ(CathedralPortalPosition(), Point(TownCathedralPortalPosition));
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 1U);
	const Missile &portal = Missiles.front();
	EXPECT_EQ(portal._mitype, MissileID::TownPortal);
	EXPECT_TRUE(portal.IsTrap());
	EXPECT_TRUE(portal._miDrawFlag);
	EXPECT_EQ(portal.position.tile, Point(TownCathedralPortalPosition));
	EXPECT_EQ(portal.getFrameGroup<PortalFrame>(), PortalFrame::Idle);
	for (const Portal &playerPortal : Portals)
		EXPECT_FALSE(playerPortal.open);
}

TEST_F(TownPortalTest, HostOptOutCreatesNeitherMissileNorLegacyTrigger)
{
	sgGameInitInfo.bPermanentCathedralPortal = 0;
	SyncPortals();
	EXPECT_TRUE(Missiles.empty());
	InitTownTriggers();
	EXPECT_EQ(FindTrigger(TownCathedralPortalPosition), nullptr);
	EXPECT_EQ(FindTrigger({ 63, 68 }), nullptr);
}

TEST_F(TownPortalTest, FullLoadDefersTheFreeMissileUntilSavedStateIsRestored)
{
	SyncPortals(false);
	EXPECT_TRUE(Missiles.empty());
	RefreshFreeTownPortal();
	ASSERT_EQ(Missiles.size(), 1U);
	EXPECT_TRUE(Missiles.front().IsTrap());
	EXPECT_EQ(Missiles.front().position.tile, Point(TownCathedralPortalPosition));
}

TEST_F(TownPortalTest, FullLoadRefreshReplacesSavedDuplicatesAndOldPlacement)
{
	for (const Point old : { Point { 64, 73 }, Point { 63, 68 } })
		ASSERT_NE(AddMissile({ 0, 0 }, old, Direction::South, MissileID::TownPortal, TARGET_MONSTERS, -1, 0, 0, nullptr, SfxID::None), nullptr);
	ASSERT_EQ(Missiles.size(), 2U);
	for (int refresh = 0; refresh < 4; ++refresh) {
		RefreshFreeTownPortal();
		ASSERT_EQ(Missiles.size(), 1U);
		EXPECT_TRUE(Missiles.front().IsTrap());
		EXPECT_EQ(Missiles.front().position.tile, Point(TownCathedralPortalPosition));
	}
}

TEST_F(TownPortalTest, DisabledFullLoadRemovesSavedFreeMissileAndKeepsPlayerCastPortal)
{
	SetPortalStats(0, true, { 40, 40 }, 7, DTYPE_CATACOMBS, false);
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 2U);
	sgGameInitInfo.bPermanentCathedralPortal = 0;
	RefreshFreeTownPortal();
	ASSERT_EQ(Missiles.size(), 1U);
	EXPECT_FALSE(Missiles.front().IsTrap());
	EXPECT_TRUE(Portals[0].open);
	EXPECT_EQ(Portals[0].level, 7);
}

TEST_F(TownPortalTest, PermanentAndPlayerCastPortalsStayIndependent)
{
	SetPortalStats(0, true, { 40, 40 }, 1, DTYPE_CATHEDRAL, false);
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 2U);
	int permanent = 0;
	int playerOwned = 0;
	for (const Missile &portal : Missiles) {
		ASSERT_EQ(portal._mitype, MissileID::TownPortal);
		if (portal.IsTrap()) {
			++permanent;
			EXPECT_EQ(portal.position.tile, Point(TownCathedralPortalPosition));
		} else {
			++playerOwned;
			EXPECT_EQ(portal.position.tile, Point(57, 40));
		}
	}
	EXPECT_EQ(permanent, 1);
	EXPECT_EQ(playerOwned, 1);
	EXPECT_TRUE(Portals[0].open);
}

TEST_F(TownPortalTest, PermanentWarpRoundTripDoesNotConsumePlayerPortals)
{
	SetPortalStats(0, true, { 40, 40 }, 1, DTYPE_CATHEDRAL, false);
	SetCurrentPortal(CathedralPortalIndex);
	GetPortalLevel();
	EXPECT_EQ(currlevel, 1);
	EXPECT_EQ(leveltype, DTYPE_CATHEDRAL);
	EXPECT_TRUE(MyPlayer->isOnLevel(1));
	EXPECT_TRUE(Portals[0].open);
	GetPortalLevel();
	EXPECT_EQ(currlevel, 0);
	EXPECT_EQ(leveltype, DTYPE_TOWN);
	EXPECT_TRUE(MyPlayer->isOnLevel(0));
	EXPECT_TRUE(Portals[0].open);
}

TEST_F(TownPortalTest, PermanentPortalDoesNotExpireAfterOneHundredTicks)
{
	SyncPortals();
	ASSERT_EQ(Missiles.size(), 1U);
	Missile &portal = Missiles.front();
	for (int i = 0; i < 150; ++i)
		ProcessTownPortal(portal);
	EXPECT_EQ(portal.duration, 1);
	EXPECT_FALSE(portal._miDelFlag);
}

TEST_F(TownPortalTest, PortalIsNotOnATownerOrPlayerPortalDropTile)
{
	LoadTownerData();
	for (const TownerDataEntry &towner : TownersDataEntries) {
		EXPECT_GE(Point(TownCathedralPortalPosition).WalkingDistance(towner.position), 2) << towner.name;
	}
	for (const Point spawn : { Point { 75, 68 }, Point { 77, 70 }, Point { 75, 70 }, Point { 77, 68 }, Point { 76, 69 },
	         Point { 75, 69 }, Point { 76, 68 }, Point { 77, 69 }, Point { 76, 70 }, Point { 57, 40 }, Point { 59, 40 },
	         Point { 61, 40 }, Point { 63, 40 } }) {
		EXPECT_NE(TownCathedralPortalPosition, spawn);
	}
}

TEST_F(TownPortalTest, PortalTileIsWalkableWellToTheScreenRightOfCain)
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
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const Point tile = TownCathedralPortalPosition + Displacement { dx, dy };
			EXPECT_FALSE(TileHasAny(tile, TileProperties::Solid | TileProperties::BlockMissile)) << tile.x << "," << tile.y;
		}
	}
	EXPECT_GE((TownCathedralPortalPosition.x - TownCathedralPortalPosition.y) - (62 - 71), 8);
	EXPECT_LE(Point(TownCathedralPortalPosition).WalkingDistance(Point { 61, 70 }), 9);
	pMegaTiles = nullptr;
}

} // namespace
} // namespace devilution
