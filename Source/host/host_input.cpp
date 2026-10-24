/**
 * @file host/host_input.cpp
 *
 * HostSubmitInput: the host's input frame becomes the SDL events the game already handles (mouse, keys, text, a
 * virtual gamepad), and its 3D pick becomes the game's cursor target (CheckCursMove asks PickedTile and
 * ApplyPickToCursor instead of picking in screen space). Runs on the host thread while the game thread is parked.
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <deque>
#include <mutex>
#include <string>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "controls/control_mode.hpp"
#include "cursor.h"
#include "engine/render/ui_scale.hpp"
#include "gmenu.h"
#include "items.h"
#include "levels/gendung.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "towners.h"
#include "utils/log.hpp"

namespace devilution::host {

namespace {

struct PickState {
	bool valid = false; // a frame was submitted
	int tileX = -1;
	int tileY = -1;
	uint32_t entityId = 0;
};

PickState Pick;
bool NoWorldPick = false;

/**
 * Input events for the game's own event loop (controls/input.h PollEvent takes them before SDL's queue). They never go
 * through SDL_PushEvent: sdl2-compat (SDL2 on SDL3) crashes converting a hand-made SDL_TEXTINPUT, and the SDL3 text event
 * only points at its string.
 */
std::mutex QueueMutex;
std::deque<SDL_Event> EventQueue;
#ifdef USE_SDL3
std::deque<std::string> TextStore; // the strings SDL3 text events point at (the last 256 stay alive)
#endif
int32_t LastPointerX = -1;
int32_t LastPointerY = -1;
uint32_t LastButtonsDown = 0;

} // namespace

void QueueEvent(const SDL_Event &event)
{
	const std::lock_guard<std::mutex> lock(QueueMutex);
	if (EventQueue.size() < 1024)
		EventQueue.push_back(event);
}

bool PollEvent(SDL_Event &event)
{
	if (!IsActive())
		return false;
	ProcessMenuCommands();
	const std::lock_guard<std::mutex> lock(QueueMutex);
	if (EventQueue.empty())
		return false;
	event = EventQueue.front();
	EventQueue.pop_front();
	static const bool Trace = [] {
		const char *value = std::getenv("DAMNATIONX_TRACE_INPUT");
		return value != nullptr && value[0] == '1';
	}();
	if (Trace)
		LogInfo("Host input: the game takes event {:#x}", static_cast<uint32_t>(event.type));
	return true;
}

