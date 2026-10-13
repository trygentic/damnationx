#pragma once
/**
 * @file render3d/skinned_renderer.hpp
 *
 * GPU skinned rendering of the 3D warrior (assets/3d/warrior/warrior.glb).
 *
 * The model (render3d/gltf_model.hpp) is loaded on first use from the game assets ("3d\\warrior\\warrior.glb",
 * packaged into devilutionx.mpq / the assets directory like the other DevilutionX assets). Each draw samples the clip
 * matching the player's mode on the CPU (TRS keys, lerp / slerp, hierarchy, inverse bind) into a joint palette, and a
 * WGSL vertex shader skins the mesh with it (4 weights). The fragment shader is a small PBR: base colour texture (body
 * and hair) or the manifest's flat equipment colours, Lambert diffuse plus GGX specular with the material's metallic /
 * roughness, hemispheric ambient, AgX tone mapping (Blender's view transform the sprites were baked with).
 *
 * Two uses:
 *
 *  1. Isometric view (hd_sprites.cpp / hd_sprites_webgpu.cpp): the warrior's prerendered HD sprite is replaced by an
 *     imposter rendered every frame with the bake camera (orthographic, 35 degree elevation, 640 x 640 cell, ground
 *     pivot (320, 424), 4.4039 m across) and the bake's three area lights (exposure -0.5 EV) into a per-player texture
 *     [colour | tile-space normal]. The imposter goes through the HD sprite instance path (occlusion against the 8-bit
 *     frame, zoom, drop shadow, hover outline); under hybrid lighting the dungeon lights light it with the rendered
 *     normals instead of the flat card normal.
 *
 *  2. First-person / 3D world (Source/render3d/world_*): SkinnedEncodePlayer draws a player into the caller's render
 *     pass with a perspective camera and the game's lights.
 *
 * Everything here is main-thread only.
 */

#include <cstdint>

#include "config.h"

#if DEVILUTIONX_WEBGPU
#include <webgpu/webgpu.h>
#endif

