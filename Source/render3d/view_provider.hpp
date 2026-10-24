#pragma once
/**
 * @file render3d/view_provider.hpp
 *
 * View providers: who decides where the 3D world is seen from. One interface for every 3D camera: the built-in
 * first- and third-person cameras (Source/fpv/view_provider.hpp) and a VR headset (Source/xr).
 *
 * A provider has two stages, each with a default, so a camera implements only what it owns:
 *
 *  1. Gameplay stage (Source/fpv, once per drawn frame): Update() turns the gameplay state (smoothed player position,
 *     floor height, mouse / stick look angles, options, the collision query) into the camera fpv::GetCamera() returns,
 *     plus flags for the renderer (draw the local player? the first-person legs? the viewmodel?) and for picking. The
 *     default is the first-person eye; the third-person rig overrides it.
 *  2. Render stage (render3d, once per presented frame, from WorldGpuPrepare()): BeginFrame() receives that camera.
 *     Returning false (the default) renders the flat camera; a provider that returns true supplies CenterView(), the
 *     view used for billboard facing, the desktop mirror image and the viewmodel, and may render EyeCount() extra
 *     stereo views itself with render3d::WorldGpuRenderViews() (docs/design/vr.md, "View provider").
 *
 * Registration: SetViewProvider() installs an overriding provider (VR). While one is installed it replaces the camera
 * mode option's built-in cameras, keeps the 3D world pass on (fpv::IsActive()) and plays by first-person gameplay
 * rules. nullptr restores the built-in cameras. Only one is active at a time.
 *
 * All views are in render3d world space (render3d/fpv_contract.hpp: 1 tile = 1 unit, y up, right-handed; the view
 * matrix looks down -Z like OpenXR and WebXR), and projections use WebGPU clip space (depth 0..1).
 */

#include <cmath>
#include <optional>

#include "fpv/camera_mode.hpp"
#include "fpv/fpv_math.hpp"
#include "render3d/fpv_contract.hpp"
#include "render3d/math3d.hpp"

namespace devilution {

namespace fpv {

/** Occupancy query of the 3D world (world units). True when the point is inside geometry or outside the map. */
using BlockedQuery = bool (*)(float x, float y, float z);

/** What the gameplay stage gets each frame. */
struct ViewInputs {
	/** Seconds since the previous frame (clamped by the caller). */
	float dt = 0.016F;
	/** The local player's drawn feet position (smoothed by the caller) and the floor height under it. */
	Vec3f feet;
	float floorY = 0.0F;
	/** Gameplay look angles (mouse / stick), radians: yaw as in fpv_contract.hpp, pitch positive looks up. */
	float yaw = 0.0F;
	float pitch = 0.0F;
	/** Model facing of the local player (radians), for chase cameras. */
	float playerYaw = 0.0F;
	/** The player walks on its own (click-to-move / path), not from WASD: chase cameras may swing behind it. */
	bool autoWalking = false;
	/** Seconds since the last mouse / stick look input. */
	float idleLookSeconds = 0.0F;
	/** Vertical field of view (degrees). */
	float fovYDeg = kDefaultFovYDeg;
	/** First person: head bob on (off in VR), and its phase (radians). */
	bool headBob = true;
	float bobPhase = 0.0F;
	/** Third person. */
	ThirdPersonStyle style = ThirdPersonStyle::OverShoulder;
	float distance = 2.2F;
	/** Wanted shoulder side: +1 camera right of the player, -1 left. */
	float shoulderSide = 1.0F;
	/** Highest eye height (dungeon ceiling minus a margin); large in town. */
	float ceilingY = 100.0F;
	/** Collision query; nullptr: no collision. */
	BlockedQuery isBlocked = nullptr;
	/** Show first-person legs (option on). */
	bool fullBody = false;
};

/** What the gameplay stage returns. */
struct ViewOutput {
	CameraState camera;
	/** Draw the local player's whole body (third person, not too close). */
	bool drawLocalPlayer = false;
	/** First-person full-body awareness: draw the body without head and arms, slightly behind the eye. */
	bool drawFirstPersonBody = false;
	/** Draw the first-person weapon viewmodel. */
	bool drawViewmodel = false;
	/** Pick from a chase camera: move the crosshair ray's origin up to `aimPivot` (see ChaseAimRay). */
	bool chaseAim = false;
	Vec3f aimPivot;
	/** Boom length after collision (third person), for diagnostics and the HUD. */
	float resolvedDistance = 0.0F;
	/** A provider that steers the gameplay yaw (movement / aim direction). */
	std::optional<float> lookYawOverride;
};

/** The first-person eye for `inputs` (the default gameplay stage). Implemented in fpv/view_provider.cpp. */
ViewOutput FirstPersonCamera(const ViewInputs &inputs);

} // namespace fpv

namespace render3d {

struct RenderView {
	Mat4 view = Mat4::Identity();
	Mat4 projection = Mat4::Identity();
	Vec3 position;
	Vec3 forward { 0.0F, 0.0F, 1.0F };
	Vec3 right { -1.0F, 0.0F, 0.0F };
	Vec3 up { 0.0F, 1.0F, 0.0F };
	/** Heading of `forward` in the fpv yaw convention (yaw 0 = +z, counter-clockwise from above). */
	float yaw = 0.0F;
	/** Vertical field of view (radians) and width / height of the image (symmetric approximation for asymmetric eyes). */
	float fovYRadians = 1.2F;
	float aspect = 1.0F;
};

class ViewProvider {
public:
	virtual ~ViewProvider() = default;

