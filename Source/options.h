#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <forward_list>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef USE_SDL3
#include <SDL3/SDL_version.h>

#ifndef NOSOUND
#include <SDL3/SDL_audio.h>
#endif
#else
#include <SDL_version.h>
#endif

#include <ankerl/unordered_dense.h>
#include <function_ref.hpp>

#include "appfat.h"
#include "controls/controller_buttons.h"
#include "engine/size.hpp"
#include "engine/sound_defs.hpp"
#include "mods/mod_identity.h"
#include "pack.h"
#include "quick_messages.hpp"
#include "utils/enum_traits.h"
#include "utils/string_view_hash.hpp"

namespace devilution {

#ifndef DEFAULT_WIDTH
#define DEFAULT_WIDTH 640
#endif
#ifndef DEFAULT_HEIGHT
#define DEFAULT_HEIGHT 480
#endif

enum class StartUpGameMode : uint8_t {
	/** @brief If hellfire is present, asks the user what game they want to start. */
	Ask = 0,
	Hellfire = 1,
	Diablo = 2,
};

enum class StartUpIntro : uint8_t {
	Off = 0,
	Once = 1,
	On = 2,
};

/** @brief Defines what splash screen should be shown at startup. */
enum class StartUpSplash : uint8_t {
	/** @brief Show no splash screen. */
	None = 0,
	/** @brief Show only TitleDialog. */
	TitleDialog = 1,
	/** @brief Show Logo and TitleDialog. */
	LogoAndTitleDialog = 2,
};

enum class ScalingQuality : uint8_t {
	NearestPixel,
	BilinearFiltering,
	AnisotropicFiltering,
};

/** @brief Presentation-only dynamic-lighting quality budget. */
enum class LightingQuality : uint8_t {
	Low,
	High,
};

/** @brief Debug views of the 2.5D hybrid G-buffer / lighting (presentation-only). */
enum class GBufferDebugView : uint8_t {
	Off,
	Kind,
	Height,
	Normal,
	Lighting,
	Volume,
	Emissive,
};

enum class FrameRateControl : uint8_t {
	None = 0,
#ifndef USE_SDL1
	VerticalSync = 1,
#endif
	CPUSleep = 2,
};

enum class Resampler : uint8_t {
#ifdef DEVILUTIONX_RESAMPLER_SPEEX
	Speex = 0,
#endif
#ifdef DVL_AULIB_SUPPORTS_SDL_RESAMPLER
	SDL,
#endif
};

enum class StoreUi : uint8_t {
	/** @brief Vanilla Diablo UI. */
	Text = 0,
	/** @brief Show item graphics to the left of item descriptions in store menus. */
	ListWithItemGraphics = 1,
	/** @brief Use visual grid-based store UI instead of text-based menus. */
	VisualGrid = 2,
};

/** @brief The sounds the game plays (see d2sound/d2sound_import.hpp). */
enum class SoundPack : uint8_t {
	/** @brief The original sounds. */
	Diablo,
	/** @brief Imported from an installed Diablo II. */
	Diablo2,
	/** @brief Imported from an installed Diablo II: Resurrected (its original-quality sounds). */
	Diablo2ResurrectedClassic,
	/** @brief Imported from an installed Diablo II: Resurrected (its remastered sounds). */
	Diablo2ResurrectedAdvanced,
};

std::string_view ResamplerToString(Resampler resampler);
std::optional<Resampler> ResamplerFromString(std::string_view resampler);

enum class OptionEntryType : uint8_t {
	Boolean,
	List,
	Key,
	PadButton,
	/** @brief Has no value: selecting it in the settings menu runs an action. */
	Action,
};

enum class OptionEntryFlags : uint8_t {
	/** @brief No special logic. */
	None = 0,
	/** @brief Shouldn't be shown in settings dialog. */
	Invisible = 1 << 0,
	/** @brief Need to restart the current running game (single- or multiplayer) to take effect. */
	CantChangeInGame = 1 << 1,
	/** @brief Need to restart the current running multiplayer game to take effect. */
	CantChangeInMultiPlayer = 1 << 2,
	/** @brief Option is only relevant for Hellfire. */
	OnlyHellfire = 1 << 3,
	/** @brief Option is only relevant for Diablo. */
	OnlyDiablo = 1 << 4,
	/** @brief After option is changed the UI needs to be recreated. */
	RecreateUI = 1 << 5,
	/** @brief diablo.mpq must be present. */
	NeedDiabloMpq = 1 << 6,
};
use_enum_as_flags(OptionEntryFlags);

class OptionEntryBase {
public:
	OptionEntryBase(std::string_view key, OptionEntryFlags flags, const char *name, const char *description)
	    : flags(flags)
	    , key(key)
	    , name(name)
	    , description(description)
	{
	}
	[[nodiscard]] virtual std::string_view GetName() const;
	[[nodiscard]] std::string_view GetDescription() const;
	[[nodiscard]] virtual OptionEntryType GetType() const = 0;
	[[nodiscard]] OptionEntryFlags GetFlags() const;

