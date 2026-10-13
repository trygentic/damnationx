#pragma once
/**
 * @file render3d/world_renderer.hpp
 *
 * Public API of the WebGPU first-person 3D world renderer.
 *
 * Frame flow with FPV on (WebGPU presenter only):
 *
 *  1. scrollrt DrawGame: when fpv::SkipIsometricWorld() the classic world pass is skipped and
 *     fpv::ClearWorldArea() fills the world rows [0, gnViewportHeight) of the 8-bit back buffer with
 *     kUiTransparentIndex (fpv::kTransparentWorldIndex). Everything drawn afterwards (labels, HUD, panels,
 *     inventory, stores, automap, cursor) lands on top of it as usual.
 *  2. dx.cpp BuildWebGpuLightingData fills WebGpuLightingData::worldLights (all game lights, up to 32,
 *     nearest first) when WorldPassActive().
 *  3. webgpu_presenter.cpp WebGpuPresent: WorldGpuPrepare() builds/updates the level mesh, atlases and
 *     billboards; EncodeAndSubmit then encodes WorldGpuEncode() (3D pass into an offscreen colour+depth
 *     target at the game's resolution, then a nearest blit into the swapchain) and runs the classic
 *     composite with loadOp=Load. In that frame the composite discards every world-row pixel whose
 *     palette index is kUiTransparentIndex, skips the 2D light mask and the HD tile/sprite passes, so
 *     only the UI covers the 3D image. The HD UI pass still draws on top.
 *
 * Source/fpv owns fpv::IsActive()/GetCamera()/GetCrosshairTarget() (render3d/fpv_contract.hpp); render3d only
 * reads them.
 *
 * Developer test knobs (environment, read every frame by WorldGpuPrepare): DVL_W3D_YAW=<deg> fixed yaw,
 * DVL_W3D_SPIN=<deg/s> rotate the view, DVL_W3D_PITCH=<deg>, DVL_W3D_POS=<x>,<z> camera position,
 * DVL_W3D_STOP_DEMO=<s> push one Escape key press after that many seconds (ends a --demo playback, the game then
 * continues live). Read at level build: DVL_W3D_DUMP_ATLAS=<prefix> writes the level atlas as
 * <prefix>_<leveltype>.ppm, DVL_W3D_DUMP_TOWN=<file.csv> the town cell classification inputs, DVL_W3D_NO_MIPS=1 skips the
 * atlas mips. Per frame: DVL_W3D_NO_SHADOWS=1 turns the shadow rays off.
 */

#include <cstdint>

#include "config.h"
#include "render3d/fpv_contract.hpp"
#include "render3d/math3d.hpp"

#if DEVILUTIONX_WEBGPU
#include <webgpu/webgpu.h>
#endif

