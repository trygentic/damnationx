/**
 * @file fpv/fpv.cpp
 *
 * First- and third-person gameplay: activation, camera modes (through the view providers of
 * fpv/view_provider.hpp), mouselook with pointer lock, WASD and click-to-move, crosshair picking into the
 * classic cursor globals and the 3D view's actions.
 */
#include "fpv/fpv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>
#include <emscripten/html5.h>
#endif

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
#include "fpv/camera_rig.hpp"
#include "fpv/fpv_math.hpp"
#include "fpv/fpv_state.hpp"
#include "fpv/fpv_viewmodel_inputs.hpp"
#include "fpv/view_provider.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "headless_mode.hpp"
#include "host/host_hooks.hpp"
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

#ifdef __EMSCRIPTEN__
// The browser grants the pointer lock only inside a user event handler, and SDL's own request is deferred to the next
// event after the game loop saw the click. So while the lock is lost, a capturing mousedown listener on the canvas
// asks for it directly, inside the click that takes the mouse back.
EM_JS(void, DxInstallPointerLockClick, (), {
	if (Module.dxPointerLockClick) return;
	var canvas = Module.canvas;
	if (!canvas || !canvas.requestPointerLock) return;
	Module.dxPointerLockClick = true;
	canvas.addEventListener('mousedown', function (event) {
		if (!Module.dxWantPointerLock || document.pointerLockElement === canvas) return;
		var rect = canvas.getBoundingClientRect();
		if (rect.height > 0 && (event.clientY - rect.top) / rect.height >= Module.dxPointerLockMaxY) return; // control panel
		try {
			var request = canvas.requestPointerLock();
			if (request && request.catch) request.catch(function () {});
		} catch (e) {}
	}, true);
});
EM_JS(void, DxSetWantPointerLock, (int want, float maxY), {
	Module.dxWantPointerLock = want !== 0;
	Module.dxPointerLockMaxY = maxY;
});
#endif

namespace devilution {
namespace fpv {

namespace {

constexpr float kMaxPitch = 75.0F * kPi / 180.0F;
constexpr float kCameraTimeConstant = 0.035F; // seconds
/** Default downward look when third person starts (radians). */
constexpr float kThirdPersonStartPitch = -0.28F;
/** Melee aim assist: an adjacent monster within this angle of the crosshair is attacked (radians). */
constexpr float kMeleeAssistAngle = 40.0F * kPi / 180.0F;
/** Dungeon ceiling margin for the third-person camera (world units below the wall height). */
constexpr float kCeilingMargin = 0.06F;
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
	/** First or third person while active (a VR provider counts as first person for the gameplay rules). */
	CameraMode mode = CameraMode::Isometric;
	bool cameraValid = false;
	CameraState camera;
	ViewOutput view;
	float yaw = 0.0F;
	float pitch = 0.0F;
	/** Pitch kept per mode, so switching first <-> third restores each one's own look height. */
	float savedPitchFirst = 0.0F;
	float savedPitchThird = kThirdPersonStartPitch;
	float bobPhase = 0.0F;
	uint32_t lastFrameMs = 0;
	uint32_t lastLookMs = 0;
	/** Smoothed player position (x, z) and floor height the view providers follow. */
	float followX = 0.0F;
	float followZ = 0.0F;
	float floorY = 0.0F;

	/** Third person: wanted shoulder side (+1 camera right of the player, -1 left) and boom length (tiles). */
	float shoulderSide = 1.0F;
	float distance = 2.2F;

