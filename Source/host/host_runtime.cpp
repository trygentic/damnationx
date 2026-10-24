/**
 * @file host/host_runtime.cpp
 *
 * The game thread of the embedding API and the hooks the game files call (host/host_hooks.hpp).
 *
 * DiabloMain runs unchanged on its own thread (its menus, dialogs and loading screens are blocking loops). The host
 * thread and the game thread take turns: ResumeGameThread() lets the game run until it presents a frame, OnPresent()
 * (called by RenderPresent) converts that frame into the UI layer and parks the game thread again. Every frame of
 * the game is therefore one HostTick, and between two HostTick calls the game's state is quiescent, so HostSubmitInput
 * and HostSnapshot read and write it from the host thread without locks. The simulation runs on a virtual clock
 * (nthread.cpp reads GameClockMs()) that only HostTick advances, so the host decides how many 20 Hz ticks are due.
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <execinfo.h>
#include <pthread.h>
#endif

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/diabloui.h"
#include "appfat.h"
#include "diablo.h"
#include "effects.h"
#include "engine/dx.h"
#include "engine/palette.h"
#include "engine/backbuffer_state.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_ui.hpp"
#include "engine/assets.hpp"
#include "engine/sound.h"
#include "engine/sound_defs.hpp"
#include "engine/sound_position.hpp"
#include "game_mode.hpp"
#include "init.hpp"
#include "menu.h"
#include "msg.h"
#include "options.h"
#include "pfile.h"
#include "player.h"
#include "tables/monstdat.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"

namespace devilution {

int DiabloMain(int argc, char **argv);

namespace host {

bool Active = false;
HostOptions Options;
UiLayerState UiLayer;

namespace {

/** Whose turn it is. Exactly one of the two threads runs game code at any time. */
enum class Turn : uint8_t {
	Host,
	Game,
};

std::mutex TurnMutex;
std::condition_variable TurnChanged;
Turn CurrentTurn = Turn::Host;
bool ThreadRunning = false;
bool Finished = false;
/** The game thread ended in diablo_quit (fatal error or quit from a menu) and sleeps forever; it cannot be joined. */
bool Abandoned = false;
int ExitStatus = 0;
/** The game presented a frame: fatal errors are shown in the UI layer from now on. */
bool PresentedOnce = false;

uint32_t VirtualClockMs = 1;
uint32_t TicksSinceTake = 0;
uint32_t TicksTotal = 0;

std::mutex SoundMutex;
std::vector<HostSoundEvent> PendingSounds;

int GameArgc = 0;
char **GameArgv = nullptr;

#ifdef _WIN32
HANDLE GameThread = nullptr;
#else
pthread_t GameThread {};
#endif

/** The game thread hands control back and waits for its next turn. */
void ParkGameThread()
{
	std::unique_lock<std::mutex> lock(TurnMutex);
	CurrentTurn = Turn::Host;
	TurnChanged.notify_all();
	TurnChanged.wait(lock, [] { return CurrentTurn == Turn::Game; });
}

void FinishGameThread(int status, bool abandoned)
{
	std::unique_lock<std::mutex> lock(TurnMutex);
	Finished = true;
	Abandoned = abandoned;
	ExitStatus = status;
	CurrentTurn = Turn::Host;
	TurnChanged.notify_all();
}

std::thread::id GameThreadId;

void RunGame()
{
	GameThreadId = std::this_thread::get_id();
	{
		// Wait for the first turn (StartGameThread hands it over right away).
		std::unique_lock<std::mutex> lock(TurnMutex);
		TurnChanged.wait(lock, [] { return CurrentTurn == Turn::Game; });
	}
	const int status = DiabloMain(GameArgc, GameArgv);
	FinishGameThread(status, /*abandoned=*/false);
}

#ifdef _WIN32
DWORD WINAPI GameThreadEntry(LPVOID)
{
	RunGame();
	return 0;
}
#else
void *GameThreadEntry(void *)
{
	RunGame();
	return nullptr;
}
#endif

