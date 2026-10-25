/**
 * @file host/host_state.cpp
 *
 * Contract v5: which modal UI is open, the 2D view geometry with the entities' screen rectangles, the audio settings,
 * and checkpoints of the game state (the game's own save format) for time travel.
 */
#include "host/host_internal.hpp"
#include "host/host_hooks.hpp"
#include "host/host_menu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <unordered_map>

#include "automap.h"
#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#include "diablo_msg.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/render/hd_view_transform.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/ui_scale_core.hpp"
#include "engine/sound.h"
#include "engine/sound_defs.hpp"
#include "effects.h"
#include "game_mode.hpp"
#include "gamemenu.h"
#include "gmenu.h"
#include "help.h"
#include "inv.h"
#include "items.h"
#include "loadsave.h"
#include "minitext.h"
#include "options.h"
#include "panels/quest_log.hpp"
#include "pfile.h"
#include "player.h"
#include "portal.h"
#include "qol/chatlog.h"
#include "qol/floatingnumbers.h"
#include "qol/stash.h"
#include "qol/visual_store.h"
#include "quests.h"
#include "stores.h"
#include "tables/playerdat.hpp"
#include "towners.h"
#include "utils/display.h"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution::host {

void HostSetHudVisible(bool visible)
{
	if (HideHud == !visible)
		return;
	HideHud = !visible;
	if (InGame())
		RedrawEverything();
}

HostHudState HostHudStateSnapshot()
{
	HostHudState state {};
	if (!InGame())
		return state;
	const Player &player = *MyPlayer;
	state.hp = player._pHitPoints;
	state.maxHP = player._pMaxHP;
	state.mana = player._pMana;
	state.maxMana = player._pMaxMana;
	state.playerLevel = player.getCharacterLevel();
	state.currentXP = player._pExperience;
	state.nextXP = player.getNextExperienceThreshold();
	state.levelStartXP = state.playerLevel > 1 ? GetNextExperienceThresholdForLevel(state.playerLevel - 1) : 0;
	state.stamina = std::clamp<uint32_t>(player.stamina, 0, 100);
	state.gold = static_cast<uint32_t>(std::max(player._pGold, 0));
	state.selectedSpell = static_cast<int32_t>(player._pRSpell);
	state.selectedSpellType = static_cast<uint32_t>(player._pRSplType);
	state.flags = kHudInGame | (MyPlayerIsDead ? kHudDead : 0U) | (player.running ? kHudRunning : 0U);
	static_assert(MaxBeltItems == 8);
	for (size_t index = 0; index < 8; ++index) {
		const Item &item = player.SpdList[index];
		if (item.isEmpty())
			continue;
		state.belt[index] = { static_cast<int32_t>(item._itype), static_cast<int32_t>(item._iMiscId),
			static_cast<int32_t>(item._iCurs), static_cast<int32_t>(item._iCurs) + CURSOR_FIRSTITEM, 1 };
	}
	return state;
}

uint32_t HostHoverInfo(uint8_t *text, uint32_t capacity)
{
	if (text == nullptr || capacity == 0)
		return 0;
	text[0] = 0;
	if (!InGame())
		return 0;
	if (HideHud)
		UpdateInfoBox();
	const std::string_view description = InfoString.str();
	const size_t length = std::min(description.size(), static_cast<size_t>(capacity - 1));
	std::memcpy(text, description.data(), length);
	text[length] = 0;
	return static_cast<uint32_t>(InfoColor);
}

bool HostToggleRun()
{
	if (!InGame() || gbIsMultiplayer || MyPlayerIsDead || !*GetOptions().Gameplay.enableRun)
		return false;
	MyPlayer->running = !MyPlayer->running;
	RedrawEverything();
	return true;
}