	bool mouseLook = false;
	bool relativeMouse = false;
	int skipMotionEvents = 0;
	/** Browser: the pointer lock was released (Esc) or never granted; a click on the 3D view takes it back. */
	bool pointerLockLost = false;
	bool pointerLockSeen = false;
	uint32_t pointerLockRequestMs = 0;

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
HudWantsCursorQuery HudCursorQuery = nullptr;
FirstPersonView FirstPersonProvider;
ThirdPersonView ThirdPersonProvider;
render3d::ViewProvider *ActiveProvider = nullptr;
float LastFrameYaw = 0.0F;
float LastFramePitch = 0.0F;
float LastTargetX = 0.0F;
float LastTargetZ = 0.0F;
VrControl Vr;

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
	    || (PartySidePanelOpen && *GetOptions().Gameplay.showMultiplayerPartyInfo && gbIsMultiplayer)
	    || (HudCursorQuery != nullptr && HudCursorQuery());
}

/** The 3D camera mode wanted now (Isometric: the 3D view is off). A VR provider plays by first-person rules. */
CameraMode WantedMode()
{
	if (render3d::GetViewProvider() != nullptr)
		return CameraMode::FirstPerson;
	return *GetOptions().Graphics.cameraMode;
}

/** Third-person boom length from the option (tenths of a tile). */
float OptionDistance()
{
	return std::clamp(static_cast<float>(*GetOptions().Graphics.thirdPersonDistance) / 10.0F, kThirdPersonMinDistance, kThirdPersonMaxDistance);
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
	if (on) {
		// Browser: SDL defers the pointer lock request to the next user event (the key or button release).
		S.pointerLockRequestMs = NowMs();
		S.pointerLockSeen = false;
	}
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

/** Switches between first and third person (or from Isometric when entering): each mode keeps its own pitch. */
void EnterMode(CameraMode mode)
{
	if (S.mode == CameraMode::FirstPerson)
		S.savedPitchFirst = S.pitch;
	else if (S.mode == CameraMode::ThirdPerson)
		S.savedPitchThird = S.pitch;
	S.mode = mode;
	S.pitch = mode == CameraMode::ThirdPerson ? S.savedPitchThird : S.savedPitchFirst;
	S.distance = OptionDistance();
	ActiveProvider = nullptr; // UpdateFrame resets the new provider
	Hud.helpUntil = NowMs() + 6000;
	LogVerbose("3D view: {} camera", mode == CameraMode::ThirdPerson ? "third-person" : "first-person");
}

void OnEnter()
{
	const Player &player = *MyPlayer;
	S.yaw = DirectionToYaw(player._pdir);
	EnterMode(WantedMode());
	S.cameraValid = false;
	LastFrameYaw = S.yaw;
	LastFramePitch = S.pitch;
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
	S.pointerLockLost = false;
#ifdef __EMSCRIPTEN__
	DxSetWantPointerLock(0, 1.0F);
#endif
	if (S.mode == CameraMode::FirstPerson)
		S.savedPitchFirst = S.pitch;
	else if (S.mode == CameraMode::ThirdPerson)
		S.savedPitchThird = S.pitch;
	S.mode = CameraMode::Isometric;
	ActiveProvider = nullptr;
	S.keys = {};
	S.moving = false;
	S.target = {};
	S.pick = {};
	Player &player = *MyPlayer;
	if (player._pmode == PM_STAND && player.CanChangeAction())
		player._pdir = YawToDirection(S.yaw);
	LogVerbose("FPV: exit, facing {} from yaw {:.2f}", static_cast<int>(player._pdir), S.yaw);
}


/**
 * Browser pointer lock: SDL's relative mode asks for it, but the browser grants it only from a user event and drops it
 * on Esc. When it is gone (or was not granted within a few seconds) the cursor comes back and a click on the 3D view
 * asks again (HandleMouseDown).
 */
void UpdatePointerLock()
{
#ifdef __EMSCRIPTEN__
	DxInstallPointerLockClick();
	// Clicks below the top of the control panel (fraction of the window height) stay normal clicks.
	const float panelTop = static_cast<float>(std::min(UiToScreen(GetMainPanel().position).y, ViewHeight())) / static_cast<float>(std::max<int>(gnScreenHeight, 1));
	DxSetWantPointerLock(S.pointerLockLost && !UiWantsCursor() ? 1 : 0, panelTop);
	if (!S.relativeMouse)
		return;
	EmscriptenPointerlockChangeEvent status {};
	if (emscripten_get_pointerlock_status(&status) != EMSCRIPTEN_RESULT_SUCCESS)
		return;
	if (status.isActive) {
		S.pointerLockSeen = true;
		return;
	}
	if (S.pointerLockSeen || NowMs() - S.pointerLockRequestMs > 3000) {
		S.pointerLockLost = true;
		LogVerbose("3D view: pointer lock {}", S.pointerLockSeen ? "released by the browser" : "not granted");
	}
#endif
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

	const CameraMode wanted = WantedMode();
	if (wanted != S.mode && wanted != CameraMode::Isometric)
		EnterMode(wanted);

	const int levelKey = CurrentLevelKey();
	if (levelKey != S.levelKey) {
		S.levelKey = levelKey;
		S.cameraValid = false;
		S.lastGoal = { -1, -1 };
		S.yaw = DirectionToYaw(MyPlayer->_pdir);
		Hud.bannerText = LevelBannerText();
		Hud.bannerUntil = NowMs() + 3000;
	}

	UpdatePointerLock();
	const bool wantLook = !UiWantsCursor() && !AltHeld() && (WindowFocused() || Vr.active) && !S.pointerLockLost
	    && !(Vr.active && Vr.uiPointing);
	if (wantLook != S.mouseLook) {
		S.mouseLook = wantLook;
		if (wantLook) {
			SetRelativeMouse(ControlMode == ControlTypes::KeyboardAndMouse && !Vr.active);
			S.skipMotionEvents = 2;
		} else {
			SetRelativeMouse(false);
			SetCursorPos(ScreenToUi(ViewCentreScreen()));
		}
	}
	if (S.mouseLook) {
		if (S.relativeMouse != (ControlMode == ControlTypes::KeyboardAndMouse && !Vr.active))
			SetRelativeMouse(ControlMode == ControlTypes::KeyboardAndMouse && !Vr.active);
		MousePosition = ScreenToUi(ViewCentreScreen());
	}
	if (!WindowFocused())
		S.keys = {};
}

void AddLook(float dxRadians, float dyRadians)
{
	S.yaw = WrapAngle(S.yaw - dxRadians);
	const float invert = *GetOptions().Gameplay.fpvInvertMouseY ? -1.0F : 1.0F;
	const bool third = S.mode == CameraMode::ThirdPerson;
	S.pitch = std::clamp(S.pitch - dyRadians * invert, third ? kThirdPersonMinPitch : -kMaxPitch, third ? kThirdPersonMaxPitch : kMaxPitch);
	if (dxRadians != 0.0F || dyRadians != 0.0F)
		S.lastLookMs = NowMs();
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

	// A goal far ahead keeps the walked line close to the view direction (the goal is a tile centre, so a near goal
	// would bend the line by up to ~13 degrees); WalkLine still stops it at the first wall.
	constexpr float LookAhead = 6.0F;
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
	// The left stick only when no movement key is held: a connected pad's resting or drifting stick flips the control
	// mode to Gamepad and must not cancel WASD.
	const bool anyKey = S.keys.forward || S.keys.back || S.keys.left || S.keys.right;
	if (!anyKey && ControlMode == ControlTypes::Gamepad) {
		forward = leftStickY;
		strafe = leftStickX;
	}
	if (Vr.active) {
		forward += Vr.moveForward;
		strafe += Vr.moveStrafe;
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
	GroundAxes(Vr.active ? Vr.moveYaw : S.yaw, fwdX, fwdZ, rightX, rightZ);
	float dirX = fwdX * forward + rightX * strafe;
	float dirZ = fwdZ * forward + rightZ * strafe;
	const float len = std::sqrt(dirX * dirX + dirZ * dirZ);
	if (len < 1e-4F) {
		StopMoving(player);
		return;
	}
	dirX /= len;
	dirZ /= len;

	const uint32_t now = NowMs();
	if (*GetOptions().Gameplay.viewMovement == ViewMovement::TileSteps) {
		// One tile per step: the next step starts once the player stands on the tile it walked to (held keys repeat).
		S.moving = true;
		if (player._pmode != PM_STAND || player.isWalking() || player.position.tile != player.position.future)
			return;
		const Point from = player.position.tile;
		const Point target = from + StepDirectionFor(dirX, dirZ);
		if (!CanStepBetween(player, from, target)) {
			S.moving = false;
			return;
		}
		if (target != S.lastGoal || now - S.lastSendMs > 300) {
			NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, target);
			S.lastGoal = target;
			S.lastSendMs = now;
		}
		return;
	}

	const std::optional<Point> goal = FindMoveGoal(player, dirX, dirZ);
	if (!goal) {
		StopMoving(player);
		return;
	}
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

/**
 * Melee aim assist for a click with nothing under the crosshair: attacks the adjacent monster closest to the
 * crosshair direction, if one is within kMeleeAssistAngle. False when there is none (the swing goes ahead).
 */
bool TryMeleeAssist(const Player &player)
{
	if (player.UsesRangedWeapon() || leveltype == DTYPE_TOWN)
		return false;
	float px;
	float pz;
	PlayerWorldPosition(player, px, pz);
	int best = -1;
	float bestAngle = kMeleeAssistAngle;
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (monster.hasNoLife() || (monster.flags & MFLAG_HIDDEN) != 0 || monster.isPlayerMinion() || !IsTileLit(monster.position.tile))
			continue;
		if (monster.position.tile.WalkingDistance(player.position.tile) > 1 && monster.position.future.WalkingDistance(player.position.tile) > 1)
			continue;
		float mx;
		float mz;
		MonsterWorldPosition(monster, mx, mz);
		const float angle = std::abs(WrapAngle(std::atan2(mx - px, mz - pz) - S.yaw));
		if (angle < bestAngle) {
			bestAngle = angle;
			best = static_cast<int>(id);
		}
	}
	if (best < 0)
		return false;
	LastPlayerAction = PlayerActionType::AttackMonsterTarget;
	NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(best));
	return true;
}

/** Ceiling the third-person camera stays under: the wall height in dungeons, none in town. */
float CeilingY()
{
	return leveltype == DTYPE_TOWN ? 100.0F : kWallHeight - kCeilingMargin;
}

bool CameraBlocked(float x, float y, float z)
{
	return render3d::CameraBlockedAt(x, y, z);
}

} // namespace

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

bool IsActive()
{
	return WantedMode() != CameraMode::Isometric && !HeadlessMode && gbRunGame && MyPlayer != nullptr && WebGpuIsReady();
}

bool IsFirstPerson()
{
	return IsActive() && WantedMode() == CameraMode::FirstPerson;
}

bool IsThirdPerson()
{
	return IsActive() && WantedMode() == CameraMode::ThirdPerson;
}

const ViewOutput &GetViewOutput()
{
	return S.view;
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
	return IsActive() || host::DrawsWorld3D(); // embedded World3D: the host renders the world, the game only its UI
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
	// Third person shows the 3D body's own attack (or the sprite's): no 2D slash there.
	return S.mode == CameraMode::FirstPerson && (!*GetOptions().Graphics.fpvWeapon || !ViewmodelAvailable());
}

bool OwnsMovement()
{
	return S.active;
}

void SetVrControl(const VrControl &control)
{
	const float pendingTurn = Vr.turn;
	Vr = control;
	Vr.turn += pendingTurn; // turns accumulate until the next UpdateFrame applies them
}

const VrControl &GetVrControl()
{
	return Vr;
}

bool UiOpen()
{
	return UiWantsCursor();
}

void ToggleFirstPerson()
{
	auto &option = GetOptions().Graphics.cameraMode;
	option.SetValue(*option == CameraMode::FirstPerson ? CameraMode::Isometric : CameraMode::FirstPerson);
	SyncState();
}

void CycleCameraMode()
{
	auto &option = GetOptions().Graphics.cameraMode;
	switch (*option) {
	case CameraMode::Isometric:
		option.SetValue(CameraMode::FirstPerson);
		break;
	case CameraMode::FirstPerson:
		option.SetValue(CameraMode::ThirdPerson);
		break;
	default:
		option.SetValue(CameraMode::Isometric);
		break;
	}
	SyncState();
}

void SwapShoulder()
{
	if (!S.active || S.mode != CameraMode::ThirdPerson)
		return;
	if (*GetOptions().Graphics.thirdPersonStyle != ThirdPersonStyle::OverShoulder)
		GetOptions().Graphics.thirdPersonStyle.SetValue(ThirdPersonStyle::OverShoulder);
	S.shoulderSide = -S.shoulderSide;
}

bool HandleWheel(int notches)
{
	if (!S.active || S.mode != CameraMode::ThirdPerson || notches == 0 || UiWantsCursor())
		return false;
	S.distance = ZoomBoom(S.distance, static_cast<float>(notches));
	GetOptions().Graphics.thirdPersonDistance.SetValue(static_cast<int>(std::lround(S.distance * 10.0F)));
	return true;
}

bool HandleMouseDown(Point uiPosition)
{
	if (!S.active || !S.pointerLockLost || UiWantsCursor())
		return false;
	if (uiPosition.y >= std::min(GetMainPanel().position.y, ScreenToUi({ 0, ViewHeight() }).y))
		return false; // on the control panel: a normal click
	S.pointerLockLost = false; // SyncState asks for the lock again; this click only takes the mouse back
	return true;
}

bool IsPointerLockLost()
{
	return S.active && S.pointerLockLost;
}

void SetHudWantsCursorQuery(HudWantsCursorQuery query)
{
	HudCursorQuery = query;
}

void ProcessInput()
{
	SyncState();
	if (!S.active)
		return;
	// Developer knob: DVL_FPV_HOLD=<letters of w, a, s, d> holds those movement keys (scripted movement tests).
	if (static const char *Hold = std::getenv("DVL_FPV_HOLD"); Hold != nullptr && gbRunGame && !MyPlayer->_pLvlChanging) {
		const std::string_view keys = Hold;
		S.keys.forward = keys.find('w') != std::string_view::npos;
		S.keys.left = keys.find('a') != std::string_view::npos;
		S.keys.back = keys.find('s') != std::string_view::npos;
		S.keys.right = keys.find('d') != std::string_view::npos;
	}
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

	// Developer knob: DVL_FPV_TURN=<degrees per second> turns the view (scripted smooth-turning tests).
	if (static const char *Turn = std::getenv("DVL_FPV_TURN"); Turn != nullptr)
		S.yaw = WrapAngle(S.yaw + std::strtof(Turn, nullptr) * kPi / 180.0F * dt);
	const bool snap = !S.cameraValid || std::hypot(x - S.followX, z - S.followZ) > 2.5F;
	if (snap) {
		S.followX = x;
		S.followZ = z;
		S.floorY = render3d::FloorHeightAt(x, z);
	} else {
		const float alpha = SmoothingAlpha(dt, kCameraTimeConstant);
		S.followX += (x - S.followX) * alpha;
		S.followZ += (z - S.followZ) * alpha;
		// The view follows the floor of the 3D world (stairs), smoothed.
		S.floorY += (render3d::FloorHeightAt(S.followX, S.followZ) - S.floorY) * SmoothingAlpha(dt, 0.08F);
	}

	if (player.isWalking())
		S.bobPhase += dt * 9.0F;
	else
		S.bobPhase *= 0.9F;
	if (Vr.active) {
		// The headset owns the head: no bob, no pitch (the view provider adds the tracked pose).
		S.yaw = WrapAngle(S.yaw + Vr.turn);
		Vr.turn = 0.0F;
		S.pitch = 0.0F;
	}

	const Options &options = GetOptions();
	ViewInputs inputs;
	inputs.dt = dt;
	inputs.feet = { S.followX, S.floorY, S.followZ };
	inputs.floorY = S.floorY;
	inputs.yaw = S.yaw;
	inputs.pitch = S.pitch;
	inputs.playerYaw = DirectionToYaw(player._pdir);
	inputs.autoWalking = player.isWalking() && !S.moving;
	inputs.idleLookSeconds = static_cast<float>(now - S.lastLookMs) / 1000.0F;
	const float aspect = static_cast<float>(ViewWidth()) / static_cast<float>(ViewHeight());
	inputs.fovYDeg = HorizontalToVerticalFov(static_cast<float>(*options.Graphics.fpvFieldOfView), aspect);
	inputs.headBob = *options.Graphics.fpvHeadBob && !Vr.active;
	inputs.bobPhase = S.bobPhase;
	inputs.style = *options.Graphics.thirdPersonStyle;
	inputs.distance = S.distance;
	inputs.shoulderSide = S.shoulderSide;
	inputs.ceilingY = CeilingY();
	inputs.isBlocked = CameraBlocked;
	inputs.fullBody = *options.Graphics.fpvFullBody;

	render3d::ViewProvider *provider = render3d::GetViewProvider();
	if (provider == nullptr)
		provider = S.mode == CameraMode::ThirdPerson ? static_cast<render3d::ViewProvider *>(&ThirdPersonProvider) : &FirstPersonProvider;
	if (provider != ActiveProvider || snap) {
		provider->Reset(inputs);
		ActiveProvider = provider;
		LogVerbose("3D view: {} view provider", provider->Name());
	}
	S.cameraValid = true;
	S.view = provider->Update(inputs);
	if (S.view.lookYawOverride)
		S.yaw = *S.view.lookYawOverride;
	else if (provider == &ThirdPersonProvider)
		S.yaw = ThirdPersonProvider.yaw(); // the orbit chase may have swung the view
	S.camera = S.view.camera;
	// Developer knob: DVL_FPV_TRACE=1 logs the camera every drawn frame (checks that movement is smooth between ticks).
	static const bool Trace = std::getenv("DVL_FPV_TRACE") != nullptr;
	if (Trace)
		Log("FPV trace: t={} x={:.4f} z={:.4f} yaw={:.3f} walking={}", now, S.camera.x, S.camera.z, S.camera.yaw, player.isWalking() ? 1 : 0);

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
	Ray ray = ScreenRay(S.camera, static_cast<float>(origin.x) + 0.5F, static_cast<float>(origin.y) + 0.5F, ViewWidth(), ViewHeight());
	if (S.view.chaseAim)
		ray = ChaseAimRay(ray, S.view.aimPivot); // third person: nothing between the camera and the player is picked
	if (Vr.active && Vr.aimValid) {
		ray.origin = { Vr.aimOrigin[0], Vr.aimOrigin[1], Vr.aimOrigin[2] };
		ray.dir = { Vr.aimDirection[0], Vr.aimDirection[1], Vr.aimDirection[2] };
	}
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
		return false; // free cursor (Alt): the classic click-to-move / click-to-act on what is under the cursor
	if (pcursitem != -1 || ObjectUnderCursor != nullptr || pcursmonst != -1 || PlayerUnderCursor != nullptr)
		return false;
	Player &player = *MyPlayer;
	// Click-to-move on the floor under the crosshair: always in town, and in the dungeon with the orbit camera (Shift
	// attacks in place there). WASD takes over again as soon as a key is pressed.
	const bool orbit = S.mode == CameraMode::ThirdPerson && *GetOptions().Graphics.thirdPersonStyle == ThirdPersonStyle::Orbit;
	if (leveltype == DTYPE_TOWN || (orbit && !shift)) {
		if (S.pick.hitWorld && IsAnyOf(S.pick.kind, TargetKind::None, TargetKind::Trigger) && CanPlayerTakeFpvAction()) {
			LastPlayerAction = PlayerActionType::Walk;
			NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, cursPosition);
		}
		return true;
	}
	if (TryMeleeAssist(player))
		return true;
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
	if (!S.active && host::DrawsWorld3D()) {
		host::QueueWorldItemLabels(out.w(), rows); // embedded World3D: the host's camera, not this one
		return;
	}
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
