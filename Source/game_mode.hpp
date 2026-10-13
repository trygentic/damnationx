#pragma once

#include <cstdint>

#include "levels/gendung_defs.hpp"
#include "utils/attributes.h"

namespace devilution {

struct GameData {
	int32_t size;
	/**
	 * Whether this is the shareware (spawn) edition. Part of the join-compatibility identity
	 * (edition + version + mod config), unlike `programid` which is now cosmetic branding.
	 */
	uint8_t isSpawn;
	/** Host-selected permanent Cathedral entrance portal (0/1). Uses a formerly reserved byte, so the layout is unchanged. */
	uint8_t bPermanentCathedralPortal;
	uint8_t reserved[2];
	/** Branding id (e.g. "DRTL"/"HRTL"/"DXMD") for display only. A mod may change it via its manifest. */
	uint32_t programid;
	uint8_t versionMajor;
	uint8_t versionMinor;
	uint8_t versionPatch;
	_difficulty nDifficulty;
	uint8_t nTickRate;
	uint8_t bRunInTown;
	uint8_t bTheoQuest;
	uint8_t bCowQuest;
	uint8_t bFriendlyFire;
	uint8_t fullQuests;
	/** Free-form (Diablo II style) movement; 0 means classic 8-direction movement. Shared so every client paths the same way. */
	uint8_t bSmoothMovement;
	/** Used to initialise the seed table for dungeon levels so players in multiplayer games generate the same layout */
	uint32_t gameSeed[4];

	void swapLE();
};

extern DVL_API_FOR_TEST GameData sgGameInitInfo;

/* Are we in-game? If false, we're in the main menu. */
extern DVL_API_FOR_TEST bool gbRunGame;
/** Indicate if we are in a multiplayer game */
extern DVL_API_FOR_TEST bool gbIsMultiplayer;
/** Indicate if we only have access to demo data */
extern DVL_API_FOR_TEST bool gbIsSpawn;
/** Indicate if we have loaded the Hellfire expansion data */
extern DVL_API_FOR_TEST bool gbIsHellfire;
/** Indicate if we want vanilla savefiles */
extern DVL_API_FOR_TEST bool gbVanilla;
/** Whether the Hellfire mode is required (forced). */
extern bool forceHellfire;

/** Whether free-form (Diablo II style) movement is on for the current game (see GameData::bSmoothMovement). */
inline bool IsSmoothMovementEnabled()
{
	return sgGameInitInfo.bSmoothMovement != 0;
}

} // namespace devilution
