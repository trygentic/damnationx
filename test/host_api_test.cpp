/**
 * Embedding API (Source/host/host_api.hpp): HostInit without a window, a single player game started with
 * --host-hero, a level change into the Cathedral and 100 ticks of scripted input, checked through HostSnapshot.
 *
 * Needs the game data: DAMNATIONX_TEST_DATA_DIR, or the player's own pref folder (diabdat.mpq or spawn.mpq).
 * Skips cleanly without it. Saves and options go to a temporary folder, never to the player's.
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "host/host_api.hpp"
#include "host/host_internal.hpp"
#include "host/host_panels.hpp"
#include "host/host_menu.hpp"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "diablo.h"
#include "interfac.h"
#include "inv.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "utils/display.h"
#include "lighting.h"
#include "loadsave.h"
#include "minitext.h"
#include "missiles.h"
#include "pfile.h"
#include "engine/sound.h"
#include "engine/assets.hpp"
#include "engine/sound_defs.hpp"
#include "engine/render/ui_scale.hpp"
#include "engine/render/ui_scale_core.hpp"
#include "monster.h"
#include "options.h"
#include "objects.h"
#include "stores.h"
#include "tables/playerdat.hpp"
#include "towners.h"
#include "player.h"
#include "portal.h"
#include "render3d/world_level.hpp"
#include "utils/sdl_compat.h"

using namespace devilution;
using namespace devilution::host;

namespace {

namespace fs = std::filesystem;

std::string FindDataDir()
{
	std::vector<std::string> candidates;
	if (const char *dir = std::getenv("DAMNATIONX_TEST_DATA_DIR"); dir != nullptr && dir[0] != '\0')
		candidates.emplace_back(dir);
#ifdef USE_SDL3
	if (const char *pref = SDL_GetPrefPath("diasurgical", "devilution"); pref != nullptr) {
		candidates.emplace_back(pref);
		SDL_free(const_cast<char *>(pref));
	}
#else
	if (char *pref = SDL_GetPrefPath("diasurgical", "devilution"); pref != nullptr) {
		candidates.emplace_back(pref);
		SDL_free(pref);
	}
#endif
	for (const std::string &dir : candidates) {
		for (const char *name : { "diabdat.mpq", "DIABDAT.MPQ", "spawn.mpq" }) {
			std::error_code error;
			if (fs::exists(fs::path(dir) / name, error))
				return dir;
		}
	}
	return {};
}

HostInputFrame EmptyInput(const HostFrame *frame)
{
	HostInputFrame input {};
	input.pointerX = 320;
	input.pointerY = 160;
	input.hoverTileX = -1;
	input.hoverTileY = -1;
	(void)frame;
	return input;
}

const HostEntity *FindLocalPlayer(const HostFrame &frame, int &count)
{
	const HostEntity *found = nullptr;
	count = 0;
	for (uint32_t i = 0; i < frame.entityCount; ++i) {
		const HostEntity &entity = frame.entities[i];
		if ((entity.flags & kEntityLocalPlayer) != 0) {
			++count;
			found = &entity;
		}
	}
	return found;
}

/** Ticks with empty input until `done` (or `limit` ticks). Returns false when the game ended. */
template <typename Done>
bool TickUntil(Done done, int limit, uint32_t &soundEvents)
{
	for (int i = 0; i < limit; ++i) {
		HostSubmitInput(EmptyInput(nullptr));
		const HostTickResult result = HostTick(50);
		if (result.quitRequested != 0) {
			ADD_FAILURE() << "the game ended after " << i << " ticks";
			return false;
		}
		const HostFrame &frame = HostSnapshot();
		soundEvents += frame.soundCount;
		if (done(result, frame))
			return true;
	}
	return false;
}

/** The UI state while the last TalkTo dialog was open (v5). */
HostUiState DialogUi {};

/** HostInit runs once per process: a second test in the same run skips (ctest runs each test on its own). */
bool HostUsedInThisProcess = false;

/** Clicks the towner of `type` with the host's 3D pick until its dialog or store opens; then closes it with Escape. */
bool TalkTo(_talker_id type, uint32_t &soundEvents)
{
	size_t index = Towners.size();
	for (size_t i = 0; i < Towners.size(); ++i) {
		if (Towners[i]._ttype == type)
			index = i;
	}
	if (index == Towners.size())
		return false;
	const Point position = Towners[index].position;
	const uint32_t id = (static_cast<uint32_t>(HostEntityKind::Towner) << 24) | static_cast<uint32_t>(index + 1);
	bool opened = false;
	for (int frame = 0; frame < 600 && !opened; ++frame) {
		// Like a host's raycast: the target shows up in the very frame of the click (and is gone afterwards), and the
		// game has seen no mouse movement yet (it has not switched to mouse control).
		HostInputFrame input = EmptyInput(nullptr);
		if (frame == 5 || frame == 6) {
			input.hoverTileX = position.x;
			input.hoverTileY = position.y;
			input.hoverEntityId = id;
		}
		if (frame == 5)
			input.buttonsPressed = input.buttonsDown = static_cast<uint32_t>(HostButton::Primary);
		else if (frame == 6)
			input.buttonsReleased = static_cast<uint32_t>(HostButton::Primary);
		HostSubmitInput(input);
		if (HostTick(50).quitRequested != 0)
			return false;
		soundEvents += HostSnapshot().soundCount;
		opened = qtextflag || IsPlayerInStore();
		if (opened)
			DialogUi = HostUiStateSnapshot();
	}
	for (int i = 0; i < 10 && (qtextflag || IsPlayerInStore()); ++i) {
		HostInputFrame input = EmptyInput(nullptr);
		input.keyCount = 2;
		input.keys[0] = { SDLK_ESCAPE, 0, 1, 0 };
		input.keys[1] = { SDLK_ESCAPE, 0, 0, 0 };
		HostSubmitInput(input);
		HostTick(50);
	}
	return opened;
}

uint32_t MakeHeroId()
{
	return (static_cast<uint32_t>(HostEntityKind::Player) << 24) | (static_cast<uint32_t>(MyPlayerId) + 1);
}

/** Exercises only the snapshot projection while the game thread is parked, then restores its inputs. */
void CheckSnapshotLightOwnership()
{
	struct RestoreSources {
		std::array<Light, MAXLIGHTS> lights = std::to_array(Lights);
		std::array<Object, MAXOBJECTS> objects = std::to_array(Objects);
		std::array<int, MAXOBJECTS> activeObjects = std::to_array(ActiveObjects);
		std::array<uint8_t, MAXLIGHTS> activeLights = ActiveLights;
		int lightCount = ActiveLightCount;
		int objectCount = ActiveObjectCount;
		size_t monsterCount = ActiveMonsterCount;
		int playerLight = MyPlayer->lightId;
		std::list<Missile> missiles;

		RestoreSources()
		{
			missiles.splice(missiles.end(), Missiles);
		}
		~RestoreSources()
		{
			std::copy(lights.begin(), lights.end(), Lights);
			std::copy(objects.begin(), objects.end(), Objects);
			std::copy(activeObjects.begin(), activeObjects.end(), ActiveObjects);
			ActiveLights = activeLights;
			ActiveLightCount = lightCount;
			ActiveObjectCount = objectCount;
			ActiveMonsterCount = monsterCount;
			MyPlayer->lightId = playerLight;
			Missiles.clear();
			Missiles.splice(Missiles.end(), missiles);
		}
	} restore;

	ActiveMonsterCount = 0;
	MyPlayer->lightId = 0;
	ActiveLightCount = 8;
	for (int i = 0; i < ActiveLightCount; ++i) {
		ActiveLights[i] = i;
		Lights[i] = {};
		Lights[i].position.tile = Point { 40 + i, 40 };
		Lights[i].radius = 10;
	}
	ActiveObjectCount = 8;
	for (int i = 0; i < ActiveObjectCount; ++i) {
		ActiveObjects[i] = i;
		Objects[i] = {};
		Objects[i]._otype = OBJ_TORCHL;
		Objects[i]._olid = i;
		Objects[i]._oVar1 = 1;
		Objects[i].position = Point { 40 + i, 40 };
	}
	Objects[0]._otype = OBJ_BARREL; // Unrelated non-emitter with the default light id zero.
	Objects[0].position = { 48, 40 };
	Objects[3].position = { 44, 40 }; // Extinguished source's id has been recycled at another tile.
	Objects[4]._oVar1 = 0; // Unlit torch still retaining an id at the same tile.
	Objects[5]._otype = OBJ_BARREL; // Matching position alone does not make an object an emitter.
	Objects[6]._otype = OBJ_FLAMEHOLE;
	Objects[6]._oVar4 = 1;
	Objects[7]._olid = 0; // Even a lit emitter cannot steal a player's live light.
	Objects[7].position = { 40, 40 };
	Missiles.emplace_back(Missile {});
	Missile &missile = Missiles.back();
	missile._mitype = MissileID::Firebolt;
	missile._mlid = 2; // The torch at this tile must not steal a missile's live light either.
	missile.position.tile = Point { 42, 40 };
	missile.position.tileForRendering = missile.position.tile;
	missile._miAnimFrame = 1;
	missile._miAnimLen = 1;
	missile._miDrawFlag = true;
	Missiles.emplace_back(missile);
	Missile &secondMissile = Missiles.back();
	secondMissile._mlid = 7;
	secondMissile.position.tile = Point { 47, 40 };
	secondMissile.position.tileForRendering = secondMissile.position.tile;

	const HostFrame &frame = HostSnapshot();
	EXPECT_EQ(frame.lightCount, 8U);
	std::array<uint32_t, 2> missileIds {};
	int missileCount = 0;
	for (uint32_t i = 0; i < frame.entityCount; ++i) {
		const HostEntity &entity = frame.entities[i];
		if (entity.kind != HostEntityKind::Missile)
			continue;
		++missileCount;
		if (entity.x == 42.5F)
			missileIds[0] = entity.id;
		else if (entity.x == 47.5F)
			missileIds[1] = entity.id;
	}
	EXPECT_EQ(missileCount, 2);
	EXPECT_NE(missileIds[0], 0U);
	EXPECT_NE(missileIds[1], 0U);
	EXPECT_NE(missileIds[0], missileIds[1]) << "simultaneously live missiles need distinct effect and light owners";
	const auto expectLight = [&](int index, uint32_t owner, uint8_t r, uint8_t g, uint8_t b) {
		const HostLight *found = nullptr;
		for (uint32_t i = 0; i < frame.lightCount; ++i) {
			if (frame.lights[i].x == 40.5F + index && frame.lights[i].z == 40.5F)
				found = &frame.lights[i];
		}
		EXPECT_NE(found, nullptr) << "source " << index;
		if (found == nullptr)
			return;
		EXPECT_EQ(found->entityId, owner) << "source " << index;
		EXPECT_EQ(found->flags, owner != 0 ? 1 : 0) << "source " << index;
		EXPECT_EQ(found->r, r) << "source " << index;
		EXPECT_EQ(found->g, g) << "source " << index;
		EXPECT_EQ(found->b, b) << "source " << index;
	};
	const auto objectId = [](uint32_t index) { return (static_cast<uint32_t>(HostEntityKind::Object) << 24) | (index + 1); };
	expectLight(0, MakeHeroId(), 242, 219, 178);
	expectLight(1, objectId(1), 255, 170, 90);
	expectLight(2, missileIds[0], 255, 140, 50);
	expectLight(3, 0, 242, 219, 178);
	expectLight(4, 0, 242, 219, 178);
	expectLight(5, 0, 242, 219, 178);
	expectLight(6, objectId(6), 255, 170, 90);
	expectLight(7, missileIds[1], 255, 140, 50);
	const HostFrame &next = HostSnapshot();
	for (uint32_t i = 0; i < next.entityCount; ++i) {
		const HostEntity &entity = next.entities[i];
		if (entity.kind == HostEntityKind::Missile)
			EXPECT_EQ(entity.id, missileIds[entity.x == 42.5F ? 0 : 1]) << "a live missile keeps its id across snapshots";
	}
	expectLight(2, missileIds[0], 255, 140, 50);
	expectLight(7, missileIds[1], 255, 140, 50);
}

