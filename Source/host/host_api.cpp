/**
 * @file host/host_api.cpp
 *
 * The embedding contract (host_api.hpp). The runtime model is described in host_hooks.hpp; the game thread lives in
 * host_runtime.cpp, input in host_input.cpp and the snapshot in host_snapshot.cpp.
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "effects.h"
#include "d2sound/d2sound_core.hpp"
#include "engine/assets.hpp"
#include "engine/sound.h"
#include "headless_mode.hpp"
#include "interfac.h"
#include "levels/gendung.h"
#include "game_mode.hpp"
#include "minitext.h"
#include "player.h"
#include "stores.h"
#include "tables/monstdat.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution::host {

namespace {

bool Initialized = false;
uint32_t ApiVersionInUse = kHostApiVersion;
/** argv for DiabloMain without the --host-* options; must outlive the game thread. */
std::vector<std::string> GameArgs;
std::vector<char *> GameArgPointers;
std::string SoundPathBuffer;
/** Set by RequestClipboard on the game thread, taken between ticks by HostTakeClipboardText. */
std::string PendingClipboard;

/** Splits the host's own options off argv (host_api.hpp lists them). */
void ParseArgs(int argc, const char *const *argv)
{
	Options = {};
	GameArgs.clear();
	GameArgs.emplace_back(argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "damnationx");
	for (int i = 1; i < argc; ++i) {
		if (argv[i] == nullptr)
			continue;
		const std::string_view arg = argv[i];
		const bool hasValue = i + 1 < argc && argv[i + 1] != nullptr;
		if (arg == "--host-hero" && hasValue) {
			Options.heroName = argv[++i];
		} else if (arg == "--host-class" && hasValue) {
			Options.heroClass = argv[++i];
		} else if (arg == "--host-assets-dir" && hasValue) {
			Options.assetsDir = argv[++i];
		} else if (arg == "--host-hd-dir" && hasValue) {
			Options.hdDir = argv[++i];
		} else if (arg == "--host-no-hd") {
			Options.hdDisabled = true;
		} else if (arg == "--host-continue") {
			Options.continueGame = true;
		} else {
			GameArgs.emplace_back(arg);
		}
	}
	GameArgPointers.clear();
	for (std::string &value : GameArgs)
		GameArgPointers.push_back(value.data());
	GameArgPointers.push_back(nullptr);
}

} // namespace