namespace {

uint32_t EventTimestamp()
{
	return static_cast<uint32_t>(SDL_GetTicks());
}

void PushMouseMotion(int32_t x, int32_t y)
{
	SDL_Event event {};
#ifdef USE_SDL3
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.motion.x = static_cast<float>(x);
	event.motion.y = static_cast<float>(y);
	event.motion.xrel = static_cast<float>(LastPointerX < 0 ? 0 : x - LastPointerX);
	event.motion.yrel = static_cast<float>(LastPointerY < 0 ? 0 : y - LastPointerY);
#else
	event.type = SDL_MOUSEMOTION;
	event.motion.timestamp = EventTimestamp();
	event.motion.x = x;
	event.motion.y = y;
	event.motion.xrel = LastPointerX < 0 ? 0 : x - LastPointerX;
	event.motion.yrel = LastPointerY < 0 ? 0 : y - LastPointerY;
#endif
	QueueEvent(event);
}

void PushMouseButton(uint8_t button, bool down, int32_t x, int32_t y)
{
	SDL_Event event {};
#ifdef USE_SDL3
	event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	event.button.button = button;
	event.button.down = down;
	event.button.clicks = 1;
	event.button.x = static_cast<float>(x);
	event.button.y = static_cast<float>(y);
#else
	event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	event.button.timestamp = EventTimestamp();
	event.button.button = button;
	event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.button.clicks = 1;
	event.button.x = x;
	event.button.y = y;
#endif
	QueueEvent(event);
}

void PushWheel(int32_t x, int32_t y, int32_t pointerX, int32_t pointerY)
{
	SDL_Event event {};
#ifdef USE_SDL3
	event.type = SDL_EVENT_MOUSE_WHEEL;
	event.wheel.x = static_cast<float>(x);
	event.wheel.y = static_cast<float>(y);
	event.wheel.integer_x = x;
	event.wheel.integer_y = y;
	event.wheel.mouse_x = static_cast<float>(pointerX);
	event.wheel.mouse_y = static_cast<float>(pointerY);
	event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
#else
	event.type = SDL_MOUSEWHEEL;
	event.wheel.timestamp = EventTimestamp();
	event.wheel.x = x;
	event.wheel.y = y;
	event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
#if SDL_VERSION_ATLEAST(2, 26, 0)
	event.wheel.mouseX = pointerX;
	event.wheel.mouseY = pointerY;
#endif
#endif
	QueueEvent(event);
}

void PushKey(const HostInputFrame::Key &key)
{
	SDL_Event event {};
	const auto keycode = static_cast<SDL_Keycode>(key.keycode);
#ifdef USE_SDL3
	event.type = key.down != 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	event.key.key = keycode;
	event.key.scancode = SDL_GetScancodeFromKey(keycode, nullptr);
	event.key.mod = key.modifiers;
	event.key.down = key.down != 0;
	event.key.repeat = key.repeat != 0;
#else
	event.type = key.down != 0 ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.timestamp = EventTimestamp();
	event.key.state = key.down != 0 ? SDL_PRESSED : SDL_RELEASED;
	event.key.repeat = key.repeat;
	event.key.keysym.sym = keycode;
	event.key.keysym.scancode = SDL_GetScancodeFromKey(keycode);
	event.key.keysym.mod = key.modifiers;
#endif
	QueueEvent(event);
}

void PushText(const char *text, size_t length)
{
#ifdef USE_SDL3
	TextStore.emplace_back(text, length);
	if (TextStore.size() > 256)
		TextStore.pop_front();
	SDL_Event event {};
	event.type = SDL_EVENT_TEXT_INPUT;
	event.text.text = TextStore.back().c_str();
	QueueEvent(event);
#else
	// SDL2 delivers text in chunks of at most SDL_TEXTINPUTEVENT_TEXT_SIZE - 1 bytes, split on code point boundaries.
	size_t start = 0;
	while (start < length) {
		size_t end = std::min(length, start + SDL_TEXTINPUTEVENT_TEXT_SIZE - 1);
		while (end > start && end < length && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
			--end;
		if (end == start)
			break;
		SDL_Event event {};
		event.type = SDL_TEXTINPUT;
		event.text.timestamp = EventTimestamp();
		std::memcpy(event.text.text, text + start, end - start);
		event.text.text[end - start] = '\0';
		QueueEvent(event);
		start = end;
	}
#endif
}

// ---- Virtual gamepad: SDL sees a real game controller, so the game's own controller code handles it. ----

#if !defined(USE_SDL3) && SDL_VERSION_ATLEAST(2, 0, 14)
SDL_Joystick *VirtualPad = nullptr;
int VirtualPadIndex = -1;
bool VirtualPadFailed = false;
uint32_t LastPadButtons = 0;
int16_t LastPadAxes[6] {};

bool EnsureVirtualPad()
{
	if (VirtualPad != nullptr)
		return true;
	if (VirtualPadFailed)
		return false;
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
		LogError("HostSubmitInput: no gamepad support: {}", SDL_GetError());
		VirtualPadFailed = true;
		return false;
	}
	VirtualPadIndex = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
	if (VirtualPadIndex < 0) {
		LogError("HostSubmitInput: cannot attach the virtual gamepad: {}", SDL_GetError());
		VirtualPadFailed = true;
		return false;
	}
	VirtualPad = SDL_JoystickOpen(VirtualPadIndex);
	if (VirtualPad == nullptr) {
		LogError("HostSubmitInput: cannot open the virtual gamepad: {}", SDL_GetError());
		SDL_JoystickDetachVirtual(VirtualPadIndex);
		VirtualPadFailed = true;
		return false;
	}
	return true;
}

void ApplyGamepad(const HostInputFrame &input)
{
	// Resting sticks report small values: below this they count as centred, so a connected but unused gamepad neither
	// creates the virtual pad nor switches the game to gamepad mode (which drops the mouse / 3D pick target).
	constexpr int Deadzone = 6000;
	const auto filtered = [&](int axis) -> int16_t {
		const int16_t value = input.padAxes[axis];
		return std::abs(static_cast<int>(value)) < Deadzone ? int16_t { 0 } : value;
	};
	const bool anyAxis = std::any_of(std::begin(input.padAxes), std::end(input.padAxes), [](int16_t v) { return std::abs(static_cast<int>(v)) >= Deadzone; });
	if (VirtualPad == nullptr && input.padButtons == 0 && !anyAxis)
		return; // never touch the joystick subsystem for a host without a gamepad
	if (!EnsureVirtualPad())
		return;
	for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
		const uint32_t bit = 1U << button;
		if (((input.padButtons ^ LastPadButtons) & bit) != 0)
			SDL_JoystickSetVirtualButton(VirtualPad, button, (input.padButtons & bit) != 0 ? SDL_PRESSED : SDL_RELEASED);
	}
	for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX && axis < 6; ++axis) {
		const int16_t value = filtered(axis);
		if (value != LastPadAxes[axis])
			SDL_JoystickSetVirtualAxis(VirtualPad, axis, value);
		LastPadAxes[axis] = value;
	}
	LastPadButtons = input.padButtons;
}