/** Chest sprite frames contain two art variants, but both must export the same closed/open model state. */
void CheckChestSnapshotState()
{
	ASSERT_GT(ActiveObjectCount, 0);
	const int index = ActiveObjects[0];
	struct RestoreObject {
		int index;
		Object original;
		~RestoreObject() { Objects[index] = original; }
	} restore { index, Objects[index] };
	Object &chest = Objects[index];
	chest = {};
	chest.position = restore.original.position;
	chest._oAnimLen = 6;
	const uint32_t id = (static_cast<uint32_t>(HostEntityKind::Object) << 24) | static_cast<uint32_t>(index + 1);
	for (const _object_id type : { OBJ_CHEST1, OBJ_CHEST2, OBJ_CHEST3, OBJ_TCHEST1, OBJ_TCHEST2, OBJ_TCHEST3 }) {
		chest._otype = type;
		for (const uint32_t spriteFrame : { 1U, 4U, 3U, 6U }) {
			const bool open = spriteFrame == 3 || spriteFrame == 6;
			chest._oAnimFrame = spriteFrame;
			chest.selectionRegion = open ? SelectionRegion::None : SelectionRegion::Middle;
			const HostFrame &snapshot = HostSnapshot();
			const HostEntity *found = nullptr;
			for (uint32_t i = 0; i < snapshot.entityCount; ++i) {
				if (snapshot.entities[i].id == id)
					found = &snapshot.entities[i];
			}
			ASSERT_NE(found, nullptr);
			EXPECT_EQ(found->anim, open ? 2 : 0);
			EXPECT_EQ(found->frame, spriteFrame);
			EXPECT_EQ(found->facing, 0);
		}
	}
}

} // namespace

TEST(HostApi, OpenedObjectsAreNotBroken)
{
	for (const _object_id type : { _object_id::OBJ_CHEST1, _object_id::OBJ_L1LDOOR }) {
		Object object {};
		object._otype = type;
		object._oBreak = 0;
		object.selectionRegion = SelectionRegion::None;
		EXPECT_FALSE(object.canInteractWith());
		EXPECT_FALSE(object.IsBroken());
	}
	Object barrel {};
	barrel._otype = _object_id::OBJ_BARREL;
	barrel._oBreak = -1;
	barrel.selectionRegion = SelectionRegion::None;
	EXPECT_TRUE(barrel.IsBroken());
	EXPECT_FALSE(barrel.canInteractWith());
}

TEST(HostApi, ExplicitHdOffClearsAnInstalledPackAndItsAssetMapping)
{
	// Exercise the real pack loader with source-free marker files, without a
	// game thread or an owned archive. Off must override a valid selected pack.
	const fs::path directory = fs::temp_directory_path()
	    / ("damnationx-hd-off-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Restore {
		HostOptions options = devilution::host::Options;
		std::string hdDirectory = HdDirectory();
		std::string assetDirectory = GetHdAssetDirectory();
		fs::path temporary;
		~Restore()
		{
			devilution::host::Options = options;
			devilution::host::Options.hdDir = hdDirectory;
			devilution::host::Options.hdDisabled = hdDirectory.empty();
			InitHdPack();
			devilution::host::Options = options;
			SetHdAssetDirectory(assetDirectory);
			std::error_code error;
			fs::remove_all(temporary, error);
		}
	} restore;
	restore.temporary = directory;
	fs::create_directories(directory / "3d/free");
	std::ofstream(directory / "sprites.txt") << "# synthetic pack marker\n";
	std::ofstream(directory / "ui.txt") << "# synthetic UI marker\n";
	std::ofstream(directory / "3d/free/manifest.json") << R"({"models":[]})";
	devilution::host::Options = {};
	devilution::host::Options.hdDir = directory.string();
	InitHdPack();
	ASSERT_NE(InstalledPacks() & kPackHd2D, 0U);
	ASSERT_NE(InstalledPacks() & kPackFree3D, 0U);
	ASSERT_FALSE(GetHdAssetDirectory().empty());

	devilution::host::Options.hdDisabled = true;
	InitHdPack();
	EXPECT_EQ(InstalledPacks(), 0U);
	EXPECT_TRUE(HdDirectory().empty());
	EXPECT_TRUE(GetHdAssetDirectory().empty());
}

