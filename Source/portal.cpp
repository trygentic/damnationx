/**
 * @file portal.cpp
 *
 * Implementation of functionality for handling town portals.
 */
#include "portal.h"

#include "engine/path.h"
#include "game_mode.hpp"
#include "levels/tile_properties.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "missiles.h"
#include "multi.h"
#include "player.h"
#include "tables/misdat.h"

namespace devilution {

/** In-game state of portals. */
Portal Portals[MAXPORTAL];

namespace {

/** Current portal number (a portal array index). */
size_t portalindex;

/** Coordinate of each player's portal in town. */
Point PortalTownPosition[MAXPORTAL] = {
	{ 57, 40 },
	{ 59, 40 },
	{ 61, 40 },
	{ 63, 40 },
};

bool NearTrigger(Point position)
{
	for (int i = 0; i < numtrigs; ++i) {
		if (trigs[i].position.WalkingDistance(position) < 2)
			return true;
	}
	return false;
}

} // namespace

bool IsCathedralPortalEnabled()
{
	return sgGameInitInfo.bPermanentCathedralPortal == 1;
}

bool IsCathedralPortalOnActiveLevel()
{
	return IsCathedralPortalEnabled() && !setlevel && (currlevel == 0 || currlevel == 1);
}

Point CathedralPortalPosition()
{
	if (leveltype == DTYPE_TOWN)
		return { 64, 73 }; // Beside Cain (62, 71), leaving his talking tiles clear.
	// Use geometry only: occupied player tiles must not change the shared location.
	for (int i = 0; i < numtrigs; ++i) {
		if (trigs[i]._tmsg != WM_DIABPREVLVL)
			continue;
		const auto position = FindClosestValidPosition([](Point p) {
			return InDungeonBounds(p) && !TileHasAny(p, TileProperties::Solid | TileProperties::BlockMissile) && !NearTrigger(p);
		}, trigs[i].position, 2, 5);
		if (position)
			return *position;
	}
	return ViewPosition;
}

void InitPortals()
{
	for (auto &portal : Portals) {
		portal.open = false;
	}
}

void SetPortalStats(int i, bool o, Point position, int lvl, dungeon_type lvltype, bool isSetLevel)
{
	Portals[i].open = o;
	Portals[i].position = position;
	Portals[i].level = lvl;
	Portals[i].ltype = lvltype;
	Portals[i].setlvl = isSetLevel;
}

void AddPortalMissile(const Player &player, Point position, bool sync)
{
	auto *missile = AddMissile({ 0, 0 }, position, Direction::South, MissileID::TownPortal, TARGET_MONSTERS, player, 0, 0, /*parent=*/nullptr, SfxID::None);
	if (missile != nullptr) {
		// Don't show portal opening animation if we sync existing portals
		if (sync)
			missile->setFrameGroup<PortalFrame>(PortalFrame::Idle);

		if (leveltype != DTYPE_TOWN)
			missile->_mlid = AddLight(missile->position.tile, 15);
	}
}

void SyncPortals()
{
	if (IsCathedralPortalOnActiveLevel()) {
		auto *missile = AddMissile({ 0, 0 }, CathedralPortalPosition(), Direction::South, MissileID::TownPortal, TARGET_MONSTERS, -1, 0, 0, nullptr, SfxID::None);
		if (missile != nullptr) {
			missile->setFrameGroup<PortalFrame>(PortalFrame::Idle);
			if (leveltype != DTYPE_TOWN)
				missile->_mlid = AddLight(missile->position.tile, 15);
		}
	}
	for (int i = 0; i < MAXPORTAL; i++) {
		if (!Portals[i].open)
			continue;
		const Player &player = Players[i];
		if (leveltype == DTYPE_TOWN)
			AddPortalMissile(player, PortalTownPosition[i], true);
		else {
			int lvl = currlevel;
			if (setlevel)
				lvl = setlvlnum;
			if (Portals[i].level == lvl && Portals[i].setlvl == setlevel)
				AddPortalMissile(player, Portals[i].position, true);
		}
	}
}

void AddPortalInTown(const Player &player)
{
	AddPortalMissile(player, PortalTownPosition[player.getId()], false);
}

void ActivatePortal(const Player &player, Point position, int lvl, dungeon_type dungeonType, bool isSetLevel)
{
	Portal &portal = Portals[player.getId()];
	portal.open = true;

	if (lvl != 0) {
		portal.position = position;
		portal.level = lvl;
		portal.ltype = dungeonType;
		portal.setlvl = isSetLevel;
	}
}

void DeactivatePortal(const Player &player)
{
	Portals[player.getId()].open = false;
}

bool PortalOnLevel(const Player &player)
{
	const Portal &portal = Portals[player.getId()];
	if (portal.setlvl == setlevel && portal.level == (setlevel ? static_cast<int>(setlvlnum) : currlevel))
		return true;

	return leveltype == DTYPE_TOWN;
}

void RemovePortalMissile(const Player &player)
{
	const size_t id = player.getId();
	Missiles.remove_if([id](Missile &missile) {
		if (missile._mitype == MissileID::TownPortal && missile._misource == static_cast<int>(id)) {
			dFlags[missile.position.tile.x][missile.position.tile.y] &= ~DungeonFlag::Missile;

			if (Portals[id].level != 0)
				AddUnLight(missile._mlid);

			return true;
		}
		return false;
	});
}

void SetCurrentPortal(size_t p)
{
	portalindex = p;
}

void GetPortalLevel()
{
	if (leveltype != DTYPE_TOWN) {
		setlevel = false;
		currlevel = 0;
		MyPlayer->setLevel(0);
		leveltype = DTYPE_TOWN;
		return;
	}

	if (portalindex == CathedralPortalIndex) {
		setlevel = false;
		currlevel = 1;
		MyPlayer->setLevel(1);
		leveltype = DTYPE_CATHEDRAL;
		return;
	}

	if (Portals[portalindex].setlvl) {
		setlevel = true;
		setlvlnum = (_setlevels)Portals[portalindex].level;
		currlevel = Portals[portalindex].level;
		MyPlayer->setLevel(setlvlnum);
		setlvltype = leveltype = Portals[portalindex].ltype;
	} else {
		setlevel = false;
		currlevel = Portals[portalindex].level;
		MyPlayer->setLevel(currlevel);
		leveltype = Portals[portalindex].ltype;
	}

	if (portalindex == MyPlayerId) {
		NetSendCmd(true, CMD_DEACTIVATEPORTAL);
		DeactivatePortal(*MyPlayer);
	}
}

void GetPortalLvlPos()
{
	if (portalindex == CathedralPortalIndex) {
		// Arrive beside the portal to avoid immediately travelling back.
		const auto landing = FindClosestValidPosition([](Point p) {
			return InDungeonBounds(p) && !TileHasAny(p, TileProperties::Solid) && dPlayer[p.x][p.y] == 0 && !NearTrigger(p);
		}, CathedralPortalPosition(), 1, 5);
		ViewPosition = landing.value_or(CathedralPortalPosition() + Displacement { 1, 1 });
		return;
	}
	if (leveltype == DTYPE_TOWN) {
		ViewPosition = PortalTownPosition[portalindex] + Displacement { 1, 1 };
	} else {
		ViewPosition = Portals[portalindex].position;

		if (portalindex != MyPlayerId) {
			ViewPosition.x++;
			ViewPosition.y++;
		}
	}
}

bool PosOkPortal(int lvl, Point position)
{
	if (IsCathedralPortalEnabled() && !setlevel && lvl == 1 && position == CathedralPortalPosition())
		return true;
	for (auto &portal : Portals) {
		if (portal.open
		    && portal.level == lvl
		    && ((portal.position == position)
		        || (portal.position == position - Displacement { 1, 1 })))
			return true;
	}
	return false;
}

} // namespace devilution