	void SetValueChangedCallback(tl::function_ref<void()> callback);

	[[nodiscard]] virtual std::string_view GetValueDescription() const = 0;
	virtual void LoadFromIni(std::string_view category) = 0;
	virtual void SaveToIni(std::string_view category) const = 0;

	OptionEntryFlags flags;

public:
	std::string_view key;

protected:
	const char *name;
	const char *description;
	void NotifyValueChanged();

private:
	std::optional<tl::function_ref<void()>> callback_;
};

class OptionEntryBoolean : public OptionEntryBase {
public:
	OptionEntryBoolean(std::string_view key, OptionEntryFlags flags, const char *name, const char *description, bool defaultValue)
	    : OptionEntryBase(key, flags, name, description)
	    , defaultValue(defaultValue)
	    , value(defaultValue)
	{
	}
	[[nodiscard]] bool operator*() const
	{
		return value;
	}
	void SetValue(bool value);

	[[nodiscard]] OptionEntryType GetType() const override;
	[[nodiscard]] std::string_view GetValueDescription() const override;
	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

private:
	bool defaultValue;
	bool value;
};

/**
 * @brief A settings menu entry that runs an action (its value changed callback) when selected.
 *
 * The settings menu runs the action after tearing down its own UI, so the action can show
 * dialogs (see `DeferSettingsMenuAction`).
 */
class OptionEntryAction : public OptionEntryBase {
public:
	OptionEntryAction(std::string_view key, OptionEntryFlags flags, const char *name, const char *description)
	    : OptionEntryBase(key, flags, name, description)
	{
	}

	/** @brief Runs the action. */
	void Activate();

	[[nodiscard]] OptionEntryType GetType() const override;
	[[nodiscard]] std::string_view GetValueDescription() const override;
	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;
};

class OptionEntryListBase : public OptionEntryBase {
public:
	[[nodiscard]] virtual size_t GetListSize() const = 0;
	[[nodiscard]] virtual std::string_view GetListDescription(size_t index) const = 0;
	[[nodiscard]] virtual size_t GetActiveListIndex() const = 0;
	virtual void SetActiveListIndex(size_t index) = 0;

	[[nodiscard]] OptionEntryType GetType() const override;
	[[nodiscard]] std::string_view GetValueDescription() const override;

protected:
	OptionEntryListBase(std::string_view key, OptionEntryFlags flags, const char *name, const char *description)
	    : OptionEntryBase(key, flags, name, description)
	{
	}
};

class OptionEntryEnumBase : public OptionEntryListBase {
public:
	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

protected:
	OptionEntryEnumBase(std::string_view key, OptionEntryFlags flags, const char *name, const char *description, int defaultValue)
	    : OptionEntryListBase(key, flags, name, description)
	    , defaultValue(defaultValue)
	    , value(defaultValue)
	{
	}

	[[nodiscard]] int GetValueInternal() const
	{
		return value;
	}
	void SetValueInternal(int value);

	void AddEntry(int value, std::string_view name);

private:
	int defaultValue;
	int value;
	std::vector<std::string_view> entryNames;
	std::vector<int> entryValues;
};

template <typename T>
class OptionEntryEnum : public OptionEntryEnumBase {
public:
	OptionEntryEnum(std::string_view key, OptionEntryFlags flags, const char *name, const char *description, T defaultValue, std::initializer_list<std::pair<T, std::string_view>> entries)
	    : OptionEntryEnumBase(key, flags, name, description, static_cast<int>(defaultValue))
	{
		for (auto &&[entryValue, entryName] : entries) {
			AddEntry(static_cast<int>(entryValue), entryName);
		}
	}
	[[nodiscard]] T operator*() const
	{
		return static_cast<T>(GetValueInternal());
	}
	void SetValue(T value)
	{
		SetValueInternal(static_cast<int>(value));
	}
};

class OptionEntryIntBase : public OptionEntryListBase {
public:
	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

protected:
	OptionEntryIntBase(std::string_view key, OptionEntryFlags flags, const char *name, const char *description, int defaultValue)
	    : OptionEntryListBase(key, flags, name, description)
	    , defaultValue(defaultValue)
	    , value(defaultValue)
	{
	}

	[[nodiscard]] int GetValueInternal() const
	{
		return value;
	}
	void SetValueInternal(int value);

