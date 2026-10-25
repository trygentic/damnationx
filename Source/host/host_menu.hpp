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
/** Graphics "UI Scale" in quarters (4 = 1x): the chosen one and the one the screen fits (EffectiveUiScaleQuarters). */
bool dxp_menu_ui_scale(int32_t *chosen, int32_t *effective);
/** Sets Graphics "UI Scale" (clamped to 4..16 quarters); saved with the options like the classic Options menu. */
bool dxp_menu_set_ui_scale(int32_t quarters);
/** Saved hover outline enablement and width in quarter screen pixels (2..12 = 0.5..3 px). */
bool dxp_menu_outlines(bool *enabled, int32_t *widthQuarters);
/** Queues both saved values; requires the open in-game menu. */
bool dxp_menu_set_outlines(bool enabled, int32_t widthQuarters);
/** Saved application render FPS overlay (default on); setter requires the open game menu. */
bool dxp_menu_fps(bool *enabled);
bool dxp_menu_set_fps(bool enabled);
}

static_assert(sizeof(DxpMenuSnapshot) == 16);
