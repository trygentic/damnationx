/**
 * @file fpv/fpv.cpp
 *
 * First-person view gameplay: activation, camera, mouselook, WASD movement,
 * crosshair picking into the classic cursor globals and FPV actions.
 */
#include "fpv/fpv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>

// SDL2 spellings of the SDL3 names used below.
#define SDL_KMOD_ALT KMOD_ALT
#define SDLK_W SDLK_w
#define SDLK_S SDLK_s
#define SDLK_A SDLK_a
#define SDLK_D SDLK_d
#endif

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "controls/controller_motion.h"
#include "cursor.h"
#include "diablo.h"
#include "doom.h"
#include "engine/freeform_movement.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/ui_scale.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "fpv/fpv_math.hpp"
#include "fpv/fpv_state.hpp"
#include "fpv/fpv_viewmodel_inputs.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "headless_mode.hpp"
#include "help.h"
#include "inv.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "minitext.h"
#include "monster.h"
#include "msg.h"
#include "objects.h"
#include "options.h"
#include "panels/partypanel.hpp"
#include "panels/quest_log.hpp"
#include "render3d/world_renderer.hpp"
#include "player.h"
#include "qol/chatlog.h"
#include "qol/itemlabels.h"
#include "qol/stash.h"
#include "qol/visual_store.h"
#include "spells.h"
#include "stores.h"
#include "towners.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {
namespace fpv {

namespace {

constexpr float kMaxPitch = 75.0F * kPi / 180.0F;
constexpr float kCameraTimeConstant = 0.035F; // seconds
constexpr float kHeadBobAmplitude = 0.012F;
/** Object::_oVar4 of a closed door (DOOR_CLOSED in objects.cpp). */
constexpr int DoorClosed = 0;

struct MoveKeys {
	bool forward = false;
	bool back = false;
	bool left = false;
	bool right = false;
};

struct State {
	bool active = false;
	bool cameraValid = false;
	CameraState camera;
	float yaw = 0.0F;
	float pitch = 0.0F;
	float bobPhase = 0.0F;
	uint32_t lastFrameMs = 0;

	bool mouseLook = false;
	bool relativeMouse = false;
	int skipMotionEvents = 0;

	MoveKeys keys;
	bool moving = false;
	Point lastGoal { -1, -1 };
	uint32_t lastSendMs = 0;

	PickResult pick;
	CrosshairTarget target;

	PLR_MODE lastMode = PM_STAND;
	std::vector<std::pair<unsigned, int>> attackSnapshot;