	void AddEntry(int value);

private:
	int defaultValue;
	int value;
	mutable std::vector<std::string> entryNames;
	std::vector<int> entryValues;
};

template <typename T>
class OptionEntryInt : public OptionEntryIntBase {
public:
	OptionEntryInt(std::string_view key, OptionEntryFlags flags, const char *name, const char *description, T defaultValue, std::initializer_list<T> entries)
	    : OptionEntryIntBase(key, flags, name, description, static_cast<int>(defaultValue))
	{
		for (auto entry : entries) {
			AddEntry(static_cast<int>(entry));
		}
	}
	OptionEntryInt(std::string_view key, OptionEntryFlags flags, const char *name, const char *description, T defaultValue)
	    : OptionEntryInt(key, flags, name, description, defaultValue, { defaultValue })
	{
	}
	[[nodiscard]] T operator*() const
	{
		return static_cast<T>(GetValueInternal());
	}
	void SetValue(T value)
	{
		SetValueInternal(static_cast<int>(value));
	}
};

class OptionEntryLanguageCode : public OptionEntryListBase {
public:
	OptionEntryLanguageCode();

	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

	std::string_view operator*() const
	{
		return szCode;
	}

	OptionEntryLanguageCode &operator=(std::string_view code)
	{
		assert(code.size() < 6);
		memcpy(szCode, code.data(), code.size());
		szCode[code.size()] = '\0';
		return *this;
	}

private:
	/** @brief Language code (ISO-15897) for text. */
	char szCode[6];
	mutable std::vector<std::pair<std::string, std::string>> languages;

	void CheckLanguagesAreInitialized() const;
};

class OptionEntryResolution : public OptionEntryListBase {
public:
	OptionEntryResolution();

	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

	void setAvailableResolutions(std::vector<std::pair<Size, std::string>> &&resolutions)
	{
		resolutions_ = std::move(resolutions);
	}

	Size operator*() const { return size_; }

private:
	/** @brief View size. */
	Size size_;
	std::vector<std::pair<Size, std::string>> resolutions_;
};

/**
 * @brief A scale kept in quarters (4 = 1x .. 16 = 4x), saved as "1", "1.25", .. "4" and listed as "1x", "1.25x", ..
 */
class OptionEntryQuarterScale : public OptionEntryListBase {
public:
	static constexpr int MinQuarters = 4;
	static constexpr int MaxQuarters = 16;

	OptionEntryQuarterScale(std::string_view key, OptionEntryFlags flags, const char *name, const char *description)
	    : OptionEntryListBase(key, flags, name, description)
	{
	}

	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

	/** @brief The stored scale in quarters (4 = 1x). */
	[[nodiscard]] int operator*() const { return quarters_; }
	/** @brief Sets the scale in quarters, clamped to [MinQuarters, MaxQuarters]. */
	void SetValue(int quarters);

	/** @brief "1", "1.25", "1.5", "1.75", "2", .. for a scale in quarters. */
	[[nodiscard]] static std::string FormatQuarters(int quarters);

protected:
	/** @brief Reads `key` as 1, 1.25, .. (missing: `fallback`). */
	void LoadQuarters(std::string_view category, float fallback);

	int quarters_ = MinQuarters;
};

/**
 * @brief How many screen pixels a world pixel covers, stored as "Zoom Level" (1, 1.25, .. 4).
 *
 * Kept in quarters (4 = 1x .. 16 = 4x). Fractional levels only apply while the HD art draws the whole world
 * (GetZoomQuarters() rounds to the nearest whole level otherwise). Without a saved "Zoom Level" the legacy boolean
 * "Zoom" key decides (on = 2x), and saving also writes "Zoom" (on when zoomed at all), so older builds keep a sensible
 * setting.
 */
class OptionEntryZoomLevel : public OptionEntryQuarterScale {
public:
	using OptionEntryQuarterScale::OptionEntryQuarterScale;

	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;
};

class OptionEntryResampler : public OptionEntryListBase {
public:
	OptionEntryResampler();

	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

	Resampler operator*() const
	{
		return resampler_;
	}

private:
	void UpdateDependentOptions() const;

	Resampler resampler_;
};

class OptionEntryAudioDevice : public OptionEntryListBase {
public:
	OptionEntryAudioDevice();

	void LoadFromIni(std::string_view category) override;
	void SaveToIni(std::string_view category) const override;

	[[nodiscard]] size_t GetListSize() const override;
	[[nodiscard]] std::string_view GetListDescription(size_t index) const override;
	[[nodiscard]] size_t GetActiveListIndex() const override;
	void SetActiveListIndex(size_t index) override;

