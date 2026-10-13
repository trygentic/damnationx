#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include <sol/sol.hpp>

#include "engine/assets.hpp"
#include "engine/demomode.h"
#include "engine/load_cl2.hpp"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/scrollrt.h"
#include "engine/surface.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "init.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "lua/lua_global.hpp"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "options.h"
#include "pfile.h"
#include "player.h"
#include "tables/monstdat.h"
#include "tables/playerdat.hpp"
#include "utils/display.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

using namespace devilution;

namespace {

bool Dummy_GetHeroInfo(_uiheroinfo *pInfo)
{
	return true;
}

/**
 * @brief How the hybrid presentation options are driven during a replay.
 *
 * `hybridLighting` / `hybridNormalMaps` are presentation-only: whatever their value and however often
 * they flip, the canonical simulation state must not change.
 *
 * The replay runs with HeadlessMode set, and under HeadlessMode every hybrid option reader in the engine is gated
 * off: DrawAndBlit and scrollrt_draw_game_screen return before CurrentHybridLatchInput / LatchForFrame,
 * PrewarmHybridSpriteNormals returns at once and gbuffer::LatchForFrame disables the mode for a headless input.
 * Flipping the options alone therefore only proves that they do not change headless simulation. To exercise the
 * hybrid code while the simulation runs, every scenario also runs HybridFrameProbe (below) at each game event.
 */
struct HybridOptionScenario {
	const char *name;
	bool lightingAtStart;
	bool normalMapsAtStart;
	/**
	 * Flip the options from every Lua game event the replay fires (GameStart, monster/player damage, experience):
	 * hybridLighting at every event, hybridNormalMaps at every other one.
	 */
	bool toggleOnEvents;
};

constexpr HybridOptionScenario HybridOptionScenarios[] {
	{ "off", false, true, false },
	{ "on", true, true, false },
	{ "on-flat", true, false, false },
	{ "toggled", false, true, true },
};

struct ReplayDigest {
	/** False when spawn.mpq / diabdat.mpq are not installed (the replay was not run). */
	bool dataAvailable = false;
	HeroCompareResult::Status heroStatus = HeroCompareResult::ReferenceNotFound;
	std::string heroMessage;
	/** Canonical state hashes sampled at every Lua game event, in firing order. */
	std::vector<uint64_t> eventHashes;
	unsigned toggles = 0;
	/** Game events at which the probe's forced latch enabled the hybrid mode (hybridLighting on, not in town). */
	unsigned hybridFrames = 0;
	/**
	 * Plane A bytes the probe's sprite capture tagged Card, split by normal: FlatCardByte (the whole flat tier, and
	 * generated pixels that quantise to the flat card normal) / any other normal (only a generated plane writes one).
	 */
	size_t flatCardPixels = 0;
	size_t generatedCardPixels = 0;
	/** Largest clx_normal_map cache entry count seen after a probe frame. */
	size_t maxNormalPlaneEntries = 0;
};

uint64_t Fnv1a(uint64_t hash, const void *data, size_t size)
{
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

template <typename T>
uint64_t FnvValue(uint64_t hash, const T &value)
{
	return Fnv1a(hash, &value, sizeof(T));
}

/**
 * @brief Hashes the simulation state that a presentation option must never influence: the level, the player,
 * every active monster, object and item, the missile count and the per-tile light / exploration fields.
 */
uint64_t HashCanonicalTickState()
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	hash = FnvValue(hash, currlevel);
	hash = FnvValue(hash, leveltype);
	hash = FnvValue(hash, ViewPosition.x);
	hash = FnvValue(hash, ViewPosition.y);
	if (MyPlayer != nullptr) {
		const Player &player = *MyPlayer;
		hash = FnvValue(hash, player.position.tile.x);
		hash = FnvValue(hash, player.position.tile.y);
		hash = FnvValue(hash, player._pmode);
		hash = FnvValue(hash, player._pdir);
		hash = FnvValue(hash, player._pHitPoints);
		hash = FnvValue(hash, player._pMana);
		hash = FnvValue(hash, player._pExperience);
		hash = FnvValue(hash, player.getCharacterLevel());
		hash = FnvValue(hash, player.AnimInfo.currentFrame);
	}
	hash = FnvValue(hash, ActiveMonsterCount);
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		hash = FnvValue(hash, monster.position.tile.x);
		hash = FnvValue(hash, monster.position.tile.y);
		hash = FnvValue(hash, monster.hitPoints);
		hash = FnvValue(hash, monster.mode);
		hash = FnvValue(hash, monster.direction);
		hash = FnvValue(hash, monster.animInfo.currentFrame);
	}
	hash = FnvValue(hash, ActiveObjectCount);
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		hash = FnvValue(hash, object.position.x);
		hash = FnvValue(hash, object.position.y);
		hash = FnvValue(hash, object._oAnimFrame);
		hash = FnvValue(hash, object._oSolidFlag);
		hash = FnvValue(hash, object._oVar1);
		hash = FnvValue(hash, object._oVar4);
	}
	hash = FnvValue(hash, ActiveItemCount);
	for (uint8_t i = 0; i < ActiveItemCount; ++i) {
		const Item &item = Items[ActiveItems[i]];
		hash = FnvValue(hash, item.position.x);
		hash = FnvValue(hash, item.position.y);
		hash = FnvValue(hash, item._iSeed);
		hash = FnvValue(hash, item.IDidx);
	}
	const size_t missileCount = Missiles.size();
	hash = FnvValue(hash, missileCount);
	hash = Fnv1a(hash, dLight, sizeof(dLight));
	hash = Fnv1a(hash, dFlags, sizeof(dFlags));
	return hash;
}