namespace {

HostUiState Ui {};
HostView2DFrame View {};
std::vector<HostEntityScreenRect> Rects;
HostAudioState Audio {};
std::vector<uint8_t> SavedState;
bool SavedStateValid = false;
uint32_t PendingMenuAction = 0;
bool MenuActionInProgress = false;
std::array<std::optional<int32_t>, 2> PendingMenuAudio;
std::optional<bool> PendingRunEnabled;
std::optional<bool> PendingFreePortalEnabled;
std::optional<int32_t> PendingUiScale;
std::optional<std::pair<bool, int32_t>> PendingOutlines;
std::optional<bool> PendingHostFPS;

DxpMenuSnapshot MenuSnapshot()
{
	DxpMenuSnapshot result { 1, 0, 0, 0 };
	if (!IsActive())
		return result;
	if (MenuActionInProgress) result.flags |= DxpMenuBusy;
	const auto percent = [](int volume) {
		return std::clamp((volume - VOLUME_MIN) * 100 / (VOLUME_MAX - VOLUME_MIN), 0, 100);
	};
	result.musicPercent = percent(*GetOptions().Audio.musicVolume);
	result.soundPercent = percent(*GetOptions().Audio.soundVolume);
	if (gbMusicOn) result.flags |= DxpMenuMusicOn;
	if (gbSoundOn) result.flags |= DxpMenuSoundOn;
	if (*GetOptions().Gameplay.enableRun) result.flags |= DxpMenuRunEnabled;
	if (!InGame())
		return result;
	result.flags |= DxpMenuInGame;
	if (IsFreeTownPortalEnabled()) result.flags |= DxpMenuFreePortal;
	if (gmenu_is_active()) result.flags |= DxpMenuOpen;
	if (gbIsMultiplayer) result.flags |= DxpMenuMultiplayer;
	if (!MenuActionInProgress && !gbIsMultiplayer && !MyPlayerIsDead && MyPlayer->_pmode != PM_DEATH && pcurs == CURSOR_HAND)
		result.flags |= DxpMenuCanSave;
	if (!MenuActionInProgress && !gbIsMultiplayer && gbValidSaveFile) result.flags |= DxpMenuCanLoad;
	if (MyPlayer->running) result.flags |= DxpMenuRunning;
	if (!gbIsMultiplayer && !MyPlayerIsDead && *GetOptions().Gameplay.enableRun)
		result.flags |= DxpMenuCanRun;
	return result;
}

bool AcceptMenuAction(uint32_t action)
{
	const uint32_t flags = MenuSnapshot().flags;
	if ((flags & DxpMenuOpen) == 0 || MenuActionInProgress || PendingMenuAction != 0 || action < 1 || action > 5)
		return false;
	if (action == 2 && (flags & DxpMenuCanSave) == 0)
		return false;
	if (action == 3 && (flags & DxpMenuCanLoad) == 0)
		return false;
	PendingMenuAction = action;
	return true;
}

uint8_t TownerOfStore(TalkID store)
{
	switch (store) {
	case TalkID::Smith:
	case TalkID::SmithBuy:
	case TalkID::SmithSell:
	case TalkID::SmithRepair:
	case TalkID::SmithPremiumBuy:
		return TOWN_SMITH;
	case TalkID::Witch:
	case TalkID::WitchBuy:
	case TalkID::WitchSell:
	case TalkID::WitchRecharge:
		return TOWN_WITCH;
	case TalkID::Boy:
	case TalkID::BoyBuy:
		return TOWN_PEGBOY;
	case TalkID::Healer:
	case TalkID::HealerBuy:
		return TOWN_HEALER;
	case TalkID::Storyteller:
	case TalkID::StorytellerIdentify:
	case TalkID::StorytellerIdentifyShow:
		return TOWN_STORY;
	case TalkID::Tavern:
		return TOWN_TAVERN;
	case TalkID::Drunk:
		return TOWN_DRUNK;
	case TalkID::Barmaid:
		return TOWN_BMAID;
	default:
		return 0xFF;
	}
}

uint32_t Mix(uint32_t hash, uint32_t value)
{
	return (hash ^ value) * 16777619U;
}

} // namespace

void ResetHostMenu()
{
	HideGameMenu = false;
	PendingMenuAction = 0;
	MenuActionInProgress = false;
	PendingMenuAudio = {};
	PendingRunEnabled.reset();
	PendingFreePortalEnabled.reset();
	PendingUiScale.reset();
	PendingOutlines.reset();
	PendingHostFPS.reset();
}