	std::string operator*() const
	{
		for (size_t i = 0; i < GetListSize(); i++) {
			std::string_view deviceName = GetDeviceName(i);
			if (deviceName == deviceName_)
				return deviceName_;
		}
		return "";
	}

#ifdef USE_SDL3
	[[nodiscard]] SDL_AudioDeviceID id() const;
#endif

private:
	std::string_view GetDeviceName(size_t index) const;

	std::string deviceName_;
#ifdef USE_SDL3
	SDL_AudioDeviceID deviceId_ = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
#endif
};

struct OptionCategoryBase {
	OptionCategoryBase(std::string_view key, const char *name, const char *description)
	    : key(key)
	    , name(name)
	    , description(description)
	{
	}

	[[nodiscard]] std::string_view GetKey() const;
	[[nodiscard]] std::string_view GetName() const;
	[[nodiscard]] std::string_view GetDescription() const;

	virtual std::vector<OptionEntryBase *> GetEntries() = 0;

protected:
	std::string_view key;
	const char *name;
	const char *description;
};

struct GameModeOptions : OptionCategoryBase {
	GameModeOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	OptionEntryEnum<StartUpGameMode> gameMode;
	OptionEntryBoolean shareware;
};

struct StartUpOptions : OptionCategoryBase {
	StartUpOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Play game intro video on diablo startup. */
	OptionEntryEnum<StartUpIntro> diabloIntro;
	/** @brief Play game intro video on hellfire startup. */
	OptionEntryEnum<StartUpIntro> hellfireIntro;
	OptionEntryEnum<StartUpSplash> splash;
};

struct DiabloOptions : OptionCategoryBase {
	DiabloOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Remembers what singleplayer hero/save was last used. */
	OptionEntryInt<std::uint32_t> lastSinglePlayerHero;
	/** @brief Remembers what multiplayer hero/save was last used. */
	OptionEntryInt<std::uint32_t> lastMultiplayerHero;
};

struct HellfireOptions : OptionCategoryBase {
	HellfireOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Cornerstone of the world item. */
	char szItem[sizeof(ItemPack) * 2 + 1];
	/** @brief Remembers what singleplayer hero/save was last used. */
	OptionEntryInt<std::uint32_t> lastSinglePlayerHero;
	/** @brief Remembers what multiplayer hero/save was last used. */
	OptionEntryInt<std::uint32_t> lastMultiplayerHero;
};

struct AudioOptions : OptionCategoryBase {
	AudioOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Movie and SFX volume. */
	OptionEntryInt<int> soundVolume;
	/** @brief Accessibility / navigation cues volume. */
	OptionEntryInt<int> audioCuesVolume;
	/** @brief Music volume. */
	OptionEntryInt<int> musicVolume;
	/** @brief Player emits sound when walking. */
	OptionEntryBoolean walkingSound;
	/** @brief Automatically equipping items on pickup emits the equipment sound. */
	OptionEntryBoolean autoEquipSound;
	/** @brief Picking up items emits the items pickup sound. */
	OptionEntryBoolean itemPickupSound;

