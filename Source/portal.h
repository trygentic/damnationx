/**
 * @file portal.h
 *
 * Interface of functionality for handling town portals.
 */
#pragma once

#include <cstddef>

#include "engine/point.hpp"
#include "free_town_portal.hpp"
#include "levels/gendung.h"

namespace devilution {

// Defined in player.h, forward declared here to allow for functions which operate in the context of a player.
struct Player;

#define MAXPORTAL 4

/** One native missile/warp endpoint beside Cain; shared by every renderer and client. */
constexpr WorldTilePosition TownCathedralPortalPosition { 67, 67 };

/** Dedicated CMD_WARP index; never occupies a player portal slot. */
constexpr size_t CathedralPortalIndex = FreeTownPortalIndex;
static_assert(MAXPORTAL == FreeTownPortalIndex);
bool IsFreeTownPortalEnabled();
/** Single-player option change; multiplayer keeps the host-selected shared value. */
bool SetFreeTownPortalEnabled(bool enabled);
uint8_t FreeTownPortalTargetLevel(const Player &player);
/** Replace only the dedicated native missile after option changes or full save loads. */
void RefreshFreeTownPortal();
bool IsCathedralPortalEnabled();
/** True only on either endpoint of the permanent connection. */
bool IsCathedralPortalOnActiveLevel();
Point CathedralPortalPosition();

struct Portal {
	bool open;
	Point position;
	int level;
	dungeon_type ltype;
	bool setlvl;
};

extern Portal Portals[MAXPORTAL];

void InitPortals();
void SetPortalStats(int i, bool o, Point position, int lvl, dungeon_type lvltype, bool isSetLevel);
void AddPortalMissile(const Player &player, Point position, bool sync);
void SyncPortals(bool includeFreeTownPortal = true);
void AddPortalInTown(const Player &player);
void ActivatePortal(const Player &player, Point position, int lvl, dungeon_type lvltype, bool sp);
void DeactivatePortal(const Player &player);
bool PortalOnLevel(const Player &player);
void RemovePortalMissile(const Player &player);
void SetCurrentPortal(size_t p);
void GetPortalLevel();
void GetPortalLvlPos();
bool PosOkPortal(int lvl, Point position);

} // namespace devilution