void ProcessMenuCommands()
{
	if (!OnGameThread() || MenuActionInProgress)
		return;
	// Clear before calling native handlers: save/load can present a frame and
	// recursively pump events while this call is still on the game-thread stack.
	const uint32_t action = std::exchange(PendingMenuAction, 0);
	const auto audio = std::exchange(PendingMenuAudio, {});
	const auto runEnabled = std::exchange(PendingRunEnabled, std::nullopt);
	const auto freePortalEnabled = std::exchange(PendingFreePortalEnabled, std::nullopt);
	const auto uiScale = std::exchange(PendingUiScale, std::nullopt);
	const auto outlines = std::exchange(PendingOutlines, std::nullopt);
	const auto hostFPS = std::exchange(PendingHostFPS, std::nullopt);
	if (!InGame() || !gmenu_is_active())
		return;
	for (size_t channel = 0; channel < audio.size(); ++channel) {
		if (!audio[channel].has_value())
			continue;
		const int volume = VOLUME_MIN + *audio[channel] * (VOLUME_MAX - VOLUME_MIN) / 100;
		if (channel == 0) {
			sound_get_or_set_music_volume(volume);
			if (volume == VOLUME_MIN && gbMusicOn) {
				gbMusicOn = false;
				music_stop();
			} else if (volume != VOLUME_MIN && !gbMusicOn) {
				gbMusicOn = true;
				music_start(GetLevelMusic(leveltype));
			}
		} else {
			sound_get_or_set_sound_volume(volume);
			gbSoundOn = volume != VOLUME_MIN;
			if (!gbSoundOn)
				sound_stop();
		}
	}
	if (runEnabled.has_value() && !gbIsMultiplayer) {
		GetOptions().Gameplay.enableRun.SetValue(*runEnabled);
		if (!*runEnabled)
			MyPlayer->running = false;
	}
	if (freePortalEnabled.has_value())
		SetFreeTownPortalEnabled(*freePortalEnabled);
	if (uiScale.has_value())
		GetOptions().Graphics.uiScale.SetValue(*uiScale); // relayouts and redraws on a change (ui_scale.cpp), saved with the options when the menu closes
	if (outlines.has_value()) {
		GetOptions().Graphics.hoverOutlines.SetValue(outlines->first);
		GetOptions().Graphics.outlineWidth.SetValue(outlines->second);
	}
	if (hostFPS.has_value())
		GetOptions().Graphics.hostFPS.SetValue(*hostFPS);
	const uint32_t flags = MenuSnapshot().flags;
	MenuActionInProgress = action != 0;
	switch (action) {
	case 1: gamemenu_off(); break;
	case 2:
		if ((flags & DxpMenuCanSave) != 0) gamemenu_save_game(true);
		break;
	case 3:
		if ((flags & DxpMenuCanLoad) != 0) gamemenu_load_game(true);
		break;
	case 4: gamemenu_exit_game(true); break;
	case 5: gamemenu_quit_game(true); break;
	default: break;
	}
	MenuActionInProgress = false;
	if (action != 0 || runEnabled.has_value() || freePortalEnabled.has_value())
		RedrawEverything();
}

const HostUiState &BuildUiState()
{
	uint32_t flags = 0;
	uint8_t store = 0;
	uint8_t towner = 0xFF;
	if (!gbRunGame) {
		flags |= kUiMainMenu;
	} else if (MyPlayer == nullptr || MyPlayer->_pLvlChanging || !InGame()) {
		flags |= kUiLoading;
	} else {
		if (IsPlayerInStore()) {
			flags |= kUiStore;
			store = static_cast<uint8_t>(ActiveStore);
			towner = TownerOfStore(ActiveStore);
			if (towner == 0xFF) // Gossip, NoMoney, NoRoom, Confirm: the store that led there
				towner = TownerOfStore(OldActiveStore);
		}
		if (qtextflag) flags |= kUiQuestText;
		if (gmenu_is_active()) flags |= kUiGameMenu;
		if (DropGoldFlag) flags |= kUiGoldDrop;
		if (IsWithdrawGoldOpen) flags |= kUiGoldWithdraw;
		if (HelpFlag) flags |= kUiHelp;
		if (CharFlag) flags |= kUiCharacter;
		if (invflag) flags |= kUiInventory;
		if (SpellbookFlag) flags |= kUiSpellbook;
		if (QuestLogIsOpen) flags |= kUiQuestLog;
		if (IsStashOpen) flags |= kUiStash;
		if (IsVisualStoreOpen) flags |= kUiVisualStore;
		if (AutomapActive) flags |= kUiAutomap;
		if (ChatFlag) flags |= kUiChat;
		if (SpellSelectFlag) flags |= kUiSpellSelect;
		if (ChatLogFlag) flags |= kUiChatLog;
		if (PauseMode != 0) flags |= kUiPaused;
		if (MyPlayerIsDead) flags |= kUiDead;
		if (IsDiabloMsgAvailable()) flags |= kUiMessage;
	}
	if (flags != Ui.flags || store != Ui.activeStore || towner != Ui.storeTowner)
		++Ui.revision;
	Ui.flags = flags;
	Ui.activeStore = store;
	Ui.storeTowner = towner;
	return Ui;
}