	int levelKey = -1;
};

State S;
HudState Hud;
ViewmodelInputs Viewmodel;
ViewmodelAvailableQuery ViewmodelQuery = nullptr;
float LastFrameYaw = 0.0F;
float LastFramePitch = 0.0F;
float LastTargetX = 0.0F;
float LastTargetZ = 0.0F;

int CurrentLevelKey()
{
	return (static_cast<int>(leveltype) << 16) | (static_cast<int>(currlevel) << 8) | (setlevel ? 0x80 | static_cast<int>(setlvlnum) : 0);
}

std::string LevelBannerText()
{
	if (setlevel)
		return "QUEST LEVEL";
	switch (leveltype) {
	case DTYPE_TOWN:
		return "TRISTRAM";
	case DTYPE_CATHEDRAL:
		return StrCat("CATHEDRAL LEVEL ", currlevel);
	case DTYPE_CATACOMBS:
		return StrCat("CATACOMBS LEVEL ", currlevel);
	case DTYPE_CAVES:
		return StrCat("CAVES LEVEL ", currlevel);
	case DTYPE_HELL:
		return StrCat("HELL LEVEL ", currlevel);
	case DTYPE_NEST:
		return StrCat("HIVE LEVEL ", currlevel - 16);
	case DTYPE_CRYPT:
		return StrCat("CRYPT LEVEL ", currlevel - 20);
	default:
		return StrCat("LEVEL ", currlevel);
	}
}

/** A panel, menu or text box wants the mouse cursor. */
bool UiWantsCursor()
{
	return invflag || CharFlag || SpellbookFlag || QuestLogIsOpen || IsStashOpen || IsVisualStoreOpen
	    || IsPlayerInStore() || gmenu_is_active() || ChatFlag || SpellSelectFlag || qtextflag || HelpFlag
	    || ChatLogFlag || DropGoldFlag || IsWithdrawGoldOpen || DoomFlag || MyPlayerIsDead || PauseMode != 0
	    || (PartySidePanelOpen && *GetOptions().Gameplay.showMultiplayerPartyInfo && gbIsMultiplayer);
}

bool AltHeld()
{
	return (SDL_GetModState() & SDL_KMOD_ALT) != 0;
}

bool WindowFocused()
{
#ifndef USE_SDL1
	return ghMainWnd != nullptr && SDL_GetKeyboardFocus() == ghMainWnd;
#else
	return true;
#endif
}

void SetRelativeMouse(bool on)
{
	if (S.relativeMouse == on)
		return;
	S.relativeMouse = on;
#ifdef USE_SDL3
	if (ghMainWnd != nullptr)
		SDL_SetWindowRelativeMouseMode(ghMainWnd, on);
#elif !defined(USE_SDL1)
	SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
#endif
}

Point ViewCentreScreen()
{
	return { ViewWidth() / 2, ViewHeight() / 2 };
}

/** The player's drawn position in world space (x, z) = tile + 0.5 + sub-tile glide. */
void PlayerWorldPosition(const Player &player, float &x, float &z)
{
	const SubTileDisplacement offset = player.position.GetDrawnSubTileOffset(player.isWalking(), player._pdir, player.AnimInfo);
	x = static_cast<float>(player.position.tile.x) + 0.5F + static_cast<float>(offset.deltaX) / static_cast<float>(SubTileUnits);
	z = static_cast<float>(player.position.tile.y) + 0.5F + static_cast<float>(offset.deltaY) / static_cast<float>(SubTileUnits);
}

void MonsterWorldPosition(const Monster &monster, float &x, float &z)
{
	const SubTileDisplacement offset = monster.position.GetDrawnSubTileOffset(monster.isWalking(), monster.direction, monster.animInfo);
	x = static_cast<float>(monster.position.tile.x) + 0.5F + static_cast<float>(offset.deltaX) / static_cast<float>(SubTileUnits);
	z = static_cast<float>(monster.position.tile.y) + 0.5F + static_cast<float>(offset.deltaY) / static_cast<float>(SubTileUnits);
}

bool CanPlayerTakeFpvAction()
{
	return !MyPlayerIsDead && PauseMode != 2 && !MyPlayer->_pInvincible;
}

void OnEnter()
{
	const Player &player = *MyPlayer;
	S.yaw = DirectionToYaw(player._pdir);
	S.pitch = 0.0F;
	S.cameraValid = false;
	LastFrameYaw = S.yaw;
	LastFramePitch = 0.0F;
	S.keys = {};
	S.moving = false;
	S.lastGoal = { -1, -1 };
	Hud.helpUntil = NowMs() + 7000;
	Hud.bannerText = LevelBannerText();
	Hud.bannerUntil = NowMs() + 3000;
	S.levelKey = CurrentLevelKey();
	LogVerbose("FPV: enter, yaw {:.2f} from facing {}", S.yaw, static_cast<int>(player._pdir));
}

void OnExit()
{
	SetRelativeMouse(false);
	if (S.mouseLook)
		SetCursorPos(ScreenToUi(ViewCentreScreen()));
	S.mouseLook = false;
	S.keys = {};
	S.moving = false;
	S.target = {};
	S.pick = {};
	Player &player = *MyPlayer;
	if (player._pmode == PM_STAND && player.CanChangeAction())
		player._pdir = YawToDirection(S.yaw);
	LogVerbose("FPV: exit, facing {} from yaw {:.2f}", static_cast<int>(player._pdir), S.yaw);
}

/** Runs the activation edge and keeps the mouse mode in sync. */
void SyncState()
{
	const bool active = IsActive();
	if (active != S.active) {
		S.active = active;
		if (active)
			OnEnter();
		else
			OnExit();
	}
	if (!active)
		return;

	const int levelKey = CurrentLevelKey();
	if (levelKey != S.levelKey) {
		S.levelKey = levelKey;
		S.cameraValid = false;
		S.lastGoal = { -1, -1 };
		S.yaw = DirectionToYaw(MyPlayer->_pdir);
		Hud.bannerText = LevelBannerText();
		Hud.bannerUntil = NowMs() + 3000;
	}

	const bool wantLook = !UiWantsCursor() && !AltHeld() && WindowFocused();
	if (wantLook != S.mouseLook) {
		S.mouseLook = wantLook;
		if (wantLook) {
			SetRelativeMouse(ControlMode == ControlTypes::KeyboardAndMouse);
			S.skipMotionEvents = 2;
		} else {
			SetRelativeMouse(false);
			SetCursorPos(ScreenToUi(ViewCentreScreen()));
		}
	}
	if (S.mouseLook) {
		if (S.relativeMouse != (ControlMode == ControlTypes::KeyboardAndMouse))
			SetRelativeMouse(ControlMode == ControlTypes::KeyboardAndMouse);
		MousePosition = ScreenToUi(ViewCentreScreen());
	}
	if (!WindowFocused())
		S.keys = {};
}

void AddLook(float dxRadians, float dyRadians)
{
	S.yaw = WrapAngle(S.yaw - dxRadians);
	const float invert = *GetOptions().Gameplay.fpvInvertMouseY ? -1.0F : 1.0F;
	S.pitch = std::clamp(S.pitch - dyRadians * invert, -kMaxPitch, kMaxPitch);
}

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

bool CanStepBetween(const Player &player, Point from, Point to)
{
	if (!InDungeonBounds(to) || !PosOkPlayer(player, to))
		return false;
	const int dx = to.x - from.x;
	const int dy = to.y - from.y;
	if (dx != 0 && dy != 0) {
		// Diagonal steps may not cut a solid corner (same rule as the path finder).
		if (IsTileSolid({ from.x + dx, from.y }) || IsTileSolid({ from.x, from.y + dy }))
			return false;
	}
	return true;
}

/** Furthest tile reachable in a straight line from `from` towards `goal`, or `from`. */
Point WalkLine(const Player &player, Point from, Point goal)
{
	const int steps = StepsBetween(from, goal);
	Point last = from;
	for (int k = 1; k <= steps; ++k) {
		const Point next = LinePoint(from, goal, k);
		if (!CanStepBetween(player, last, next))
			break;
		last = next;
	}
	return last;
}

std::optional<Point> FindMoveGoal(const Player &player, float dirX, float dirZ)
{
	const Point from = player.position.future;
	float px;
	float pz;
	PlayerWorldPosition(player, px, pz);

	constexpr float LookAhead = 3.0F;
	const Point target { static_cast<int>(std::floor(px + dirX * LookAhead)), static_cast<int>(std::floor(pz + dirZ * LookAhead)) };
	if (target != from) {
		const Point reach = WalkLine(player, from, target);
		if (reach != from)
			return reach;
	}

	// Blocked straight ahead: slide along walls with the step directions closest to the wanted one.
	std::array<std::pair<float, Direction>, 8> candidates;
	for (int d = 0; d < 8; ++d) {
		const Direction dir = static_cast<Direction>(d);
		const SubTileDisplacement step = TileStep(dir);
		const float sx = static_cast<float>(step.deltaX);
		const float sz = static_cast<float>(step.deltaY);
		const float len = std::sqrt(sx * sx + sz * sz);
		candidates[d] = { (sx * dirX + sz * dirZ) / len, dir };
	}
	std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	for (const auto &[score, dir] : candidates) {
		if (score < 0.35F)
			break;
		const Point first = from + dir;
		if (!CanStepBetween(player, from, first))
			continue;
		const Point second = first + dir;
		return CanStepBetween(player, first, second) ? second : first;
	}
	return std::nullopt;
}

void StopMoving(Player &player)
{
	if (!S.moving)
		return;
	S.moving = false;
	S.lastGoal = { -1, -1 };
	if (player.walkpath[0] != WALK_NONE && (player.destAction == ACTION_NONE || player.destAction == ACTION_WALK))
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, player.position.future);
}

void UpdateMovement()
{
	Player &player = *MyPlayer;
	float forward = (S.keys.forward ? 1.0F : 0.0F) - (S.keys.back ? 1.0F : 0.0F);
	float strafe = (S.keys.right ? 1.0F : 0.0F) - (S.keys.left ? 1.0F : 0.0F);
	if (ControlMode == ControlTypes::Gamepad) {
		forward += leftStickY;
		strafe += leftStickX;
	}

	const bool blocked = MyPlayerIsDead || PauseMode != 0 || (!gbIsMultiplayer && gmenu_is_active()) || IsPlayerInStore()
	    || qtextflag || player._pInvincible;
	if (blocked || std::abs(forward) + std::abs(strafe) < 0.25F) {
		StopMoving(player);
		return;
	}

	// Never cancel an attack, cast or a targeted action (walk-to-attack, operate, pick up) the player asked for.
	if (IsAnyOf(player._pmode, PM_ATTACK, PM_RATTACK, PM_SPELL, PM_BLOCK, PM_GOTHIT, PM_DEATH))
		return;
	if (player.destAction != ACTION_NONE && player.destAction != ACTION_WALK)
		return;

	float fwdX;
	float fwdZ;
	float rightX;
	float rightZ;
	GroundAxes(S.yaw, fwdX, fwdZ, rightX, rightZ);
	float dirX = fwdX * forward + rightX * strafe;
	float dirZ = fwdZ * forward + rightZ * strafe;
	const float len = std::sqrt(dirX * dirX + dirZ * dirZ);
	if (len < 1e-4F) {
		StopMoving(player);
		return;
	}
	dirX /= len;
	dirZ /= len;

	const std::optional<Point> goal = FindMoveGoal(player, dirX, dirZ);
	if (!goal) {
		StopMoving(player);
		return;
	}

	const uint32_t now = NowMs();
	if (*goal != S.lastGoal || now - S.lastSendMs > 250) {
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, *goal);
		S.lastGoal = *goal;
		S.lastSendMs = now;
	}
	S.moving = true;
}

