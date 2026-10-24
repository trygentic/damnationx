/**
 * @file xr/xr_core.cpp
 *
 * Runtime-neutral VR: the head-tracked view provider, the world-from-tracking transform (world scale, body yaw,
 * recentring), the floating game screen panel and its laser cursor, comfort (snap / smooth turn, vignette) and the
 * controller mapping. The runtime itself is a Backend (xr_openxr.cpp, xr_webxr.cpp). See docs/design/vr.md.
 */
#include "xr/xr.hpp"

#if DEVILUTIONX_XR && DEVILUTIONX_WEBGPU

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "control/control.hpp"
#include "engine/render/webgpu_platform.hpp"
#include "engine/render/ui_scale.hpp"
#include "fpv/fpv.hpp"
#include "game_mode.hpp"
#include "options.h"
#include "render3d/view_provider.hpp"
#include "render3d/world_renderer.hpp"
#include "utils/display.h"
#include "utils/log.hpp"
#include "xr/xr_types.hpp"

namespace devilution::xr {

namespace {

using render3d::Normalize;
using render3d::RenderView;
using render3d::Vec4;

constexpr float kPi = 3.14159265358979F;
constexpr float kNearMeters = 0.05F;
constexpr float kFarMeters = 500.0F;
/** Default laser length when it hits nothing (metres). */
constexpr float kLaserMeters = 6.0F;
/** Leaning range before the tracking anchor follows the head (metres). */
constexpr float kLeanMeters = 0.3F;
/** The panel follows the head once it is this far (radians) off to the side. */
constexpr float kPanelFollowStart = 45.0F * kPi / 180.0F;
constexpr float kPanelFollowStop = 4.0F * kPi / 180.0F;

float WrapAngle(float a)
{
	while (a > kPi)
		a -= 2.0F * kPi;
	while (a <= -kPi)
		a += 2.0F * kPi;
	return a;
}

/** Yaw of a tracking-space forward vector: RotationY(yaw) maps -z onto it. */
float TrackingYaw(const Vec3 &forward)
{
	return std::atan2(-forward.x, -forward.z);
}

/** Yaw of a world-space forward vector in the fpv convention (yaw 0 = +z). */
float WorldYaw(const Vec3 &forward)
{
	return std::atan2(forward.x, forward.z);
}

Vec3 TransformDirection(const Mat4 &m, const Vec3 &v)
{
	const Vec4 r = m * Vec4 { v.x, v.y, v.z, 0.0F };
	return { r.x, r.y, r.z };
}

struct PanelHit {
	bool hit = false;
	float distance = 0.0F; // metres along the ray
	float u = 0.0F;        // 0..1 left to right
	float v = 0.0F;        // 0..1 top to bottom
};

struct State {
	std::unique_ptr<Backend> backend;
	bool commandLine = false;
	bool preInit = false;
	bool initialized = false;
	bool running = false;
	bool failed = false;

	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUTextureFormat colorFormat = WGPUTextureFormat_Undefined;

	WGPUTexture uiTexture = nullptr;
	WGPUTextureView uiView = nullptr;
	uint32_t uiWidth = 0;
	uint32_t uiHeight = 0;

	FrameInput input;
	bool haveInput = false;

	fpv::CameraState camera;
	bool cameraValid = false;

	// Tracking space bookkeeping.
	Vec3 anchor;
	bool anchorValid = false;
	float seatedHeadY = 1.2F;
	bool seatedValid = false;
	float panelYaw = 0.0F;
	float panelY = 1.4F;
	bool panelValid = false;
	bool panelFollowing = false;