	/** @brief Output sample rate (Hz). */
	OptionEntryInt<std::uint32_t> sampleRate;
	/** @brief The number of output channels (1 or 2) */
	OptionEntryInt<std::uint8_t> channels;
	/** @brief Buffer size (number of frames per channel) */
	OptionEntryInt<std::uint32_t> bufferSize;
	/** @brief Resampler implementation. */
	OptionEntryResampler resampler;
	/** @brief Quality of the resampler, from 0 (lowest) to 10 (highest). Available for the speex resampler only. */
	OptionEntryInt<std::uint8_t> resamplingQuality;
	/** @brief Audio device. */
	OptionEntryAudioDevice device;
#ifdef DEVILUTIONX_D2_SOUND_IMPORT
	/** @brief Sounds imported from Diablo II / Diablo II: Resurrected that replace the original ones. */
	OptionEntryEnum<SoundPack> soundPack;
	OptionEntryAction importDiablo2Sounds;
	OptionEntryAction importDiablo2ResurrectedSounds;
	/** @brief Diablo II install folder (empty: detect it). Only settable in diablo.ini. */
	std::string diablo2Path;
	/** @brief Diablo II: Resurrected install folder (empty: detect it). Only settable in diablo.ini. */
	std::string diablo2ResurrectedPath;
#endif
};

struct GraphicsOptions : OptionCategoryBase {
	GraphicsOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	OptionEntryResolution resolution;
	/** @brief Run in fullscreen or windowed mode. */
	OptionEntryBoolean fullscreen;
#if !defined(USE_SDL1) || defined(__3DS__)
	/** @brief Expand the aspect ratio to match the screen. */
	OptionEntryBoolean fitToScreen;
#endif
#ifndef USE_SDL1
	/** @brief Scale the image after rendering. */
	OptionEntryBoolean upscale;
	/** @brief See SDL_HINT_RENDER_SCALE_QUALITY. */
	OptionEntryEnum<ScalingQuality> scaleQuality;
	/** @brief Only scale by values divisible by the width and height. */
	OptionEntryBoolean integerScaling;
#endif
	/** @brief Limit frame rate either for vsync or CPU load. */
	OptionEntryEnum<FrameRateControl> frameRateControl;
	/** @brief Brightness level. */
	OptionEntryInt<int> brightness;
	/** @brief Screen pixels per world pixel in quarters (4 = no zoom), changed in game with Ctrl (Cmd on macOS) + mouse wheel. */
	OptionEntryZoomLevel zoomLevel;
	/** @brief Screen pixels per in-game UI pixel in quarters (4 = 1x), independent of the zoom (ui_scale.hpp). */
	OptionEntryQuarterScale uiScale;
	/** @brief Use the HD art pack (an `hd` folder next to the game or its data) when the WebGPU renderer runs. */
	OptionEntryBoolean hdArt;
	/** @brief Draw the HD art pack when this run has one (hd_graphics.hpp); toggled in game with H. */
	OptionEntryBoolean hdGraphics;
	/** @brief Subtile lighting for smoother light gradients. */
	OptionEntryBoolean perPixelLighting;
	/** @brief Trace presentation-only light visibility in the WebGPU compute pipeline. */
	OptionEntryBoolean rayTracedLighting;
	OptionEntryEnum<LightingQuality> lightingQuality;
	OptionEntryBoolean debugLightList;
	OptionEntryBoolean debugLightGeometry;
	OptionEntryBoolean debugLightDiagnostics;
	OptionEntryBoolean bloom;
	/** @brief 2.5D hybrid: full-bright CPU frame + G-buffer, GPU ray-marched lighting (WebGPU builds only). */
	OptionEntryBoolean hybridLighting;
	/** @brief Generate per-sprite normal maps at load (silhouette + luminance). Off = flat card normals. */
	OptionEntryBoolean hybridNormalMaps;
	/** @brief Ambient scale for hybrid lighting, percent of the classic light level. */
	OptionEntryInt<int> hybridAmbient;
	/** @brief G-buffer / lighting debug view for the hybrid presenter. */
	OptionEntryEnum<GBufferDebugView> hybridDebugView;
	/** @brief Draw the Warrior as the skinned 3D model (render3d/skinned_renderer.hpp) instead of its prerendered HD sprites (WebGPU builds only). */
	OptionEntryBoolean warrior3dModel;
	/** @brief Enable color cycling animations. */
	OptionEntryBoolean colorCycling;
	/** @brief Use alternate nest palette. */
	OptionEntryBoolean alternateNestArt;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	/** @brief Use a hardware cursor (SDL2 only). */
	OptionEntryBoolean hardwareCursor;
	/** @brief Use a hardware cursor for items. */
	OptionEntryBoolean hardwareCursorForItems;
	/** @brief Maximum width / height for the hardware cursor. Larger cursors fall back to software. */
	OptionEntryInt<int> hardwareCursorMaxSize;
#endif
	/** @brief Show FPS, even without the -f command line flag. */
	OptionEntryBoolean showFPS;
	/** @brief First-person view over the WebGPU 3D world (WebGPU builds only, toggled in game with G). */
	OptionEntryBoolean firstPersonView;
	/** @brief Horizontal field of view in degrees for the first-person view. */
	OptionEntryInt<int> fpvFieldOfView;
	/** @brief Show the 3D weapon viewmodel in the first-person view (the 2D slash overlay replaces it when off). */
	OptionEntryBoolean fpvWeapon;
	/** @brief Draw the red outline around the monster under the cursor (off by default: it does not follow HD sprites). */
};

struct GameplayOptions : OptionCategoryBase {
	GameplayOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Gameplay ticks per second. */
	OptionEntryInt<int> tickRate;
	/** @brief Enable double walk speed when in town. */
	OptionEntryBoolean runInTown;
	/** @brief Show the run button and enable movement-only sprinting with stamina. */
	OptionEntryBoolean enableRun;
	/** @brief Free-form (Diablo II style) movement: straight paths at any angle and smooth sub-tile drawing. */
	OptionEntryBoolean smoothMovement;
	/** @brief A permanent portal next to the town fountain leads to the Cathedral (dungeon level 1). */
	OptionEntryBoolean cathedralPortal;
	/** @brief Mouse look speed in the first-person view (5 = default). */
	OptionEntryInt<int> fpvMouseSensitivity;
	/** @brief Invert vertical mouse look in the first-person view. */
	OptionEntryBoolean fpvInvertMouseY;
	/** @brief Do not let the mouse leave the application window. */
	OptionEntryBoolean grabInput;
	/** @brief Pause the game when focus is lost. */
	OptionEntryBoolean pauseOnFocusLoss;
	/** @brief Enable the Theo quest. */
	OptionEntryBoolean theoQuest;
	/** @brief Enable the cow quest. */
	OptionEntryBoolean cowQuest;
	/** @brief Will players still damage other players in non-PvP mode. */
	OptionEntryBoolean friendlyFire;
	/** @brief Enables the full/uncut singleplayer version of quests. */
	OptionEntryBoolean multiplayerFullQuests;
	/** @brief Enable the bard hero class. */
	OptionEntryBoolean testBard;
	/** @brief Enable the babarian hero class. */
	OptionEntryBoolean testBarbarian;
	/** @brief Show the current level progress. */
	OptionEntryBoolean experienceBar;
	/** @brief Display current/max health values on health globe. */
	OptionEntryBoolean showHealthValues;
	/** @brief Display current/max mana values on mana globe. */
	OptionEntryBoolean showManaValues;
	/** @brief Enable the multiplayer party information display */
	OptionEntryBoolean showMultiplayerPartyInfo;
	/** @brief Show enemy health at the top of the screen. */
	OptionEntryBoolean enemyHealthBar;
	/** @brief Displays item info in a floating box when hovering over an ite. */
	OptionEntryBoolean floatingInfoBox;
	/** @brief Automatically pick up gold when walking over it. */
	OptionEntryBoolean autoGoldPickup;
	/** @brief Auto-pickup elixirs */
	OptionEntryBoolean autoElixirPickup;
	/** @brief Auto-pickup oils */
	OptionEntryBoolean autoOilPickup;
	/** @brief Enable or Disable auto-pickup in town */
	OptionEntryBoolean autoPickupInTown;
	/** @brief Automatically attempt to equip weapon-type items when picking them up. */
	OptionEntryBoolean autoEquipWeapons;
	/** @brief Automatically attempt to equip armor-type items when picking them up. */
	OptionEntryBoolean autoEquipArmor;
	/** @brief Automatically attempt to equip helm-type items when picking them up. */
	OptionEntryBoolean autoEquipHelms;
	/** @brief Automatically attempt to equip shield-type items when picking them up. */
	OptionEntryBoolean autoEquipShields;
	/** @brief Automatically attempt to equip jewelry-type items when picking them up. */
	OptionEntryBoolean autoEquipJewelry;
	/** @brief Only enable 2/3 quests in each game session */
	OptionEntryBoolean randomizeQuests;
	/** @brief Indicates whether or not monster type (Animal, Demon, Undead) is shown along with other monster information. */
	OptionEntryBoolean showMonsterType;
	/** @brief Displays item labels for items on the ground.  */
	OptionEntryBoolean showItemLabels;
	/** @brief Refill belt from inventory, or rather, use potions/scrolls from inventory first when belt item is consumed.  */
	OptionEntryBoolean autoRefillBelt;
	/** @brief Locally disable clicking on shrines which permanently cripple character. */
	OptionEntryBoolean disableCripplingShrines;
	/** @brief Spell hotkeys instantly cast the spell. */
	OptionEntryBoolean quickCast;
	/** @brief Number of Healing potions to pick up automatically */
	OptionEntryInt<int> numHealPotionPickup;
	/** @brief Number of Full Healing potions to pick up automatically */
	OptionEntryInt<int> numFullHealPotionPickup;
	/** @brief Number of Mana potions to pick up automatically */
	OptionEntryInt<int> numManaPotionPickup;
	/** @brief Number of Full Mana potions to pick up automatically */
	OptionEntryInt<int> numFullManaPotionPickup;
	/** @brief Number of Rejuvenating potions to pick up automatically */
	OptionEntryInt<int> numRejuPotionPickup;
	/** @brief Number of Full Rejuvenating potions to pick up automatically */
	OptionEntryInt<int> numFullRejuPotionPickup;
	/** @brief Store user interface. */
	OptionEntryEnum<StoreUi> storeUi;