/**
 * @brief Runs the hybrid frame-start and sprite-capture code from inside the replay.
 *
 * HeadlessMode keeps the engine's own hybrid path dormant (see HybridOptionScenario), so the probe does what a
 * presented frame does with exactly two latch gates forced open: it takes the live CurrentHybridLatchInput()
 * (which reads `hybridLighting`, gbRunGame and leveltype), sets presenterReady and clears headless, and calls
 * LatchHybridWorldFrame (which reads `hybridNormalMaps`, applies clx_normal_map::SetOptions and runs Pump). When
 * the mode latches on it prewarms a real monster sprite (the zombie stand animation from the game data) and
 * captures a few of its frames through DrawWorldSprite into a probe surface that owns its own G-buffer planes,
 * which resolves normal planes through GetOrBuildNormalPlane. The probe runs in every scenario after the canonical
 * sample of the event, so a render-to-simulation coupling in that code (an RNG draw, a side effect of Pump or of
 * the normal-map generator) changes the following samples and the hero comparison.
 */
class HybridFrameProbe {
public:
	static constexpr int Width = 256;
	static constexpr int Height = 192;

	HybridFrameProbe()
	    : surface_(Width, Height)
	{
	}

	~HybridFrameProbe()
	{
		gbuffer::EndFrame();
		gbuffer::Free();
		clx_normal_map::ClearNormalPlaneCache();
		clx_normal_map::SetOptions(clx_normal_map::Options {});
	}

	HybridFrameProbe(const HybridFrameProbe &) = delete;
	HybridFrameProbe &operator=(const HybridFrameProbe &) = delete;

	/** Loads the probe sprite from the game data; false (with a test failure) when it cannot be read. */
	bool LoadSprite()
	{
		std::expected<OwnedClxSpriteListOrSheet, std::string> loaded = LoadCl2ListOrSheetWithStatus("monsters\\zombie\\zombien", PointerOrValue<uint16_t> { 128 });
		if (!loaded.has_value()) {
			ADD_FAILURE() << "cannot load the probe sprite: " << loaded.error();
			return false;
		}
		sprites_.emplace(std::move(*loaded));
		const ClxSpriteListOrSheet sprites { *sprites_ };
		frames_ = sprites.isSheet() ? sprites.sheet()[0] : sprites.list();
		gbuffer::Allocate(surface_.begin(), static_cast<uint16_t>(surface_.pitch()), Width, Height);
		return true;
	}