/** The game's menus, level loads and Lua need far more stack than a secondary thread gets by default (512 KiB on macOS). */
constexpr size_t GameThreadStackSize = 64U * 1024U * 1024U;

bool CreateGameThread()
{
#ifdef _WIN32
	GameThread = CreateThread(nullptr, GameThreadStackSize, GameThreadEntry, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	return GameThread != nullptr;
#else
	pthread_attr_t attributes;
	if (pthread_attr_init(&attributes) != 0)
		return false;
	pthread_attr_setstacksize(&attributes, GameThreadStackSize);
	const bool created = pthread_create(&GameThread, &attributes, GameThreadEntry, nullptr) == 0;
	pthread_attr_destroy(&attributes);
	return created;
#endif
}

/** Decibel attenuation (hundredths, VOLUME_MIN..0, as the game's mixer takes it) to a linear gain 0..1. */
float GainOf(int attenuation)
{
	if (attenuation <= VOLUME_MIN)
		return 0.0F;
	return std::clamp(std::pow(10.0F, static_cast<float>(std::min(attenuation, 0)) / 2000.0F), 0.0F, 1.0F);
}

void PushSound(const HostSoundEvent &event)
{
	const std::lock_guard<std::mutex> lock(SoundMutex);
	if (PendingSounds.size() < 4096)
		PendingSounds.push_back(event);
}

HostSoundEvent MakeSound(uint32_t soundId, float volume)
{
	HostSoundEvent event {};
	event.soundId = soundId;
	event.x = std::numeric_limits<float>::quiet_NaN();
	event.y = std::numeric_limits<float>::quiet_NaN();
	event.volume = volume;
	return event;
}

/** Positional attenuation as CalculateSoundPosition gives it; false when the sound is out of earshot. */
bool PositionalAttenuation(Point position, int &attenuation)
{
	if (MyPlayer == nullptr)
		return false;
	int pan = 0;
	return CalculateSoundPosition(position, &attenuation, &pan);
}

/** Converts the presented 8-bit frame into UiLayer: alpha 0 where the world shows through. */
void CaptureUiLayer()
{
	SDL_Surface *surface = PalSurface;
	if (surface == nullptr || surface->pixels == nullptr)
		return;
	const auto width = static_cast<uint32_t>(surface->w);
	const auto height = static_cast<uint32_t>(surface->h);
	const auto pitch = static_cast<size_t>(surface->pitch);
	const auto *pixels = static_cast<const uint8_t *>(surface->pixels);

	if (UiLayer.width != width || UiLayer.height != height) {
		UiLayer.width = width;
		UiLayer.height = height;
		UiLayer.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
		// Force a full upload.
		UiLayer.dirtyX0 = 0;
		UiLayer.dirtyY0 = 0;
		UiLayer.dirtyX1 = width;
		UiLayer.dirtyY1 = height;
		++UiLayer.revision;
	}

	// In a level the world rows were cleared to the transparent index (fpv::ClearWorldArea, since
	// fpv::SkipIsometricWorld() is true when embedded) and every UI pixel drawn over them was stamped Unlit in the
	// G-buffer's aux plane (render3d::BeginUiCoverage), which tells UI black from the world.
	const bool worldShows = InGame() && CurrentRenderMode() == HostRenderMode::World3D;
	const uint32_t worldRows = worldShows ? (HideHud ? height : std::min<uint32_t>(GetViewportHeight(), height)) : 0;
	const gbuffer::Planes &planes = gbuffer::CurrentPlanes;
	const bool haveCoverage = worldShows && planes.aux != nullptr && planes.outBase == pixels && planes.pitch == pitch
	    && planes.width >= width && planes.height >= worldRows;
	constexpr uint8_t UntouchedAux = gbuffer::PackAux(gbuffer::Kind::Floor, 0);
	constexpr uint8_t TransparentIndex = 0; // fpv::kTransparentWorldIndex

	std::array<uint32_t, 256> colors;
	for (size_t i = 0; i < 256; ++i) {
		const SDL_Color &c = system_palette[i];
		colors[i] = static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16 | 0xFF000000U;
	}

	uint32_t x0 = width, y0 = height, x1 = 0, y1 = 0;
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *row = pixels + y * pitch;
		const uint8_t *aux = haveCoverage && y < worldRows ? planes.aux + y * pitch : nullptr;
		auto *out = reinterpret_cast<uint32_t *>(UiLayer.rgba.data() + static_cast<size_t>(y) * width * 4);
		const bool worldRow = y < worldRows;
		uint32_t rowMin = width, rowMax = 0;
		for (uint32_t x = 0; x < width; ++x) {
			const uint8_t index = row[x];
			uint32_t value = colors[index];
			if (worldRow && index == TransparentIndex && (aux == nullptr || aux[x] == UntouchedAux))
				value = 0;
			if (out[x] != value) {
				out[x] = value;
				rowMin = std::min(rowMin, x);
				rowMax = x + 1;
			}
		}
		if (rowMax != 0) {
			x0 = std::min(x0, rowMin);
			x1 = std::max(x1, rowMax);
			y0 = std::min(y0, y);
			y1 = y + 1;
		}
	}
	if (x1 == 0)
		return; // unchanged
	if (UiLayer.dirtyX1 == 0) {
		UiLayer.dirtyX0 = x0;
		UiLayer.dirtyY0 = y0;
		UiLayer.dirtyX1 = x1;
		UiLayer.dirtyY1 = y1;
	} else {
		UiLayer.dirtyX0 = std::min(UiLayer.dirtyX0, x0);
		UiLayer.dirtyY0 = std::min(UiLayer.dirtyY0, y0);
		UiLayer.dirtyX1 = std::max(UiLayer.dirtyX1, x1);
		UiLayer.dirtyY1 = std::max(UiLayer.dirtyY1, y1);
	}
	++UiLayer.revision;
}