const HostView2DFrame &BuildView2D()
{
	Rects.clear();
	View = {};
	if (!InGame()) {
		View.rects = nullptr;
		return View;
	}
	// The game's own mapping (cursor picking uses the same): a tile's bounding box in world-pass pixels, then the zoom.
	const HdWorldTransform transform = GetViewTransform();
	const Point view = ViewPosition;
	const auto centre = [&](Point tile) {
		const Point bottomLeft = GetScreenPosition(tile);
		const float x = static_cast<float>(bottomLeft.x + TILE_WIDTH / 2);
		const float y = static_cast<float>(bottomLeft.y - TILE_HEIGHT / 2);
		return std::pair { transform.ToScreenX(x), transform.ToScreenY(y) };
	};
	const auto [ox, oy] = centre(view);
	const auto [xx, xy] = centre(view + Displacement { 1, 0 });
	const auto [yx, yy] = centre(view + Displacement { 0, 1 });
	HostView2D &out = View.view;
	out.valid = 1;
	out.zoom = transform.Scale();
	const float ax = xx - ox, ay = xy - oy; // per +1 world x
	const float bx = yx - ox, by = yy - oy; // per +1 world z
	const float vx = static_cast<float>(view.x) + 0.5F;
	const float vz = static_cast<float>(view.y) + 0.5F;
	out.worldToScreen[0] = ax;
	out.worldToScreen[1] = bx;
	out.worldToScreen[2] = ox - ax * vx - bx * vz;
	out.worldToScreen[3] = ay;
	out.worldToScreen[4] = by;
	out.worldToScreen[5] = oy - ay * vx - by * vz;
	out.viewportX = 0;
	out.viewportY = 0;
	out.viewportW = GetScreenWidth();
	out.viewportH = GetViewportHeight();

	std::unordered_map<uint32_t, const HostEntity *> byId;
	for (const HostEntity &entity : SnapshotEntities())
		byId.emplace(entity.id, &entity);
	for (const EntitySpriteSource &source : SnapshotSprites()) {
		const auto it = byId.find(source.entityId);
		if (it == byId.end())
			continue;
		const HostEntity &entity = *it->second;
		// Not drawn (a monster in the dark, burrowed or invisible): the game would not pick it either, so no rectangle.
		if ((entity.flags & kEntityInvisible) != 0)
			continue;
		const float feetX = out.worldToScreen[0] * entity.x + out.worldToScreen[1] * entity.z + out.worldToScreen[2];
		const float feetY = out.worldToScreen[3] * entity.x + out.worldToScreen[4] * entity.z + out.worldToScreen[5];
		const float w = static_cast<float>(source.sprite.width()) * out.zoom;
		const float h = static_cast<float>(source.sprite.height()) * out.zoom;
		const float bottom = feetY + static_cast<float>(TILE_HEIGHT / 2) * out.zoom;
		HostEntityScreenRect rect {};
		rect.entityId = entity.id;
		rect.x = static_cast<int32_t>(std::lround(feetX - w / 2.0F));
		rect.y = static_cast<int32_t>(std::lround(bottom - h));
		rect.w = static_cast<int32_t>(std::lround(w));
		rect.h = static_cast<int32_t>(std::lround(h));
		rect.feetX = feetX;
		rect.feetY = feetY;
		Rects.push_back(rect);
	}
	View.rects = Rects.empty() ? nullptr : Rects.data();
	View.rectCount = static_cast<uint32_t>(Rects.size());
	return View;
}

const HostAudioState &BuildAudioState()
{
	const AudioOptions &options = GetOptions().Audio;
	HostAudioState next {};
	next.musicGain = VolumeGain(*options.musicVolume);
	next.soundGain = VolumeGain(*options.soundVolume);
	next.audioCuesGain = VolumeGain(*options.audioCuesVolume);
	next.musicOn = gbMusicOn ? 1 : 0;
	next.soundOn = gbSoundOn ? 1 : 0;
	uint32_t hash = 2166136261U;
	for (const int value : { *options.musicVolume, *options.soundVolume, *options.audioCuesVolume, gbMusicOn ? 1 : 0, gbSoundOn ? 1 : 0 })
		hash = Mix(hash, static_cast<uint32_t>(value));
	static uint32_t lastHash = 0;
	static uint32_t revision = 0;
	if (hash != lastHash) {
		lastHash = hash;
		++revision;
	}
	next.revision = revision;
	Audio = next;
	return Audio;
}