// ---------------------------------------------------------------------------
// Combat feedback (slash overlay / hit marker)
// ---------------------------------------------------------------------------

void UpdateCombatFeedback()
{
	const Player &player = *MyPlayer;
	const PLR_MODE mode = player._pmode;
	if (IsAnyOf(mode, PM_ATTACK, PM_RATTACK) && !IsAnyOf(S.lastMode, PM_ATTACK, PM_RATTACK)) {
		++Viewmodel.swingCount;
		Viewmodel.swingStartMs = NowMs();
		Viewmodel.swingMirrored = !Viewmodel.swingMirrored;
	}
	if (mode == PM_ATTACK && S.lastMode != PM_ATTACK) {
		if (!player.UsesRangedWeapon()) {
			Hud.slashActive = true;
			Hud.slashStart = NowMs();
			Hud.slashMirrored = !Hud.slashMirrored;
		}
		S.attackSnapshot.clear();
		if (leveltype != DTYPE_TOWN) {
			for (size_t i = 0; i < ActiveMonsterCount; ++i) {
				const unsigned id = ActiveMonsters[i];
				const Monster &monster = Monsters[id];
				if (monster.position.tile.WalkingDistance(player.position.tile) <= 2)
					S.attackSnapshot.emplace_back(id, monster.hitPoints);
			}
		}
	}
	if (mode == PM_ATTACK) {
		for (auto &[id, hp] : S.attackSnapshot) {
			const Monster &monster = Monsters[id];
			if (monster.hitPoints < hp) {
				Hud.hitMarkerUntil = NowMs() + 4 * 1000 / std::max(1, static_cast<int>(*GetOptions().Gameplay.tickRate));
				hp = monster.hitPoints;
			}
		}
	} else {
		S.attackSnapshot.clear();
	}
	S.lastMode = mode;
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

bool IsAttackSpell(SpellID id)
{
	return TargetsMonster(id)
	    || IsAnyOf(id, SpellID::Firebolt, SpellID::ChargedBolt, SpellID::ChainLightning, SpellID::HolyBolt, SpellID::Elemental,
	        SpellID::BloodStar, SpellID::BoneSpirit, SpellID::Apocalypse, SpellID::Nova, SpellID::Immolation, SpellID::LightningWall,
	        SpellID::RingOfFire, SpellID::Search, SpellID::RuneOfFire, SpellID::RuneOfLight, SpellID::RuneOfNova, SpellID::RuneOfImmolation);
}

bool HasLongAttackRange(const Player &player)
{
	if (player.UsesRangedWeapon())
		return true;
	return IsValidSpell(player._pRSpell) && player._pRSplType != SpellType::Invalid && IsAttackSpell(player._pRSpell);
}

float HorizontalDistanceToPlayer(float x, float z)
{
	float px;
	float pz;
	PlayerWorldPosition(*MyPlayer, px, pz);
	return std::hypot(x - px, z - pz);
}

struct Candidate {
	TargetKind kind = TargetKind::None;
	int id = -1;
	float t = std::numeric_limits<float>::max();
	Point tile {};
	float worldX = 0.0F;
	float worldZ = 0.0F;
};

void Consider(Candidate &best, const std::optional<float> &t, TargetKind kind, int id, Point tile, float wx, float wz)
{
	if (t && *t < best.t) {
		best.kind = kind;
		best.id = id;
		best.t = *t;
		best.tile = tile;
		best.worldX = wx;
		best.worldZ = wz;
	}
}

float SpriteHeight(const AnimationInfo &anim, float fallback)
{
	if (!anim.sprites)
		return fallback;
	const int h = anim.currentSprite().height();
	return std::clamp(static_cast<float>(h) / 128.0F, 0.45F, 1.8F);
}

void ConsiderObject(const Ray &ray, Candidate &best, Candidate &passThrough, const Object &object, int index)
{
	if (!object.canInteractWith())
		return;
	const float x = static_cast<float>(object.position.x);
	const float z = static_cast<float>(object.position.y);
	const float cx = x + 0.5F;
	const float cz = z + 0.5F;
	if (object.isDoor()) {
		const std::optional<float> t = RayBox(ray, { x + 0.05F, 0.0F, z + 0.05F }, { x + 0.95F, kWallHeight, z + 0.95F });
		if (object._oVar4 != DoorClosed) // open or blocked (open, something in the doorway): let the ray through
			Consider(passThrough, t, TargetKind::Door, index, object.position, cx, cz);
		else
			Consider(best, t, TargetKind::Door, index, object.position, cx, cz);
		return;
	}
	std::optional<float> t;
	if (object.IsBarrel()) {
		t = RayCylinder(ray, cx, cz, 0.3F, 0.0F, 0.6F);
	} else if (object.IsChest()) {
		t = RayBox(ray, { x + 0.2F, 0.0F, z + 0.2F }, { x + 0.8F, 0.45F, z + 0.8F });
	} else if (object._otype == OBJ_SARC) {
		t = RayBox(ray, { x + 0.05F, 0.0F, z + 0.05F }, { x + 0.95F, 0.55F, z + 0.95F });
	} else {
		t = RayBox(ray, { x + 0.15F, 0.0F, z + 0.15F }, { x + 0.85F, 0.8F, z + 0.85F });
	}
	Consider(best, t, TargetKind::Object, index, object.position, cx, cz);
}

/** Checks whether a tile triggers a level change / portal (CheckTrigForce) without leaving side effects on cursPosition. */
std::optional<Point> TriggerAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return std::nullopt;
	const Point saved = cursPosition;
	cursPosition = tile;
	CheckTrigForce();
	std::optional<Point> result;
	if (trigflag)
		result = cursPosition;
	trigflag = false;
	cursPosition = saved;
	return result;
}