TEST(HostApi, HundredTicksInTheCathedral)
{
	const std::string dataDir = FindDataDir();
	if (dataDir.empty())
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
	if (HostUsedInThisProcess)
		GTEST_SKIP() << "one HostInit per process: run this test on its own (--gtest_filter)";
	HostUsedInThisProcess = true;

	setenv("SDL_AUDIODRIVER", "dummy", 1); // the host mixes nothing, but never open a device

	const fs::path saveDir = fs::temp_directory_path()
	    / ("damnationx-host-api-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(saveDir);
	const std::string save = saveDir.string() + "/";

	std::vector<std::string> args { "host_api_test", "--data-dir", dataDir, "--save-dir", save, "--config-dir", save,
		"--diablo", "--host-hero", "HostTest", "--host-class", "warrior", "--host-assets-dir", HOST_API_TEST_ASSETS_DIR };
	// The HD pack: DAMNATIONX_TEST_HD_DIR, else wherever the game finds one (the Hd2D check skips without it).
	if (const char *hdDir = std::getenv("DAMNATIONX_TEST_HD_DIR"); hdDir != nullptr && hdDir[0] != '\0') {
		args.emplace_back("--host-hd-dir");
		args.emplace_back(hdDir);
	}
	std::vector<const char *> argv;
	for (const std::string &arg : args)
		argv.push_back(arg.c_str());

	HostInitParams params {};
	params.apiVersion = kHostApiVersion;
	params.argc = static_cast<int32_t>(argv.size());
	params.argv = argv.data();
	params.uiWidth = 640;
	params.uiHeight = 480;
	ASSERT_TRUE(HostInit(params));
	// Ends the game thread even when an assertion returns early.
	struct ShutdownGuard {
		fs::path saveDir;
		~ShutdownGuard()
		{
			HostShutdown();
			std::error_code error;
			fs::remove_all(saveDir, error);
		}
	} shutdownGuard { saveDir };

	uint32_t soundEvents = 0;

	// --host-hero skips the menus: the new game loads the town.
	ASSERT_TRUE(TickUntil([](const HostTickResult &result, const HostFrame &) { return result.inGame != 0; }, 400, soundEvents))
	    << "the game never reached the town";
	const HostFrame &town = HostSnapshot();
	const HostHudState hud = HostHudStateSnapshot();
	EXPECT_EQ(hud.flags & kHudInGame, kHudInGame);
	EXPECT_EQ(hud.flags & kHudDead, MyPlayerIsDead ? kHudDead : 0U);
	EXPECT_EQ(hud.hp, MyPlayer->_pHitPoints);
	EXPECT_EQ(hud.maxHP, MyPlayer->_pMaxHP);
	EXPECT_EQ(hud.mana, MyPlayer->_pMana);
	EXPECT_EQ(hud.maxMana, MyPlayer->_pMaxMana);
	EXPECT_EQ(hud.playerLevel, MyPlayer->getCharacterLevel());
	EXPECT_EQ(hud.currentXP, MyPlayer->_pExperience);
	EXPECT_EQ(hud.nextXP, MyPlayer->getNextExperienceThreshold());
	EXPECT_EQ(hud.levelStartXP, hud.playerLevel > 1 ? GetNextExperienceThresholdForLevel(hud.playerLevel - 1) : 0U);
	EXPECT_EQ(hud.stamina, std::min<uint32_t>(MyPlayer->stamina, 100));
	EXPECT_LE(hud.stamina, 100U);
	EXPECT_EQ(hud.gold, static_cast<uint32_t>(MyPlayer->_pGold));
	EXPECT_EQ(hud.selectedSpell, static_cast<int32_t>(MyPlayer->_pRSpell));
	EXPECT_EQ(hud.selectedSpellType, static_cast<uint32_t>(MyPlayer->_pRSplType));
	for (size_t index = 0; index < 8; ++index) {
		const Item &item = MyPlayer->SpdList[index];
		EXPECT_EQ(hud.belt[index].count, item.isEmpty() ? 0U : 1U);
		if (!item.isEmpty()) {
			EXPECT_EQ(hud.belt[index].type, static_cast<int32_t>(item._itype));
			EXPECT_EQ(hud.belt[index].miscId, static_cast<int32_t>(item._iMiscId));
			EXPECT_EQ(hud.belt[index].graphicId, item._iCurs);
			EXPECT_EQ(hud.belt[index].iconId, item._iCurs + CURSOR_FIRSTITEM);
		}
	}
	HostSetHudVisible(false);
	EXPECT_EQ(HostHudStateSnapshot().hp, hud.hp);
	EXPECT_EQ(HostHudStateSnapshot().currentXP, hud.currentXP);
	HostSetHudVisible(true);
	{
		const bool enabled = *GetOptions().Gameplay.enableRun;
		const bool running = MyPlayer->running;
		GetOptions().Gameplay.enableRun.SetValue(true);
		EXPECT_TRUE(HostToggleRun());
		EXPECT_EQ(MyPlayer->running, !running);
		EXPECT_EQ((HostHudStateSnapshot().flags & kHudRunning) != 0, !running);
		EXPECT_EQ(HostHudStateSnapshot().stamina, hud.stamina);
		GetOptions().Gameplay.enableRun.SetValue(false);
		EXPECT_FALSE(HostToggleRun());
		MyPlayer->running = running;
		GetOptions().Gameplay.enableRun.SetValue(enabled);
	}
	{
		// The host menu retains native game-thread actions and audio preferences.
		DxpMenuSnapshot menu {};
		EXPECT_FALSE(dxp_menu_snapshot(nullptr, sizeof(menu)));
		EXPECT_FALSE(dxp_menu_snapshot(&menu, sizeof(menu) - 1));
		ASSERT_TRUE(dxp_menu_snapshot(&menu, sizeof(menu)));
		EXPECT_EQ(menu.version, 1U);
		EXPECT_NE(menu.flags & DxpMenuInGame, 0U);
		EXPECT_EQ(menu.flags & DxpMenuOpen, 0U);
		EXPECT_FALSE(dxp_menu_action(2));
		EXPECT_FALSE(dxp_menu_audio(0, 50));
		EXPECT_FALSE(dxp_menu_gameplay(2, false));
		EXPECT_FALSE(dxp_menu_set_ui_scale(8));
		int32_t originalUiScale = 0;
		int32_t effectiveUiScale = 0;
		EXPECT_FALSE(dxp_menu_ui_scale(nullptr, &effectiveUiScale));
		ASSERT_TRUE(dxp_menu_ui_scale(&originalUiScale, &effectiveUiScale));
		const DxpMenuSnapshot original = menu;
		dxp_menu_enable(true);
		HostInputFrame input = EmptyInput(nullptr);
		input.keyCount = 2;
		input.keys[0] = { SDLK_ESCAPE, 0, 1, 0 };
		input.keys[1] = { SDLK_ESCAPE, 0, 0, 0 };
		HostSubmitInput(input);
		HostTick(50);
		ASSERT_TRUE(dxp_menu_snapshot(&menu, sizeof(menu)));
		ASSERT_NE(menu.flags & DxpMenuOpen, 0U);
		EXPECT_FALSE(dxp_menu_action(0));
		EXPECT_FALSE(dxp_menu_action(6));
		EXPECT_FALSE(dxp_menu_audio(2, 50));
		EXPECT_FALSE(dxp_menu_gameplay(1, true));
		const auto freePortalCount = [] {
			return std::count_if(Missiles.begin(), Missiles.end(), [](const Missile &missile) {
				return missile._mitype == MissileID::TownPortal && missile._misource == -1 && !missile._miDelFlag;
			});
		};
		EXPECT_TRUE(dxp_menu_gameplay(2, false));
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_snapshot(&menu, sizeof(menu));
			return (menu.flags & DxpMenuFreePortal) == 0;
		}, 20, soundEvents));
		EXPECT_EQ(freePortalCount(), 0);
		EXPECT_TRUE(dxp_menu_gameplay(2, true));
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_snapshot(&menu, sizeof(menu));
			return (menu.flags & DxpMenuFreePortal) != 0;
		}, 20, soundEvents));
		EXPECT_EQ(freePortalCount(), 1);
		EXPECT_TRUE(dxp_menu_gameplay(2, (original.flags & DxpMenuFreePortal) != 0));
		// UI Scale is the native Graphics option (quarters, clamped to 1x .. 4x) and applies on the game thread.
		EXPECT_TRUE(dxp_menu_set_ui_scale(99));
		int32_t chosenUiScale = 0;
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_ui_scale(&chosenUiScale, &effectiveUiScale);
			return chosenUiScale == OptionEntryQuarterScale::MaxQuarters;
		}, 20, soundEvents));
		EXPECT_EQ(*GetOptions().Graphics.uiScale, OptionEntryQuarterScale::MaxQuarters);
		EXPECT_EQ(effectiveUiScale, EffectiveUiScaleQuarters(chosenUiScale, gnScreenWidth, gnScreenHeight));
		EXPECT_EQ(effectiveUiScale, GetUiScaleQuarters());
		EXPECT_TRUE(dxp_menu_set_ui_scale(originalUiScale));
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_ui_scale(&chosenUiScale, &effectiveUiScale);
			return chosenUiScale == originalUiScale;
		}, 20, soundEvents));
		EXPECT_TRUE(dxp_menu_audio(0, -50));
		EXPECT_TRUE(dxp_menu_audio(1, 300));
		EXPECT_TRUE(dxp_menu_gameplay(0, false));
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_snapshot(&menu, sizeof(menu));
			return menu.musicPercent == 0 && menu.soundPercent == 100 && (menu.flags & DxpMenuRunEnabled) == 0;
		}, 20, soundEvents));
		EXPECT_EQ(menu.flags & DxpMenuMusicOn, 0U);
		EXPECT_NE(menu.flags & DxpMenuSoundOn, 0U);
		EXPECT_FALSE(HostToggleRun());
		EXPECT_TRUE(dxp_menu_audio(0, original.musicPercent));
		EXPECT_TRUE(dxp_menu_audio(1, original.soundPercent));
		EXPECT_TRUE(dxp_menu_gameplay(0, (original.flags & DxpMenuRunEnabled) != 0));
		EXPECT_TRUE(dxp_menu_action(1));
		EXPECT_FALSE(dxp_menu_action(1)) << "one pending menu action cannot be dispatched twice";
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_snapshot(&menu, sizeof(menu));
			return (menu.flags & DxpMenuOpen) == 0;
		}, 20, soundEvents));
		EXPECT_EQ(menu.musicPercent, original.musicPercent);
		EXPECT_EQ(menu.soundPercent, original.soundPercent);
		EXPECT_EQ(menu.flags & DxpMenuRunEnabled, original.flags & DxpMenuRunEnabled);
		EXPECT_EQ(menu.flags & DxpMenuFreePortal, original.flags & DxpMenuFreePortal);
		// Save/load may present from inside a queued callback. Drive them through
		// the real host/game handoff rather than calling handlers on this thread.
		HostSubmitInput(input);
		HostTick(50);
		ASSERT_TRUE(dxp_menu_action(2));
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_snapshot(&menu, sizeof(menu));
			return (menu.flags & (DxpMenuOpen | DxpMenuBusy)) == 0 && gbValidSaveFile && pcurs == CURSOR_HAND;
		}, 40, soundEvents));
		// The first Escape can dismiss the native "Game saved" notification.
		for (int attempt = 0; attempt < 10; ++attempt) {
			HostSubmitInput(input);
			HostTick(50);
			dxp_menu_snapshot(&menu, sizeof(menu));
			if ((menu.flags & DxpMenuOpen) != 0)
				break;
		}
		ASSERT_NE(menu.flags & DxpMenuOpen, 0U);
		ASSERT_TRUE(dxp_menu_action(3));
		ASSERT_TRUE(TickUntil([&](const HostTickResult &, const HostFrame &) {
			dxp_menu_snapshot(&menu, sizeof(menu));
			return (menu.flags & (DxpMenuOpen | DxpMenuBusy)) == 0 && pcurs == CURSOR_HAND && (menu.flags & DxpMenuInGame) != 0;
		}, 60, soundEvents));
		dxp_menu_enable(false);
	}
	EXPECT_EQ(town.level.levelType, DTYPE_TOWN);
	EXPECT_GT(town.level.vertexCount, 0U);
	const uint32_t townRevision = town.level.revision;
	EXPECT_NE(townRevision, 0U);
	// A snapshot without a level change keeps the revision.
	EXPECT_EQ(HostSnapshot().level.revision, townRevision);
	{
		// Auto/HD models need exact base items: the starting warrior carries a Short Sword and Buckler,
		// rather than every weapon and shield in the exported scene.
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 0), "Short_Sword");
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 1), "Buckler");
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 2), "");
		EXPECT_STREQ(HostPlayerEquipmentName(0, 0), "");
		EXPECT_STREQ(HostPlayerEquipmentName((static_cast<uint32_t>(HostEntityKind::Object) << 24) | 1, 0), "");
		const Item left = MyPlayer->InvBody[INVLOC_HAND_LEFT];
		const Item right = MyPlayer->InvBody[INVLOC_HAND_RIGHT];
		std::swap(MyPlayer->InvBody[INVLOC_HAND_LEFT], MyPlayer->InvBody[INVLOC_HAND_RIGHT]);
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 0), "Short_Sword");
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 1), "Buckler");
		MyPlayer->InvBody[INVLOC_HAND_LEFT].clear();
		MyPlayer->InvBody[INVLOC_HAND_RIGHT].clear();
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 0), "");
		EXPECT_STREQ(HostPlayerEquipmentName(MakeHeroId(), 1), "");
		MyPlayer->InvBody[INVLOC_HAND_LEFT] = left;
		MyPlayer->InvBody[INVLOC_HAND_RIGHT] = right;
	}

	// Native inventory uses copied core data and rejects stale or invalid actions.
	// Closing through the adapter follows the same inventory flag as the I key.
	{
		DxpPanelSnapshot panels {};
		EXPECT_FALSE(dxp_panels_snapshot(nullptr, sizeof(panels)));
		EXPECT_FALSE(dxp_panels_snapshot(&panels, sizeof(panels) - 1));
		invflag = true;
		dxp_panels_enable(1);
		ASSERT_TRUE(dxp_panels_snapshot(&panels, sizeof(panels)));
		EXPECT_EQ(panels.version, 1U);
		EXPECT_EQ(panels.flags & 1U, 1U);
		EXPECT_EQ(panels.gold, static_cast<uint32_t>(MyPlayer->_pGold));
		for (const auto &slot : panels.slots) {
			EXPECT_GT(slot[2], 0);
			EXPECT_GT(slot[3], 0);
		}
		for (const auto &item : panels.items) {
			if (item.icon < 0)
				continue;
			uint32_t width = 0, height = 0;
			const auto bytes = dxp_panels_icon(item.icon, nullptr, 0, &width, &height);
			EXPECT_EQ(bytes, width * height * 4);
			ASSERT_GT(bytes, 0U);
			std::vector<uint8_t> pixels(bytes);
			EXPECT_EQ(dxp_panels_icon(item.icon, pixels.data(), bytes, &width, &height), bytes);
			bool opaque = false;
			for (size_t offset = 3; offset < pixels.size(); offset += 4)
				opaque |= pixels[offset] == 255;
			EXPECT_TRUE(opaque);
			break;
		}
		const auto oldGeneration = panels.generation;
		ASSERT_TRUE(dxp_panels_snapshot(&panels, sizeof(panels)));
		EXPECT_FALSE(dxp_panels_action(oldGeneration, 3, 0, 0, 0, 0));
		EXPECT_TRUE(invflag);
		EXPECT_FALSE(dxp_panels_action(panels.generation, 1, 47, 0.5F, 0.5F, 0));
		EXPECT_FALSE(dxp_panels_action(panels.generation, 1, 7, 1.0F, 0.5F, 0));
		EXPECT_TRUE(dxp_panels_action(panels.generation, 3, 0, 0, 0, 0));
		EXPECT_FALSE(invflag);
		EXPECT_FALSE(dxp_panels_action(panels.generation, 3, 0, 0, 0, 0));
		dxp_panels_enable(0);
	}

	// ---- Towners: walking up to them and talking (quest text, then a store) ----
	EXPECT_TRUE(TalkTo(TOWN_TAVERN, soundEvents)) << "Ogden's quest text should open";
	EXPECT_NE(DialogUi.flags & kUiQuestText, 0U) << "v5: Ogden's speech shows as quest text";
	EXPECT_TRUE(TalkTo(TOWN_SMITH, soundEvents)) << "Griswold's store should open";
	EXPECT_NE(DialogUi.flags & kUiStore, 0U) << "v5: the store shows in the UI state";
	EXPECT_EQ(DialogUi.storeTowner, TOWN_SMITH);
	EXPECT_EQ(DialogUi.activeStore, static_cast<uint8_t>(TalkID::Smith));
	{
		const HostUiState &closed = HostUiStateSnapshot();
		EXPECT_EQ(closed.flags & (kUiStore | kUiQuestText | kUiMainMenu | kUiLoading), 0U);
		EXPECT_NE(closed.revision, DialogUi.revision);
	}

	// ---- v5: audio settings for the host's mixer (sound and music are on although the game opened no device) ----
	{
		const HostAudioState audio = HostAudioSnapshot();
		EXPECT_EQ(audio.musicOn, 1);
		EXPECT_EQ(audio.soundOn, 1);
		EXPECT_GT(audio.musicGain, 0.0F);
		const int before = sound_get_or_set_music_volume(1);
		sound_get_or_set_music_volume(VOLUME_MIN); // what the game menu's slider does
		const HostAudioState &quiet = HostAudioSnapshot();
		EXPECT_EQ(quiet.musicGain, 0.0F);
		EXPECT_NE(quiet.revision, audio.revision);
		sound_get_or_set_music_volume(before);
	}
	{
		for (const _music_id track : { TMUSIC_TOWN, TMUSIC_INTRO }) {
			const uint32_t id = kHostMusicBase + static_cast<uint32_t>(track);
			const char *path = HostSoundPath(id);
			ASSERT_NE(path, nullptr);
			EXPECT_EQ(std::string(path), GetSoundAssetPath(GetMusicTrackPath(track)));
			EXPECT_GT(HostReadAsset(path, nullptr, 0), 0U);
		}
		EXPECT_EQ(HostSoundPath(kHostMusicBase + NUM_MUSIC), nullptr);
	}
	EXPECT_FALSE(qtextflag);
	EXPECT_FALSE(IsPlayerInStore());

	// ---- Text input (v4: never through SDL's event queue): typing in town must not crash and is consumed ----
	{
		HostInputFrame input = EmptyInput(nullptr);
		std::snprintf(input.text, sizeof(input.text), "%s", "text \xc3\xa9 ok");
		HostSubmitInput(input);
		EXPECT_EQ(HostTick(50).quitRequested, 0);
	}

	// ---- Render modes (contract v3), in town ----
	EXPECT_EQ(HostGetRenderMode(), HostRenderMode::World3D);
	{
		// Classic: the game draws the whole frame, opaque, like the SDL executable.
		HostSetRenderMode(HostRenderMode::Classic);
		EXPECT_EQ(HostGetRenderMode(), HostRenderMode::Classic);
		for (int i = 0; i < 3; ++i) {
			HostSubmitInput(EmptyInput(nullptr));
			const HostTickResult result = HostTick(50);
			EXPECT_EQ(result.reserved[0], 0) << "a mode set by the host is not reported back as a menu change";
		}
		const HostFrame &classic = HostSnapshot();
		ASSERT_NE(classic.ui.rgba, nullptr);
		size_t transparent = 0;
		std::vector<bool> seen(1 << 18, false);
		size_t colours = 0;
		for (size_t i = 0; i < static_cast<size_t>(classic.ui.width) * classic.ui.height; ++i) {
			const uint8_t *p = classic.ui.rgba + i * 4;
			if (p[3] != 255)
				++transparent;
			const size_t key = (static_cast<size_t>(p[0] >> 2) << 12) | (static_cast<size_t>(p[1] >> 2) << 6) | (p[2] >> 2);
			if (!seen[key]) {
				seen[key] = true;
				++colours;
			}
		}
		EXPECT_EQ(transparent, 0U) << "the Classic layer is fully opaque";

		// Outside World3D the game picks from the pointer in its own frame, whatever the host's pick says: a click two
		// tiles away from the hero on screen (tile (dx, dy) is 32 (dx - dy), 16 (dx + dy) pixels from the hero) walks there.
		const Point before = MyPlayer->position.tile;
		const int centreY = static_cast<int>(GetViewportHeight()) / 2;
		Displacement step { 2, -2 };
		for (const Displacement candidate : { Displacement { 2, -2 }, Displacement { -2, 2 }, Displacement { 2, 2 }, Displacement { -2, -2 }, Displacement { 2, 0 }, Displacement { 0, 2 } }) {
			if (IsTileWalkable(before + candidate) && PosOkPlayer(*MyPlayer, before + candidate)) {
				step = candidate;
				break;
			}
		}
		for (int frame = 0; frame < 60; ++frame) {
			HostInputFrame input = EmptyInput(nullptr);
			// v5: the game's own tile -> screen mapping (the target tile's centre).
			const HostView2D &view2d = HostView2DSnapshot().view;
			const float wx = static_cast<float>(before.x + step.deltaX) + 0.5F;
			const float wz = static_cast<float>(before.y + step.deltaY) + 0.5F;
			input.pointerX = static_cast<int32_t>(std::lround(view2d.worldToScreen[0] * wx + view2d.worldToScreen[1] * wz + view2d.worldToScreen[2]));
			input.pointerY = static_cast<int32_t>(std::lround(view2d.worldToScreen[3] * wx + view2d.worldToScreen[4] * wz + view2d.worldToScreen[5]));
			input.hoverTileX = -1; // what a 2D host sends: no 3D pick
			input.hoverTileY = -1;
			if (frame == 2)
				input.buttonsPressed = input.buttonsDown = static_cast<uint32_t>(HostButton::Primary);
			else if (frame == 3)
				input.buttonsReleased = static_cast<uint32_t>(HostButton::Primary);
			HostSubmitInput(input);
			HostTick(50);
		}
		EXPECT_EQ(MyPlayer->position.tile, before + step) << "a Classic-mode click at the transformed tile centre walks there";
		{
			// v5: the hero's screen rectangle holds its feet, which lie on the transformed position.
			HostSnapshot(); // the rectangles describe the entities of the last snapshot
			const HostView2DFrame &view2d = HostView2DSnapshot();
			ASSERT_EQ(view2d.view.valid, 1);
			EXPECT_GT(view2d.view.zoom, 0.0F);
			EXPECT_GT(view2d.rectCount, 0U);
			int heroRects = 0;
			for (uint32_t i = 0; i < view2d.rectCount; ++i) {
				const HostEntityScreenRect &rect = view2d.rects[i];
				if (rect.entityId != MakeHeroId())
					continue;
				++heroRects;
				EXPECT_GE(rect.feetX, static_cast<float>(rect.x));
				EXPECT_LE(rect.feetX, static_cast<float>(rect.x + rect.w));
				EXPECT_LT(rect.feetY, static_cast<float>(rect.y + rect.h));
				EXPECT_GT(rect.feetY, static_cast<float>(rect.y));
				// The camera follows the hero: its feet are near the middle of the view.
				EXPECT_NEAR(rect.feetX, view2d.view.viewportW / 2.0F, 80.0F);
				EXPECT_NEAR(rect.feetY, view2d.view.viewportH / 2.0F, 80.0F);
			}
			EXPECT_EQ(heroRects, 1);
		}
		EXPECT_GT(colours, 32U) << "the Classic layer holds the drawn town, not a blank frame";
		EXPECT_EQ(HostHdSnapshot().drawCount, 0U);

		// Hd2D: the HD pack's records as a draw list over the classic frame.
		if ((HostInstalledPacks() & kPackHd2D) != 0) {
			HostSetRenderMode(HostRenderMode::Hd2D);
			uint32_t draws = 0;
			uint32_t sprites = 0;
			for (int i = 0; i < 20; ++i) {
				HostSubmitInput(EmptyInput(nullptr));
				HostTick(50);
				const HostHdFrame &hd = HostHdSnapshot();
				draws = hd.drawCount;
				sprites = 0;
				for (uint32_t d = 0; d < hd.drawCount; ++d)
					sprites += hd.draws[d].kind == HostHdDrawKind::Sprite ? 1 : 0;
			}
			const HostHdFrame &hd = HostHdSnapshot();
			uint32_t kinds[4] {};
			for (uint32_t d = 0; d < hd.drawCount; ++d)
				++kinds[static_cast<int>(hd.draws[d].kind) & 3];
			std::cout << "[  NOTE    ] Hd2D: " << kinds[0] << " tiles, " << kinds[1] << " sprites, " << kinds[2] << " UI images, " << kinds[3] << " UI tints, "
			          << hd.textureCount << " textures\n";
			EXPECT_GT(draws, 0U) << "the HD pack is installed but nothing is drawn in HD";
			EXPECT_GT(sprites, 0U) << "towners and the hero have HD clips";
			if (fs::exists(fs::path(HostHdDirectory()) / "ui.txt"))
				EXPECT_GT(kinds[2], 0U) << "the control panel has HD UI art";
			ASSERT_NE(hd.palette, nullptr);
			for (uint32_t d = 0; d < hd.drawCount; ++d) {
				const HostHdDraw &draw = hd.draws[d];
				if (draw.kind == HostHdDrawKind::UiTint)
					continue;
				bool known = false;
				for (uint32_t t = 0; t < hd.textureCount && !known; ++t) {
					if (hd.textures[t].id == draw.texture) {
						known = true;
						EXPECT_TRUE(fs::exists(hd.textures[t].path)) << hd.textures[t].path;
					}
				}
				EXPECT_TRUE(known) << "draw " << d << " names an unknown texture";
				EXPECT_GT(draw.srcW, 0.0F);
				EXPECT_GT(draw.dstW, 0.0F);
			}
			if (sprites > 0)
				EXPECT_NE(hd.spriteMask.data, nullptr);
			// The texture files are readable through HostReadAsset too.
			if (hd.textureCount > 0)
				EXPECT_GT(HostReadAsset(hd.textures[0].assetPath, nullptr, 0), 0U);
		} else {
			std::cout << "[  NOTE    ] no HD 2D pack (set DAMNATIONX_TEST_HD_DIR): Hd2D draw list not checked\n";
		}
		HostSetRenderMode(HostRenderMode::World3D);
		for (int i = 0; i < 3; ++i) {
			HostSubmitInput(EmptyInput(nullptr));
			HostTick(50);
		}
		EXPECT_EQ(HostHdSnapshot().drawCount, 0U);
	}

	// Into the Cathedral, as taking the stairs does.
	StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, 1);
	ASSERT_TRUE(TickUntil([](const HostTickResult &result, const HostFrame &frame) {
		return result.inGame != 0 && frame.level.currentLevel == 1;
	}, 400, soundEvents))
	    << "the level change to dungeon 1 never finished";

	const HostFrame &dungeon = HostSnapshot();
	EXPECT_EQ(dungeon.level.levelType, DTYPE_CATHEDRAL);
	ASSERT_GT(dungeon.level.vertexCount, 0U);
	ASSERT_NE(dungeon.level.vertices, nullptr);
	EXPECT_GT(dungeon.level.indexCount, 0U);
	EXPECT_EQ(dungeon.level.indexCount % 3, 0U);
	ASSERT_NE(dungeon.level.atlasRgba, nullptr);
	EXPECT_GT(dungeon.level.atlasWidth, 0U);
	EXPECT_GT(dungeon.level.atlasHeight, 0U);
	const uint32_t dungeonRevision = dungeon.level.revision;
	EXPECT_NE(dungeonRevision, townRevision);
	CheckSnapshotLightOwnership();
	CheckChestSnapshotState();
	HostSnapshot(); // Refresh presentation after the synthetic source inputs have been restored.
	for (uint32_t i = 0; i < dungeon.level.indexCount; ++i)
		ASSERT_LT(dungeon.level.indices[i], dungeon.level.vertexCount);
	{
		render3d::LevelGeometry geometry;
		ASSERT_TRUE(render3d::UpdateLevelGeometry(geometry));
		const auto savedPosition = MyPlayer->position;
		int checkedStairs = 0;
		for (size_t cell = 0; cell < geometry.stairs.size(); ++cell) {
			if (geometry.stairs[cell] == 0)
				continue;
			MyPlayer->position.tile = { static_cast<uint8_t>(cell % MAXDUNX), static_cast<uint8_t>(cell / MAXDUNX) };
			const HostFrame &stairFrame = HostSnapshot();
			int playerCount = 0;
			const HostEntity *stairPlayer = FindLocalPlayer(stairFrame, playerCount);
			if (stairPlayer != nullptr) {
				const float floorHeight = render3d::FloorHeightOf(geometry, stairPlayer->x, stairPlayer->z);
				EXPECT_NE(floorHeight, 0.0F);
				EXPECT_NEAR(stairPlayer->y, floorHeight, 0.0001F);
				EXPECT_NEAR(stairFrame.camera.targetY, floorHeight, 0.0001F);
				++checkedStairs;
			}
		}
		MyPlayer->position = savedPosition;
		HostSnapshot();
		EXPECT_GT(checkedStairs, 0);
	}

	// A walkable tile outside the host's generous object pick area. Otherwise
	// this walking check can open a nearby door and legitimately rebuild geometry.
	const Point start = MyPlayer->position.tile;
	Point target = start;
	const auto hasNearbyObject = [](Point candidate) {
		for (const Displacement offset : { Displacement { 0, 0 }, Displacement { 1, 1 }, Displacement { 1, 0 }, Displacement { 0, 1 }, Displacement { 2, 2 } }) {
			const Object *object = FindObjectAtPosition(candidate + offset);
			if (object != nullptr && object->canInteractWith())
				return true;
		}
		return false;
	};
	for (int radius = 4; radius >= 2 && target == start; --radius) {
		for (const Displacement step : { Displacement { radius, 0 }, Displacement { 0, radius }, Displacement { -radius, 0 }, Displacement { 0, -radius },
		         Displacement { radius, radius }, Displacement { -radius, -radius } }) {
			const Point candidate = start + step;
			if (InDungeonBounds(candidate) && IsTileWalkable(candidate) && PosOkPlayer(*MyPlayer, candidate) && !hasNearbyObject(candidate)) {
				target = candidate;
				break;
			}
		}
	}
	ASSERT_NE(target, start) << "no walkable tile near the player";

	{
		Point barrelTile = start;
		for (const Displacement offset : { Displacement { 1, 0 }, Displacement { 0, 1 }, Displacement { -1, 0 }, Displacement { 0, -1 } }) {
			const Point candidate = start + offset;
			if (InDungeonBounds(candidate) && PosOkPlayer(*MyPlayer, candidate) && FindObjectAtPosition(candidate) == nullptr) {
				barrelTile = candidate;
				break;
			}
		}
		ASSERT_NE(barrelTile, start);
		Object *barrel = AddObject(_object_id::OBJ_BARREL, barrelTile);
		ASSERT_NE(barrel, nullptr);
		const uint32_t barrelId = (static_cast<uint32_t>(HostEntityKind::Object) << 24) | static_cast<uint32_t>(barrel - Objects + 1);
		EXPECT_NE(HostReachableObject(static_cast<uint32_t>(_object_id::OBJ_BARREL)), 0U);
		for (int tick = 0; tick < 160 && !barrel->IsBroken(); ++tick) {
			HostInputFrame input = EmptyInput(nullptr);
			input.hoverTileX = barrelTile.x;
			input.hoverTileY = barrelTile.y;
			if (tick % 20 == 0)
				input.buttonsPressed = input.buttonsDown = static_cast<uint32_t>(HostButton::Primary);
			else if (tick % 20 == 1)
				input.buttonsReleased = static_cast<uint32_t>(HostButton::Primary);
			HostSubmitInput(input);
			ASSERT_EQ(HostTick(50).quitRequested, 0);
		}
		ASSERT_TRUE(barrel->IsBroken()) << "tile-only World3D primary input must break an adjacent barrel";
		EXPECT_FALSE(barrel->canInteractWith());
		EXPECT_NE(HostReachableObject(static_cast<uint32_t>(_object_id::OBJ_BARREL)), barrelId);
		const HostFrame &brokenFrame = HostSnapshot();
		bool foundBroken = false;
		for (uint32_t index = 0; index < brokenFrame.entityCount; ++index) {
			if (brokenFrame.entities[index].id == barrelId) {
				foundBroken = true;
				EXPECT_NE(brokenFrame.entities[index].flags & kEntityDead, 0);
			}
		}
		EXPECT_TRUE(foundBroken);
		HostSubmitInput(EmptyInput(nullptr));
		HostTick(50);
	}
	const uint32_t postBarrelRevision = HostSnapshot().level.revision;

	// 100 ticks of scripted input: hover and click the target tile, open and close the inventory.
	uint32_t ticks = 0;
	bool inventoryOpened = false;
	bool inventoryClosedAgain = false;
	for (int frame = 0; frame < 100; ++frame) {
		HostInputFrame input = EmptyInput(nullptr);
		input.pointerX = 320 + frame % 7;
		input.pointerY = 160;
		input.hoverTileX = target.x;
		input.hoverTileY = target.y;
		if (frame == 5) {
			input.buttonsPressed = static_cast<uint32_t>(HostButton::Primary);
			input.buttonsDown = input.buttonsPressed;
		} else if (frame == 6) {
			input.buttonsReleased = static_cast<uint32_t>(HostButton::Primary);
		}
		if (frame == 60 || frame == 80) {
			input.keyCount = 2;
			input.keys[0] = { SDLK_I, 0, 1, 0 };
			input.keys[1] = { SDLK_I, 0, 0, 0 };
		}
		HostSubmitInput(input);
		const HostTickResult result = HostTick(50);
		ASSERT_EQ(result.quitRequested, 0);
		ticks += result.gameTicks;
		const HostFrame &snapshot = HostSnapshot();
		soundEvents += snapshot.soundCount;
		if (frame == 61)
			inventoryOpened = invflag && (HostUiStateSnapshot().flags & kUiInventory) != 0;
		if (frame == 81)
			inventoryClosedAgain = !invflag;
	}
	EXPECT_EQ(ticks, 100U);
	EXPECT_TRUE(inventoryOpened) << "the I key did not open the inventory";
	EXPECT_TRUE(inventoryClosedAgain) << "the second I key did not close it";
	EXPECT_NE(MyPlayer->position.tile, start) << "clicking the hovered tile did not move the player";

	const HostFrame &after = HostSnapshot();
	EXPECT_EQ(after.apiVersion, kHostApiVersion);
	EXPECT_EQ(after.level.revision, postBarrelRevision);
	EXPECT_EQ(after.level.currentLevel, 1);

	// Exactly one local player, inside the dungeon, followed by the camera.
	int localPlayers = 0;
	const HostEntity *player = FindLocalPlayer(after, localPlayers);
	ASSERT_EQ(localPlayers, 1);
	ASSERT_NE(player, nullptr);
	EXPECT_EQ(player->kind, HostEntityKind::Player);
	EXPECT_NE(player->id, 0U);
	EXPECT_GT(player->x, 0.0F);
	EXPECT_LT(player->x, static_cast<float>(MAXDUNX));
	EXPECT_GT(player->z, 0.0F);
	EXPECT_LT(player->z, static_cast<float>(MAXDUNY));
	EXPECT_NEAR(player->x, after.camera.targetX, 0.01F);
	EXPECT_NEAR(player->z, after.camera.targetZ, 0.01F);
	EXPECT_NEAR(player->x, static_cast<float>(MyPlayer->position.tile.x) + 0.5F, 1.0F);
	EXPECT_NEAR(player->z, static_cast<float>(MyPlayer->position.tile.y) + 0.5F, 1.0F);
	EXPECT_GT(after.lightCount, 0U) << "the player's own light";

	// v4: the classic frame of every entity in the sprite atlas, and the model clips.
	const HostEntityArt &art = HostEntityArtSnapshot();
	ASSERT_NE(art.atlas.rgba, nullptr);
	EXPECT_EQ(art.atlas.width, 2048U);
	const HostEntitySprite *heroSprite = nullptr;
	for (uint32_t i = 0; i < art.spriteCount; ++i) {
		if (art.sprites[i].entityId == player->id)
			heroSprite = &art.sprites[i];
	}
	ASSERT_NE(heroSprite, nullptr) << "the hero's classic frame";
	EXPECT_GT(heroSprite->width, 0);
	EXPECT_EQ(static_cast<float>(heroSprite->width), heroSprite->srcW);
	size_t opaqueTexels = 0;
	for (int y = 0; y < static_cast<int>(heroSprite->srcH); ++y) {
		for (int x = 0; x < static_cast<int>(heroSprite->srcW); ++x) {
			const size_t at = ((static_cast<size_t>(heroSprite->srcY) + y) * art.atlas.width + static_cast<size_t>(heroSprite->srcX) + x) * 4;
			opaqueTexels += art.atlas.rgba[at + 3] != 0 ? 1 : 0;
		}
	}
	EXPECT_GT(opaqueTexels, 100U) << "the hero's frame is decoded into the atlas";
	if ((HostInstalledPacks() & kPackCharacters3D) != 0) {
		ASSERT_GT(art.clipCount, 0U);
		EXPECT_EQ(art.clips[0].entityId, player->id);
		EXPECT_NE(std::string(art.clips[0].clips).find("stand"), std::string::npos) << art.clips[0].clips;
	}

	// World3D extras: the 3D character of the pack for the hero.
	const HostWorldFrame &world = HostWorldSnapshot();
	if ((HostInstalledPacks() & kPackCharacters3D) != 0) {
		bool hinted = false;
		for (uint32_t i = 0; i < world.modelHintCount; ++i)
			hinted = hinted || world.modelHints[i].entityId == player->id;
		EXPECT_TRUE(hinted) << "the pack's 3D warrior should be offered for the hero";
	}
	for (uint32_t i = 0; i < world.placementCount; ++i)
		EXPECT_TRUE(fs::exists(world.placements[i].path)) << world.placements[i].path;

	// The classic UI layer: opaque panel, transparent world.
	ASSERT_EQ(after.ui.width, 640U);
	ASSERT_EQ(after.ui.height, 480U);
	ASSERT_NE(after.ui.rgba, nullptr);
	EXPECT_GT(after.ui.revision, 0U);
	size_t transparent = 0;
	size_t opaque = 0;
	for (size_t i = 0; i < static_cast<size_t>(after.ui.width) * after.ui.height; ++i) {
		if (after.ui.rgba[i * 4 + 3] == 0)
			++transparent;
		else
			++opaque;
	}
	EXPECT_GT(transparent, opaque / 4) << "the world area should show through the UI layer";
	EXPECT_GT(opaque, 640U * 100U) << "the control panel should be drawn";

	// Sounds: music started on each level load at least; every event is reported once.
	EXPECT_GT(soundEvents, 0U);

	// ---- Classic: aim at a monster through HostView2DSnapshot and click (a 2D host's click: pointer and button in one
	// frame, no 3D pick) until it dies; the hero's experience rises ----
	{
		// The nearest living monster of the level, moved a few tiles from the hero into its light (exploring the level
		// for one is not what this checks; a monster created from scratch lacked parts of its graphics).
		Point spawnAt = MyPlayer->position.tile;
		for (const Displacement step : { Displacement { 3, 0 }, Displacement { 0, 3 }, Displacement { -3, 0 }, Displacement { 0, -3 },
		         Displacement { 2, 2 }, Displacement { -2, -2 }, Displacement { 2, -2 }, Displacement { -2, 2 } }) {
			const Point candidate = MyPlayer->position.tile + step;
			if (InDungeonBounds(candidate) && IsTileWalkable(candidate) && dMonster[candidate.x][candidate.y] == 0 && dPlayer[candidate.x][candidate.y] == 0) {
				spawnAt = candidate;
				break;
			}
		}
		ASSERT_NE(spawnAt, MyPlayer->position.tile) << "no free tile next to the hero";
		Monster *spawned = nullptr;
		int best = 1 << 30;
		for (size_t i = 0; i < ActiveMonsterCount; ++i) {
			Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.isUnique() || monster.hitPoints >> 6 <= 0 || !monster.animInfo.sprites || (monster.flags & MFLAG_HIDDEN) != 0)
				continue;
			const int distance = monster.position.tile.WalkingDistance(MyPlayer->position.tile);
			if (distance < best) {
				best = distance;
				spawned = &monster;
			}
		}
		ASSERT_NE(spawned, nullptr) << "no monster on this level";
		M_ClearSquares(*spawned);
		spawned->position.tile = spawned->position.future = spawned->position.old = spawnAt;
		dMonster[spawnAt.x][spawnAt.y] = static_cast<int16_t>(spawned->getId() + 1);
		if (spawned->lightId != NO_LIGHT)
			ChangeLightXY(spawned->lightId, spawnAt);
		M_StartStand(*spawned, Direction::South);
		const uint32_t target = (static_cast<uint32_t>(HostEntityKind::Monster) << 24) | (static_cast<uint32_t>(spawned->getId()) + 1);

		HostSetRenderMode(HostRenderMode::Classic);
		const uint32_t experienceBefore = MyPlayer->_pExperience;
		int clicks = 0;
		bool sawTarget = false;
		for (int frame = 0; frame < 1200 && MyPlayer->_pExperience == experienceBefore; ++frame) {
			const HostFrame &snap = HostSnapshot();
			const HostView2DFrame &view2d = HostView2DSnapshot();
			const HostEntityScreenRect *aim = nullptr;
			for (uint32_t r = 0; r < view2d.rectCount; ++r) {
				if (view2d.rects[r].entityId == target)
					aim = &view2d.rects[r];
				// Only what the classic view draws (and so can pick) has a rectangle: nothing in the dark.
				for (uint32_t e = 0; e < snap.entityCount; ++e) {
					if (snap.entities[e].id == view2d.rects[r].entityId)
						ASSERT_EQ(snap.entities[e].flags & kEntityInvisible, 0U) << "screen rectangle for an undrawn entity";
				}
			}
			HostInputFrame input = EmptyInput(nullptr);
			input.inputFlags = kInputNoWorldPick;
			if (aim != nullptr) {
				sawTarget = true;
				// Like a scripted 2D host: the pointer jumps onto the monster in the frame of the click (the middle of the
				// sprite, kept inside the view) and rests in a corner otherwise, so the click must use its own position.
				input.pointerX = 8;
				input.pointerY = 8;
				if (frame % 10 == 0 || frame % 10 == 1) {
					input.pointerX = std::clamp(aim->x + aim->w / 2, 0, view2d.view.viewportW - 1);
					input.pointerY = std::clamp(aim->y + aim->h / 2, 0, view2d.view.viewportH - 1);
				}
				if (frame % 10 == 0) {
					input.buttonsPressed = input.buttonsDown = static_cast<uint32_t>(HostButton::Primary);
					++clicks;
				} else if (frame % 10 == 1) {
					input.buttonsReleased = static_cast<uint32_t>(HostButton::Primary);
				}
			}
			HostSubmitInput(input);
			ASSERT_EQ(HostTick(50).quitRequested, 0);
			ASSERT_FALSE(MyPlayerIsDead) << "the hero died fighting";
		}
		EXPECT_TRUE(sawTarget) << "the spawned monster has no screen rectangle";
		EXPECT_GT(MyPlayer->_pExperience, experienceBefore) << "Classic clicks on the monster did not kill it (" << clicks << " clicks)";
		std::cout << "[  NOTE    ] Classic kill: " << clicks << " clicks, experience " << experienceBefore << " -> " << MyPlayer->_pExperience << "\n";
		HostSetRenderMode(HostRenderMode::World3D);
		HostSnapshot();
	}

	// ---- v5: checkpoint the game state, walk on, restore; the player's save on disk is never written by either ----
	{
		const auto readSave = [] {
			std::vector<char> bytes;
			std::FILE *file = std::fopen(pfile_get_save_path().c_str(), "rb");
			if (file == nullptr)
				return bytes;
			char buffer[65536];
			for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
				bytes.insert(bytes.end(), buffer, buffer + read);
			std::fclose(file);
			return bytes;
		};
		SaveGame(); // the game's own save (menu Save Game)
		const std::vector<char> savedOnDisk = readSave();
		ASSERT_FALSE(savedOnDisk.empty()) << pfile_get_save_path();
		const auto started = std::chrono::steady_clock::now();
		const uint32_t size = HostSaveState(nullptr, 0);
		ASSERT_GT(size, 0U) << "a state can be taken in a single player level";
		std::vector<uint8_t> state(size);
		ASSERT_EQ(HostSaveState(state.data(), size), size) << "the second call returns the same state";
		const auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		const Point savedTile = MyPlayer->position.tile;
		const int savedHp = MyPlayer->_pHitPoints;
		const size_t savedMonsters = ActiveMonsterCount;

		// Walk somewhere else.
		Point away = savedTile;
		for (const Displacement step : { Displacement { 3, 0 }, Displacement { 0, 3 }, Displacement { -3, 0 }, Displacement { 0, -3 } }) {
			if (IsTileWalkable(savedTile + step) && PosOkPlayer(*MyPlayer, savedTile + step)) {
				away = savedTile + step;
				break;
			}
		}
		for (int frame = 0; frame < 40; ++frame) {
			HostInputFrame input = EmptyInput(nullptr);
			input.hoverTileX = away.x;
			input.hoverTileY = away.y;
			if (frame == 1)
				input.buttonsPressed = input.buttonsDown = static_cast<uint32_t>(HostButton::Primary);
			else if (frame == 2)
				input.buttonsReleased = static_cast<uint32_t>(HostButton::Primary);
			HostSubmitInput(input);
			HostTick(50);
		}
		EXPECT_NE(MyPlayer->position.tile, savedTile) << "the hero walked away from the checkpoint";

		const auto loadStarted = std::chrono::steady_clock::now();
		EXPECT_EQ(readSave(), savedOnDisk) << "taking a checkpoint and playing on must not write the player's save";
		ASSERT_TRUE(HostLoadState(state.data(), size));
		EXPECT_EQ(readSave(), savedOnDisk) << "restoring a checkpoint must not write the player's save";
		const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loadStarted).count();
		EXPECT_EQ(MyPlayer->position.tile, savedTile) << "the restore puts the hero back";
		EXPECT_EQ(MyPlayer->_pHitPoints, savedHp);
		EXPECT_EQ(ActiveMonsterCount, savedMonsters);
		EXPECT_EQ(currlevel, 1);
		std::cout << "[  NOTE    ] state " << size << " bytes, save " << saveMs << " ms, load " << loadMs << " ms\n";
		// The game plays on from the restored state.
		for (int frame = 0; frame < 10; ++frame) {
			HostSubmitInput(EmptyInput(nullptr));
			const HostTickResult result = HostTick(50);
			ASSERT_EQ(result.quitRequested, 0);
			EXPECT_EQ(result.inGame, 1);
		}
		EXPECT_FALSE(HostLoadState(state.data(), 16)) << "a truncated state is refused";
		HostSnapshot(); // deliver the sounds of these ticks
		EXPECT_EQ(readSave(), savedOnDisk) << "playing on after the restore does not save either";
	}
	// Native integration: each actual section entrance, remembered lower visits,
	// and full save-state loads in Town and dungeon. Ordinary rendered portal
	// clicks are qualified separately; StartWarpLvl here isolates native lifecycle.
	{
		const auto freeCount = [] {
			return std::count_if(Missiles.begin(), Missiles.end(), [](const Missile &missile) {
				return missile._mitype == MissileID::TownPortal && missile.IsTrap();
			});
		};
		const auto checkpoint = [] {
			HostSubmitInput(EmptyInput(nullptr));
			EXPECT_EQ(HostTick(50).quitRequested, 0);
			const uint32_t size = HostSaveState(nullptr, 0);
			EXPECT_GT(size, 0U);
			std::vector<uint8_t> bytes(size);
			if (size != 0)
				EXPECT_EQ(HostSaveState(bytes.data(), size), size);
			return bytes;
		};
		ASSERT_TRUE(SetFreeTownPortalEnabled(true));
		// The final5 is deliberately lower than13: this is recency, not progress.
		const std::vector<uint8_t> sections = gbIsSpawn ? std::vector<uint8_t> { 1 } : std::vector<uint8_t> { 1, 5, 9, 13, 5 };
		for (const uint8_t section : sections) {
			SCOPED_TRACE("free portal section " + std::to_string(section));
			StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, section);
			ASSERT_TRUE(TickUntil([section](const HostTickResult &result, const HostFrame &frame) {
				return result.inGame != 0 && frame.level.currentLevel == section;
			}, 400, soundEvents));
			EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), section);
			EXPECT_EQ(freeCount(), 1);
			EXPECT_EQ(leveltype, GetLevelType(section));
			const Point endpoint = CathedralPortalPosition();
			EXPECT_FALSE(TileHasAny(endpoint, TileProperties::Solid | TileProperties::BlockMissile));
			const auto dungeonState = checkpoint();
			ASSERT_FALSE(dungeonState.empty());

			StartWarpLvl(*MyPlayer, FreeTownPortalWarp(section));
			ASSERT_TRUE(TickUntil([](const HostTickResult &result, const HostFrame &frame) {
				return result.inGame != 0 && frame.level.currentLevel == 0;
			}, 400, soundEvents));
			EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), section);
			EXPECT_EQ(freeCount(), 1);
			EXPECT_TRUE(std::any_of(Missiles.begin(), Missiles.end(), [](const Missile &missile) {
				return missile._mitype == MissileID::TownPortal && missile.IsTrap() && missile.position.tile == TownCathedralPortalPosition;
			}));
			const auto townState = checkpoint();
			ASSERT_FALSE(townState.empty());
			MyPlayer->lastFreeTownPortalSection = section == 1 ? 13 : 1;
			ASSERT_TRUE(HostLoadState(townState.data(), static_cast<uint32_t>(townState.size())));
			EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), section);
			EXPECT_EQ(freeCount(), 1);

			// Options are live preferences, not restored gameplay bytes. Loading
			// a saved enabled portal cannot resurrect it after an explicit opt-out.
			ASSERT_TRUE(SetFreeTownPortalEnabled(false));
			ASSERT_TRUE(HostLoadState(townState.data(), static_cast<uint32_t>(townState.size())));
			EXPECT_FALSE(IsFreeTownPortalEnabled());
			EXPECT_EQ(freeCount(), 0);
			EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), section);
			ASSERT_TRUE(SetFreeTownPortalEnabled(true));
			ASSERT_TRUE(HostLoadState(townState.data(), static_cast<uint32_t>(townState.size())));
			EXPECT_EQ(freeCount(), 1);

			StartWarpLvl(*MyPlayer, FreeTownPortalWarp(FreeTownPortalTargetLevel(*MyPlayer)));
			ASSERT_TRUE(TickUntil([section](const HostTickResult &result, const HostFrame &frame) {
				return result.inGame != 0 && frame.level.currentLevel == section;
			}, 400, soundEvents));
			EXPECT_EQ(freeCount(), 1);
			ASSERT_TRUE(HostLoadState(dungeonState.data(), static_cast<uint32_t>(dungeonState.size())));
			EXPECT_EQ(currlevel, section);
			EXPECT_EQ(FreeTownPortalTargetLevel(*MyPlayer), section);
			EXPECT_EQ(freeCount(), 1) << "full dungeon reload must replace the saved free missile, not duplicate it";
		}
		HostSnapshot(); // drain sounds once before the repeated-snapshot assertion.
	}
	EXPECT_EQ(HostSnapshot().soundCount, 0U) << "a second snapshot in the same tick must not repeat the sounds";

}