// ---- state checkpoints ----

namespace {

bool CanCheckpoint()
{
	return IsActive() && InGame() && !gbIsMultiplayer && MyPlayer != nullptr && MyPlayer->plractive;
}

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	out.clear();
	uint8_t buffer[65536];
	for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
		out.insert(out.end(), buffer, buffer + read);
	std::fclose(file);
	return !out.empty();
}

bool WriteWholeFile(const std::string &path, const uint8_t *data, size_t size)
{
	FILE *file = OpenFile(path.c_str(), "wb");
	if (file == nullptr)
		return false;
	const bool ok = std::fwrite(data, 1, size, file) == size;
	return std::fclose(file) == 0 && ok;
}

/**
 * The working folder of the checkpoints: a folder of this process under the system's temporary folder, never next to
 * the player's saves. The archives in it live only for the duration of one HostSaveState / HostLoadState call.
 */
const std::string &StateDirectory()
{
	static const std::string Directory = [] {
		std::error_code error;
		std::filesystem::path path = std::filesystem::temp_directory_path(error);
		if (error)
			path = std::filesystem::path(paths::PrefPath()) / "host-cache";
		path /= "damnationx-host-" + std::to_string(static_cast<unsigned long>(getpid()));
		std::filesystem::create_directories(path, error);
		return path.string() + DIRECTORY_SEPARATOR_STR;
	}();
	return Directory;
}

/** While alive, every read and write of the hero's save archive goes to `path` instead (pfile.cpp). */
class SaveRedirect {
public:
	explicit SaveRedirect(const std::string &path)
	{
		pfile_set_save_path_override(&path);
	}
	~SaveRedirect()
	{
		pfile_set_save_path_override(nullptr);
	}
	SaveRedirect(const SaveRedirect &) = delete;
	SaveRedirect &operator=(const SaveRedirect &) = delete;
};

} // namespace

void ForgetSavedState()
{
	SavedStateValid = false;
}

void RemoveStateDirectory()
{
	std::error_code error;
	std::filesystem::remove_all(StateDirectory(), error);
}

uint32_t SaveState(uint8_t *buffer, uint32_t capacity)
{
	if (!CanCheckpoint())
		return 0;
	if (!SavedStateValid) {
		// A copy of the hero's archive (read only) plus the game as a save would write it, all in the temporary folder.
		const std::string path = StateDirectory() + "checkpoint.sv";
		if (!pfile_write_game_snapshot(path) || !ReadWholeFile(path, SavedState)) {
			LogError("HostSaveState: could not write the game state");
			RemoveFile(path.c_str());
			return 0;
		}
		RemoveFile(path.c_str());
		SavedStateValid = true;
	}
	if (SavedState.size() > UINT32_MAX)
		return 0;
	if (buffer != nullptr && capacity >= SavedState.size())
		std::memcpy(buffer, SavedState.data(), SavedState.size());
	return static_cast<uint32_t>(SavedState.size());
}

bool LoadState(const uint8_t *data, uint32_t size)
{
	// Every refusal is logged with its reason: a host scrubbing a timeline must see why a restore did not happen.
	const auto refuse = [](std::string_view reason) {
		LogError("HostLoadState: not restored: {}", reason);
		return false;
	};
	if (!IsActive())
		return refuse("the game is not running (HostInit)");
	if (gbIsMultiplayer)
		return refuse("multiplayer game");
	if (!gbRunGame)
		return refuse("not in a game (menus)");
	if (MyPlayer == nullptr || MyPlayer->_pLvlChanging)
		return refuse("a level is loading");
	if (data == nullptr || size < 32)
		return refuse("no state data");
	if (std::memcmp(data, "MPQ\x1A", 4) != 0)
		return refuse("not a state taken by HostSaveState");
	// The game loads the state like the game menu's Load Game, but from a temporary archive: while it loads, the hero's
	// save archive is that file (reads, and the temp-file cleanup LoadGame does), so the player's save is not touched.
	const std::string path = StateDirectory() + "restore.sv";
	if (!WriteWholeFile(path, data, size))
		return refuse("could not write the temporary archive " + path);
	const auto started = std::chrono::steady_clock::now();
	if (IsPlayerInStore())
		ActiveStore = TalkID::None;
	qtextflag = false;
	gamemenu_off();
	ClearFloatingNumbers();
	ClrDiabloMsg();
	std::expected<void, std::string> result;
	{
		const SaveRedirect redirect(path);
		result = LoadGame(false);
	}
	RemoveFile(path.c_str());
	if (!result.has_value()) {
		// LoadGame freed the level first: the game is not in a playable state now. Say so as loudly as possible.
		return refuse("the game could not load the state (" + result.error() + "); restore another state or quit");
	}
	LoadPWaterPalette();
	NewCursor(CURSOR_HAND);
	CornerStone.activated = false;
	MyPlayerIsDead = false;
	ResetHdRecords();
	RedrawEverything();
	SavedStateValid = false;
	// Draw the restored frame now (into the UI layer, on this thread): the host shows it without waiting for a tick.
	DrawAndBlit();
	LogInfo("HostLoadState: restored level {} at {},{} ({} bytes, {} ms)", currlevel, MyPlayer->position.tile.x, MyPlayer->position.tile.y, size,
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
	return true;
}

} // namespace devilution::host