std::string ToLower(std::string_view text)
{
	std::string result(text);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return result;
}

HeroClass ParseHeroClass(std::string_view name)
{
	const std::string lower = ToLower(name);
	if (lower == "rogue") return HeroClass::Rogue;
	if (lower == "sorcerer" || lower == "sorceror") return HeroClass::Sorcerer;
	if (lower == "monk") return HeroClass::Monk;
	if (lower == "bard") return HeroClass::Bard;
	if (lower == "barbarian") return HeroClass::Barbarian;
	return HeroClass::Warrior;
}

std::optional<HostRenderMode> PendingMode;
HostRenderMode RenderMode = HostRenderMode::World3D;
bool ApplyingHostMode = false;
bool RenderModeChanged = false;
HostRenderModeCallback ModeCallback = nullptr;
void *ModeCallbackUser = nullptr;

/** Graphics > Render Mode changed (settings menu, or HostSetRenderMode). */
void OnRenderModeOptionChanged()
{
	const auto mode = static_cast<HostRenderMode>(*GetOptions().Graphics.hostRenderMode);
	if (mode == RenderMode)
		return;
	RenderMode = mode;
	if (!Active)
		return;
	// Nothing recorded for the old mode may outlive the switch; the next frame is drawn from scratch.
	ResetHdRecords();
	RedrawEverything();
	if (!ApplyingHostMode && PresentedOnce) { // the player changed it in the settings menu
		RenderModeChanged = true;
		if (ModeCallback != nullptr)
			ModeCallback(mode, ModeCallbackUser);
	}
}

const auto OptionChangeHandlerRenderMode = (GetOptions().Graphics.hostRenderMode.SetValueChangedCallback(OnRenderModeOptionChanged), true);

bool AutoStartPending = false;
bool AutoStartTaken = false;
bool FoundHero = false;
_uiheroinfo FoundHeroInfo {};

