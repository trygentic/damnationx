#pragma once
/**
 * @file host/host_hooks.hpp
 *
 * The few calls the game files make into the embedding host (host_api.hpp, docs/design/bevy-host.md). Every hook is
 * a no-op unless HostInit started the game: the normal executable keeps its own window, loop, renderer and mixer.
 * Without DAMNATIONX_HOST_LIB the hooks are constant no-ops the compiler removes.
 *
 * Runtime model (host_runtime.cpp): the game runs DiabloMain on its own thread with its own stack. Host and game
 * thread strictly take turns, so game state is never touched by both at once: HostTick hands control to the game
 * thread, the game thread hands it back at the end of the frame it presents (OnPresent), and HostSubmitInput /
 * HostSnapshot run while the game thread is parked there. The game's 20 Hz tick clock (nthread) reads the host's
 * virtual clock, which only HostTick advances.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "engine/point.hpp"

union SDL_Event;

namespace devilution {

struct GameData;
namespace hd_tiles {
struct Manifest;
}

namespace host {

#ifdef DAMNATIONX_HOST_LIB

/** True from HostInit until the game thread finished: the game runs inside a host. */
extern bool Active;
inline bool HideHud = false;
inline bool HideInventory = false;
inline bool HideStore = false;
inline bool HideGameMenu = false;
bool NativeInventoryPanel();
bool NativeStorePanel();
inline bool NativeGameMenu() { return Active && HideGameMenu; }
inline bool IsActive() { return Active; }
/** Puts text on the OS clipboard through the host (HostTakeClipboardText). */
void RequestClipboard(std::string text);

/** ApplicationInit: SDL events and timers only, the back buffer and palette without a window (diablo.cpp). */
void InitVideo();
/** RenderPresent: converts the frame into the UI layer and hands control back to the host until the next HostTick. */
void OnPresent();
/** The game tick clock in milliseconds (nthread.cpp): virtual, advanced by HostTick. */
uint32_t GameClockMs();
/** One simulation tick ran (game_loop). */
void OnGameTick();
/** diablo_quit: ends the game thread instead of the process. Never returns. */
[[noreturn]] void OnGameQuit(int exitStatus);
/** Fatal errors show the game's error dialog only once the host is presenting frames (HostInit returns false before). */
bool ShowFatalDialogs();

/** CheckCursMove: the tile the host's 3D pick hit; false when the host sends no pick (classic picking runs). */
bool PickedTile(Point &tile);
/** CheckCursMove: turns the host's picked entity into the cursor target (pcursmonst, pcursitem, ...). */
bool ApplyPickToCursor();
/** plrctrls_after_check_curs_move: the host's 3D pick set this frame's target (the gamepad targeting keeps out). */
bool PickedTarget();

/** Sounds are queued for the host instead of being mixed (effects.cpp, monster.cpp, engine/sound.cpp). */
void QueueSfx(int16_t sfxId, bool positional, Point position);
void QueueMonsterSfx(int monsterType, int mode, int variant, Point position);
void QueueMusic(int track);
void StopMusic();
void StopStream(int16_t sfxId);
/** GetSFXLength (quest text scroll speed): the sound's length in ms from its file header, never 0. */
int SfxLengthMs(int16_t sfxId);

/** mainmenu_loop: true once when --host-hero asked to start straight into a game. */
bool TakeAutoStart();
/** mainmenu_select_hero_dialog: picks or creates the hero named by --host-hero. False when not auto-starting. */
bool SelectAutoStartHero(GameData *gameData);

/** controls/input.h PollEvent: the host's input events, taken before SDL's own queue. */
bool PollEvent(SDL_Event &event);

/** fpv::SkipIsometricWorld: the World3D render mode, where the host draws the world and the game only its UI. */
bool DrawsWorld3D();
/** fpv::ClearWorldArea in World3D: queues the ground item labels (Alt) at the items' places in the host's 3D view
 * (HostSetWorldView); `width` x `height` is the screen area they may anchor in. */
void QueueWorldItemLabels(int width, int height);

// ---- The HD pack without WebGPU (host_hd.cpp): the stubs of hd_*_webgpu.cpp ask the host ----

/** Hd2D render mode with the HD pack's 2D art installed (and Graphics > HD Graphics on). */
bool HdSpritesAvailable();
bool HdTilesAvailable();
bool HdUiAvailable();
/** The HD UI hooks are installed at startup whenever the pack has hd/ui.txt (UI art loads once). */
bool HdUiWanted();
size_t HdTextureBudget();
/** Reads `url` ("hd/..." resolves to the HD pack folder) as hd_loader::FetchText does. */
bool HdFetchText(const char *url, std::string &text, bool &failed, bool optional = false);
/** The tile manifest the current world pass draws from (texture table of the Hd2D draw list). */
void HdUseTileManifest(int tileset, const hd_tiles::Manifest &manifest);

#else

constexpr bool IsActive() { return false; }
inline void InitVideo() { }
inline void OnPresent() { }
inline uint32_t GameClockMs() { return 0; }
inline void OnGameTick() { }
inline void OnGameQuit(int) { }
inline bool ShowFatalDialogs() { return true; }
inline bool PickedTile(Point &) { return false; }
inline bool ApplyPickToCursor() { return false; }
inline void RequestClipboard(std::string) { }
inline bool PickedTarget() { return false; }
inline void QueueSfx(int16_t, bool, Point) { }
inline void QueueMonsterSfx(int, int, int, Point) { }
inline void QueueMusic(int) { }
inline void StopMusic() { }
inline void StopStream(int16_t) { }
inline int SfxLengthMs(int16_t) { return 1; }
inline bool TakeAutoStart() { return false; }
inline bool SelectAutoStartHero(GameData *) { return false; }
constexpr bool DrawsWorld3D() { return false; }
inline void QueueWorldItemLabels(int, int) { }
inline bool PollEvent(SDL_Event &) { return false; }
inline bool HdSpritesAvailable() { return false; }
inline bool HdTilesAvailable() { return false; }
inline bool HdUiAvailable() { return false; }
constexpr bool HideHud = false;
inline bool NativeInventoryPanel() { return false; }
inline bool NativeStorePanel() { return false; }
inline bool NativeGameMenu() { return false; }
inline bool HdUiWanted() { return false; }
inline size_t HdTextureBudget() { return 0; }
inline bool HdFetchText(const char *, std::string &, bool &failed, bool = false)
{
	failed = true;
	return false;
}
inline void HdUseTileManifest(int, const hd_tiles::Manifest &) { }

#endif

} // namespace host
} // namespace devilution
