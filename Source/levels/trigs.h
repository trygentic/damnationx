/**
 * @file trigs.h
 *
 * Interface of functionality for triggering events when the player enters an area.
 */
#pragma once

#include "engine/point.hpp"
#include "interfac.h"
#include "levels/gendung.h"

namespace devilution {

#define MAXTRIGGERS 8

struct TriggerStruct {
	WorldTilePosition position;
	interface_mode _tmsg;
	int _tlvl;
};

extern bool trigflag;
extern int numtrigs;
extern TriggerStruct trigs[MAXTRIGGERS];
extern int TWarpFrom;

/**
 * @brief Tile of the permanent Cathedral portal next to the town fountain (DamnationX).
 *
 * Standing on it does the same as the Cathedral stairs (WM_DIABNEXTLVL to dungeon level 1). The position is a
 * constant so every client builds the same trigger table.
 */
constexpr WorldTilePosition TownCathedralPortalPosition { 63, 68 };

/** @brief Whether the fountain portal to the Cathedral exists in town (Gameplay option "Cathedral Portal"). */
bool IsTownCathedralPortalEnabled();

void InitNoTriggers();
bool IsWarpOpen(dungeon_type type);
void InitTownTriggers();
void InitL1Triggers();
void InitL2Triggers();
void InitL3Triggers();
void InitL4Triggers();
void InitHiveTriggers();
void InitCryptTriggers();
void InitSKingTriggers();
void InitSChambTriggers();
void InitPWaterTriggers();
void InitVPTriggers();
void Freeupstairs();
void CheckTrigForce();
void CheckTriggers();

/**
 * @brief Check if the provided position is in the entrance boundary of the entrance.
 * @param entrance The entrance to check.
 * @param position The position to check against the entrance boundary.
 */
bool EntranceBoundaryContains(Point entrance, Point position);

} // namespace devilution
