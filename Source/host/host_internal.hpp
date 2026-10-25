#pragma once
/**
 * @file host/host_internal.hpp
 *
 * State shared by the files of the embedding API (host_api.cpp, host_runtime.cpp, host_input.cpp,
 * host_snapshot.cpp). Not part of the contract.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "engine/clx_sprite.hpp"
#include "host/host_api.hpp"
#include "host/host_hooks.hpp"
#include "utils/attributes.h"

namespace devilution::host {

/** Command line options of the host itself, removed before the game's option parser sees argv. */
struct HostOptions {
	std::string heroName;  // --host-hero: start straight into a single player game with this hero
	std::string heroClass; // --host-class: class of the hero when it has to be created (default warrior)
	bool continueGame = false; // --host-continue: load the hero's saved game instead of a new dungeon
	std::string assetsDir; // --host-assets-dir: the game's own assets (devilutionx.mpq contents)
	std::string hdDir;     // --host-hd-dir: the HD pack folder (default: found like the game finds it)
	bool hdDisabled = false; // --host-no-hd: suppress every HD pack discovery path for this run
};

extern DVL_API_FOR_TEST HostOptions Options;

// ---- host_runtime.cpp: the game thread and the turn taking ----

/** Starts DiabloMain on the game thread and waits until it presented its first frame or gave up. */
bool StartGameThread(int argc, char **argv);
/** Lets the game thread run until it presents its next frame (or finishes). Host thread only. */
void ResumeGameThread();
/** Advances the virtual tick clock (HostTick). */
void AdvanceGameClock(uint32_t elapsedMs);
bool GameThreadFinished();
/** Joins (or, after a fatal error, abandons) the game thread. */
void StopGameThread();
/** Simulation ticks run since the last call. */
uint32_t TakeTicksRun();
uint32_t TotalTicksRun();
/** Moves the sounds queued since the last call to `out` (cleared first). */
void TakeSounds(std::vector<HostSoundEvent> &out);

/** The classic UI as RGBA8 (written by the game thread when it presents). */
struct UiLayerState {
	std::vector<uint8_t> rgba;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t revision = 0;
	// Union of the rectangles that changed since the last snapshot (x0, y0 inclusive, x1, y1 exclusive).
	uint32_t dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
};
extern UiLayerState UiLayer;

/** True while the game runs a level (not in DiabloUI menus, not loading). Safe to read game state then. */
bool InGame();
/** True on the game thread (false on the host thread, e.g. inside HostLoadState). */
bool OnGameThread();
/** A volume option (hundredths of dB, VOLUME_MIN..0) as a linear gain 0..1. */
float VolumeGain(int volume);

// ---- host_state.cpp (v5) ----

const HostUiState &BuildUiState();
const HostView2DFrame &BuildView2D();
const HostAudioState &BuildAudioState();
uint32_t SaveState(uint8_t *buffer, uint32_t capacity);
bool LoadState(const uint8_t *data, uint32_t size);
/** A new tick: the state taken by SaveState is stale. */
void ForgetSavedState();
/** Runs queued menu commands on the game thread before polling another event. */
void ProcessMenuCommands();
/** Clears companion menu state when the core is initialized or stopped. */
void ResetHostMenu();
/** HostShutdown: deletes the temporary folder of the checkpoints. */
void RemoveStateDirectory();

/** The render mode in effect (host_runtime.cpp). */
HostRenderMode CurrentRenderMode();
void SetRenderMode(HostRenderMode mode);
bool TakeRenderModeChanged();
void SetRenderModeCallback(HostRenderModeCallback callback, void *user);

// ---- host_hd.cpp ----

/** Finds the HD pack and its tiers (InitVideo, before the HD UI hooks install). */
void InitHdPack();
uint32_t InstalledPacks();
const std::string &HdDirectory();
/** "hd/..." -> the absolute path in the HD pack folder; other paths unchanged. */
std::string ResolvePackPath(std::string_view assetPath);
/** OnPresent: resolves the HD records against the presented frame (Hd2D only). */
void OnHdPresent(bool inGame);
/** Drops every HD record (render mode switch). */
void ResetHdRecords();
const HostHdFrame &BuildHdSnapshot(bool inGame);

// ---- host_world.cpp ----

/** Placements, environment and model hints; `entities` are the entities of the snapshot just built. */
const HostWorldFrame &BuildWorldSnapshot(bool inGame, const std::vector<HostEntity> &entities);
/** HostSnapshot: true when Objects[index] is drawn by a placement mesh. */
bool ObjectReplacedByMesh(int objectIndex);

/** The contract version the host was built against (HostInitParams::apiVersion): older hosts pass smaller structs. */
uint32_t HostApiVersionInUse();

// ---- host_input.cpp ----

/** Queues an event for the game's event loop (see PollEvent in host_hooks.hpp). */
void QueueEvent(const SDL_Event &event);

void ApplyInput(const HostInputFrame &input);
void ReleaseInput();
/** Entity ids: kind << 24 | (index + 1), so that 0 stays "none" (the first player has index 0). */
constexpr uint32_t MakeEntityId(HostEntityKind kind, uint32_t index)
{
	return (static_cast<uint32_t>(kind) << 24) | ((index + 1) & 0xFFFFFF);
}

// ---- host_snapshot.cpp ----

const HostFrame &BuildSnapshot();
/** The entities of the last BuildSnapshot. */
const std::vector<HostEntity> &SnapshotEntities();
/** The classic frame each entity of the last BuildSnapshot shows. */
struct EntitySpriteSource {
	uint32_t entityId;
	ClxSprite sprite;
	const uint8_t *trn;
};
const std::vector<EntitySpriteSource> &SnapshotSprites();

// ---- host_art.cpp ----

const HostEntityArt &BuildEntityArt(bool inGame);
/** The HD 2D frames of the entities of the last BuildSnapshot that the pack has art for (World3D billboards). */
const HostEntityHdArt &BuildEntityHdArt(bool inGame);
/** True when the pack has a skinned model for this hero class (host_world.cpp). */
bool HasCharacterModel(int heroClass);
void ResetSnapshot();

} // namespace devilution::host