bool FindAutoStartHero(_uiheroinfo *hero)
{
	if (!FoundHero && Options.heroName == hero->name) {
		FoundHero = true;
		FoundHeroInfo = *hero;
	}
	return true;
}

} // namespace

// ---- game thread control (host thread) ----

bool StartGameThread(int argc, char **argv)
{
	GameArgc = argc;
	GameArgv = argv;
	Finished = false;
	Abandoned = false;
	PresentedOnce = false;
	CurrentTurn = Turn::Host;
	AutoStartPending = !Options.heroName.empty();
	AutoStartTaken = false;
	if (!CreateGameThread()) {
		LogError("HostInit: cannot create the game thread");
		return false;
	}
	ThreadRunning = true;
	ResumeGameThread();
	return !Finished;
}

void ResumeGameThread()
{
	std::unique_lock<std::mutex> lock(TurnMutex);
	if (!ThreadRunning || Finished)
		return;
	CurrentTurn = Turn::Game;
	TurnChanged.notify_all();
	TurnChanged.wait(lock, [] { return CurrentTurn == Turn::Host; });
}

void AdvanceGameClock(uint32_t elapsedMs)
{
	VirtualClockMs += elapsedMs;
}

bool GameThreadFinished()
{
	return Finished;
}

void StopGameThread()
{
	if (!ThreadRunning)
		return;
	if (Finished && !Abandoned) {
#ifdef _WIN32
		WaitForSingleObject(GameThread, INFINITE);
		CloseHandle(GameThread);
#else
		pthread_join(GameThread, nullptr);
#endif
	} else {
#ifdef _WIN32
		CloseHandle(GameThread);
#else
		pthread_detach(GameThread);
#endif
	}
	ThreadRunning = false;
}

uint32_t TakeTicksRun()
{
	const uint32_t ticks = TicksSinceTake;
	TicksSinceTake = 0;
	return ticks;
}

uint32_t TotalTicksRun()
{
	return TicksTotal;
}

void TakeSounds(std::vector<HostSoundEvent> &out)
{
	out.clear();
	const std::lock_guard<std::mutex> lock(SoundMutex);
	out.swap(PendingSounds);
}

HostRenderMode CurrentRenderMode()
{
	return RenderMode;
}

void SetRenderMode(HostRenderMode mode)
{
	if (!ThreadRunning || Finished) {
		PendingMode = mode;
		RenderMode = mode;
		return;
	}
	ApplyingHostMode = true;
	GetOptions().Graphics.hostRenderMode.SetValue(static_cast<HostRenderModeOption>(mode));
	OnRenderModeOptionChanged(); // in case SetValue does not notify
	ApplyingHostMode = false;
}

bool TakeRenderModeChanged()
{
	const bool changed = RenderModeChanged;
	RenderModeChanged = false;
	return changed;
}

void SetRenderModeCallback(HostRenderModeCallback callback, void *user)
{
	ModeCallback = callback;
	ModeCallbackUser = user;
}

bool DrawsWorld3D()
{
	return Active && RenderMode == HostRenderMode::World3D;
}

bool OnGameThread()
{
	return std::this_thread::get_id() == GameThreadId;
}

float VolumeGain(int volume)
{
	return GainOf(volume);
}

bool InGame()
{
	return gbRunGame && MyPlayer != nullptr && !MyPlayer->_pLvlChanging && ThreadRunning && !Finished;
}

// ---- hooks (game thread) ----