	void RunFrame(ReplayDigest &digest)
	{
		if (!sprites_)
			return;
		gbuffer::LatchInput in = CurrentHybridLatchInput();
		// The only two gates a headless replay closes. Everything else (option, gbRunGame, level) stays live.
		in.presenterReady = true;
		in.headless = false;
		LatchHybridWorldFrame(in);
		if (!gbuffer::Enabled()) {
			gbuffer::EndFrame();
			return;
		}
		++digest.hybridFrames;
		clx_normal_map::EnqueuePrewarm(*frames_);
		(void)clx_normal_map::Pump(1500);

		gbuffer::BeginWorldPass(Height);
		constexpr unsigned DrawsPerFrame = 3;
		for (unsigned i = 0; i < DrawsPerFrame; ++i) {
			const ClxSprite sprite = (*frames_)[(frameIndex_ + i) % frames_->numSprites()];
			const WorldSpriteDraw draw {
				.feet = { static_cast<int>(i) * 64, Height - 1 },
				.sprite = sprite,
			};
			DrawWorldSprite(surface_, draw, /*hybrid=*/true);
		}
		frameIndex_ += DrawsPerFrame;
		gbuffer::EndWorldPass();

		for (const uint8_t aux : gbuffer::AuxRows(Height)) {
			if (gbuffer::AuxKind(aux) != gbuffer::Kind::Card)
				continue;
			if (aux == gbuffer::FlatCardByte)
				++digest.flatCardPixels;
			else
				++digest.generatedCardPixels;
		}
		gbuffer::EndFrame();
		digest.maxNormalPlaneEntries = std::max(digest.maxNormalPlaneEntries, clx_normal_map::GetStats().entries);
	}

private:
	OwnedSurface surface_;
	std::optional<OwnedClxSpriteListOrSheet> sprites_;
	std::optional<ClxSpriteList> frames_;
	unsigned frameIndex_ = 0;
};

ReplayDigest *CurrentDigest = nullptr;
HybridFrameProbe *CurrentProbe = nullptr;
bool CurrentToggleOnEvents = false;

/**
 * Lua event callback: samples the canonical state, then (in the toggled scenario) flips both options, then runs
 * the hybrid probe frame with whatever the options now say.
 */
void OnReplayGameEvent()
{
	if (CurrentDigest == nullptr)
		return;
	CurrentDigest->eventHashes.push_back(HashCanonicalTickState());
	if (CurrentToggleOnEvents) {
		// hybridLighting flips at every event and hybridNormalMaps at every other one, so starting from (off, on)
		// the probe frames alternate between the flat tier and generated normals and SetOptions clears the cache.
		GetOptions().Graphics.hybridLighting.SetValue(!*GetOptions().Graphics.hybridLighting);
		if (CurrentDigest->toggles % 2 == 0)
			GetOptions().Graphics.hybridNormalMaps.SetValue(!*GetOptions().Graphics.hybridNormalMaps);
		++CurrentDigest->toggles;
	}
	if (CurrentProbe != nullptr)
		CurrentProbe->RunFrame(*CurrentDigest);
}

void InstallReplayEventHooks()
{
	sol::state &lua = GetLuaState();
	sol::table *events = GetLuaEvents();
	ASSERT_NE(events, nullptr);
	lua.set_function("timedemo_replay_event", &OnReplayGameEvent);
	const sol::object callback = lua["timedemo_replay_event"];
	for (const char *name : { "GameStart", "OnMonsterTakeDamage", "OnPlayerTakeDamage", "OnPlayerGainExperience" }) {
		const sol::protected_function add = (*events)[name]["add"];
		ASSERT_TRUE(add.valid()) << name;
		const sol::protected_function_result result = add(callback);
		ASSERT_TRUE(result.valid()) << name;
	}
}