	/**
	 * @brief If loading takes less than this value, skips displaying the loading screen.
	 *
	 * Advanced option, not displayed in the UI.
	 */
	OptionEntryInt<int> skipLoadingScreenThresholdMs;
};

struct ControllerOptions : OptionCategoryBase {
	ControllerOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief SDL Controller mapping, see SDL_GameControllerDB. */
	char szMapping[1024];
	/** @brief Configure gamepad joysticks deadzone */
	float fDeadzone;
#ifdef __vita__
	/** @brief Enable input via rear touchpad */
	bool bRearTouch;
#endif
};

struct NetworkOptions : OptionCategoryBase {
	NetworkOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Optionally bind to a specific network interface. */
	char szBindAddress[129];
	/** @brief Most recently entered ZeroTier Game ID. */
	char szPreviousZTGame[129];
	/** @brief Most recently entered Hostname in join dialog. */
	char szPreviousHost[129];
	/** @brief What network port to use. */
	OptionEntryInt<uint16_t> port;
	OptionEntryBoolean publicLobbyNoticeAccepted;
};

struct ChatOptions : OptionCategoryBase {
	ChatOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	/** @brief Quick chat messages. */
	std::vector<std::string> szHotKeyMsgs[QuickMessages.size()];
};

struct LanguageOptions : OptionCategoryBase {
	LanguageOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	OptionEntryLanguageCode code;
};

constexpr uint32_t KeymapperMouseButtonMask = 1 << 31;
constexpr uint32_t MouseScrollUpButton = 65536 | KeymapperMouseButtonMask;
constexpr uint32_t MouseScrollDownButton = 65537 | KeymapperMouseButtonMask;
constexpr uint32_t MouseScrollLeftButton = 65538 | KeymapperMouseButtonMask;
constexpr uint32_t MouseScrollRightButton = 65539 | KeymapperMouseButtonMask;

/** The Keymapper maps keys to actions. */
struct KeymapperOptions : OptionCategoryBase {
	/**
	 * Action represents an action that can be triggered using a keyboard
	 * shortcut.
	 */
	class Action final : public OptionEntryBase {
	public:
		// OptionEntryBase::key may be referencing Action::dynamicKey.
		// The implicit copy constructor would copy that reference instead of referencing the copy.
		Action(const Action &) = delete;