void InitVideo()
{
	HideHud = false;
#ifdef USE_SDL3
	if (!SDL_Init(SDL_INIT_EVENTS))
		app_fatal(SDL_GetError());
#else
	if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
		app_fatal(SDL_GetError());
#endif
	if (forceResolution.width == 0 || forceResolution.height == 0)
		forceResolution = { 640, 480 };
	AdjustToScreenGeometry(forceResolution);
	InitHdPack(); // before the HD UI hooks and any UI art load
	dx_init();
	HdUiInstall(); // the HD UI tracks UI art from its first load (drawn in the Hd2D mode only)
	gbActive = true;

	// The render mode setting is listed only when embedded; the host's choice (HostSetRenderMode before HostInit)
	// wins over the saved one.
	OptionEntryEnum<HostRenderModeOption> &option = GetOptions().Graphics.hostRenderMode;
	option.flags &= ~OptionEntryFlags::Invisible;
	if (PendingMode) {
		ApplyingHostMode = true;
		option.SetValue(static_cast<HostRenderModeOption>(*PendingMode));
		ApplyingHostMode = false;
		PendingMode.reset();
	}
	RenderMode = static_cast<HostRenderMode>(*option);
}

void OnPresent()
{
	if (!OnGameThread()) {
		// The host thread runs game code between ticks (HostLoadState): keep its frame, never park the host.
		OnHdPresent(InGame());
		CaptureUiLayer();
		if (InGame())
			gbuffer::EndFrame();
		return;
	}
	OnHdPresent(InGame());
	CaptureUiLayer();
	if (InGame())
		gbuffer::EndFrame(); // as after a WebGPU present: the UI coverage stamping ends with the frame
	PresentedOnce = true;
	ParkGameThread();
}

uint32_t GameClockMs()
{
	return VirtualClockMs;
}

void OnGameTick()
{
	++TicksSinceTake;
	++TicksTotal;
}

void OnGameQuit(int exitStatus)
{
	LogInfo("Embedded game ended with status {}", exitStatus);
	FinishGameThread(exitStatus, /*abandoned=*/true);
	// diablo_quit must not return and must not end the host's process: this thread sleeps until the process exits.
	std::unique_lock<std::mutex> lock(TurnMutex);
	TurnChanged.wait(lock, [] { return false; });
	std::abort(); // unreachable
}

bool ShowFatalDialogs()
{
#ifndef _WIN32
	// Called when the game hits a fatal error (app_fatal, a failed assertion): log where it came from, the host keeps
	// running and the player only sees the game's error dialog.
	void *frames[64];
	const int count = backtrace(frames, 64);
	LogError("Embedded game: fatal error, stack:");
	backtrace_symbols_fd(frames, count, 2);
#endif
	return PresentedOnce;
}

void QueueSfx(int16_t sfxId, bool positional, Point position)
{
	if (!gbSoundOn || gbBufferMsgs != 0 || sfxId < 0)
		return;
	if (MyPlayer != nullptr && MyPlayer->pLvlLoad != 0 && gbIsMultiplayer)
		return;
	const bool cue = sfxId >= static_cast<int16_t>(SfxID::AccessibilityWeapon) && sfxId <= static_cast<int16_t>(SfxID::AccessibilityInteract);
	const int userVolume = cue ? *GetOptions().Audio.audioCuesVolume : *GetOptions().Audio.soundVolume;
	int attenuation = 0;
	if (positional && !PositionalAttenuation(position, attenuation))
		return;
	HostSoundEvent event = MakeSound(static_cast<uint32_t>(sfxId), GainOf(attenuation + userVolume));
	if (positional) {
		event.x = static_cast<float>(position.x) + 0.5F;
		event.y = static_cast<float>(position.y) + 0.5F;
	}
	PushSound(event);
}

void QueueMonsterSfx(int monsterType, int mode, int variant, Point position)
{
	if (!gbSoundOn || monsterType < 0 || static_cast<size_t>(monsterType) >= MonstersData.size())
		return;
	if (mode == 3 && !MonstersData[monsterType].hasSpecialSound)
		return;
	int attenuation = 0;
	if (!PositionalAttenuation(position, attenuation))
		return;
	HostSoundEvent event = MakeSound(kHostMonsterSoundBase + static_cast<uint32_t>(monsterType) * 8 + static_cast<uint32_t>(mode) * 2 + static_cast<uint32_t>(variant & 1),
	    GainOf(attenuation + *GetOptions().Audio.soundVolume));
	event.x = static_cast<float>(position.x) + 0.5F;
	event.y = static_cast<float>(position.y) + 0.5F;
	PushSound(event);
}