int32_t HostImportD2Sounds(const char *installPath, const char *modsPath, const char *mapDir)
{
	if (Initialized || Active || installPath == nullptr || modsPath == nullptr || mapDir == nullptr) {
		std::cerr << "D2R sound import requires a fresh process and explicit source/output/map directories.\n";
		return 2;
	}
	try {
		if (!IsDiablo2ResurrectedInstallDir(installPath)) {
			std::cerr << "D2R sound import: the source is not an installed Diablo II Resurrected archive.\n";
			return 2;
		}
		std::vector<D2SoundMapEntry> entries;
		for (const char *name : { "d2_sfx_map.tsv", "d2_speech_map.tsv" }) {
			const auto path = std::filesystem::path(mapDir) / name;
			std::ifstream input(path, std::ios::binary);
			if (!input) {
				std::cerr << "D2R sound import: missing sound map " << path << '\n';
				return 2;
			}
			const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			auto parsed = ParseD2SoundMap(contents);
			if (parsed.empty()) {
				std::cerr << "D2R sound import: empty or invalid sound map " << path << '\n';
				return 2;
			}
			entries.insert(entries.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
		}
		D2SoundDirectorySink sink(modsPath);
		D2SoundImportJob job(D2SoundSource::Diablo2Resurrected, installPath, std::move(entries), sink);
		int previous = -1;
		for (;;) {
			const int progress = job.Step(std::chrono::milliseconds(100));
			if (progress != previous) {
				std::cout << "D2R sound import: progress=" << progress << std::endl;
				previous = progress;
			}
			if (progress >= 100)
				break;
		}
		const auto &result = job.Result();
		std::cout << "D2R sound import: classic=" << result.imported
		          << " advanced=" << result.importedAdvanced << " remastered=" << result.remastered
		          << " missing=" << result.missing.size() << " total=" << result.total << std::endl;
		if (!result.ok()) {
			std::cerr << "D2R sound import failed: error=" << static_cast<int>(result.error)
			          << " path=" << result.errorPath << '\n';
			return 1;
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "D2R sound import failed: " << error.what() << '\n';
		return 1;
	}
}

bool HostInit(const HostInitParams &params)
{
	if (params.apiVersion == 0 || params.apiVersion > kHostApiVersion) {
		LogError("HostInit: contract version {} not supported (this game implements 1..{})", params.apiVersion, kHostApiVersion);
		return false;
	}
	if (Initialized || Active) {
		LogError("HostInit: the game is already running (one HostInit per process)");
		return false;
	}
	ApiVersionInUse = params.apiVersion;
	ParseArgs(params.argc, params.argv);
	if (!Options.assetsDir.empty()) {
		paths::SetAssetsPath(Options.assetsDir);
	}
	forceResolution = { static_cast<int>(params.uiWidth != 0 ? params.uiWidth : 640), static_cast<int>(params.uiHeight != 0 ? params.uiHeight : 480) };

	// Embedded is not headless: the game draws its UI (into the UI layer instead of a window).
	HeadlessMode = false;
	Active = true;
	Initialized = true;
	ResetSnapshot();
	ResetHostMenu();
	if (!StartGameThread(static_cast<int>(GameArgPointers.size()) - 1, GameArgPointers.data())) {
		// Missing game data, a bad option or another fatal error before the first frame (logged by the game).
		StopGameThread();
		Active = false;
		return false;
	}
	return true;
}

void HostSubmitInput(const HostInputFrame &input)
{
	if (!Active || GameThreadFinished())
		return;
	ApplyInput(input);
}

HostTickResult HostTick(uint32_t elapsedMs)
{
	HostTickResult result {};
	if (!Active || GameThreadFinished()) {
		result.quitRequested = 1;
		return result;
	}
	AdvanceGameClock(elapsedMs);
	ResumeGameThread();
	ResetSnapshot();
	ForgetSavedState();
	result.gameTicks = TakeTicksRun();
	result.quitRequested = GameThreadFinished() ? 1 : 0;
	result.inGame = InGame() ? 1 : 0;
	result.reserved[0] = TakeRenderModeChanged() ? 1 : 0;
	static const bool Trace = [] {
		const char *value = std::getenv("DAMNATIONX_TRACE_INPUT");
		return value != nullptr && value[0] == '1';
	}();
	// Test hooks for unattended runs (screenshots of later levels with a new hero):
	// DAMNATIONX_HOST_START_LEVEL=N: once in town, go straight down to dungeon level N.
	// DAMNATIONX_HOST_GOD=1: the local hero's life is refilled every tick.
	static const int StartLevel = [] {
		const char *value = std::getenv("DAMNATIONX_HOST_START_LEVEL");
		return value != nullptr ? std::clamp(std::atoi(value), 0, 16) : 0;
	}();
	static const bool God = [] {
		const char *value = std::getenv("DAMNATIONX_HOST_GOD");
		return value != nullptr && value[0] == '1';
	}();
	static bool startLevelDone = false;
	if (result.inGame != 0 && MyPlayer != nullptr && !gbIsMultiplayer) {
		if (StartLevel > 0 && !startLevelDone && TotalTicksRun() > 30 && leveltype == DTYPE_TOWN) {
			startLevelDone = true;
			LogInfo("DAMNATIONX_HOST_START_LEVEL: going to level {}", StartLevel);
			StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, StartLevel);
		}
		if (God && MyPlayer->_pHitPoints > 0)
			MyPlayer->_pHitPoints = MyPlayer->_pMaxHP;
	}
	// DAMNATIONX_HOST_STATE_SELFTEST=N: once in a level, take a state and restore it N times, logging size and timings.
	static const int StateSelfTest = [] {
		const char *value = std::getenv("DAMNATIONX_HOST_STATE_SELFTEST");
		return value != nullptr ? std::max(0, std::atoi(value)) : 0;
	}();
	static bool stateSelfTested = false;
	if (StateSelfTest > 0 && !stateSelfTested && result.inGame != 0 && TotalTicksRun() > 100) {
		stateSelfTested = true;
		const auto start = std::chrono::steady_clock::now();
		std::vector<uint8_t> state(SaveState(nullptr, 0));
		SaveState(state.data(), static_cast<uint32_t>(state.size()));
		const auto saved = std::chrono::steady_clock::now();
		bool loaded = !state.empty();
		for (int i = 0; i < StateSelfTest && loaded; ++i)
			loaded = LoadState(state.data(), static_cast<uint32_t>(state.size()));
		const auto done = std::chrono::steady_clock::now();
		LogInfo("Host state self-test: {} bytes, save {} ms, {} loads {} ({} ms each)", state.size(),
		    std::chrono::duration_cast<std::chrono::milliseconds>(saved - start).count(), StateSelfTest, loaded ? "ok" : "FAILED",
		    std::chrono::duration_cast<std::chrono::milliseconds>(done - saved).count() / StateSelfTest);
	}
	if (Trace && result.inGame != 0) {
		static bool dialog = false;
		const bool now = qtextflag || IsPlayerInStore();
		if (now != dialog)
			LogInfo("HostTick: {} {}", now ? "dialog open" : "dialog closed", IsPlayerInStore() ? "(store)" : qtextflag ? "(quest text)" : "");
		dialog = now;
	}
	return result;
}

const HostFrame &HostSnapshot()
{
	return BuildSnapshot();
}

void HostShutdown()
{
	if (!Initialized)
		return;
	if (Active && !GameThreadFinished()) {
		// Ask the game to quit the way closing its window does, then let it run until DiabloMain returns (or the
		// menus end the thread through diablo_quit).
		for (int attempt = 0; attempt < 600 && !GameThreadFinished(); ++attempt) {
			if (attempt % 50 == 0) {
				SDL_Event event {};
#ifdef USE_SDL3
				event.type = SDL_EVENT_QUIT;
#else
				event.type = SDL_QUIT;
#endif
				QueueEvent(event);
			}
			AdvanceGameClock(50);
			ResumeGameThread();
		}
		if (!GameThreadFinished())
			LogError("HostShutdown: the game did not quit; abandoning its thread");
	}
	ReleaseInput();
	RemoveStateDirectory();
	StopGameThread();
	Active = false;
	ResetHostMenu();
}

const char *HostSoundPath(uint32_t soundId)
{
	if (soundId >= kHostMonsterSoundBase) {
		const uint32_t local = soundId - kHostMonsterSoundBase;
		const uint32_t type = local / 8;
		const uint32_t kind = (local % 8) / 2;
		const uint32_t variant = local % 2;
		if (type >= MonstersData.size() || kind > 3)
			return nullptr;
		constexpr const char *Prefixes[] { "a", "h", "d", "s" };
		SoundPathBuffer = StrCat("monsters\\", MonstersData[type].soundPath(), Prefixes[kind], variant + 1, ".wav");
		SoundPathBuffer = GetSoundAssetPath(SoundPathBuffer.c_str());
		return SoundPathBuffer.c_str();
	}
	if (soundId >= kHostMusicBase) {
		const uint32_t track = soundId - kHostMusicBase;
		if (track >= NUM_MUSIC)
			return nullptr;
		SoundPathBuffer = GetSoundAssetPath(GetMusicTrackPath(static_cast<_music_id>(track)));
		return SoundPathBuffer.c_str();
	}
	if (soundId > 0x7FFF)
		return nullptr;
	const TSFX *sfx = GetSfxEntry(static_cast<SfxID>(soundId));
	if (sfx == nullptr || sfx->pszName.empty())
		return nullptr;
	SoundPathBuffer = GetSoundAssetPath(sfx->pszName.c_str());
	return SoundPathBuffer.c_str();
}

uint32_t HostReadAsset(const char *path, uint8_t *buffer, uint32_t capacity)
{
	if (path == nullptr || path[0] == '\0')
		return 0;
	const std::string_view name { path };
	if ((name.starts_with("hd/") || name.starts_with("hd\\")) && !HdDirectory().empty()) {
		// The HD pack is loose files next to the game data, read like the game's HD loader does.
		std::string bytes;
		bool failed = false;
		if (!HdFetchText(path, bytes, failed, /*optional=*/true))
			return 0;
		if (buffer != nullptr && capacity > 0)
			std::memcpy(buffer, bytes.data(), std::min<size_t>(bytes.size(), capacity));
		return static_cast<uint32_t>(std::min<size_t>(bytes.size(), UINT32_MAX));
	}
	size_t size = 0;
	AssetHandle handle = OpenAsset(path, size, /*threadsafe=*/true);
	if (!handle.ok())
		return 0;
	if (buffer != nullptr && capacity > 0) {
		const size_t toRead = std::min<size_t>(size, capacity);
		if (!handle.read(buffer, toRead))
			return 0;
	}
	return static_cast<uint32_t>(std::min<size_t>(size, UINT32_MAX));
}

void HostSetHeroVoiceD2(bool d2)
{
	SetDiablo2HeroVoices(d2);
}

uint32_t HostApiVersionInUse()
{
	return ApiVersionInUse;
}

// ---- version 3 ----

void HostSetRenderMode(HostRenderMode mode)
{
	if (static_cast<uint32_t>(mode) > static_cast<uint32_t>(HostRenderMode::Hd2D))
		return;
	SetRenderMode(mode);
}

HostRenderMode HostGetRenderMode()
{
	return CurrentRenderMode();
}

void HostSetRenderModeCallback(HostRenderModeCallback callback, void *user)
{
	SetRenderModeCallback(callback, user);
}

uint32_t HostInstalledPacks()
{
	return InstalledPacks();
}

const char *HostHdDirectory()
{
	return HdDirectory().c_str();
}

const HostHdFrame &HostHdSnapshot()
{
	return BuildHdSnapshot(Active && InGame());
}

const HostEntityArt &HostEntityArtSnapshot()
{
	return BuildEntityArt(Active && InGame());
}

const HostEntityHdArt &HostEntityHdArtSnapshot()
{
	return BuildEntityHdArt(Active && InGame());
}

const HostWorldFrame &HostWorldSnapshot()
{
	return BuildWorldSnapshot(Active && InGame(), SnapshotEntities());
}

// ---- version 5 ----

const HostUiState &HostUiStateSnapshot()
{
	return BuildUiState();
}

const HostView2DFrame &HostView2DSnapshot()
{
	return BuildView2D();
}

const HostAudioState &HostAudioSnapshot()
{
	return BuildAudioState();
}

uint32_t HostSaveState(uint8_t *buffer, uint32_t capacity)
{
	return SaveState(buffer, capacity);
}

bool HostLoadState(const uint8_t *data, uint32_t size)
{
	return LoadState(data, size);
}

void RequestClipboard(std::string text)
{
	PendingClipboard = std::move(text);
}

const char *HostTakeClipboardText()
{
	static std::string text;
	text = std::exchange(PendingClipboard, {});
	return text.c_str();
}

const char *HostHeroName()
{
	static std::string name;
	name.clear();
	if (Active && InGame() && MyPlayer != nullptr)
		name = MyPlayer->_pName;
	return name.c_str();
}

} // namespace devilution::host