PickResult Pick(const Ray &ray)
{
	PickResult result;
	const Player &myPlayer = *MyPlayer;

	// Walls and the floor.
	std::optional<Point> floorTile;
	float floorT = kPickRange;
	const GridHit wall = TraceGrid(
	    ray, kPickRange,
	    [](int x, int y) {
		    const Point p { x, y };
		    return !InDungeonBounds(p) || IsTileSolid(p);
	    },
	    [&](int x, int y, float tEnter, float tExit) {
		    if (floorTile)
			    return;
		    const std::optional<float> t = RayFloor(ray);
		    if (t && *t >= tEnter && *t <= tExit) {
			    floorTile = Point { x, y };
			    floorT = *t;
		    }
	    });
	const float wallT = wall.hit ? wall.distance : kPickRange;
	result.hitWorld = wall.hit || floorTile.has_value();

	Candidate best;
	Candidate passThrough;

	if (leveltype != DTYPE_TOWN) {
		if (IsNoneOf(pcurs, CURSOR_HEALOTHER, CURSOR_RESURRECT)) {
			for (size_t i = 0; i < ActiveMonsterCount; ++i) {
				const unsigned id = ActiveMonsters[i];
				const Monster &monster = Monsters[id];
				if (monster.hasNoLife() || (monster.flags & MFLAG_HIDDEN) != 0 || monster.isPlayerMinion())
					continue;
				if (!IsTileLit(monster.position.tile))
					continue;
				float mx;
				float mz;
				MonsterWorldPosition(monster, mx, mz);
				const float height = SpriteHeight(monster.animInfo, 0.8F);
				const float radius = std::clamp(height * 0.4F, 0.3F, 0.6F);
				Consider(best, RayCylinder(ray, mx, mz, radius, 0.0F, height), TargetKind::Monster, static_cast<int>(id), monster.position.tile, mx, mz);
			}
		}
	} else {
		for (size_t i = 0; i < GetNumTowners(); ++i) {
			const Towner &towner = Towners[i];
			const float tx = static_cast<float>(towner.position.x) + 0.5F;
			const float tz = static_cast<float>(towner.position.y) + 0.5F;
			Consider(best, RayCylinder(ray, tx, tz, 0.35F, 0.0F, 0.8F), TargetKind::Towner, static_cast<int>(i), towner.position, tx, tz);
		}
	}

	for (const Player &player : Players) {
		if (&player == MyPlayer || !player.plractive || !player.isOnActiveLevel())
			continue;
		if (player.hasNoLife() && pcurs != CURSOR_RESURRECT)
			continue;
		float px;
		float pz;
		PlayerWorldPosition(player, px, pz);
		const float height = player.hasNoLife() ? 0.25F : 0.7F;
		Consider(best, RayCylinder(ray, px, pz, 0.32F, 0.0F, height), TargetKind::Player, static_cast<int>(player.getId()), player.position.tile, px, pz);
	}

	if (pcurs != CURSOR_IDENTIFY) {
		for (uint8_t i = 0; i < ActiveItemCount; ++i) {
			const int id = ActiveItems[i];
			const Item &item = Items[id];
			if (item.selectionRegion == SelectionRegion::None)
				continue;
			const float x = static_cast<float>(item.position.x);
			const float z = static_cast<float>(item.position.y);
			Consider(best, RayBox(ray, { x + 0.2F, 0.0F, z + 0.2F }, { x + 0.8F, 0.3F, z + 0.8F }), TargetKind::Item, id, item.position, x + 0.5F, z + 0.5F);
		}

		for (int i = 0; i < ActiveObjectCount; ++i) {
			const int index = ActiveObjects[i];
			ConsiderObject(ray, best, passThrough, Objects[index], index);
		}
	}

	if (best.t > wallT + 0.05F)
		best = {};
	if (best.kind == TargetKind::None && passThrough.t <= wallT + 0.05F)
		best = passThrough;

	if (best.kind != TargetKind::None) {
		result.kind = best.kind;
		result.id = best.id;
		result.distance = best.t;
		result.tile = best.tile;
		result.playerDistance = HorizontalDistanceToPlayer(best.worldX, best.worldZ);
		return result;
	}

	// Triggers (stairs, town entrances, portals): the floor tile under the crosshair, or the wall / stair tile hit.
	std::array<std::optional<Point>, 3> triggerTiles {};
	if (floorTile && floorT <= wallT) {
		triggerTiles[0] = floorTile;
	} else if (wall.hit) {
		triggerTiles[0] = Point { wall.tileX, wall.tileY };
		triggerTiles[1] = Point { wall.lastOpenX, wall.lastOpenY };
	}
	for (const std::optional<Point> &tile : triggerTiles) {
		if (!tile)
			continue;
		if (const std::optional<Point> trigger = TriggerAt(*tile)) {
			result.kind = TargetKind::Trigger;
			result.id = -1;
			result.distance = floorTile && floorT <= wallT ? floorT : wallT;
			result.tile = *tile;
			result.playerDistance = HorizontalDistanceToPlayer(static_cast<float>(trigger->x) + 0.5F, static_cast<float>(trigger->y) + 0.5F);
			return result;
		}
	}

	// Nothing selectable: the floor tile under the crosshair, else the last open tile before the wall / range.
	if (floorTile && floorT <= wallT) {
		result.tile = *floorTile;
		result.distance = floorT;
	} else {
		result.tile = { wall.lastOpenX, wall.lastOpenY };
		result.distance = wallT;
	}
	(void)myPlayer;
	return result;
}