void QueueMusic(int track)
{
	HostSoundEvent event = MakeSound(kHostMusicBase + static_cast<uint32_t>(track), GainOf(*GetOptions().Audio.musicVolume));
	event.loop = 1;
	PushSound(event);
}

void StopMusic()
{
	HostSoundEvent event = MakeSound(kHostMusicBase + static_cast<uint32_t>(sgnMusicTrack), 0.0F);
	event.stop = 1;
	PushSound(event);
}

int SfxLengthMs(int16_t sfxId)
{
	constexpr int Fallback = 10000;
	const TSFX *entry = GetSfxEntry(static_cast<SfxID>(sfxId));
	if (entry == nullptr || entry->pszName.empty())
		return Fallback;
	size_t size = 0;
	AssetHandle handle = OpenAsset(entry->pszName, size);
	if (!handle.ok() || size < 12)
		return Fallback;
	std::vector<uint8_t> bytes(std::min<size_t>(size, 1024));
	if (!handle.read(bytes.data(), bytes.size()))
		return Fallback;
	const auto u32 = [&](size_t at) {
		return at + 4 <= bytes.size() ? static_cast<uint32_t>(bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | bytes[at + 3] << 24) : 0U;
	};
	if (std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
		uint32_t byteRate = 0;
		for (size_t at = 12; at + 8 <= bytes.size();) {
			const uint32_t chunkSize = u32(at + 4);
			if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0) {
				byteRate = u32(at + 8 + 8);
			} else if (std::memcmp(bytes.data() + at, "data", 4) == 0 && byteRate != 0) {
				const uint64_t dataSize = std::min<uint64_t>(chunkSize, size - std::min<size_t>(size, at + 8));
				return std::max(1, static_cast<int>(dataSize * 1000 / byteRate));
			}
			at += 8 + chunkSize + (chunkSize & 1);
		}
	}
	// Not a PCM wav (an mp3 voice pack): about 128 kbit/s.
	return std::max(1000, static_cast<int>(size / 16));
}

void StopStream(int16_t sfxId)
{
	HostSoundEvent event = MakeSound(static_cast<uint32_t>(sfxId), 0.0F);
	event.stop = 1;
	PushSound(event);
}

bool TakeAutoStart()
{
	if (!AutoStartPending || AutoStartTaken)
		return false;
	AutoStartTaken = true;
	gbIsMultiplayer = false;
	return true;
}

bool SelectAutoStartHero(GameData *gameData)
{
	if (!AutoStartPending || gbIsMultiplayer)
		return false;
	AutoStartPending = false; // later games go through the hero selection dialog

	FoundHero = false;
	pfile_ui_set_hero_infos(FindAutoStartHero);
	if (!FoundHero) {
		_uiheroinfo hero {};
		hero.saveNumber = pfile_ui_get_first_unused_save_num();
		hero.heroclass = ParseHeroClass(Options.heroClass);
		CopyUtf8(hero.name, Options.heroName, sizeof(hero.name));
		if (!pfile_ui_save_create(&hero)) {
			LogError("--host-hero: cannot create the hero {}", Options.heroName);
			return false;
		}
		LogInfo("--host-hero: created {}", hero.name);
		FoundHeroInfo = hero;
		FoundHeroInfo.hassaved = false;
	}
	gSaveNumber = FoundHeroInfo.saveNumber;
	gbLoadGame = Options.continueGame && FoundHeroInfo.hassaved;
	if (gameData != nullptr)
		gameData->nDifficulty = DIFF_NORMAL;
	return true;
}

} // namespace host
} // namespace devilution