namespace devilution {

struct Surface;
struct WebGpuHybridLight;

namespace render3d {

/**
 * Palette index the world rows are cleared to while the 3D pass is active (== fpv::kTransparentWorldIndex). The
 * composite discards world-row pixels holding it, so UI pixels of that exact index show the 3D image too.
 */
constexpr uint8_t kUiTransparentIndex = 0;

/** Sprite pixels per world unit for billboards (same scale as the fpv crosshair picking: frame height / 128). */
constexpr float kSpritePixelsPerUnit = 128.0F;

/**
 * True when this frame renders the 3D world: fpv::IsActive() (game running, WebGPU ready) and no level change is
 * in progress (the load thread rebuilds the dungeon globals meanwhile; the world rows then stay black).
 */
bool WorldPassActive();

/**
 * Floor height (world units) of the current 3D level under world point (x, z): 0 except on stairs, whose steps ramp
 * up or down. The FPV camera adds it to the eye height. 0 when no level mesh is built.
 */
float FloorHeightAt(float x, float z);

/**
 * UI coverage mask (call right after clearing the world rows of `out` to kUiTransparentIndex): the gbuffer's aux plane
 * rows [0, rows) are marked "not UI" and UI stamping is armed, so every pixel the UI draws afterwards (blitters,
 * text, half-transparent boxes, lines) is stamped Unlit. ResolveUiCoverage then tells UI black from the 3D area.
 */
void BeginUiCoverage(const Surface &out, int rows);

/**
 * Before the presenter uploads the back buffer: every world-row pixel the UI drew with kUiTransparentIndex is
 * replaced by the palette's darkest other index, so only untouched pixels show the 3D image. `pixels` is the 8-bit
 * back buffer (the planes' mirror).
 */
void ResolveUiCoverage(uint8_t *pixels, int pitch, int width, int rows);

#if DEVILUTIONX_WEBGPU

/** Per-frame inputs from the presenter. */
struct WorldGpuFrame {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUTextureFormat colorFormat = WGPUTextureFormat_Undefined; // swapchain format
	int sourceWidth = 0;      // the game's back buffer size
	int sourceHeight = 0;
	int drawableWidth = 0;    // swapchain size
	int drawableHeight = 0;
	int viewportRows = 0;     // world rows of the back buffer (gnViewportHeight)
	const WebGpuHybridLight *lights = nullptr; // WebGpuLightingData::worldLights (tile space, worldZ = height in tiles)
	uint32_t lightCount = 0;
};

/** Updates level geometry, atlases, billboards and uniforms. Returns false when no 3D pass should be encoded. */
bool WorldGpuPrepare(const WorldGpuFrame &frame);

/**
 * Encodes the 3D pass. `target` is the swapchain view, (x, y, width, height) the drawable-pixel rectangle the
 * whole back buffer is shown in (the composite's letterbox viewport). Clears the target to black outside the image.
 */
void WorldGpuEncode(WGPUCommandEncoder encoder, WGPUTextureView target, float x, float y, float width, float height);

/** Releases every GPU resource (presenter shutdown / device loss). */
void WorldGpuRelease();

/** Depth format of the 3D pass (for pipelines drawn inside it). */
constexpr WGPUTextureFormat kWorldDepthFormat = WGPUTextureFormat_Depth32Float;

/** One light as the world shaders see it (tile space, y up). */
struct WorldLight {
	Vec3 position;
	float radius = 0.0F;
	Vec3 color { 1.0F, 1.0F, 1.0F };
	float intensity = 1.0F;
};

/**
 * Context handed to the skinned-actor renderer while the 3D pass is open. Draw with pipelines whose colour target
 * is `colorFormat`, depth `depthFormat` (depth test Less, depth write on), sampleCount 1.
 *
 * Lighting: `frameUniforms` is the world pass's uniform buffer (WGSL `WorldFrame` in render3d/world_shaders.hpp,
 * `frameUniformsSize` bytes) and `occluderGrid` the shadow grid (array<u32, 112*112>); bind them and copy the
 * `worldShade()` WGSL function from world_shaders.hpp (prepend kWorldCommonWgsl) to light skinned meshes exactly like the
 * world, with ray-traced shadows. The CPU copy of the same lights is in `lights`.
 */
struct SkinnedDrawContext {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPURenderPassEncoder pass = nullptr;
	WGPUTextureFormat colorFormat = WGPUTextureFormat_Undefined;
	WGPUTextureFormat depthFormat = kWorldDepthFormat;
	Mat4 view;
	Mat4 projection;
	Mat4 viewProjection;
	Vec3 cameraPosition;
	const WorldLight *lights = nullptr;
	uint32_t lightCount = 0;
	float ambient = 0.0F;
	WGPUBuffer frameUniforms = nullptr;
	uint64_t frameUniformsSize = 0;
	WGPUBuffer occluderGrid = nullptr;
	uint64_t occluderGridSize = 0;
};

/** Draws skinned actors inside the 3D pass (after the opaque level geometry and the billboards). */
using SkinnedDrawFn = void (*)(const SkinnedDrawContext &context);
/** Returns true when the skinned renderer draws Players[playerId] itself; the world renderer then skips its billboard. */
using SkinnedWantsPlayerFn = bool (*)(int playerId);

/**
 * Registers the skinned glTF renderer (render3d/skinned_*). Pass nullptr to unregister. The local player is never
 * drawn in FPV (the camera is inside it), whatever wantsPlayer says.
 */
void SetSkinnedActorRenderer(SkinnedDrawFn draw, SkinnedWantsPlayerFn wantsPlayer);

/**
 * First-person weapon viewmodel (Hexen-style sword/shield/arms from warrior.glb, drawn by the skinned renderer).
 * Called after the world, the billboards and the skinned actors, in a separate render pass on the same colour
 * target whose depth buffer was cleared to 1.0, so the viewmodel never clips into walls. The context is the world's
 * (same formats, view/projection of the world camera, the frame lights and uniform/occluder buffers) plus the
 * player's own light, for lighting the weapon like the world.
 */
struct ViewModelDrawContext : SkinnedDrawContext {
	bool hasPlayerLight = false;
	WorldLight playerLight; // the local player's light (radius 0 when it has none)
	Vec3 cameraForward;
	Vec3 cameraRight;
	Vec3 cameraUp;
	float fovYRadians = 0.0F;
	float aspect = 1.0F;
};
using ViewModelDrawFn = void (*)(const ViewModelDrawContext &context);

/** Registers the viewmodel renderer; nullptr unregisters (no extra pass is encoded then). */
void SetViewModelRenderer(ViewModelDrawFn draw);

#endif // DEVILUTIONX_WEBGPU

} // namespace render3d
} // namespace devilution