void UpdateTargetInfo()
{
	CrosshairTarget target;
	target.kind = S.pick.kind;
	target.id = S.pick.id;
	target.distance = S.pick.distance;
	const Player &player = *MyPlayer;
	switch (S.pick.kind) {
	case TargetKind::Monster:
	case TargetKind::Player: {
		const Point targetTile = S.pick.tile;
		if (HasLongAttackRange(player))
			target.inRange = S.pick.playerDistance <= kRangedAttackRange;
		else
			target.inRange = std::max(std::abs(targetTile.x - player.position.tile.x), std::abs(targetTile.y - player.position.tile.y)) <= 1
			    || S.pick.playerDistance <= 1.5F;
		break;
	}
	case TargetKind::None:
		break;
	default:
		target.inRange = S.pick.playerDistance <= kUseRange;
		break;
	}
	if (target.kind != S.target.kind || target.id != S.target.id || target.inRange != S.target.inRange) {
		LogVerbose("FPV target: kind {} id {} tile ({}, {}) distance {:.2f} player distance {:.2f} inRange {}",
		    static_cast<int>(target.kind), target.id, S.pick.tile.x, S.pick.tile.y, target.distance, S.pick.playerDistance, target.inRange);
	}
	S.target = target;
}

} // namespace

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

bool IsActive()
{
	return *GetOptions().Graphics.firstPersonView && !HeadlessMode && gbRunGame && MyPlayer != nullptr && WebGpuIsReady();
}