		Action(std::string_view key, const char *name, const char *description, uint32_t defaultKey, std::function<void()> actionPressed, std::function<void()> actionReleased, std::function<bool()> enable, unsigned index);

		[[nodiscard]] std::string_view GetName() const override;
		[[nodiscard]] OptionEntryType GetType() const override
		{
			return OptionEntryType::Key;
		}

		void LoadFromIni(std::string_view category) override;
		void SaveToIni(std::string_view category) const override;

		[[nodiscard]] std::string_view GetValueDescription() const override;

		bool SetValue(int value);

		[[nodiscard]] bool isEnabled() const { return !enable || enable(); }

		std::function<void()> actionPressed;
		std::function<void()> actionReleased;

	private:
		uint32_t defaultKey;
		std::function<bool()> enable;
		uint32_t boundKey = SDLK_UNKNOWN;
		unsigned dynamicIndex;
		std::string dynamicKey;
		mutable std::string dynamicName;

		friend struct KeymapperOptions;
	};

	KeymapperOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	void AddAction(
	    std::string_view key, const char *name, const char *description, uint32_t defaultKey,
	    std::function<void()> actionPressed,
	    std::function<void()> actionReleased = nullptr,
	    std::function<bool()> enable = nullptr,
	    unsigned index = 0);
	void CommitActions();

	[[nodiscard]] const Action *findAction(uint32_t key) const;

	std::string_view KeyNameForAction(std::string_view actionName) const;
	uint32_t KeyForAction(std::string_view actionName) const;

private:
	std::forward_list<Action> actions;
	ankerl::unordered_dense::segmented_map<uint32_t, std::reference_wrapper<Action>> keyIDToAction;
	ankerl::unordered_dense::segmented_map<uint32_t, std::string> keyIDToKeyName;
	ankerl::unordered_dense::segmented_map<std::string, uint32_t, StringViewHash, StringViewEquals> keyNameToKeyID;
};

/** The Padmapper maps gamepad buttons to actions. */
struct PadmapperOptions : OptionCategoryBase {
	/**
	 * Action represents an action that can be triggered using a gamepad
	 * button combo.
	 */
	class Action final : public OptionEntryBase {
	public:
		Action(std::string_view key, const char *name, const char *description, ControllerButtonCombo defaultInput, std::function<void()> actionPressed, std::function<void()> actionReleased, std::function<bool()> enable, unsigned index);

		// OptionEntryBase::key may be referencing Action::dynamicKey.
		// The implicit copy constructor would copy that reference instead of referencing the copy.
		Action(const Action &) = delete;

		[[nodiscard]] std::string_view GetName() const override;
		[[nodiscard]] OptionEntryType GetType() const override
		{
			return OptionEntryType::PadButton;
		}