namespace devilution {

struct Player;
struct WebGpuHybridLight;

namespace render3d {

/** Size of the imposter cell in HD (4x) pixels and its ground pivot: the bake camera's frame. */
constexpr int kImposterCellSize = 640;
constexpr int kImposterPivotX = 320;
constexpr int kImposterPivotY = 424;

/** True when the 3D warrior option is on and `player` can be drawn with the model (a warrior). Cheap. */
[[nodiscard]] bool SkinnedPlayerWanted(const Player &player);

/** Loads the model on the first call (CPU side: parse, image decode); false if it failed (logged once). */
bool SkinnedModelLoaded();

/**
 * World-space yaw (radians, fpv_contract.hpp: 0 looks toward +z = tile +y, counter-clockwise seen from above) the
 * model faces for `player`, smoothed per player so 8-way turns rotate over a few frames instead of snapping.
 */
[[nodiscard]] float SkinnedPlayerYaw(const Player &player);

// ---- Isometric imposter (hd_sprites.cpp / hd_sprites_webgpu.cpp) ----

/** A new world pass begins: forgets the imposters requested by the previous one. */
void SkinnedBeginWorldPass();

/**
 * Requests this frame's imposter of `player`, to be shown through the HD sprite `clip` (an external clip of
 * kImposterCellSize x kImposterCellSize, frame 0, see hd_sprites.cpp). False when the model cannot draw this player
 * (option off, not a warrior, model failed to load): the caller falls back to the prerendered / classic sprite.
 */
bool SkinnedRequestImposter(const Player &player, int clip);

#if DEVILUTIONX_WEBGPU

/** One rendered imposter: HD sprite clip `clip` now samples `view` ([colour | normal], 2:1). */
struct SkinnedImposterOutput {
	int clip = -1;
	WGPUTextureView view = nullptr;
};

/**
 * Per presented frame, before the HD sprite instances are built (hd_sprites_webgpu.cpp): remembers the device, resets
 * the per-frame uniform slots, and renders every requested imposter (its own command buffer, submitted now) at
 * `texelsPerPixel` texels per back-buffer pixel. `count` receives the number of outputs.
 */
const SkinnedImposterOutput *SkinnedPrepareFrame(WGPUDevice device, WGPUQueue queue, float texelsPerPixel, int &count);

/** Releases every GPU object (device teardown); the CPU model stays loaded. */
void SkinnedReleaseGpu();

// ---- First-person / 3D world API (Source/render3d/world_*) ----

/**
 * True once the model is loaded and its GPU resources exist for the presenter's device (SkinnedPrepareFrame ran this
 * frame or earlier). When false, draw the player as a billboard instead.
 */
[[nodiscard]] bool SkinnedReady();

/** Extended draw parameters (SkinnedEncodePlayer fills the defaults). */
struct SkinnedDrawParams {
	/** Column-major world -> clip (WebGPU depth 0..1), the camera of the pass. */
	const float *viewProj = nullptr;
	/**
	 * Column-major model -> world. Model space: glTF meters, feet at the origin, facing +z, y up. World space
	 * (fpv_contract.hpp): x = tile x, z = tile y, y up, 1 tile = 1 unit. SkinnedPlayerModelMatrix builds it.
	 */
	const float *model = nullptr;
	/** Eye position in world space (specular). */
	float eye[3] { 0.0F, 0.0F, 0.0F };
	/** Game lights in tile space (WebGpuHybridLight: worldX = tile x, worldY = tile y, worldZ = height). Up to 16 are used. */
	const WebGpuHybridLight *lights = nullptr;
	int lightCount = 0;
	/** Hemispheric ambient (linear RGB) added to the lights: sky (normal up) and ground (normal down). */
	float ambientUp[3] { 0.10F, 0.10F, 0.11F };
	float ambientDown[3] { 0.03F, 0.03F, 0.03F };
	/** Linear exposure multiplier before AgX tone mapping (2^-0.5 = the bake's -0.5 EV). */
	float exposure = 0.70710678F;
	/** Brightness of the game lights (their falloff is (1 - d / radius)^2 * intensity, as the hybrid pass). */
	float lightScale = 2.0F;
	WGPUTextureFormat colorFormat = WGPUTextureFormat_BGRA8Unorm;
	/** WGPUTextureFormat_Undefined: the pass has no depth attachment (no depth test). */
	WGPUTextureFormat depthFormat = WGPUTextureFormat_Depth24Plus;
	uint32_t sampleCount = 1;
};

/**
 * Builds the model matrix for `player` in the fpv_contract world: feet on the player's drawn position (tile centre
 * plus the free-form sub-tile offset), scaled by fpv::kWarriorMetersToWorld, turned to SkinnedPlayerYaw(player).
 */
void SkinnedPlayerModelMatrix(const Player &player, float outModel[16]);

/**
 * Draws `player` with the skinned model into `pass` (an open render pass of the presenter's device whose colour
 * attachment has `colorFormat` and depth attachment `depthFormat`, single-sampled). The pose comes from the player's
 * mode and AnimInfo exactly as in the isometric view; `model` is SkinnedPlayerModelMatrix's or the caller's own.
 * The uniforms are written with wgpuQueueWriteBuffer before the pass's command buffer is submitted, so up to 64 draws
 * per frame are possible (slots reset by SkinnedPrepareFrame each presented frame). No-op unless SkinnedReady().
 * Writes depth; colour is opaque (alpha 1), sRGB-encoded unless `colorFormat` is an *Srgb format.
 */
void SkinnedEncodePlayer(WGPURenderPassEncoder pass, const float viewProj[16], const float model[16], const Player &player,
    const WebGpuHybridLight *lights, int lightCount, WGPUTextureFormat colorFormat, WGPUTextureFormat depthFormat);

/** SkinnedEncodePlayer with every parameter (eye position for specular, ambient, exposure, MSAA sample count). */
void SkinnedEncodePlayerEx(WGPURenderPassEncoder pass, const Player &player, const SkinnedDrawParams &params);

// ---- First-person weapon viewmodel (Hexen style) ----

/**
 * Parameters of SkinnedEncodeViewModel. Everything not marked required has a usable default.
 *
 * The viewmodel is the warrior's right arm + hand with Sword_RIGHT and the left arm + hand with Shield_LEFT (equipment
 * per warrior_manifest.json: no sword when unarmed or in town, no shield without one), cut out of the skinned mesh by a
 * palette-entry filter (at the shoulder, off screen) and placed with hand-authored, procedurally animated view-space
 * poses framed above the control panel: idle = sword lower right, blade up and to the left, shield lower left showing
 * its outer face; attack = wind-up and anticipation, then a fast planar diagonal swing (forehand upper right to lower
 * left, or the mirrored backhand on alternate swings) whose blade crosses the screen centre exactly at the game's hit
 * frame (AnimInfo frame _pAFNum - 1), with a swept motion trail; block = shield raised to the centre; hit = recoil and
 * shake; cast = off hand forward with a glow orb tinted by the spell's element (shield put aside); death = both drop
 * away. Walk bob and strafe sway follow the player's movement, and the weapons lag a little behind mouselook turns.
 */
struct ViewModelParams {
	/** Required: the player whose equipment and mode drive the viewmodel (normally MyPlayer). */
	const Player *player = nullptr;
	/**
	 * Required: column-major world -> camera matrix of the first-person camera (fpv_contract world: x = tile x,
	 * y up, z = tile y, 1 tile = 1 unit; camera looks down -z, +y up). Used to move the scene lights into view space
	 * and to derive yaw / pitch for the look lag.
	 */
	const float *view = nullptr;
	/** Viewport width / height. */
	float aspect = 16.0F / 9.0F;
	/** The viewmodel's own vertical FOV (degrees), independent of the world camera's, so the weapon does not distort. */
	float fovYDeg = 62.0F;
	/**
	 * Fraction of the viewport's height, from the top, the weapon poses are framed in: the part of the 3D view above
	 * the control panel (the weapons then rest just above the panel and only their arms reach behind it).
	 */
	float frameFraction = 1.0F;
	/** Scene lights in tile space (as for SkinnedEncodePlayer; include the player's own light). Up to 16. */
	const WebGpuHybridLight *lights = nullptr;
	int lightCount = 0;
	float ambientUp[3] { 0.10F, 0.10F, 0.11F };
	float ambientDown[3] { 0.03F, 0.03F, 0.03F };
	float exposure = 0.70710678F;
	float lightScale = 2.0F;
	/** Movement speed in tiles per second for the walk bob; negative: derived from the player's mode (walking or not). */
	float moveSpeed = -1.0F;
	WGPUTextureFormat colorFormat = WGPUTextureFormat_BGRA8Unorm;
	/** The pass's depth format; the caller clears depth before this call (WGPUTextureFormat_Undefined: no depth). */
	WGPUTextureFormat depthFormat = WGPUTextureFormat_Depth24Plus;
	uint32_t sampleCount = 1;
};

/**
 * Draws the first-person weapon viewmodel into `pass` (two draws, own perspective projection, depth tested and
 * written). Call it last in the 3D pass, after clearing depth. No-op when SkinnedReady() is false, when the option
 * Graphics > First-Person Weapon (fpvWeapon) is off, or for a non-warrior player.
 */
void SkinnedEncodeViewModel(WGPURenderPassEncoder pass, const ViewModelParams &params);

#endif // DEVILUTIONX_WEBGPU

} // namespace render3d
} // namespace devilution