const CameraState &GetCamera()
{
	return S.camera;
}

const CrosshairTarget &GetCrosshairTarget()
{
	return S.target;
}

// ---------------------------------------------------------------------------
// Internal shared state
// ---------------------------------------------------------------------------

const PickResult &GetPick()
{
	return S.pick;
}

HudState &GetHud()
{
	return Hud;
}

int ViewWidth()
{
	return std::max<int>(gnScreenWidth, 1);
}

int ViewHeight()
{
	return std::max<int>(gnViewportHeight, 1);
}

uint32_t NowMs()
{
	return static_cast<uint32_t>(SDL_GetTicks());
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

bool IsMouseLook()
{
	return S.active && S.mouseLook;
}

bool SkipIsometricWorld()
{
	return IsActive();
}

void OnGameEnd()
{
	SetRelativeMouse(false);
	S = State {};
	Hud = HudState {};
	Viewmodel = ViewmodelInputs {};
}

const ViewmodelInputs &GetViewmodelInputs()
{
	return Viewmodel;
}

bool ViewmodelAvailable()
{
	return ViewmodelQuery != nullptr && ViewmodelQuery();
}

void SetViewmodelAvailableQuery(ViewmodelAvailableQuery query)
{
	ViewmodelQuery = query;
}

bool ShowSlashOverlay()
{
	return !*GetOptions().Graphics.fpvWeapon || !ViewmodelAvailable();
}

bool OwnsMovement()
{
	return S.active;
}

void ToggleFirstPerson()
{
	auto &option = GetOptions().Graphics.firstPersonView;
	option.SetValue(!*option);
	SyncState();
}

void ProcessInput()
{
	SyncState();
	if (!S.active)
		return;
	UpdateMovement();
	UpdateCombatFeedback();
}

void UpdateFrame()
{
	SyncState();
	const uint32_t now = NowMs();
	const float dt = S.lastFrameMs == 0 ? 0.016F : std::clamp(static_cast<float>(now - S.lastFrameMs) / 1000.0F, 0.0F, 0.1F);
	S.lastFrameMs = now;
	if (!S.active)
		return;

	const Player &player = *MyPlayer;
	float x;
	float z;
	PlayerWorldPosition(player, x, z);

	if (!S.cameraValid || std::hypot(x - S.camera.x, z - S.camera.z) > 2.5F) {
		S.camera.x = x;
		S.camera.z = z;
		S.cameraValid = true;
	} else {
		const float alpha = SmoothingAlpha(dt, kCameraTimeConstant);
		S.camera.x += (x - S.camera.x) * alpha;
		S.camera.z += (z - S.camera.z) * alpha;
	}

	if (player.isWalking())
		S.bobPhase += dt * 9.0F;
	else
		S.bobPhase *= 0.9F;
	// The eye follows the floor of the 3D world (stairs), smoothed.
	static float FloorY = 0.0F;
	FloorY += (render3d::FloorHeightAt(S.camera.x, S.camera.z) - FloorY) * SmoothingAlpha(dt, 0.08F);
	S.camera.y = FloorY + kEyeHeight + kHeadBobAmplitude * std::sin(S.bobPhase);
	S.camera.yaw = S.yaw;
	S.camera.pitch = S.pitch;
	const float aspect = static_cast<float>(ViewWidth()) / static_cast<float>(ViewHeight());
	S.camera.fovYDeg = HorizontalToVerticalFov(static_cast<float>(*GetOptions().Graphics.fpvFieldOfView), aspect);

	// Viewmodel motion inputs (fpv_viewmodel_inputs.hpp).
	{
		const float invDt = dt > 0.0F ? 1.0F / dt : 0.0F;
		float vx = (x - LastTargetX) * invDt;
		float vz = (z - LastTargetZ) * invDt;
		if (std::hypot(x - LastTargetX, z - LastTargetZ) > 2.0F)
			vx = vz = 0.0F; // teleport / level change
		LastTargetX = x;
		LastTargetZ = z;
		constexpr float FullSpeed = 3.0F; // tiles per second, about a walk in the dungeon
		const float speed = std::min(std::hypot(vx, vz) / FullSpeed, 1.0F);
		const float blend = SmoothingAlpha(dt, 0.08F);
		float fwdX;
		float fwdZ;
		float rightX;
		float rightZ;
		GroundAxes(S.yaw, fwdX, fwdZ, rightX, rightZ);
		const float len = std::hypot(vx, vz);
		const float forward = len > 1e-4F ? (vx * fwdX + vz * fwdZ) / len * speed : 0.0F;
		const float strafe = len > 1e-4F ? (vx * rightX + vz * rightZ) / len * speed : 0.0F;
		Viewmodel.moveSpeed += (speed - Viewmodel.moveSpeed) * blend;
		Viewmodel.forward += (forward - Viewmodel.forward) * blend;
		Viewmodel.strafe += (strafe - Viewmodel.strafe) * blend;
		Viewmodel.yawRate = WrapAngle(S.yaw - LastFrameYaw) * invDt;
		Viewmodel.pitchRate = (S.pitch - LastFramePitch) * invDt;
		LastFrameYaw = S.yaw;
		LastFramePitch = S.pitch;
		Viewmodel.bobPhase = S.bobPhase;
		Viewmodel.attacking = IsAnyOf(player._pmode, PM_ATTACK, PM_RATTACK);
		Viewmodel.rangedWeapon = player.UsesRangedWeapon();
		Viewmodel.hitMarkerUntilMs = Hud.hitMarkerUntil;
	}

	if (!Hud.slashActive)
		return;
	if (now - Hud.slashStart > 400)
		Hud.slashActive = false;
}

bool HandleMouseMotion(const SDL_Event &event)
{
	if (!IsMouseLook() || !S.relativeMouse)
		return false;
	if (S.skipMotionEvents > 0) {
		--S.skipMotionEvents;
		return true;
	}
	const float sensitivity = static_cast<float>(*GetOptions().Gameplay.fpvMouseSensitivity);
	const float radiansPerPixel = 0.0022F * sensitivity / 5.0F;
	AddLook(static_cast<float>(event.motion.xrel) * radiansPerPixel, static_cast<float>(event.motion.yrel) * radiansPerPixel);
	return true;
}

bool HandleGamepadLook(float stickX, float stickY)
{
	if (!IsMouseLook())
		return false;
	static uint32_t lastMs = 0;
	const uint32_t now = NowMs();
	const float dt = lastMs == 0 ? 0.016F : std::clamp(static_cast<float>(now - lastMs) / 1000.0F, 0.0F, 0.1F);
	lastMs = now;
	const float sensitivity = static_cast<float>(*GetOptions().Gameplay.fpvMouseSensitivity) / 5.0F;
	AddLook(stickX * 2.5F * sensitivity * dt, -stickY * 1.8F * sensitivity * dt);
	return true;
}

bool HandleKey(SDL_Keycode vkey, bool pressed)
{
	if (!S.active)
		return false;
	if (pressed && (IsChatActive() || ChatFlag || IsPlayerInStore() || gmenu_is_active()))
		return false;
	bool *key = nullptr;
	switch (vkey) {
	case SDLK_W:
		key = &S.keys.forward;
		break;
	case SDLK_S:
		key = &S.keys.back;
		break;
	case SDLK_A:
		key = &S.keys.left;
		break;
	case SDLK_D:
		key = &S.keys.right;
		break;
	default:
		return false;
	}
	*key = pressed;
	return true;
}

Point PickFromScreen(Point screenPosition)
{
	if (!S.cameraValid)
		UpdateFrame();
	const Point origin = IsMouseLook() ? ViewCentreScreen() : screenPosition;
	const Ray ray = ScreenRay(S.camera, static_cast<float>(origin.x) + 0.5F, static_cast<float>(origin.y) + 0.5F, ViewWidth(), ViewHeight());
	S.pick = Pick(ray);
	UpdateTargetInfo();
	Point tile = S.pick.tile;
	tile.x = std::clamp(tile.x, 0, MAXDUNX - 1);
	tile.y = std::clamp(tile.y, 0, MAXDUNY - 1);
	return tile;
}

bool ApplyPickToCursor()
{
	if (pcurs == CURSOR_IDENTIFY)
		return false;
	const PickResult &pick = S.pick;
	switch (pick.kind) {
	case TargetKind::Monster:
	case TargetKind::Towner:
		pcursmonst = pick.id;
		cursPosition = pick.tile;
		return true;
	case TargetKind::Player:
		PlayerUnderCursor = &Players[pick.id];
		cursPosition = pick.tile;
		return true;
	case TargetKind::Item:
		pcursitem = static_cast<int8_t>(pick.id);
		cursPosition = pick.tile;
		return true;
	case TargetKind::Object:
	case TargetKind::Door:
		ObjectUnderCursor = &Objects[pick.id];
		cursPosition = pick.tile;
		return true;
	default:
		return false;
	}
}

bool HandleWorldClick(bool shift)
{
	if (!IsMouseLook())
		return false;
	if (pcursitem != -1 || ObjectUnderCursor != nullptr || pcursmonst != -1 || PlayerUnderCursor != nullptr)
		return false;
	if (leveltype == DTYPE_TOWN)
		return true; // no click-to-walk in first person: WASD moves
	Player &player = *MyPlayer;
	LastPlayerAction = PlayerActionType::Attack;
	if (player.UsesRangedWeapon()) {
		NetSendCmdLoc(MyPlayerId, true, CMD_RATTACKXY, cursPosition);
	} else {
		const Point front = player.position.tile + YawToDirection(S.yaw);
		NetSendCmdLoc(MyPlayerId, true, CMD_SATTACKXY, InDungeonBounds(front) ? front : cursPosition);
	}
	(void)shift;
	return true;
}

void Interact()
{
	if (!S.active || !CanPlayerTakeFpvAction())
		return;
	if (S.target.kind == TargetKind::None || !S.target.inRange)
		return;
	switch (S.target.kind) {
	case TargetKind::Item:
		if (pcursitem != -1 && pcurs == CURSOR_HAND)
			NetSendCmdLocParam1(true, invflag ? CMD_GOTOGETITEM : CMD_GOTOAGETITEM, cursPosition, pcursitem);
		break;
	case TargetKind::Object:
	case TargetKind::Door:
		if (ObjectUnderCursor != nullptr && !ObjectUnderCursor->IsDisabled()) {
			LastPlayerAction = PlayerActionType::OperateObject;
			NetSendCmdLoc(MyPlayerId, true, pcurs == CURSOR_DISARM ? CMD_DISARMXY : CMD_OPOBJXY, cursPosition);
		}
		break;
	case TargetKind::Towner:
		if (pcursmonst != -1)
			NetSendCmdLocParam1(true, CMD_TALKXY, cursPosition, pcursmonst);
		break;
	case TargetKind::Monster:
		if (pcursmonst != -1 && CanTalkToMonst(Monsters[pcursmonst]))
			NetSendCmdParam1(true, CMD_ATTACKID, pcursmonst);
		break;
	case TargetKind::Trigger:
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, cursPosition);
		break;
	default:
		break;
	}
}

