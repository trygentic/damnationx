#pragma once

#include <cstdint>

/** Pointer-free companion API for a host-rendered in-game menu. Calls are made
 * between HostTick calls. Mutations run on the game thread at its next event poll. */
struct DxpMenuSnapshot {
	uint32_t version;
	uint32_t flags;
	int32_t musicPercent;
	int32_t soundPercent;
};

enum DxpMenuFlags : uint32_t {
	DxpMenuInGame = 1,
	DxpMenuOpen = 2,
	DxpMenuCanSave = 4,
	DxpMenuCanLoad = 8,
	DxpMenuMultiplayer = 16,
	DxpMenuRunEnabled = 32,
	DxpMenuRunning = 64,
	DxpMenuCanRun = 128,
	DxpMenuMusicOn = 256,
	DxpMenuSoundOn = 512,
	DxpMenuBusy = 1024,
	DxpMenuFreePortal = 2048,
};

extern "C" {
bool dxp_menu_snapshot(DxpMenuSnapshot *output, uint32_t size);
void dxp_menu_enable(bool enabled);
/** 1 resume, 2 save, 3 load, 4 main menu, 5 quit. Requires an open menu. */
bool dxp_menu_action(uint32_t action);
/** 0 music, 1 sound; percent is clamped to 0..100. */
bool dxp_menu_audio(uint32_t channel, int32_t percent);
/** 0 enables native singleplayer running, 2 enables the free town portal. */
bool dxp_menu_gameplay(uint32_t setting, bool enabled);
}

static_assert(sizeof(DxpMenuSnapshot) == 16);