	// Input.
	std::array<HandInput, 2> previous;
	bool snapArmed = true;
	uint32_t snapFlashUntil = 0;
	float vignette = 0.0F;
	bool leftDown = false;
	bool rightDown = false;
	bool moveTriggerDown = false;
	bool moveGripDown = false;
	int cursorX = -1;
	int cursorY = -1;
	bool uiPointing = false;
	PanelHit laserHit;
};

State X;

const VrOptions &Options()
{
	return GetOptions().Vr;
}

float MetersToWorld()
{
	return fpv::kWarriorMetersToWorld * 100.0F / static_cast<float>(std::max(10, *Options().worldScale));
}

Hand AimHand()
{
	return *Options().dominantHand == VrHand::Left ? LeftHand : RightHand;
}

Hand MoveHand()
{
	return AimHand() == RightHand ? LeftHand : RightHand;
}

bool SeatedEyes()
{
	return *Options().seated || X.input.trackingHasNoFloor;
}

/** Tracking (metres) -> render3d world. */
Mat4 WorldFromTracking()
{
	const float s = MetersToWorld();
	float floorY = 0.0F;
	Vec3 position;
	float bodyYaw = 0.0F;
	if (X.cameraValid) {
		floorY = X.camera.y - fpv::kEyeHeight;
		position = { X.camera.x, 0.0F, X.camera.z };
		bodyYaw = X.camera.yaw;
	}
	position.y = SeatedEyes() ? floorY + fpv::kEyeHeight - X.seatedHeadY * s : floorY;
	return Mat4::Translation(position) * Mat4::RotationY(bodyYaw + kPi) * Mat4::Scale({ s, s, s })
	    * Mat4::Translation({ -X.anchor.x, 0.0F, -X.anchor.z });
}

RenderView MakeView(const Mat4 &worldFromTracking, const Pose &pose, const Fov *fov, float fovYRadians, float aspect)
{
	const Mat4 world = worldFromTracking * pose.Matrix();
	RenderView view;
	view.view = render3d::Inverse(world);
	view.position = world.TransformPoint({ 0.0F, 0.0F, 0.0F });
	view.forward = Normalize(TransformDirection(world, { 0.0F, 0.0F, -1.0F }));
	view.right = Normalize(TransformDirection(world, { 1.0F, 0.0F, 0.0F }));
	view.up = Normalize(TransformDirection(world, { 0.0F, 1.0F, 0.0F }));
	view.yaw = WorldYaw(view.forward);
	if (fov != nullptr) {
		view.projection = render3d::PerspectiveFromFov(fov->angleLeft, fov->angleRight, fov->angleUp, fov->angleDown, kNearMeters, kFarMeters);
		view.fovYRadians = fov->angleUp - fov->angleDown;
		view.aspect = (std::tan(fov->angleRight) - std::tan(fov->angleLeft)) / std::max(1e-3F, std::tan(fov->angleUp) - std::tan(fov->angleDown));
	} else {
		view.projection = render3d::Perspective(fovYRadians, aspect, kNearMeters, kFarMeters);
		view.fovYRadians = fovYRadians;
		view.aspect = aspect;
	}
	return view;
}

/** Head pose for the views; a default standing pose until the runtime reports one. */
Pose HeadPose()
{
	if (X.input.head.valid)
		return X.input.head;
	Pose pose;
	pose.position = { 0.0F, X.seatedHeadY, 0.0F };
	pose.valid = true;
	return pose;
}

class HeadViewProvider final : public render3d::ViewProvider {
public:
	[[nodiscard]] const char *Name() const override
	{
		return X.backend != nullptr ? X.backend->Name() : "xr";
	}

	bool BeginFrame(const fpv::CameraState &camera) override
	{
		if (!X.running)
			return false;
		X.camera = camera;
		X.cameraValid = true;
		return true;
	}

	[[nodiscard]] RenderView CenterView(float aspect) const override
	{
		const float fovY = X.camera.fovYDeg * kPi / 180.0F;
		return MakeView(WorldFromTracking(), HeadPose(), nullptr, fovY, aspect);
	}

	[[nodiscard]] int EyeCount() const override
	{
		return 2;
	}