/**
 * The main menu with the host's input only: Single Player, a new Warrior named by typing (text input, v4: delivered
 * without SDL's event queue, which crashed under sdl2-compat), Normal difficulty, into the town.
 */
TEST(HostApi, NewHeroTypedInTheMenus)
{
	const std::string dataDir = FindDataDir();
	if (dataDir.empty())
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
	if (HostUsedInThisProcess)
		GTEST_SKIP() << "one HostInit per process: run this test on its own (--gtest_filter)";
	HostUsedInThisProcess = true;
	setenv("SDL_AUDIODRIVER", "dummy", 1);

	const fs::path saveDir = fs::temp_directory_path()
	    / ("damnationx-host-menu-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(saveDir);
	const std::string save = saveDir.string() + "/";
	std::vector<std::string> args { "host_api_test", "--data-dir", dataDir, "--save-dir", save, "--config-dir", save,
		"--diablo", "--host-assets-dir", HOST_API_TEST_ASSETS_DIR };
	std::vector<const char *> argv;
	for (const std::string &arg : args)
		argv.push_back(arg.c_str());
	HostInitParams params {};
	params.apiVersion = kHostApiVersion;
	params.argc = static_cast<int32_t>(argv.size());
	params.argv = argv.data();
	params.uiWidth = 640;
	params.uiHeight = 480;
	ASSERT_TRUE(HostInit(params));
	struct ShutdownGuard {
		fs::path saveDir;
		~ShutdownGuard()
		{
			HostShutdown();
			std::error_code error;
			fs::remove_all(saveDir, error);
		}
	} shutdownGuard { saveDir };

	const auto tick = [](int frames, HostInputFrame input = EmptyInput(nullptr)) {
		for (int i = 0; i < frames; ++i) {
			input.inputFlags = kInputNoWorldPick;
			HostSubmitInput(input);
			const HostTickResult result = HostTick(50);
			if (result.quitRequested != 0)
				return false;
			if (result.inGame != 0)
				return true;
			input = EmptyInput(nullptr);
		}
		return true;
	};
	const auto key = [](SDL_Keycode code) {
		HostInputFrame input = EmptyInput(nullptr);
		input.keyCount = 2;
		input.keys[0] = { static_cast<int32_t>(code), 0, 1, 0 };
		input.keys[1] = { static_cast<int32_t>(code), 0, 0, 0 };
		return input;
	};

	ASSERT_TRUE(tick(60));           // the main menu fades in
	EXPECT_NE(HostUiStateSnapshot().flags & kUiMainMenu, 0U) << "v5: DiabloUI screens show as kUiMainMenu";
	ASSERT_TRUE(tick(30, key(SDLK_RETURN))); // Single Player: no heroes yet, so the class list opens
	ASSERT_TRUE(tick(30, key(SDLK_RETURN))); // Warrior: the name dialog
	HostInputFrame erase = EmptyInput(nullptr);
	erase.keyCount = 16;
	for (int i = 0; i < 8; ++i) {
		erase.keys[i * 2] = { SDLK_BACKSPACE, 0, 1, 0 };
		erase.keys[i * 2 + 1] = { SDLK_BACKSPACE, 0, 0, 0 };
	}
	ASSERT_TRUE(tick(2, erase)); // clear a generated name (twice: up to 16 characters)
	ASSERT_TRUE(tick(2, erase));
	HostInputFrame typed = EmptyInput(nullptr);
	std::snprintf(typed.text, sizeof(typed.text), "%s", "Typist");
	ASSERT_TRUE(tick(5, typed));
	ASSERT_TRUE(tick(30, key(SDLK_RETURN))); // OK: the hero is created, the difficulty list opens
	ASSERT_TRUE(tick(30, key(SDLK_RETURN))); // Normal
	bool inGame = false;
	for (int i = 0; i < 400 && !inGame; ++i) {
		HostSubmitInput(EmptyInput(nullptr));
		const HostTickResult result = HostTick(50);
		ASSERT_EQ(result.quitRequested, 0);
		inGame = result.inGame != 0;
		if (!inGame && i % 60 == 59)
			tick(1, key(SDLK_RETURN)); // any further confirmation
	}
	ASSERT_TRUE(inGame) << "the menus did not lead into the game";
	EXPECT_EQ(HostUiStateSnapshot().flags & kUiMainMenu, 0U);
	ASSERT_NE(MyPlayer, nullptr);
	EXPECT_STREQ(MyPlayer->_pName, "Typist") << "the typed name";
}