		void LoadFromIni(std::string_view category) override;
		void SaveToIni(std::string_view category) const override;

		[[nodiscard]] std::string_view GetValueDescription() const override;
		[[nodiscard]] std::string_view GetValueDescription(bool useShortName) const;

		bool SetValue(ControllerButtonCombo value);

		[[nodiscard]] bool isEnabled() const { return !enable || enable(); }

		std::function<void()> actionPressed;
		std::function<void()> actionReleased;
		ControllerButtonCombo boundInput;

	private:
		ControllerButtonCombo defaultInput;
		std::function<bool()> enable;
		mutable GamepadLayout boundInputDescriptionType = GamepadLayout::Generic;
		mutable std::string boundInputDescription;
		mutable std::string boundInputShortDescription;
		unsigned dynamicIndex;
		std::string dynamicKey;
		mutable std::string dynamicName;

		void UpdateValueDescription() const;
		std::string_view Shorten(std::string_view buttonName) const;

		friend struct PadmapperOptions;
	};

	PadmapperOptions();
	std::vector<OptionEntryBase *> GetEntries() override;

	void AddAction(
	    std::string_view key, const char *name, const char *description, ControllerButtonCombo defaultInput,
	    std::function<void()> actionPressed,
	    std::function<void()> actionReleased = nullptr,
	    std::function<bool()> enable = nullptr,
	    unsigned index = 0);
	void CommitActions();
	std::string_view InputNameForAction(std::string_view actionName, bool useShortName = false) const;
	ControllerButtonCombo ButtonComboForAction(std::string_view actionName) const;

	[[nodiscard]] const Action *findAction(ControllerButton button, tl::function_ref<bool(ControllerButton)> isModifierPressed) const;

	std::forward_list<Action> actions;

private:
	std::array<std::string, enum_size<ControllerButton>::value> buttonToButtonName;
	ankerl::unordered_dense::segmented_map<std::string, ControllerButton, StringViewHash, StringViewEquals> buttonNameToButton;
	bool committed = false;
};

struct ModOptions : OptionCategoryBase {
	ModOptions();
	std::vector<std::string_view> GetActiveModList();
	std::vector<std::string_view> GetModList();
	std::vector<OptionEntryBase *> GetEntries() override;
	void AddModEntry(const std::string &modName);
	void RemoveModEntry(const std::string &modName);
	void SetHellfireEnabled(bool enableHellfire);

private:
	struct ModEntry {
		// OptionEntryBase::key references ModEntry::name.
		// The implicit copy constructor would copy that reference instead of referencing the copy.
		ModEntry(const ModEntry &) = delete;

		ModEntry(std::string_view name);
		// `name` is the mod id (MPQ filename stem / INI key). `displayName` and `description`
		// come from the mod's `manifest.ini` (falling back to `name` and empty), and are what
		// the settings UI shows via `enabled`.
		std::string name;
		std::string displayName;
		std::string description;
		OptionEntryBoolean enabled;

	private:
		ModEntry(std::string_view name, const ModManifest &manifest);
	};

	std::forward_list<ModEntry> &GetModEntries();
	std::optional<std::forward_list<ModEntry>> modEntries;
};

struct Options {
	GameModeOptions GameMode;
	StartUpOptions StartUp;
	DiabloOptions Diablo;
	HellfireOptions Hellfire;
	AudioOptions Audio;
	GameplayOptions Gameplay;
	GraphicsOptions Graphics;
	ControllerOptions Controller;
	NetworkOptions Network;
	ChatOptions Chat;
	LanguageOptions Language;
	KeymapperOptions Keymapper;
	PadmapperOptions Padmapper;
	ModOptions Mods;

	[[nodiscard]] std::vector<OptionCategoryBase *> GetCategories()
	{
		return {
			&Language,
			&Mods,
			&GameMode,
			&StartUp,
			&Graphics,
			&Audio,
			&Diablo,
			&Hellfire,
			&Gameplay,
			&Controller,
			&Network,
			&Chat,
			&Keymapper,
			&Padmapper,
		};
	}
};

/**
 * @brief Get the Options singleton object
 */
[[nodiscard]] Options &GetOptions();

bool HardwareCursorSupported();

/**
 * @brief Save game configurations to ini file
 */
void SaveOptions();

/**
 * @brief Queue a `Section.Key=Value` ini write applied when the ini is loaded (`--set`).
 * @return false if the assignment is malformed
 */
bool AddIniOverride(std::string_view assignment);

/**
 * @brief Load game configurations from ini file
 */
void LoadOptions();

} // namespace devilution