ReplayDigest RunTimedemo(std::string timedemoFolderName, const HybridOptionScenario &scenario)
{
	ReplayDigest digest;
	if (
#ifdef USE_SDL3
	    !SDL_Init(SDL_INIT_EVENTS)
#elif !defined(USE_SDL1)
	    SDL_Init(SDL_INIT_EVENTS) < 0
#else
	    SDL_Init(0) < 0
#endif
	) {
		ErrSdl();
	}

	LoadCoreArchives();
	LoadGameArchives();

	// The tests need spawn.mpq or diabdat.mpq
	if (!HaveMainData()) {
		SDL_Quit();
		return digest;
	}
	digest.dataAvailable = true;

	// The MPQ search path includes the pref path: keep the defaults so a later replay in this process finds the data.
	const std::string savedPrefPath = paths::PrefPath();
	const std::string savedConfigPath = paths::ConfigPath();
	// Every replay reads the demo and the saves from its pref path and rewrites the hero save there. Several
	// replays run in this binary and several ctest entries run it concurrently, so each replay works on its own
	// copy of the fixture folder instead of mutating the shared one.
	const int demoNumber = 0;
	namespace fs = std::filesystem;
	const fs::path fixtureFolder = fs::path(paths::BasePath()) / "test" / "fixtures" / "timedemo" / timedemoFolderName;
	static std::atomic<unsigned> replayCounter { 0 };
	const fs::path replayFolder = fs::temp_directory_path()
	    / StrCat("devilutionx-timedemo-", timedemoFolderName, "-",
	        std::chrono::steady_clock::now().time_since_epoch().count(), "-", replayCounter++);
	std::error_code copyError;
	fs::create_directories(replayFolder, copyError);
	fs::copy(fixtureFolder, replayFolder, fs::copy_options::overwrite_existing | fs::copy_options::recursive, copyError);
	if (!copyError && !fs::exists(replayFolder / StrCat("demo_", demoNumber, ".dmo")))
		copyError = std::make_error_code(std::errc::no_such_file_or_directory);
	if (copyError) {
		ADD_FAILURE() << "cannot copy " << fixtureFolder << " to " << replayFolder << ": " << copyError.message();
		SDL_Quit();
		return digest;
	}
	paths::SetPrefPath(replayFolder.string());
	paths::SetConfigPath(replayFolder.string());

	InitKeymapActions();
	LoadOptions();
	demo::OverrideOptions();
	LuaInitialize();

	Players.resize(1);
	MyPlayerId = demoNumber;
	MyPlayer = &Players[MyPlayerId];
	*MyPlayer = {};

	// Currently only spawn.mpq is present when building on github actions
	gbIsSpawn = true;
	gbIsHellfire = false;
	gbMusicOn = false;
	gbSoundOn = false;
	HeadlessMode = true;
	demo::InitPlayBack(demoNumber, true);

	LoadSpellData();
	LoadPlayerDataFiles();
	LoadMissileData();
	LoadMonsterData();
	LoadItemData();
	LoadObjectData();
	pfile_ui_set_hero_infos(Dummy_GetHeroInfo);
	gbLoadGame = true;

	demo::OverrideOptions();

	// The hybrid options are set after every OverrideOptions so nothing resets them before the replay starts.
	GetOptions().Graphics.hybridLighting.SetValue(scenario.lightingAtStart);
	GetOptions().Graphics.hybridNormalMaps.SetValue(scenario.normalMapsAtStart);
	std::optional<HybridFrameProbe> probe;
	probe.emplace();
	if (probe->LoadSprite())
		CurrentProbe = &*probe;
	CurrentDigest = &digest;
	CurrentToggleOnEvents = scenario.toggleOnEvents;
	InstallReplayEventHooks();

	AdjustToScreenGeometry(forceResolution);

	StartGame(false, true);

	const HeroCompareResult result = pfile_compare_hero_demo(demoNumber, true);
	digest.heroStatus = result.status;
	digest.heroMessage = result.message;
	EXPECT_FALSE(gbRunGame);
	CurrentDigest = nullptr;
	CurrentProbe = nullptr;
	probe.reset();
	gbRunGame = false;
	init_cleanup();
	LuaShutdown();
	SDL_Quit();
	paths::SetPrefPath(savedPrefPath);
	paths::SetConfigPath(savedConfigPath);
	fs::remove_all(replayFolder, copyError);
	return digest;
}

} // namespace