void ReleaseGamepad()
{
	if (VirtualPad == nullptr)
		return;
	SDL_JoystickClose(VirtualPad);
	SDL_JoystickDetachVirtual(VirtualPadIndex);
	VirtualPad = nullptr;
	VirtualPadIndex = -1;
	LastPadButtons = 0;
	std::fill(std::begin(LastPadAxes), std::end(LastPadAxes), 0);
}
#else
void ApplyGamepad(const HostInputFrame &)
{
}
void ReleaseGamepad()
{
}
#endif

constexpr struct {
	HostButton bit;
	uint8_t sdlButton;
} ButtonMap[] = {
	{ HostButton::Primary, SDL_BUTTON_LEFT },
	{ HostButton::Secondary, SDL_BUTTON_RIGHT },
	{ HostButton::Middle, SDL_BUTTON_MIDDLE },
};

} // namespace

void ApplyInput(const HostInputFrame &input)
{
	// DAMNATIONX_TRACE_INPUT=1: log every key, button and text the host sends (debugging hosts).
	static const bool Trace = [] {
		const char *value = std::getenv("DAMNATIONX_TRACE_INPUT");
		return value != nullptr && value[0] == '1';
	}();
	if (Trace && (input.keyCount != 0 || input.buttonsPressed != 0 || input.buttonsReleased != 0 || input.text[0] != '\0')) {
		LogInfo("HostSubmitInput: {} keys (first {} down {}), pressed {:#x} released {:#x}, pointer {},{} hover {},{} #{}, text '{}'",
		    input.keyCount, input.keyCount > 0 ? input.keys[0].keycode : 0, input.keyCount > 0 ? input.keys[0].down : 0,
		    input.buttonsPressed, input.buttonsReleased, input.pointerX, input.pointerY, input.hoverTileX, input.hoverTileY,
		    input.hoverEntityId, std::string_view(input.text, strnlen(input.text, sizeof(input.text))));
	}
	Pick.valid = true;
	Pick.tileX = input.hoverTileX;
	Pick.tileY = input.hoverTileY;
	Pick.entityId = input.hoverEntityId;
	NoWorldPick = HostApiVersionInUse() >= 4 && (input.inputFlags & kInputNoWorldPick) != 0;
	if (HostApiVersionInUse() >= 5 && (input.inputFlags & kInputToggleRun) != 0)
		HostToggleRun();

	if (input.pointerX != LastPointerX || input.pointerY != LastPointerY) {
		PushMouseMotion(input.pointerX, input.pointerY);
		LastPointerX = input.pointerX;
		LastPointerY = input.pointerY;
	}

	// Modifiers are read with SDL_GetModState when the events are fetched; the last key transition sets them.
	const uint32_t keyCount = std::min<uint32_t>(input.keyCount, static_cast<uint32_t>(std::size(input.keys)));
	if (keyCount > 0)
		SDL_SetModState(static_cast<SDL_Keymod>(input.keys[keyCount - 1].modifiers));

	for (const auto &[bit, sdlButton] : ButtonMap) {
		if ((input.buttonsPressed & static_cast<uint32_t>(bit)) != 0)
			PushMouseButton(sdlButton, true, input.pointerX, input.pointerY);
	}
	for (uint32_t i = 0; i < keyCount; ++i)
		PushKey(input.keys[i]);
	const size_t textLength = strnlen(input.text, sizeof(input.text));
	if (textLength > 0)
		PushText(input.text, textLength);
	for (const auto &[bit, sdlButton] : ButtonMap) {
		if ((input.buttonsReleased & static_cast<uint32_t>(bit)) != 0)
			PushMouseButton(sdlButton, false, input.pointerX, input.pointerY);
	}
	LastButtonsDown = input.buttonsDown;
	if (HostApiVersionInUse() >= 4 && (input.wheelX != 0 || input.wheelY != 0))
		PushWheel(input.wheelX, input.wheelY, input.pointerX, input.pointerY);

	ApplyGamepad(input);

	// The game handles a frame's clicks before its own cursor update (ProcessInput). A host's raycast usually
	// arrives in the same frame as the click, so take the new 3D target now: the click then acts on what the host
	// shows under the pointer (talking to a towner, attacking, picking up), not on last frame's target.
	const bool hostPick = PickedTarget();
	if (InGame() && !gmenu_is_active() && (hostPick || input.buttonsPressed != 0)) {
		// Without a 3D pick the game picks from the pointer: move it there first, as the motion event queued above
		// will, so a click that comes with its own pointer position (a 2D host aiming at an entity) acts on what lies
		// under that position, not under last frame's pointer.
		if (!hostPick)
			SetMousePositionFromScreen({ input.pointerX, input.pointerY });
		CheckCursMove();
		if (Trace && input.buttonsPressed != 0)
			LogInfo("HostSubmitInput: click target monster/towner {} item {} object {} at {},{} (control mode {})", pcursmonst, pcursitem,
			    ObjectUnderCursor != nullptr, cursPosition.x, cursPosition.y, static_cast<int>(ControlMode));
	}
}