void OnBeltItemUsed()
{
	if (!S.active || pcurs != CURSOR_TELEPORT)
		return;
	// Scrolls cast at once at what the crosshair (or the Alt cursor) is on: no targeting cursor in first person.
	TryIconCurs();
}

std::optional<Direction> DropDirection()
{
	if (!S.active)
		return std::nullopt;
	return YawToDirection(S.yaw);
}

void ClearWorldArea(const Surface &out, int viewportHeight)
{
	const int rows = std::min(viewportHeight, out.h());
	for (int y = 0; y < rows; ++y)
		std::fill_n(out.at(0, y), out.w(), kTransparentWorldIndex);
	render3d::BeginUiCoverage(out, rows); // every UI pixel drawn from here on is recorded (black UI stays black)

	// Ground item labels (Alt / "show item labels"): project the visible items into the view.
	if (!IsHighlightingLabelsEnabled() || !S.cameraValid)
		return;
	for (uint8_t i = 0; i < ActiveItemCount; ++i) {
		const int id = ActiveItems[i];
		const Item &item = Items[id];
		if (!IsTileVisible(item.position))
			continue;
		const Vec3f anchor { static_cast<float>(item.position.x) + 0.5F, 0.35F, static_cast<float>(item.position.y) + 0.5F };
		const Vec3f eye { S.camera.x, S.camera.y, S.camera.z };
		const float distance = (anchor - eye).length();
		if (distance > kPickRange)
			continue;
		const std::optional<ScreenPoint> screen = ProjectToScreen(S.camera, anchor, ViewWidth(), ViewHeight());
		if (!screen || screen->x < 0 || screen->x >= static_cast<float>(ViewWidth()) || screen->y < 0 || screen->y >= static_cast<float>(ViewHeight()))
			continue;
		const Ray ray { eye, (anchor - eye).normalized() };
		const GridHit wall = TraceGrid(
		    ray, distance,
		    [](int x, int y) {
			    const Point p { x, y };
			    return !InDungeonBounds(p) || IsTileSolid(p);
		    },
		    [](int, int, float, float) {});
		if (wall.hit)
			continue;
		AddItemToLabelQueueAt(id, ScreenToUi({ static_cast<int>(screen->x), static_cast<int>(screen->y) }));
	}
}

} // namespace fpv
} // namespace devilution
