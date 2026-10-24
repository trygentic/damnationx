#pragma once
/**
 * @file fpv/fpv.hpp
 *
 * First- and third-person (3D view) gameplay: camera modes, mouselook with
 * pointer lock, WASD and click-to-move, crosshair picking, actions and the 2D
 * HUD. The fpv:: functions declared in render3d/fpv_contract.hpp (IsActive,
 * IsFirstPerson, IsThirdPerson, GetCamera, DirectionToYaw, YawToDirection,
 * GetCrosshairTarget) are implemented here; the cameras themselves are view
 * providers (fpv/view_provider.hpp, also the VR hook). This header adds the
 * hooks the rest of the game calls. See docs/design/first-person-3d.md.
 *
 * Game logic stays classic: FPV only steers the existing cursor globals
 * (pcursmonst, pcursitem, ObjectUnderCursor, PlayerUnderCursor, cursPosition,
 * trigflag) from a ray through the crosshair, and sends the same net commands
 * as mouse / gamepad play.
 */

#include <optional>

#ifdef USE_SDL1
#include <SDL.h>
#elif defined(USE_SDL3)
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#else
#include <SDL.h>
#endif

#include "engine/direction.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "render3d/fpv_contract.hpp"

namespace devilution::fpv {

/**
 * True while the crosshair drives the cursor: FPV is active and no panel, menu
 * or Alt free cursor is up. The OS / software cursor is hidden and relative
 * mouse motion turns the camera.
 */
bool IsMouseLook();

/**
 * World-pass skip for scrollrt: true when the isometric world must not be drawn
 * into the 8-bit back buffer this frame (same as IsActive()). The world rows of
 * the back buffer are then cleared to palette index 0 (ClearWorldArea) so the
 * presenter can show the 3D image there; the UI is drawn over it as usual.
 */
bool SkipIsometricWorld();

/** Palette index the skipped world area is cleared to (treated as transparent by the 3D presenter). */
constexpr uint8_t kTransparentWorldIndex = 0;

/** Hotkey G: toggles between the isometric and the first-person camera (from third person: first person). */
void ToggleFirstPerson();

/** Keymapper "Swap shoulder" (middle mouse): moves the over-the-shoulder camera to the other side. */
void SwapShoulder();

/** Hotkey T: cycles Graphics > Camera Mode isometric -> first person -> third person -> isometric. */
void CycleCameraMode();

/**
 * Mouse wheel with no panel open: zooms the third-person camera (and saves the distance option). Returns true when
 * consumed; false in the other modes (the wheel keeps its bindings there).
 */
bool HandleWheel(int notches);

/**
 * Mouse button pressed at a UI position. In the browser, after the pointer lock was dropped (Esc) or not granted, a
 * click on the 3D view takes the mouse back instead of acting: returns true then (the click is consumed).
 */
bool HandleMouseDown(Point uiPosition);

/** The browser dropped the pointer lock: the HUD shows "Click to look around". */
bool IsPointerLockLost();

/**
 * HUD core hook (modules/hud): returns true while a HUD window, modal or element needs the free cursor. Mouselook then
 * pauses like for the classic panels; IsMouseLook() is the answer the HUD reads back for "the world owns the pointer".
 */
using HudWantsCursorQuery = bool (*)();
void SetHudWantsCursorQuery(HudWantsCursorQuery query);

/** Per input poll (diablo.cpp ProcessInput, before CheckCursMove): mode changes, mouse mode, WASD movement. */
void ProcessInput();

/** Per drawn frame (DrawAndBlit, after the game tick progress is updated): camera interpolation. */
void UpdateFrame();

/** Relative mouse motion in mouselook. Returns true when the event was consumed. */
bool HandleMouseMotion(const SDL_Event &event);

/** Movement keys (W, A, S, D) while FPV is active. Returns true when the key was consumed. */
bool HandleKey(SDL_Keycode vkey, bool pressed);

/** Keymapper action "Interact" (E): use / pick up / talk to what the crosshair is on. */
void Interact();

/**
 * Picks the world under a screen position (back-buffer pixels) by casting a
 * ray from the camera. Stores the result for ApplyPickToCursor and the HUD and
 * returns the tile cursPosition should use when nothing is selected (the floor
 * tile hit, the trigger tile or the last open tile before a wall).
 */
Point PickFromScreen(Point screenPosition);

/** Sets the cursor globals from the last pick. Returns true when an entity was selected. */
bool ApplyPickToCursor();

/**
 * Left click on the world in FPV (LeftMouseCmd). Returns true when FPV handled
 * it: with nothing under the crosshair a melee character swings at the tile in
 * front and a ranged one shoots at the crosshair instead of walking.
 */
bool HandleWorldClick(bool shift);

/** Called after a belt item was used: a scroll waiting for a target is cast at the crosshair at once. */
void OnBeltItemUsed();

/** The direction a dropped item should land in (in front of the camera), or nullopt when FPV is off. */
std::optional<Direction> DropDirection();

/** Clears the world rows of the back buffer for the 3D presenter and queues FPV item labels. */
void ClearWorldArea(const Surface &out, int viewportHeight);

/** HUD: crosshair, prompts, target name, slash, hit marker, help strip, level banner. Drawn in UI space before panels. */
void DrawHud(const Surface &ui);

/** Gamepad right stick in FPV: turns the camera. Returns true when consumed. */
bool HandleGamepadLook(float stickX, float stickY);

/** The game loop ended: release relative mouse mode and forget the FPV session (the next game re-enters cleanly). */
void OnGameEnd();

/** True when FPV moves the player itself (the classic stick / d-pad walking must stay idle). */
bool OwnsMovement();

/**
 * VR control (Source/xr, docs/design/vr.md), set once per headset frame. While `active`: the head pose replaces
 * mouselook pitch, movement comes from the analog stick in the `moveYaw` frame, the crosshair ray is the controller's
 * aim ray, head bob and the 2D crosshair are off.
 */
struct VrControl {
	bool active = false;
	/** Radians to add to the body yaw this frame (snap or smooth turn). */
	float turn = 0.0F;
	/** World yaw the stick moves in (head or off-hand direction). */
	float moveYaw = 0.0F;
	float moveForward = 0.0F; // -1..1
	float moveStrafe = 0.0F;  // -1..1, positive = right
	/** The laser points at the UI on the game screen panel: free cursor (no mouselook), clicks go to the UI. */
	bool uiPointing = false;
	/** Aim ray in world space (picking, attacks, spells). */
	bool aimValid = false;
	float aimOrigin[3] {};
	float aimDirection[3] {};
};
void SetVrControl(const VrControl &control);
/** True while a panel, menu, store or dialog wants the cursor (mouselook is off then). */
bool UiOpen();
const VrControl &GetVrControl();

} // namespace devilution::fpv