void ReleaseInput()
{
	ReleaseGamepad();
	Pick = {};
	NoWorldPick = false;
	{
		const std::lock_guard<std::mutex> lock(QueueMutex);
		EventQueue.clear();
	}
	LastPointerX = -1;
	LastPointerY = -1;
	LastButtonsDown = 0;
}

// ---- hooks (game thread, CheckCursMove) ----

bool PickedTile(Point &tile)
{
	// Only the 3D view picks in 3D: in Classic and Hd2D (and without a host pick) the game picks from the pointer in its
	// own isometric frame, exactly as the SDL executable does.
	if (!IsActive() || !Pick.valid || MyPlayer == nullptr || !DrawsWorld3D() || NoWorldPick)
		return false;
	if (Pick.tileX >= 0 && Pick.tileY >= 0) {
		tile = { std::clamp(Pick.tileX, 0, MAXDUNX - 1), std::clamp(Pick.tileY, 0, MAXDUNY - 1) };
		return true;
	}
	// The ray missed the dungeon: the player's own tile, so a click there does nothing.
	tile = MyPlayer->position.tile;
	return true;
}

bool PickedTarget()
{
	Point tile;
	return (Pick.tileX >= 0 || Pick.entityId != 0) && PickedTile(tile);
}

bool ApplyPickToCursor()
{
	if (pcurs == CURSOR_IDENTIFY)
		return false;
	if (Pick.entityId == 0) {
		Point tile;
		if (!PickedTile(tile))
			return false;
		Object *object = FindObjectAtPosition(tile);
		if (object == nullptr || !object->canInteractWith())
			return false;
		ObjectUnderCursor = object;
		cursPosition = object->position;
		return true;
	}
	const auto kind = static_cast<HostEntityKind>(Pick.entityId >> 24);
	const uint32_t index = (Pick.entityId & 0xFFFFFF) - 1;
	switch (kind) {
	case HostEntityKind::Monster: {
		if (index >= MaxMonsters)
			return false;
		const Monster &monster = Monsters[index];
		if ((monster.flags & MFLAG_HIDDEN) != 0 || monster.hitPoints >> 6 <= 0 || leveltype == DTYPE_TOWN)
			return false;
		pcursmonst = static_cast<int>(index);
		cursPosition = monster.position.tile;
		return true;
	}
	case HostEntityKind::Towner: {
		if (leveltype != DTYPE_TOWN || index >= Towners.size())
			return false;
		pcursmonst = static_cast<int>(index); // towners are selected through pcursmonst, like TrySelectTowner
		cursPosition = Towners[index].position;
		return true;
	}
	case HostEntityKind::Player: {
		if (index >= Players.size() || &Players[index] == MyPlayer || !Players[index].plractive)
			return false;
		PlayerUnderCursor = &Players[index];
		cursPosition = Players[index].position.tile;
		return true;
	}
	case HostEntityKind::Item: {
		if (index >= MAXITEMS)
			return false;
		pcursitem = static_cast<int8_t>(index);
		cursPosition = Items[index].position;
		return true;
	}
	case HostEntityKind::Object: {
		if (index >= MAXOBJECTS)
			return false;
		Object &object = Objects[index];
		if (!object.canInteractWith())
			return false;
		ObjectUnderCursor = &object;
		cursPosition = object.position;
		return true;
	}
	default:
		return false; // missiles cannot be targeted
	}
}

} // namespace devilution::host