enum class FixtureDataClass { Distributable, OwnedData, Unavailable };

struct TimedemoFixture {
	std::string_view id;
	std::string_view family;
	std::string_view movementDirection;
	std::string_view category;
	FixtureDataClass dataClass;
	std::string_view simulationHashBoundary;
};

// Deterministic simulation coverage is separate from renderer-only evidence.
constexpr TimedemoFixture RequiredTimedemoFixtures[] {
	{ "town-north", "town", "north", "movement", FixtureDataClass::Unavailable, "end" },
	{ "town-east", "town", "east", "movement", FixtureDataClass::Unavailable, "end" },
	{ "town-south", "town", "south", "movement", FixtureDataClass::Unavailable, "end" },
	{ "town-west", "town", "west", "movement", FixtureDataClass::Unavailable, "end" },
	{ "dungeon-north", "dungeon", "north", "movement", FixtureDataClass::Unavailable, "end" },
	{ "dungeon-east", "dungeon", "east", "movement", FixtureDataClass::Unavailable, "end" },
	{ "dungeon-south", "dungeon", "south", "movement", FixtureDataClass::Unavailable, "end" },
	{ "dungeon-west", "dungeon", "west", "movement", FixtureDataClass::Unavailable, "end" },
	{ "occlusion", "dungeon", "none", "occlusion", FixtureDataClass::Unavailable, "end" },
	{ "palette-effects", "town", "none", "palette", FixtureDataClass::Unavailable, "end" },
	{ "hud", "town", "none", "hud", FixtureDataClass::Unavailable, "end" },
	{ "movies", "town", "none", "movie", FixtureDataClass::OwnedData, "not-applicable" },
	{ "hellfire", "dungeon", "none", "hellfire", FixtureDataClass::OwnedData, "end" },
};

TEST(Timedemo, FixtureInventoryDefinesRequiredCoverage)
{
	EXPECT_EQ(std::size(RequiredTimedemoFixtures), 13U);
	for (const TimedemoFixture &fixture : RequiredTimedemoFixtures) {
		SCOPED_TRACE(fixture.id);
		EXPECT_FALSE(fixture.family.empty());
		EXPECT_FALSE(fixture.category.empty());
		EXPECT_FALSE(fixture.simulationHashBoundary.empty());
	}
}

TEST(Timedemo, WarriorLevel1to2)
{
	const ReplayDigest digest = RunTimedemo("WarriorLevel1to2", HybridOptionScenarios[0]);
	if (!digest.dataAvailable) {
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
	}
	ASSERT_EQ(digest.heroStatus, HeroCompareResult::Same) << digest.heroMessage;
}

/**
 * @brief Toggling the hybrid presentation options during the WarriorLevel1to2 replay
 * leaves the canonical simulation state unchanged.
 *
 * Every scenario replays the same demo; the canonical state is hashed at each Lua game event the replay fires
 * (GameStart plus every monster / player damage and experience event) and the end state is checked through the
 * hero reference save. The "toggled" scenario flips both options at every one of those events, so the options
 * change value repeatedly while the replay is in flight.
 *
 * After each sample HybridFrameProbe runs the hybrid frame latch, Pump and a sprite capture with the headless
 * gates forced open, so the hybrid code actually executes in the "on", "on-flat" and "toggled" runs and not in
 * the "off" run; the per-scenario probe counters below assert that it did.
 */