extern "C" bool dxp_menu_snapshot(DxpMenuSnapshot *output, uint32_t size)
{
	if (output == nullptr || size != sizeof(DxpMenuSnapshot))
		return false;
	*output = devilution::host::MenuSnapshot();
	return devilution::host::IsActive();
}

extern "C" void dxp_menu_enable(bool enabled)
{
	using namespace devilution::host;
	if (HideGameMenu == enabled)
		return;
	HideGameMenu = enabled;
	if (InGame()) devilution::RedrawEverything();
}

extern "C" bool dxp_menu_action(uint32_t action)
{
	return devilution::host::AcceptMenuAction(action);
}

extern "C" bool dxp_menu_audio(uint32_t channel, int32_t percent)
{
	using namespace devilution::host;
	if (channel >= PendingMenuAudio.size() || (MenuSnapshot().flags & DxpMenuOpen) == 0)
		return false;
	PendingMenuAudio[channel] = std::clamp(percent, 0, 100);
	return true;
}

extern "C" bool dxp_menu_gameplay(uint32_t setting, bool enabled)
{
	using namespace devilution::host;
	const uint32_t flags = MenuSnapshot().flags;
	if ((setting != 0 && setting != 2) || (flags & DxpMenuOpen) == 0 || (flags & DxpMenuMultiplayer) != 0)
		return false;
	if (setting == 2)
		PendingFreePortalEnabled = enabled;
	else
		PendingRunEnabled = enabled;
	return true;
}

extern "C" bool dxp_menu_ui_scale(int32_t *chosen, int32_t *effective)
{
	using namespace devilution;
	if (chosen == nullptr || effective == nullptr || !host::IsActive())
		return false;
	*chosen = *GetOptions().Graphics.uiScale;
	*effective = EffectiveUiScaleQuarters(*chosen, gnScreenWidth, gnScreenHeight);
	return true;
}

extern "C" bool dxp_menu_set_ui_scale(int32_t quarters)
{
	using namespace devilution;
	if ((host::MenuSnapshot().flags & DxpMenuOpen) == 0)
		return false;
	host::PendingUiScale = std::clamp<int32_t>(quarters, OptionEntryQuarterScale::MinQuarters, OptionEntryQuarterScale::MaxQuarters);
	return true;
}

extern "C" bool dxp_menu_outlines(bool *enabled, int32_t *widthQuarters)
{
	using namespace devilution;
	if (enabled == nullptr || widthQuarters == nullptr || !host::IsActive())
		return false;
	*enabled = *GetOptions().Graphics.hoverOutlines;
	*widthQuarters = std::clamp<int32_t>(*GetOptions().Graphics.outlineWidth, 2, 12);
	return true;
}

extern "C" bool dxp_menu_set_outlines(bool enabled, int32_t widthQuarters)
{
	using namespace devilution;
	if ((host::MenuSnapshot().flags & DxpMenuOpen) == 0)
		return false;
	host::PendingOutlines = std::pair { enabled, std::clamp<int32_t>(widthQuarters, 2, 12) };
	return true;
}

extern "C" bool dxp_menu_fps(bool *enabled)
{
	using namespace devilution;
	if (enabled == nullptr || !host::IsActive())
		return false;
	*enabled = *GetOptions().Graphics.hostFPS;
	return true;
}

extern "C" bool dxp_menu_set_fps(bool enabled)
{
	using namespace devilution;
	if ((host::MenuSnapshot().flags & DxpMenuOpen) == 0)
		return false;
	host::PendingHostFPS = enabled;
	return true;
}