	/** Short name for logs ("first-person", "third-person", "openxr", "webxr", ...). */
	[[nodiscard]] virtual const char *Name() const = 0;

	// ---- Gameplay stage (Source/fpv) ----

	/** Called when the provider becomes the active one or the player teleported: snap instead of easing. */
	virtual void Reset(const fpv::ViewInputs & /*inputs*/) { }

	/** Once per drawn frame: the camera and render flags for this frame. Default: the first-person eye. */
	virtual fpv::ViewOutput Update(const fpv::ViewInputs &inputs);

	// ---- Render stage (render3d) ----

	/** Once per presented frame, before anything is drawn. Returns false to render the flat camera (the default). */
	virtual bool BeginFrame(const fpv::CameraState & /*camera*/) { return false; }

	/** The single view of this frame (valid after BeginFrame returned true). `aspect` is the mirror image's aspect. */
	[[nodiscard]] virtual RenderView CenterView(float aspect) const
	{
		RenderView view;
		view.aspect = aspect;
		return view;
	}

	/** Number of extra stereo views the provider renders itself (0 = none, 2 = a headset). */
	[[nodiscard]] virtual int EyeCount() const { return 0; }

	/** Eye `index` of this frame, 0 = left. Only called when EyeCount() > index. */
	[[nodiscard]] virtual RenderView EyeView(int /*index*/) const { return CenterView(1.0F); }
};

/**
 * Installs the overriding provider (VR; nullptr = the camera mode's built-in cameras). The provider must outlive its
 * registration. Implemented in fpv/view_provider.cpp (pure, so it works in every build and in tests).
 */
void SetViewProvider(ViewProvider *provider);
[[nodiscard]] ViewProvider *GetViewProvider();

/**
 * Builds a WebGPU projection (right-handed, depth 0..1) from the four half-angles of an OpenXR / WebXR field of view
 * (radians; left and down are negative).
 */
inline Mat4 PerspectiveFromFov(float angleLeft, float angleRight, float angleUp, float angleDown, float zNear, float zFar)
{
	const float tanLeft = std::tan(angleLeft);
	const float tanRight = std::tan(angleRight);
	const float tanUp = std::tan(angleUp);
	const float tanDown = std::tan(angleDown);
	const float width = tanRight - tanLeft;
	const float height = tanUp - tanDown;
	Mat4 r;
	r.at(0, 0) = 2.0F / width;
	r.at(0, 2) = (tanRight + tanLeft) / width;
	r.at(1, 1) = 2.0F / height;
	r.at(1, 2) = (tanUp + tanDown) / height;
	r.at(2, 2) = zFar / (zNear - zFar);
	r.at(2, 3) = (zNear * zFar) / (zNear - zFar);
	r.at(3, 2) = -1.0F;
	return r;
}

} // namespace render3d

} // namespace devilution