TEST(CanonicalTickState, HybridOptionsDoNotAffectTheReplay)
{
	const bool savedLighting = *GetOptions().Graphics.hybridLighting;
	const bool savedNormalMaps = *GetOptions().Graphics.hybridNormalMaps;

	std::vector<ReplayDigest> digests;
	for (const HybridOptionScenario &scenario : HybridOptionScenarios) {
		SCOPED_TRACE(scenario.name);
		digests.push_back(RunTimedemo("WarriorLevel1to2", scenario));
		const ReplayDigest &digest = digests.back();
		if (!digest.dataAvailable) {
			GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
		}
		EXPECT_EQ(digest.heroStatus, HeroCompareResult::Same) << digest.heroMessage;
		EXPECT_FALSE(digest.eventHashes.empty()) << "GameStart must fire during the replay";
		if (scenario.toggleOnEvents) {
			EXPECT_EQ(digest.toggles, digest.eventHashes.size());
			EXPECT_GE(digest.toggles, 1U);
		} else {
			EXPECT_EQ(digest.toggles, 0U);
		}
		// The probe must have done hybrid work exactly when the options ask for it.
		const bool lightingEverOn = scenario.lightingAtStart || scenario.toggleOnEvents;
		if (!lightingEverOn) {
			EXPECT_EQ(digest.hybridFrames, 0U) << "hybridLighting off must keep the latch off";
			EXPECT_EQ(digest.flatCardPixels + digest.generatedCardPixels, 0U);
		} else {
			EXPECT_GE(digest.hybridFrames, 1U) << "the probe never latched the hybrid mode on";
			EXPECT_GT(digest.flatCardPixels + digest.generatedCardPixels, 0U) << "the probe captured no card pixels";
		}
		if (scenario.lightingAtStart && !scenario.toggleOnEvents) {
			EXPECT_EQ(digest.hybridFrames, digest.eventHashes.size()) << "every event of this replay is in the dungeon";
			if (scenario.normalMapsAtStart) {
				EXPECT_GT(digest.generatedCardPixels, 0U) << "normal maps on must generate card normals";
				EXPECT_GT(digest.maxNormalPlaneEntries, 0U);
			} else {
				EXPECT_EQ(digest.generatedCardPixels, 0U) << "flat tier must write only the flat card byte";
				EXPECT_EQ(digest.maxNormalPlaneEntries, 0U);
			}
		}
		if (scenario.toggleOnEvents)
			EXPECT_GT(digest.generatedCardPixels, 0U) << "the toggled run never generated a normal plane";
		std::cout << "scenario " << scenario.name << ": " << digest.eventHashes.size() << " canonical samples, " << digest.toggles << " toggles, "
		          << digest.hybridFrames << " hybrid probe frames, card pixels " << digest.generatedCardPixels << " generated / " << digest.flatCardPixels
		          << " flat, max normal-plane entries " << digest.maxNormalPlaneEntries << "\n";
	}

	const ReplayDigest &reference = digests.front();
	for (size_t i = 1; i < digests.size(); ++i) {
		SCOPED_TRACE(HybridOptionScenarios[i].name);
		ASSERT_EQ(digests[i].eventHashes.size(), reference.eventHashes.size()) << "the replay fired a different number of game events";
		for (size_t sample = 0; sample < reference.eventHashes.size(); ++sample)
			EXPECT_EQ(digests[i].eventHashes[sample], reference.eventHashes[sample]) << "canonical state differs at game event " << sample;
	}

	GetOptions().Graphics.hybridLighting.SetValue(savedLighting);
	GetOptions().Graphics.hybridNormalMaps.SetValue(savedNormalMaps);
}