	[[nodiscard]] RenderView EyeView(int index) const override
	{
		const EyeInput &eye = X.input.eyes[std::clamp(index, 0, 1)];
		return MakeView(WorldFromTracking(), eye.pose.valid ? eye.pose : HeadPose(), &eye.fov, 0.0F, 1.0F);
	}
};

HeadViewProvider Provider;

// ---------------------------------------------------------------------------
// Tracking anchors and the panel

void UpdateAnchors(bool recenter)
{
	const Pose head = HeadPose();
	const Vec3 headXZ { head.position.x, 0.0F, head.position.z };
	if (!X.anchorValid || recenter) {
		X.anchor = headXZ;
		X.anchorValid = true;
	} else {
		const Vec3 delta = headXZ - X.anchor;
		const float distance = render3d::Length(delta);
		if (distance > kLeanMeters)
			X.anchor = X.anchor + delta * ((distance - kLeanMeters) / distance);
	}
	if (!X.seatedValid || recenter) {
		X.seatedHeadY = std::max(0.3F, head.position.y);
		X.seatedValid = X.input.head.valid;
	}

	const float headYaw = TrackingYaw(head.Forward());
	if (!X.panelValid || recenter) {
		X.panelYaw = headYaw;
		X.panelY = head.position.y;
		X.panelValid = X.input.head.valid;
		X.panelFollowing = false;
	}
	const float off = WrapAngle(headYaw - X.panelYaw);
	if (std::abs(off) > kPanelFollowStart)
		X.panelFollowing = true;
	if (X.panelFollowing) {
		X.panelYaw = WrapAngle(X.panelYaw + off * std::min(1.0F, X.input.deltaSeconds * 6.0F));
		X.panelY += (head.position.y - X.panelY) * std::min(1.0F, X.input.deltaSeconds * 6.0F);
		if (std::abs(off) < kPanelFollowStop)
			X.panelFollowing = false;
	}
}

float PanelDistance()
{
	return static_cast<float>(*Options().panelDistance) / 100.0F;
}

void PanelSize(int sourceWidth, int sourceHeight, float &width, float &height)
{
	width = PanelDistance() * 1.15F;
	height = width * static_cast<float>(std::max(1, sourceHeight)) / static_cast<float>(std::max(1, sourceWidth));
}

/** Panel centre and axes in tracking space. */
void PanelFrame(Vec3 &centre, Vec3 &normal, Vec3 &axisX)
{
	const float s = std::sin(X.panelYaw);
	const float c = std::cos(X.panelYaw);
	const Vec3 forward { -s, 0.0F, -c };
	normal = { s, 0.0F, c };
	axisX = { c, 0.0F, -s };
	const Pose head = HeadPose();
	centre = Vec3 { head.position.x, X.panelY, head.position.z } + forward * PanelDistance();
}

Mat4 PanelModel(int sourceWidth, int sourceHeight)
{
	float width;
	float height;
	PanelSize(sourceWidth, sourceHeight, width, height);
	Vec3 centre;
	Vec3 normal;
	Vec3 axisX;
	PanelFrame(centre, normal, axisX);
	return WorldFromTracking() * Mat4::Translation(centre) * Mat4::RotationY(X.panelYaw) * Mat4::Scale({ width, height, 1.0F });
}

PanelHit IntersectPanel(const Pose &aim, int sourceWidth, int sourceHeight)
{
	PanelHit result;
	if (!aim.valid)
		return result;
	float width;
	float height;
	PanelSize(sourceWidth, sourceHeight, width, height);
	Vec3 centre;
	Vec3 normal;
	Vec3 axisX;
	PanelFrame(centre, normal, axisX);
	const Vec3 direction = aim.Forward();
	const float denominator = render3d::Dot(direction, normal);
	if (denominator > -1e-4F)
		return result;
	const float t = render3d::Dot(centre - aim.position, normal) / denominator;
	if (t < 0.0F)
		return result;
	const Vec3 point = aim.position + direction * t;
	const float u = render3d::Dot(point - centre, axisX) / width + 0.5F;
	const float v = 0.5F - (point.y - centre.y) / height;
	if (u < 0.0F || u > 1.0F || v < 0.0F || v > 1.0F)
		return result;
	result = { true, t, u, v };
	return result;
}

// ---------------------------------------------------------------------------
// Input injection

void PushKey(uint32_t key, bool down)
{
	if (key == SDLK_UNKNOWN)
		return;
	SDL_Event event {};
#ifdef USE_SDL3
	event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	event.key.key = static_cast<SDL_Keycode>(key);
	event.key.down = down;
	if (ghMainWnd != nullptr)
		event.key.windowID = SDL_GetWindowID(ghMainWnd);
#else
	event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.keysym.sym = static_cast<SDL_Keycode>(key);
	event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
	SDL_PushEvent(&event);
}

void TapAction(std::string_view action)
{
	const uint32_t key = GetOptions().Keymapper.KeyForAction(action);
	PushKey(key, true);
	PushKey(key, false);
}

void TapKey(uint32_t key)
{
	PushKey(key, true);
	PushKey(key, false);
}

/** Back-buffer pixel -> the window coordinates SDL events carry (the event pipeline maps them back). */
void OutputPosition(int x, int y, float &outX, float &outY)
{
	int ox = x;
	int oy = y;
	LogicalToOutput(&ox, &oy);
	outX = static_cast<float>(ox);
	outY = static_cast<float>(oy);
}

void PushMouseMotion(int x, int y)
{
	SDL_Event event {};
	float ox;
	float oy;
	OutputPosition(x, y, ox, oy);
#ifdef USE_SDL3
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.motion.x = ox;
	event.motion.y = oy;
	if (ghMainWnd != nullptr)
		event.motion.windowID = SDL_GetWindowID(ghMainWnd);
#else
	event.type = SDL_MOUSEMOTION;
	event.motion.x = static_cast<int>(ox);
	event.motion.y = static_cast<int>(oy);
#endif
	SDL_PushEvent(&event);
}

void PushMouseButton(uint8_t button, bool down)
{
	SDL_Event event {};
	float ox = 0.0F;
	float oy = 0.0F;
	if (X.cursorX >= 0)
		OutputPosition(X.cursorX, X.cursorY, ox, oy);
#ifdef USE_SDL3
	event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	event.button.button = button;
	event.button.down = down;
	event.button.clicks = 1;
	event.button.x = ox;
	event.button.y = oy;
	if (ghMainWnd != nullptr)
		event.button.windowID = SDL_GetWindowID(ghMainWnd);
#else
	event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	event.button.button = button;
	event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.button.clicks = 1;
	event.button.x = static_cast<int>(ox);
	event.button.y = static_cast<int>(oy);
#endif
	SDL_PushEvent(&event);
}

bool Pressed(const HandInput &now, const HandInput &before, bool HandInput::*button)
{
	return now.*button && !(before.*button);
}

/** Trigger / grip with hysteresis. */
bool Held(float value, bool wasHeld)
{
	return wasHeld ? value > 0.35F : value > 0.65F;
}

float Deadzone(float value)
{
	constexpr float Dead = 0.18F;
	const float magnitude = std::abs(value);
	if (magnitude < Dead)
		return 0.0F;
	return std::copysign((magnitude - Dead) / (1.0F - Dead), value);
}

void ApplyInput(int sourceWidth, int sourceHeight)
{
	const FrameInput &in = X.input;
	const HandInput &aim = in.hands[AimHand()];
	const HandInput &move = in.hands[MoveHand()];
	const HandInput &aimBefore = X.previous[AimHand()];
	const HandInput &moveBefore = X.previous[MoveHand()];
	const uint32_t now = static_cast<uint32_t>(SDL_GetTicks());
	const bool inGame = gbRunGame && fpv::IsActive();

	// Recentre: off-hand stick click.
	const bool recenter = Pressed(move, moveBefore, &HandInput::stickClick);
	UpdateAnchors(recenter);

	// Laser on the game screen panel.
	X.laserHit = aim.active ? IntersectPanel(aim.aim, sourceWidth, sourceHeight) : PanelHit {};
	bool uiTarget = false;
	if (X.laserHit.hit) {
		const int px = std::clamp(static_cast<int>(X.laserHit.u * static_cast<float>(sourceWidth)), 0, sourceWidth - 1);
		const int py = std::clamp(static_cast<int>(X.laserHit.v * static_cast<float>(sourceHeight)), 0, sourceHeight - 1);
		const Point ui = ScreenToUi({ px, py });
		uiTarget = !inGame || fpv::UiOpen() || GetMainPanel().contains(ui);
		if (uiTarget && (px != X.cursorX || py != X.cursorY)) {
			X.cursorX = px;
			X.cursorY = py;
			PushMouseMotion(px, py);
		}
	}
	X.uiPointing = uiTarget;

	// Primary (trigger) and secondary (grip) actions: mouse buttons at the laser cursor or the crosshair ray.
	const bool left = Held(aim.trigger, X.leftDown);
	if (left != X.leftDown) {
		X.leftDown = left;
		PushMouseButton(SDL_BUTTON_LEFT, left);
	}
	const bool right = Held(aim.grip, X.rightDown);
	if (right != X.rightDown) {
		X.rightDown = right;
		PushMouseButton(SDL_BUTTON_RIGHT, right);
	}

	// Buttons.
	if (Pressed(move, moveBefore, &HandInput::menu))
		TapKey(SDLK_ESCAPE);
	if (inGame) {
		if (Pressed(aim, aimBefore, &HandInput::primary))
			TapAction("FirstPersonInteract");
		if (Pressed(aim, aimBefore, &HandInput::secondary))
			TapAction("Inventory");
		if (Pressed(move, moveBefore, &HandInput::primary))
			TapAction("Character");
		if (Pressed(move, moveBefore, &HandInput::secondary))
			TapAction("SpellBook");
		if (Pressed(aim, aimBefore, &HandInput::stickClick))
			TapAction("UseHealthPotion");
		const bool moveTrigger = Held(move.trigger, X.moveTriggerDown);
		if (moveTrigger && !X.moveTriggerDown)
			TapAction("UseManaPotion");
		X.moveTriggerDown = moveTrigger;
		const bool moveGrip = Held(move.grip, X.moveGripDown);
		if (moveGrip && !X.moveGripDown)
			TapAction("ToggleAutomap");
		X.moveGripDown = moveGrip;
		if (aim.stickY < -0.75F && aimBefore.stickY >= -0.75F)
			TapAction("DisplaySpells");
	} else if (Pressed(aim, aimBefore, &HandInput::primary) && X.cursorX >= 0) {
		PushMouseButton(SDL_BUTTON_LEFT, true); // menus: A clicks like the trigger
		PushMouseButton(SDL_BUTTON_LEFT, false);
	}

	// Turning and movement.
	fpv::VrControl control;
	control.active = inGame;
	control.uiPointing = X.uiPointing;
	const float turnStick = Deadzone(aim.stickX);
	float turnActivity = 0.0F;
	if (*Options().turnMode == VrTurnMode::Smooth) {
		control.turn = -turnStick * static_cast<float>(*Options().smoothTurnSpeed) * kPi / 180.0F * in.deltaSeconds;
		turnActivity = std::abs(turnStick);
	} else {
		if (X.snapArmed && std::abs(aim.stickX) > 0.7F) {
			control.turn = -std::copysign(static_cast<float>(*Options().snapTurnAngle) * kPi / 180.0F, aim.stickX);
			X.snapArmed = false;
			X.snapFlashUntil = now + 120;
		} else if (std::abs(aim.stickX) < 0.35F) {
			X.snapArmed = true;
		}
		if (now < X.snapFlashUntil)
			turnActivity = 1.0F;
	}
	const Mat4 worldFromTracking = WorldFromTracking();
	const Pose head = HeadPose();
	const Vec3 headWorldForward = TransformDirection(worldFromTracking, head.Forward());
	control.moveYaw = WorldYaw(headWorldForward);
	if (*Options().moveDirection == VrMoveDirection::Controller && move.active && move.aim.valid)
		control.moveYaw = WorldYaw(TransformDirection(worldFromTracking, move.aim.Forward()));
	control.moveForward = Deadzone(move.stickY);
	control.moveStrafe = Deadzone(move.stickX);
	if (aim.active && aim.aim.valid) {
		const Vec3 origin = worldFromTracking.TransformPoint(aim.aim.position);
		const Vec3 direction = Normalize(TransformDirection(worldFromTracking, aim.aim.Forward()));
		control.aimValid = true;
		control.aimOrigin[0] = origin.x;
		control.aimOrigin[1] = origin.y;
		control.aimOrigin[2] = origin.z;
		control.aimDirection[0] = direction.x;
		control.aimDirection[1] = direction.y;
		control.aimDirection[2] = direction.z;
	}
	fpv::SetVrControl(control);

	// Comfort vignette.
	float level = 0.0F;
	switch (*Options().vignette) {
	case VrComfortVignette::Off: level = 0.0F; break;
	case VrComfortVignette::Low: level = 0.35F; break;
	case VrComfortVignette::Medium: level = 0.6F; break;
	case VrComfortVignette::High: level = 0.85F; break;
	}
	const float activity = inGame ? std::min(1.0F, std::max(std::hypot(control.moveForward, control.moveStrafe), turnActivity)) : 0.0F;
	const float target = level * activity;
	const float rate = target > X.vignette ? 10.0F : 4.0F;
	X.vignette += (target - X.vignette) * std::min(1.0F, in.deltaSeconds * rate);

	X.previous = in.hands;
}

void ReleaseUiImage()
{
	if (X.uiView != nullptr)
		wgpuTextureViewRelease(X.uiView);
	if (X.uiTexture != nullptr) {
		wgpuTextureDestroy(X.uiTexture);
		wgpuTextureRelease(X.uiTexture);
	}
	X.uiView = nullptr;
	X.uiTexture = nullptr;
	X.uiWidth = X.uiHeight = 0;
}

void Stop()
{
	if (X.running)
		Log("VR: session stopped");
	X.running = false;
	fpv::SetVrControl({});
	render3d::SetViewProvider(nullptr);
}

} // namespace

void RequestFromCommandLine()
{
	X.commandLine = true;
}

bool Requested()
{
#ifdef __ANDROID__
	return true; // the VR APK is a headset app (Packaging: -Pvr)
#elif defined(__EMSCRIPTEN__)
	return X.commandLine || *GetOptions().Vr.enabled || GetWebGpuPlatform().queryFlag("vr");
#else
	return X.commandLine || *GetOptions().Vr.enabled || std::getenv("DVL_VR") != nullptr;
#endif
}

bool PreDeviceInit()
{
	if (X.preInit)
		return X.backend != nullptr;
	X.preInit = true;
	if (!Requested())
		return false;
	X.backend.reset(CreateBackend());
	if (X.backend == nullptr || !X.backend->PreDeviceInit()) {
		LogError("VR: no headset runtime ({}); playing on the flat screen", X.backend != nullptr ? X.backend->Name() : "none");
		X.backend.reset();
		return false;
	}
#if SDL_VERSION_ATLEAST(2, 0, 0)
	// The hardware cursor is not part of the back buffer, so the floating screen would show none.
	GetOptions().Graphics.hardwareCursor.SetValue(false);
#endif
	Log("VR: {} runtime found", X.backend->Name());
	return true;
}

WGPUTextureFormat PreferredColorFormat()
{
	return X.backend != nullptr ? X.backend->PreferredColorFormat() : WGPUTextureFormat_Undefined;
}

void AppendDeviceFeatures(WGPUAdapter adapter, std::vector<WGPUFeatureName> &features)
{
	if (X.backend != nullptr)
		X.backend->AppendDeviceFeatures(adapter, features);
}

bool Initialize(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat)
{
	if (X.backend == nullptr)
		return false;
	X.device = device;
	X.queue = queue;
	X.colorFormat = colorFormat;
	const float renderScale = static_cast<float>(*Options().renderScale) / 100.0F;
	if (!X.backend->Initialize(instance, adapter, device, queue, colorFormat, renderScale)) {
		LogError("VR: {} session setup failed; playing on the flat screen", X.backend->Name());
		X.backend->Shutdown();
		X.backend.reset();
		return false;
	}
	X.initialized = true;
	Log("VR: {} ready", X.backend->Name());
	return true;
}

bool IsActive()
{
	return X.running;
}

bool OwnsDisplay()
{
	return X.running && X.backend != nullptr && X.backend->OwnsDisplay();
}

bool WantsUiImage()
{
	return X.running;
}

WGPUTextureView UiImageView(uint32_t width, uint32_t height)
{
	if (X.device == nullptr || width == 0 || height == 0)
		return nullptr;
	if (X.uiView != nullptr && X.uiWidth == width && X.uiHeight == height)
		return X.uiView;
	ReleaseUiImage();
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = { "DamnationX VR screen panel", WGPU_STRLEN };
	descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { width, height, 1 };
	descriptor.format = X.colorFormat;
	descriptor.mipLevelCount = 1;
	X.uiTexture = wgpuDeviceCreateTexture(X.device, &descriptor);
	if (X.uiTexture == nullptr)
		return nullptr;
	X.uiView = wgpuTextureCreateView(X.uiTexture, nullptr);
	X.uiWidth = width;
	X.uiHeight = height;
	return X.uiView;
}

void RenderFrame(bool worldPrepared, int sourceWidth, int sourceHeight)
{
	if (!X.initialized || X.backend == nullptr)
		return;
	const bool running = X.backend->Update();
	if (running != X.running) {
		if (running) {
			X.running = true;
			X.anchorValid = X.seatedValid = X.panelValid = false;
			render3d::SetViewProvider(&Provider);
			Log("VR: session running");
		} else {
			Stop();
		}
	}
	if (X.backend->ExitRequested()) {
		SDL_Event quit {};
#ifdef USE_SDL3
		quit.type = SDL_EVENT_QUIT;
#else
		quit.type = SDL_QUIT;
#endif
		SDL_PushEvent(&quit);
	}
	if (!X.running)
		return;
	// VR plays in first person.
	if (gbRunGame && !*GetOptions().Graphics.firstPersonView)
		GetOptions().Graphics.firstPersonView.SetValue(true);
	X.cameraValid = X.cameraValid && worldPrepared;

	FrameInput input;
	if (!X.backend->BeginFrame(input))
		return;
	X.input = input;
	X.haveInput = true;
	ApplyInput(sourceWidth, sourceHeight);
	if (!input.shouldRender) {
		X.backend->EndFrame(false);
		return;
	}

	std::array<WGPUTextureView, 2> targets {};
	uint32_t width = 0;
	uint32_t height = 0;
	if (!X.backend->AcquireEyeTargets(targets, width, height)) {
		X.backend->EndFrame(false);
		return;
	}
	std::array<RenderView, 2> views { Provider.EyeView(0), Provider.EyeView(1) };
	render3d::StereoViews stereo;
	stereo.device = X.device;
	stereo.queue = X.queue;
	stereo.colorFormat = X.colorFormat;
	stereo.drawWorld = worldPrepared && X.cameraValid;
	stereo.count = 2;
	stereo.views = views.data();
	stereo.targets = targets.data();
	stereo.width = width;
	stereo.height = height;
	stereo.overlay.panelImage = X.uiView;
	stereo.overlay.panelModel = PanelModel(sourceWidth, sourceHeight);
	stereo.overlay.panelOpacity = 1.0F;
	stereo.overlay.vignette = X.vignette;
	const HandInput &aim = X.input.hands[AimHand()];
	if (aim.active && aim.aim.valid) {
		const float length = X.laserHit.hit ? X.laserHit.distance : kLaserMeters;
		stereo.overlay.laserVisible = true;
		stereo.overlay.laserModel = WorldFromTracking() * aim.aim.Matrix() * Mat4::Scale({ 0.006F, 0.006F, length });
		stereo.overlay.laserColor = X.uiPointing ? Vec4 { 0.95F, 0.85F, 0.55F, 0.85F } : Vec4 { 1.0F, 0.2F, 0.12F, 0.7F };
	}
	const bool rendered = render3d::WorldGpuRenderViews(stereo);
	X.backend->EndFrame(rendered);
}

void Shutdown()
{
	Stop();
	ReleaseUiImage();
	if (X.backend != nullptr)
		X.backend->Shutdown();
	X.backend.reset();
	X.initialized = false;
	X.preInit = false;
	X.device = nullptr;
	X.queue = nullptr;
}

} // namespace devilution::xr

#endif // DEVILUTIONX_XR && DEVILUTIONX_WEBGPU
