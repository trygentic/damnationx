#include "render3d/skinned_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifndef USE_SDL1
#include "utils/png.h"
#endif

#include "control/control.hpp"
#include "engine/animationinfo.h"
#include "engine/assets.hpp"
#include "engine/render/hd_webgpu_async.hpp"
#include "engine/render/ui_scale.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "levels/gendung.h"
#include "multi.h"
#include "nthread.h"
#include "options.h"
#include "player.h"
#include "fpv/fpv_viewmodel_inputs.hpp"
#include "render3d/fpv_contract.hpp"
#include "render3d/gltf_model.hpp"
#include "render3d/world_renderer.hpp"
#include "render3d/world_shaders.hpp"
#include "utils/log.hpp"
#include "utils/ui_fwd.h"

namespace devilution::render3d {
namespace {

using skm::Mat4;
using skm::Vec3;

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
// 1024 px base colour: a quarter of the browser download, and half the APK and GPU memory on phones.
constexpr char ModelAsset[] = "3d\\warrior\\warrior_1024.glb";
#else
constexpr char ModelAsset[] = "3d\\warrior\\warrior.glb";
#endif

constexpr float Pi = 3.14159265358979F;
/** Game time (s) of the attack clip's hit (warrior_manifest.json clips[attack].hit_time_s). */
constexpr float AttackHitTime = 0.4F;
/** Angular speed of the smoothed facing (rad/s): an 8-way turn of 180 degrees takes ~0.16 s. */
constexpr float TurnSpeed = 20.0F;

// ---- Bake camera and lights (warrior_bake.blend, converted from Blender Z-up to glTF Y-up) ----
constexpr float BakeOrthoWidth = 4.403918743F; // meters across the 640 px frame
constexpr float BakeElevationDeg = 35.0F;
struct BakeLight {
	Vec3 position; // meters, camera-fixed frame (the actor turns, the lights do not)
	float power;   // Blender area light watts
};
constexpr std::array<BakeLight, 3> BakeLights { {
	{ { 3.0F, 5.0F, 4.0F }, 650.0F },  // key, front right
	{ { -3.0F, 3.0F, 2.0F }, 350.0F }, // fill, front left
	{ { 1.0F, 4.0F, -3.0F }, 500.0F }, // rim, behind
} };
/** Area light radiant intensity on its axis = power / pi (Lambertian emitter), times this calibration. */
constexpr float BakeLightScale = 1.0F;
/** World background colour * strength of the bake (uniform environment). */
constexpr std::array<float, 3> BakeAmbient { 0.06F * 0.6F, 0.065F * 0.6F, 0.075F * 0.6F };
constexpr float BakeExposure = 0.70710678F; // -0.5 EV

// ---- Model state ----
enum class LoadState : uint8_t { Unloaded, Loaded, Failed };
LoadState State = LoadState::Unloaded;
/** The GPU side failed (shader / pipeline / resources): draw the prerendered sprites instead. */
bool GpuBroken = false;
GltfModel Model;
/** One decoded image mip chain, RGBA8, level 0 first. */
struct ImageLevel {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
};
using MipChain = std::vector<ImageLevel>;
/** Decoded GltfModel::images (same indices); an empty chain when an image could not be decoded. */
std::vector<MipChain> Images;
/**
 * The texture sets the shader samples: the body's base colour (Model.bodyImage) and the equipment's base colour,
 * tangent-space normal and metallic-roughness atlases (the first material using each, e.g. the baked sword / shield
 * atlas). Indices into Images, -1 when absent.
 */
struct TextureSets {
	int body = -1;
	int equipBase = -1;
	int equipNormal = -1;
	int equipMr = -1;
} Textures;

struct ClipIds {
	int stand = -1, walk = -1, townStand = -1, townWalk = -1, attack = -1, block = -1, hit = -1, cast = -1, death = -1;
} Clips;

struct PartBits {
	uint32_t swordRight = 0, shield = 0, swordStowed = 0, scabbard = 0, frog = 0, chape = 0;
	uint32_t equipment = 0; // every equipment part
} Parts;

uint32_t PartBit(std::string_view name)
{
	const int part = Model.findPart(name);
	return part >= 0 && part < 32 ? 1U << part : 0U;
}

bool DecodeImage(const std::vector<uint8_t> &bytes, MipChain &chain)
{
#ifdef USE_SDL1
	return false;
#else
#ifdef USE_SDL3
	SDL_IOStream *io = SDL_IOFromConstMem(bytes.data(), bytes.size());
	SDL_Surface *decoded = io != nullptr ? IMG_Load_IO(io, true) : nullptr;
	SDL_Surface *rgba = decoded != nullptr ? SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_RGBA32) : nullptr;
	if (decoded != nullptr)
		SDL_DestroySurface(decoded);
#else
	SDL_RWops *io = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
	SDL_Surface *decoded = io != nullptr ? IMG_LoadPNG_RW(io) : nullptr;
	if (io != nullptr)
		SDL_RWclose(io);
	SDL_Surface *rgba = decoded != nullptr ? SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBA32, 0) : nullptr;
	if (decoded != nullptr)
		SDL_FreeSurface(decoded);
#endif
	if (rgba == nullptr)
		return false;
	ImageLevel level;
	level.width = rgba->w;
	level.height = rgba->h;
	level.pixels.resize(static_cast<size_t>(level.width) * level.height * 4);
	for (int y = 0; y < level.height; ++y)
		std::memcpy(level.pixels.data() + static_cast<size_t>(y) * level.width * 4, static_cast<const uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch, static_cast<size_t>(level.width) * 4);
#ifdef USE_SDL3
	SDL_DestroySurface(rgba);
#else
	SDL_FreeSurface(rgba);
#endif
	chain.clear();
	chain.push_back(std::move(level));
	// Box-filtered mips: the 2048 px texture covers a ~250 px tall figure.
	while (chain.back().width > 1 || chain.back().height > 1) {
		const ImageLevel &source = chain.back();
		ImageLevel next;
		next.width = std::max(source.width / 2, 1);
		next.height = std::max(source.height / 2, 1);
		next.pixels.resize(static_cast<size_t>(next.width) * next.height * 4);
		for (int y = 0; y < next.height; ++y) {
			for (int x = 0; x < next.width; ++x) {
				for (int c = 0; c < 4; ++c) {
					int sum = 0;
					for (int dy = 0; dy < 2; ++dy) {
						for (int dx = 0; dx < 2; ++dx) {
							const int sx = std::min(x * 2 + dx, source.width - 1);
							const int sy = std::min(y * 2 + dy, source.height - 1);
							sum += source.pixels[(static_cast<size_t>(sy) * source.width + sx) * 4 + c];
						}
					}
					next.pixels[(static_cast<size_t>(y) * next.width + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
				}
			}
		}
		chain.push_back(std::move(next));
	}
	return true;
#endif
}

bool LoadModel()
{
	const auto start = std::chrono::steady_clock::now();
	auto asset = LoadAsset(ModelAsset);
	if (!asset.has_value()) {
		LogError("3D warrior: {}", asset.error());
		return false;
	}
	const std::string_view bytes = static_cast<std::string_view>(*asset);
	std::string error;
	if (!LoadGltfModel(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()), Model, error)) {
		LogError("3D warrior: {}: {}", ModelAsset, error);
		return false;
	}
	Images.assign(Model.images.size(), {});
	for (size_t i = 0; i < Model.images.size(); ++i) {
		if (!DecodeImage(Model.images[i].bytes, Images[i]))
			LogError("3D warrior: could not decode image {} ({})", i, Model.images[i].mimeType);
		Model.images[i].bytes.clear();
		Model.images[i].bytes.shrink_to_fit();
	}
	if (Model.bodyImage >= 0 && Images[static_cast<size_t>(Model.bodyImage)].empty()) {
		LogError("3D warrior: could not decode the base colour image");
		return false;
	}
	Model.imageBytes.clear();
	Model.imageBytes.shrink_to_fit();
	// Texture sets: the body's base colour, then the first equipment material's base colour / normal / metallic-roughness.
	const auto decoded = [](int image) { return image >= 0 && !Images[static_cast<size_t>(image)].empty() ? image : -1; };
	Textures = {};
	Textures.body = decoded(Model.bodyImage);
	for (const GltfMaterial &material : Model.materials) {
		if (Textures.equipBase < 0 && material.baseColorImage != Model.bodyImage)
			Textures.equipBase = decoded(material.baseColorImage);
		if (Textures.equipNormal < 0)
			Textures.equipNormal = decoded(material.normalImage);
		if (Textures.equipMr < 0)
			Textures.equipMr = decoded(material.metallicRoughnessImage);
	}
	if (Model.paletteSize() > 64) {
		LogError("3D warrior: {} palette entries (max 64)", Model.paletteSize());
		return false;
	}
	Clips.stand = Model.findClip("stand");
	Clips.walk = Model.findClip("walk");
	Clips.townStand = Model.findClip("town_stand");
	Clips.townWalk = Model.findClip("town_walk");
	Clips.attack = Model.findClip("attack");
	Clips.block = Model.findClip("block");
	Clips.hit = Model.findClip("hit");
	Clips.cast = Model.findClip("cast");
	Clips.death = Model.findClip("death");
	if (Clips.stand < 0 || Clips.walk < 0) {
		LogError("3D warrior: the stand / walk clips are missing");
		return false;
	}
	Parts.swordRight = PartBit("Sword_RIGHT");
	Parts.shield = PartBit("Shield_LEFT");
	Parts.swordStowed = PartBit("Sword_Town_Stowed");
	Parts.scabbard = PartBit("Town_Scabbard");
	Parts.frog = PartBit("Town_Scabbard_Frog");
	Parts.chape = PartBit("Town_Scabbard_Gold_Chape");
	Parts.equipment = Parts.swordRight | Parts.shield | Parts.swordStowed | Parts.scabbard | Parts.frog | Parts.chape;
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	const auto width = [](int image) { return image >= 0 ? Images[static_cast<size_t>(image)][0].width : 0; };
	Log("3D warrior: {} vertices, {} triangles, {} joints, {} clips, {} px body texture, equipment {} / {} / {} px ({} ms)", Model.vertices.size(),
	    Model.indices.size() / 3, Model.joints.size(), Model.clips.size(), width(Textures.body), width(Textures.equipBase), width(Textures.equipNormal),
	    width(Textures.equipMr), elapsed);
	return true;
}

// ---- Pose from the player ----

struct Pose {
	int clip = -1;
	float time = 0.0F;
	bool loop = false;
	uint32_t visibleParts = 1; // the body
	float worldYaw = 0.0F;     // fpv_contract yaw
};

enum class Appearance : uint8_t { SwordShield, Sword, UnarmedShield, Unarmed };

Appearance AppearanceOf(const Player &player)
{
	switch (static_cast<PlayerWeaponGraphic>(player._pgfxnum & 0xF)) {
	case PlayerWeaponGraphic::UnarmedShield:
		return Appearance::UnarmedShield;
	case PlayerWeaponGraphic::Sword:
	case PlayerWeaponGraphic::Axe:
	case PlayerWeaponGraphic::Mace:
		return Appearance::Sword;
	case PlayerWeaponGraphic::SwordShield:
	case PlayerWeaponGraphic::MaceShield:
		return Appearance::SwordShield;
	default: // unarmed, bow, staff: the model has no such weapon
		return Appearance::Unarmed;
	}
}

/** warrior_manifest.json equipment_visibility.by_context. */
uint32_t VisibleParts(Appearance appearance, bool inTown, bool dead)
{
	uint32_t parts = ~Parts.equipment; // the body (and anything not listed)
	if (dead)
		return parts;
	const bool sword = appearance == Appearance::Sword || appearance == Appearance::SwordShield;
	const bool shield = appearance == Appearance::SwordShield || appearance == Appearance::UnarmedShield;
	if (sword)
		parts |= (inTown ? Parts.swordStowed : Parts.swordRight) | Parts.scabbard | Parts.frog | Parts.chape;
	if (shield)
		parts |= Parts.shield;
	return parts;
}

/** Fraction [0, 1] of the player's current game animation, sub-tick smooth (the HD sprites' frame choice). */
float AnimationProgress(const AnimationInfo &anim)
{
	if (anim.distributesFrames())
		return static_cast<float>(anim.getAnimationProgress()) / AnimationInfo::baseValueFraction;
	const int frames = std::max<int>(anim.numberOfFrames, 1);
	const int ticksPerFrame = std::max<int>(anim.ticksPerFrame, 1);
	const float subtick = anim.isPetrified ? 0.0F : static_cast<float>(ProgressToNextGameTick) / AnimationInfo::baseValueFraction;
	const float position = static_cast<float>(std::max<int>(anim.currentFrame, 0))
	    + (static_cast<float>(std::max<int>(anim.tickCounterOfCurrentFrame, 0)) + std::clamp(subtick, 0.0F, 1.0F)) / ticksPerFrame;
	return std::clamp(position / frames, 0.0F, 1.0F);
}

/** Tile-space step of each Direction (South = screen down = +x +y). */
constexpr std::array<std::array<int, 2>, 8> DirectionSteps { { { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 } } };

float DirectionYaw(Direction direction)
{
	const auto &step = DirectionSteps[static_cast<size_t>(direction) & 7];
	return std::atan2(static_cast<float>(step[0]), static_cast<float>(step[1])); // fpv_contract: yaw 0 = +z (tile +y)
}

struct YawState {
	bool valid = false;
	float yaw = 0.0F;
	std::chrono::steady_clock::time_point last;
};
std::array<YawState, MAX_PLRS> Yaws;

float SmoothedYaw(const Player &player)
{
	const float target = DirectionYaw(player.getSpriteDirection());
	const size_t id = player.getId();
	if (id >= Yaws.size())
		return target;
	YawState &state = Yaws[id];
	const auto now = std::chrono::steady_clock::now();
	if (!state.valid) {
		state = { true, target, now };
		return target;
	}
	const float dt = std::min(std::chrono::duration<float>(now - state.last).count(), 0.25F);
	state.last = now;
	float delta = std::remainder(target - state.yaw, 2.0F * Pi);
	const float maxStep = TurnSpeed * dt;
	// Death and mode changes snap rather than spin; small turns rotate.
	if (std::abs(delta) <= maxStep || player._pmode == PM_DEATH)
		state.yaw = target;
	else
		state.yaw = std::remainder(state.yaw + std::copysign(maxStep, delta), 2.0F * Pi);
	return state.yaw;
}

bool BuildPose(const Player &player, Pose &pose)
{
	const bool inTown = leveltype == DTYPE_TOWN;
	const player_graphic graphic = player.getGraphic();
	const float progress = AnimationProgress(player.AnimInfo);
	int clip = -1;
	switch (graphic) {
	case player_graphic::Stand:
		clip = inTown && Clips.townStand >= 0 ? Clips.townStand : Clips.stand;
		break;
	case player_graphic::Walk:
		clip = inTown && Clips.townWalk >= 0 ? Clips.townWalk : Clips.walk;
		break;
	case player_graphic::Attack:
		clip = Clips.attack;
		break;
	case player_graphic::Block:
		clip = Clips.block;
		break;
	case player_graphic::Hit:
		clip = Clips.hit;
		break;
	case player_graphic::Lightning:
	case player_graphic::Fire:
	case player_graphic::Magic:
		clip = Clips.cast;
		break;
	case player_graphic::Death:
		clip = Clips.death;
		break;
	}
	if (clip < 0)
		clip = Clips.stand;
	const GltfClip &source = Model.clips[clip];
	pose.clip = clip;
	pose.loop = clip == Clips.stand || clip == Clips.walk || clip == Clips.townStand || clip == Clips.townWalk;
	if (pose.loop) {
		pose.time = progress * source.duration;
	} else if (clip == Clips.attack && source.duration > AttackHitTime) {
		// Align the game's action frame (the hit, AnimInfo frame _pAFNum - 1) with the clip's hit at 0.4 s.
		const float frames = static_cast<float>(std::max<int>(player.AnimInfo.numberOfFrames, 2));
		const float hitFrame = std::clamp(static_cast<float>(player._pAFNum - 1), 1.0F, frames - 1.0F);
		const float frame = progress * frames;
		pose.time = frame < hitFrame
		    ? frame / hitFrame * AttackHitTime
		    : AttackHitTime + (frame - hitFrame) / (frames - hitFrame) * (source.duration - AttackHitTime);
	} else {
		pose.time = progress * source.duration;
	}
	if (!pose.loop)
		pose.time = std::min(pose.time, source.duration);
	pose.visibleParts = VisibleParts(AppearanceOf(player), inTown, graphic == player_graphic::Death);
	pose.worldYaw = SmoothedYaw(player);
	return true;
}

std::vector<NodePose> ScratchPose;
std::vector<Mat4> ScratchGlobals;
std::vector<Mat4> ScratchPalette;

void ComputePosePalette(const Pose &pose)
{
	SampleClip(Model, pose.clip, pose.time, pose.loop, ScratchPose);
	ComputeGlobalMatrices(Model, ScratchPose, ScratchGlobals);
	ComputePalette(Model, ScratchGlobals, ScratchPalette);
}

// ---- Imposter requests (world pass) ----
struct ImposterRequest {
	bool active = false;
	int clip = -1;
	Pose pose;
};
std::array<ImposterRequest, MAX_PLRS> Requests;

} // namespace

bool SkinnedPlayerWanted(const Player &player)
{
#if DEVILUTIONX_WEBGPU
	return player._pClass == HeroClass::Warrior && *GetOptions().Graphics.warrior3dModel && State != LoadState::Failed && !GpuBroken;
#else
	(void)player;
	return false;
#endif
}

bool SkinnedModelLoaded()
{
	if (State == LoadState::Unloaded)
		State = LoadModel() ? LoadState::Loaded : LoadState::Failed;
	return State == LoadState::Loaded;
}

float SkinnedPlayerYaw(const Player &player)
{
	return SmoothedYaw(player);
}

void SkinnedBeginWorldPass()
{
	for (ImposterRequest &request : Requests)
		request.active = false;
}

bool SkinnedRequestImposter(const Player &player, int clip)
{
	if (!SkinnedPlayerWanted(player) || !SkinnedModelLoaded() || clip < 0)
		return false;
	const size_t id = player.getId();
	if (id >= Requests.size())
		return false;
	ImposterRequest &request = Requests[id];
	if (!BuildPose(player, request.pose))
		return false;
	request.active = true;
	request.clip = clip;
	return true;
}

} // namespace devilution::render3d

#if DEVILUTIONX_WEBGPU

namespace devilution::render3d {
namespace {

constexpr char ShaderSource[] = R"(
// ---- Skinned glTF mesh (render3d/skinned_renderer.cpp) ----
struct Light {
  posRadius: vec4f,  // position; w > 0: game light radius ((1 - d / r)^2 falloff), w <= 0: inverse-square light
  color: vec4f,      // rgb radiant intensity (already times pi for game lights), w unused
};
struct Draw {
  viewProj: mat4x4f,
  model: mat4x4f,
  normalToOut: mat4x4f, // imposter: world normal -> tile-space normal (hybrid lighting)
  eye: vec4f,           // w = 0: eye position; w = 1: direction towards the (orthographic) viewer
  params: vec4f,        // exposure, light count, diffuse wrap, 1 = the target is *Srgb (write linear)
  ambientUp: vec4f,     // viewmodel (world lit): rim light colour
  ambientDown: vec4f,   // viewmodel (world lit): fill
  visible: vec4u,       // x: part mask; y, z: palette entry mask (entries 0-31, 32-63) used when w != 0 (viewmodel);
                        // w: 0 actor / imposter, 1 viewmodel, 2 viewmodel whose world light 0 is the player's own
  rim: vec4f,           // viewmodel: world direction towards the rim light (xyz), strength
  glow: vec4f,          // spell glow colour (rgb), strength (0: none)
  glowCentre: vec4f,    // spell glow centre (world xyz), radius
  lights: array<Light, 16>,
  palette: array<mat4x4f, 64>,
};
struct Material {
  baseColor: vec4f,
  params: vec4f, // metallic, roughness, specular factor, base colour texture (0 none, 1 body, 2 equipment atlas)
  maps: vec4f,   // equipment normal map (0 / 1), metallic-roughness map (0 / 1), normal scale, unused
};

@group(0) @binding(0) var<uniform> draw: Draw;
@group(0) @binding(1) var<storage, read> materials: array<Material>;
@group(0) @binding(2) var baseColorTexture: texture_2d<f32>;
@group(0) @binding(3) var baseColorSampler: sampler;
@group(0) @binding(4) var equipBaseTexture: texture_2d<f32>;
@group(0) @binding(5) var equipNormalTexture: texture_2d<f32>;
@group(0) @binding(6) var equipMrTexture: texture_2d<f32>;

struct VertexInput {
  @location(0) position: vec3f,
  @location(1) normal: vec3f,
  @location(2) uv: vec2f,
  @location(3) joints: vec4u,
  @location(4) weights: vec4f,
  @location(5) packed: u32,
};
struct VertexOutput {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) normal: vec3f,
  @location(2) uv: vec2f,
  @location(3) @interpolate(flat) material: u32,
};

@vertex fn vertexMain(v: VertexInput) -> VertexOutput {
  var out: VertexOutput;
  let part = (v.packed >> 8u) & 31u;
  if ((draw.visible.x & (1u << part)) == 0u) {
    // Hidden equipment: outside the depth range, so the whole triangle is clipped.
    out.clip = vec4f(0.0, 0.0, -2.0, 1.0);
    out.world = vec3f(0.0);
    out.normal = vec3f(0.0, 1.0, 0.0);
    out.uv = vec2f(0.0);
    out.material = 0u;
    return out;
  }
  let j = min(v.joints, vec4u(63u));
  if (draw.visible.w != 0u) {
    // Viewmodel: keep only the vertices mostly weighted to the selected palette entries (arm, hand, weapon).
    var kept = 0.0;
    for (var k = 0u; k < 4u; k++) {
      let entry = j[k];
      var bits = draw.visible.y;
      if (entry >= 32u) { bits = draw.visible.z; }
      if ((bits & (1u << (entry & 31u))) != 0u) { kept += v.weights[k]; }
    }
    if (kept < 0.5) {
      out.clip = vec4f(0.0, 0.0, -2.0, 1.0);
      out.world = vec3f(0.0);
      out.normal = vec3f(0.0, 1.0, 0.0);
      out.uv = vec2f(0.0);
      out.material = 0u;
      return out;
    }
  }
  let skin = draw.palette[j.x] * v.weights.x + draw.palette[j.y] * v.weights.y
           + draw.palette[j.z] * v.weights.z + draw.palette[j.w] * v.weights.w;
  let world = draw.model * (skin * vec4f(v.position, 1.0));
  out.clip = draw.viewProj * world;
  out.world = world.xyz;
  out.normal = (draw.model * (skin * vec4f(v.normal, 0.0))).xyz;
  out.uv = v.uv;
  out.material = v.packed & 255u;
  return out;
}

const PI = 3.14159265;

fn distributionGgx(nh: f32, a: f32) -> f32 {
  let a2 = a * a;
  let d = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d);
}
fn visibilitySmith(nv: f32, nl: f32, a: f32) -> f32 {
  let k = a * 0.5;
  return 1.0 / ((nv * (1.0 - k) + k) * (nl * (1.0 - k) + k) * 4.0); // G / (4 nl nv)
}

// AgX (Blender's default view transform), minimal fit: linear Rec.709 in, display-encoded out.
fn agxContrast(x: vec3f) -> vec3f {
  let x2 = x * x;
  let x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - vec3f(0.00232);
}
fn agx(color: vec3f) -> vec3f {
  let inset = mat3x3f(vec3f(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
                      vec3f(0.0784335999999992, 0.878468636469772, 0.0784336),
                      vec3f(0.0792237451477643, 0.0791661274605434, 0.879142973793104));
  let outset = mat3x3f(vec3f(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
                       vec3f(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
                       vec3f(-0.0990297440797205, -0.0989611768448433, 1.15107367264116));
  let minEv = -12.47393;
  let maxEv = 4.026069;
  var v = inset * max(color, vec3f(1e-10));
  v = clamp(log2(v), vec3f(minEv), vec3f(maxEv));
  v = (v - vec3f(minEv)) / (maxEv - minEv);
  v = agxContrast(v);
  return clamp(outset * v, vec3f(0.0), vec3f(1.0));
}

struct SurfaceInput {
  albedo: vec3f, // linear
  n: vec3f,      // shading normal (normal map applied), on the viewer's side of the surface
  metallic: f32,
  roughness: f32,
  specular: f32,
};

// Material inputs. Every texture is sampled and the derivatives taken before any branch (uniform control flow).
fn surfaceInput(input: VertexOutput, front: bool) -> SurfaceInput {
  let bodyTexel = textureSample(baseColorTexture, baseColorSampler, input.uv).rgb;
  let equipTexel = textureSample(equipBaseTexture, baseColorSampler, input.uv).rgb;
  let normalTexel = textureSample(equipNormalTexture, baseColorSampler, input.uv).xyz;
  let mrTexel = textureSample(equipMrTexture, baseColorSampler, input.uv).rgb;
  let dp1 = dpdx(input.world);
  let dp2 = dpdy(input.world);
  let duv1 = dpdx(input.uv);
  let duv2 = dpdy(input.uv);
  let m = materials[input.material];
  var s: SurfaceInput;
  s.albedo = m.baseColor.rgb;
  if (m.params.w > 1.5) {
    s.albedo = s.albedo * equipTexel;
  } else if (m.params.w > 0.5) {
    s.albedo = s.albedo * bodyTexel;
  }
  var n = normalize(input.normal);
  if (!front) { n = -n; }
  if (m.maps.x > 0.5) {
    // Cotangent frame from screen-space derivatives (the file has no vertex tangents). glTF v runs down the image
    // while the normal map's +Y points up it, hence -b.
    let tn = vec3f((normalTexel.xy * 2.0 - 1.0) * m.maps.z, normalTexel.z * 2.0 - 1.0);
    let dp2perp = cross(dp2, n);
    let dp1perp = cross(n, dp1);
    let t = dp2perp * duv1.x + dp1perp * duv2.x;
    let b = dp2perp * duv1.y + dp1perp * duv2.y;
    let len2 = max(dot(t, t), dot(b, b));
    if (len2 > 1e-30) {
      let scale = inverseSqrt(len2);
      let perturbed = t * (scale * tn.x) - b * (scale * tn.y) + n * tn.z;
      if (dot(perturbed, perturbed) > 1e-12) { n = normalize(perturbed); }
    }
  }
  s.n = n;
  s.metallic = m.params.x;
  s.roughness = m.params.y;
  if (m.maps.y > 0.5) {
    s.roughness = s.roughness * mrTexel.g;
    s.metallic = s.metallic * mrTexel.b;
  }
  s.specular = m.params.z;
  return s;
}

// Spell glow (viewmodel cast): a light at the glow centre on the hand plus a faint emissive silhouette.
fn spellGlow(p: vec3f, n: vec3f, v: vec3f, albedo: vec3f) -> vec3f {
  if (draw.glow.w <= 0.0) { return vec3f(0.0); }
  let toCentre = draw.glowCentre.xyz - p;
  let near = clamp(1.0 - length(toCentre) / max(draw.glowCentre.w, 1e-4), 0.0, 1.0);
  let facing = max(dot(n, normalize(toCentre)), 0.0) * 0.75 + 0.25;
  let edge = pow(1.0 - max(dot(n, v), 0.0), 3.0);
  return draw.glow.rgb * draw.glow.w * ((albedo + vec3f(0.08)) * near * near * facing * 3.0 + 0.12 * edge * near);
}

struct Surface {
  normal: vec3f,
  color: vec3f, // display-encoded
};

fn shade(input: VertexOutput, front: bool) -> Surface {
  let s = surfaceInput(input, front);
  let albedo = s.albedo;
  let n = s.n;
  var v: vec3f;
  if (draw.eye.w > 0.5) { v = normalize(draw.eye.xyz); } else { v = normalize(draw.eye.xyz - input.world); }
  let metallic = s.metallic;
  let roughness = clamp(s.roughness, 0.045, 1.0);
  let a = roughness * roughness;
  let f0 = mix(vec3f(0.04 * s.specular), albedo, metallic);
  let diffuse = albedo * (1.0 - metallic);
  let nv = max(dot(n, v), 1e-3);
  let wrap = draw.params.z;
  var radiance = vec3f(0.0);
  let count = min(u32(draw.params.y), 16u);
  for (var i = 0u; i < count; i++) {
    let light = draw.lights[i];
    let toLight = light.posRadius.xyz - input.world;
    let dist = length(toLight);
    var attenuation: f32;
    if (light.posRadius.w > 0.0) {
      if (dist >= light.posRadius.w) { continue; }
      let t = 1.0 - dist / light.posRadius.w;
      attenuation = t * t;
    } else {
      attenuation = 1.0 / max(dist * dist, 1e-4);
    }
    let l = toLight / max(dist, 1e-4);
    let nlRaw = dot(n, l);
    let nlWrap = clamp((nlRaw + wrap) / (1.0 + wrap), 0.0, 1.0);
    let nl = max(nlRaw, 0.0);
    let h = normalize(l + v);
    let nh = max(dot(n, h), 0.0);
    let vh = max(dot(v, h), 0.0);
    let fresnel = f0 + (vec3f(1.0) - f0) * pow(1.0 - vh, 5.0);
    let specular = distributionGgx(nh, a) * visibilitySmith(nv, nl, a) * fresnel;
    radiance += light.color.rgb * attenuation * (diffuse / PI * nlWrap + specular * nl);
  }
  // Hemispheric ambient; metals reflect it along the reflection vector (a two-colour environment).
  let ambient = mix(draw.ambientDown.rgb, draw.ambientUp.rgb, n.y * 0.5 + 0.5);
  let r = reflect(-v, n);
  let environment = mix(mix(draw.ambientDown.rgb, draw.ambientUp.rgb, clamp(r.y * 0.5 + 0.5, 0.0, 1.0)), ambient, roughness);
  let ambientFresnel = f0 + (max(vec3f(1.0 - roughness), f0) - f0) * pow(1.0 - nv, 5.0);
  radiance += ambient * diffuse + environment * ambientFresnel;
  radiance += spellGlow(input.world, n, v, albedo);
  var out: Surface;
  out.normal = n;
  out.color = agx(radiance * draw.params.x);
  return out;
}

struct ImposterOutput {
  @location(0) color: vec4f,  // premultiplied (opaque mesh: alpha 1, the MSAA resolve makes the soft edge)
  @location(1) normal: vec4f, // tile-space normal * 0.5 + 0.5
};

@fragment fn fragmentImposter(input: VertexOutput, @builtin(front_facing) front: bool) -> ImposterOutput {
  let surface = shade(input, front);
  var out: ImposterOutput;
  out.color = vec4f(surface.color, 1.0);
  let tileNormal = normalize((draw.normalToOut * vec4f(surface.normal, 0.0)).xyz);
  out.normal = vec4f(tileNormal * 0.5 + vec3f(0.5), 1.0);
  return out;
}

@fragment fn fragmentWorld(input: VertexOutput, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  var color = shade(input, front).color;
  if (draw.params.w > 0.5) { color = pow(color, vec3f(2.2)); } // *Srgb target: the hardware encodes
  return vec4f(color, 1.0);
}
)";

/**
 * World pass variant (render3d/world_renderer.hpp): compiled after kWorldCommonWgsl (group 0: the world frame
 * uniforms and occluder grid) with this file's bindings moved to group 1, so skinned actors and the viewmodel get the
 * world's own lights with ray-traced shadows. The world's albedos are display-encoded atlas texels, so actors encode
 * their linear material colour the same way before worldShade(). The viewmodel uses its own GGX shading of the same
 * lights (metal needs real specular), with the player's light moved over the shoulder as a key (lights[0]), a fill,
 * a rim light and a small reflection term scaled by the light around the weapon.
 */
constexpr char WorldLitFragmentSource[] = R"(
fn viewModelBrdf(n: vec3f, v: vec3f, l: vec3f, diffuse: vec3f, f0: vec3f, a: f32, nv: f32) -> vec3f {
  let nlRaw = dot(n, l);
  let nl = max(nlRaw, 0.0);
  let wrap = clamp((nlRaw + 0.3) / 1.3, 0.0, 1.0);
  let h = normalize(l + v);
  let nh = max(dot(n, h), 0.0);
  let vh = max(dot(v, h), 0.0);
  let fresnel = f0 + (vec3f(1.0) - f0) * pow(1.0 - vh, 5.0);
  let specular = distributionGgx(nh, a) * visibilitySmith(nv, nl, a) * fresnel;
  // The world's lights are calibrated so a lit surface shows albedo * light (no 1 / pi): scale the BRDF to match.
  return diffuse * wrap + specular * (nl * PI);
}

fn viewModelShade(p: vec3f, s: SurfaceInput) -> vec3f {
  let n = s.n;
  let v = normalize(draw.eye.xyz - p);
  let nv = max(dot(n, v), 1e-3);
  let roughness = clamp(s.roughness, 0.06, 1.0);
  let a = roughness * roughness;
  let f0 = mix(vec3f(0.04 * s.specular), s.albedo, s.metallic);
  let diffuse = s.albedo * (1.0 - s.metallic);
  var radiance = vec3f(0.0);
  var level = 0.0; // light reaching the weapon from any direction: scales the reflection term
  let count = min(frame.counts.x, 32u);
  for (var i = select(0u, 1u, draw.visible.w == 2u); i < count; i++) {
    let light = frame.lights[i];
    let toLight = light.posRadius.xyz - p;
    let dist = length(toLight);
    if (dist >= light.posRadius.w) { continue; }
    let falloff = 1.0 - dist / light.posRadius.w;
    let visibility = shadowRay(p + n * 0.01, light.posRadius.xyz);
    if (visibility <= 0.0) { continue; }
    let e = light.colorIntensity.rgb * (light.colorIntensity.a * falloff * (2.0 - falloff) * visibility);
    radiance += e * viewModelBrdf(n, v, toLight / max(dist, 1e-4), diffuse, f0, a, nv);
    level += dot(e, vec3f(0.3333));
  }
  let own = min(u32(draw.params.y), 16u);
  for (var i = 0u; i < own; i++) {
    let light = draw.lights[i];
    let toLight = light.posRadius.xyz - p;
    let dist = length(toLight);
    var attenuation = 1.0;
    if (light.posRadius.w > 0.0) {
      if (dist >= light.posRadius.w) { continue; }
      let f = 1.0 - dist / light.posRadius.w;
      attenuation = f * (2.0 - f);
    }
    let e = light.color.rgb * attenuation;
    radiance += e * viewModelBrdf(n, v, toLight / max(dist, 1e-4), diffuse, f0, a, nv);
    level += dot(e, vec3f(0.3333));
  }
  // Fill, so the weapon is never pitch black, plus the town's moonlit ambient.
  let fill = draw.ambientDown.rgb + vec3f(frame.params.x * 0.6);
  radiance += fill * diffuse;
  // Reflection of a dim room: dark floor, lighter walls at the horizon, a warm hot spot overhead; blurred by roughness.
  let r = reflect(-v, n);
  var room = vec3f(0.04, 0.035, 0.03) + vec3f(0.28, 0.25, 0.21) * exp(-abs(r.y) * 3.0)
           + vec3f(0.55, 0.48, 0.38) * smoothstep(0.45, 0.95, r.y);
  room = mix(room, vec3f(dot(room, vec3f(0.3333))), roughness);
  let envFresnel = f0 + (max(vec3f(1.0 - roughness), f0) - f0) * pow(1.0 - nv, 5.0);
  radiance += room * envFresnel * (0.3 * level + 4.0 * dot(fill, vec3f(0.3333))) * (1.0 - 0.5 * roughness);
  // Rim: a thin edge on the side facing the rim light, so the silhouette reads against dark walls.
  let rim = pow(1.0 - nv, 3.0) * smoothstep(-0.2, 0.6, dot(n, draw.rim.xyz));
  radiance += draw.ambientUp.rgb * (rim * draw.rim.w * (0.5 + 0.5 * s.metallic));
  radiance += spellGlow(p, n, v, s.albedo);
  return radiance * draw.params.x;
}

@fragment fn fragmentWorldLit(input: VertexOutput, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  let s = surfaceInput(input, front);
  if (draw.visible.w != 0u) {
    // Soft shoulder above 0.8, then the world's display encoding.
    let linear = max(viewModelShade(input.world, s), vec3f(0.0));
    let shoulder = select(linear, vec3f(0.8) + 0.25 * (vec3f(1.0) - exp(-(linear - vec3f(0.8)) * 4.0)), linear > vec3f(0.8));
    return vec4f(pow(shoulder, vec3f(1.0 / 2.2)), 1.0);
  }
  let display = pow(s.albedo, vec3f(1.0 / 2.2));
  return vec4f(worldShade(input.world, s.n, display, false), 1.0);
}
)";

/**
 * Viewmodel effects (own pipeline, premultiplied-alpha blending, no depth write): the sword's swept motion-blur
 * ribbon (mode 0, a triangle strip through the blade's root / tip positions over the last few hundredths of the
 * swing) and the cast glow orb in the off hand (mode 1, a camera-facing quad). Positions are camera meters.
 */
constexpr char EffectsShaderSource[] = R"(
struct Fx {
  viewProj: mat4x4f, // camera meters -> clip
  color: vec4f,      // rgb, intensity
  params: vec4f,     // x: mode (0 ribbon, 1 orb), y: ribbon sample count, z: orb radius (meters), w: time (s)
  centre: vec4f,     // orb centre (camera meters)
  samples: array<vec4f, 64>, // ribbon: (root xyz, alpha), (tip xyz, alpha) per sample, newest first
};
@group(0) @binding(0) var<uniform> fx: Fx;

struct FxOut {
  @builtin(position) clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) alpha: f32,
};

@vertex fn fxVertex(@builtin(vertex_index) index: u32) -> FxOut {
  var out: FxOut;
  if (fx.params.x < 0.5) {
    let count = max(u32(fx.params.y), 2u);
    let i = min(index / 2u, count - 1u);
    let side = index % 2u;
    let sample = fx.samples[i * 2u + side];
    out.clip = fx.viewProj * vec4f(sample.xyz, 1.0);
    out.uv = vec2f(f32(i) / f32(count - 1u), f32(side));
    out.alpha = sample.w;
  } else {
    let corner = vec2f(f32(index & 1u), f32(index >> 1u)) * 2.0 - vec2f(1.0);
    out.clip = fx.viewProj * vec4f(fx.centre.xyz + vec3f(corner * fx.params.z, 0.0), 1.0);
    out.uv = corner;
    out.alpha = 1.0;
  }
  return out;
}

@fragment fn fxFragment(input: FxOut) -> @location(0) vec4f {
  if (fx.params.x < 0.5) {
    // Brightest along the edge the tip sweeps, fading towards the root and with age; a faint dark smear under it.
    let across = input.uv.y;
    let streak = input.alpha * smoothstep(0.05, 0.7, across) * (0.25 + 0.75 * across * across);
    return vec4f(fx.color.rgb * fx.color.a * streak, streak * 0.35);
  }
  let d = length(input.uv);
  let falloff = max(1.0 - d, 0.0);
  let flicker = 0.88 + 0.12 * sin(fx.params.w * 21.0 + d * 7.0) * sin(fx.params.w * 13.0);
  let core = pow(falloff, 5.0) * 2.2;
  let halo = pow(falloff, 1.6) * 0.9;
  return vec4f((fx.color.rgb * halo + mix(fx.color.rgb, vec3f(1.0), 0.6) * core) * fx.color.a * flicker, 0.0);
}
)";

struct alignas(16) GpuLight {
	std::array<float, 4> posRadius;
	std::array<float, 4> color;
};

struct alignas(16) GpuDraw {
	std::array<float, 16> viewProj;
	std::array<float, 16> model;
	std::array<float, 16> normalToOut;
	std::array<float, 4> eye;
	std::array<float, 4> params;
	std::array<float, 4> ambientUp;
	std::array<float, 4> ambientDown;
	std::array<uint32_t, 4> visible;
	std::array<float, 4> rim;        // viewmodel: world direction towards the rim light, strength
	std::array<float, 4> glow;       // spell glow colour, strength
	std::array<float, 4> glowCentre; // spell glow centre (world), radius
	std::array<GpuLight, 16> lights;
	std::array<std::array<float, 16>, 64> palette;
};
static_assert(sizeof(GpuDraw) == 4928);
constexpr uint64_t DrawSlotStride = 5120; // 256-aligned dynamic offsets
constexpr uint32_t DrawSlots = 64;

struct GpuMaterial {
	std::array<float, 4> baseColor;
	std::array<float, 4> params; // metallic, roughness, specular, base colour texture (0 none, 1 body, 2 equipment)
	std::array<float, 4> maps;   // normal map, metallic-roughness map, normal scale, unused
};

/** One viewmodel effect draw (EffectsShaderSource `Fx`). */
struct alignas(16) GpuFx {
	std::array<float, 16> viewProj;
	std::array<float, 4> color;
	std::array<float, 4> params;
	std::array<float, 4> centre;
	std::array<std::array<float, 4>, 64> samples;
};
static_assert(sizeof(GpuFx) == 1136);
constexpr uint64_t FxSlotStride = 1280; // 256-aligned dynamic offsets
constexpr uint32_t FxSlots = 8;
constexpr int TrailSamples = 32;

struct PipelineKey {
	WGPUTextureFormat color;
	WGPUTextureFormat depth;
	uint32_t samples;
	bool imposter;
	bool worldLit = false; // lit by the world pass's frame uniforms (group 0), see WorldLitFragmentSource
	bool operator==(const PipelineKey &) const = default;
};

struct ImposterSlot {
	int width = 0;
	int height = 0;
	WGPUTexture texture = nullptr; // 2 width x height: [colour | normal]
	WGPUTextureView view = nullptr;
};

/** 4x MSAA colour + normal + depth and their single-sample resolves, shared by the slots rendered one after another. */
struct RenderTargets {
	int width = 0;
	int height = 0;
	WGPUTexture msaaColor = nullptr, msaaNormal = nullptr, msaaDepth = nullptr, resolveColor = nullptr, resolveNormal = nullptr;
	WGPUTextureView msaaColorView = nullptr, msaaNormalView = nullptr, msaaDepthView = nullptr, resolveColorView = nullptr, resolveNormalView = nullptr;
};

struct Gpu {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	bool failed = false;
	WGPUBuffer vertexBuffer = nullptr;
	WGPUBuffer indexBuffer = nullptr;
	WGPUBuffer materialBuffer = nullptr;
	WGPUBuffer drawBuffer = nullptr;
	WGPUTexture baseColor = nullptr;
	WGPUTextureView baseColorView = nullptr;
	/** Equipment atlases: base colour (sRGB), tangent-space normal and metallic-roughness (linear). */
	std::array<WGPUTexture, 3> equipTextures {};
	std::array<WGPUTextureView, 3> equipViews {};
	WGPUSampler sampler = nullptr;
	/** Viewmodel effects (motion trail, cast glow): own module, layout and per-frame uniform slots. */
	WGPUShaderModule fxShader = nullptr;
	WGPUBindGroupLayout fxBindGroupLayout = nullptr;
	WGPUPipelineLayout fxPipelineLayout = nullptr;
	WGPUBuffer fxBuffer = nullptr;
	WGPUBindGroup fxBindGroup = nullptr;
	std::vector<std::pair<std::pair<WGPUTextureFormat, WGPUTextureFormat>, WGPURenderPipeline>> fxPipelines;
	uint32_t fxSlotsUsed = 0;
	WGPUShaderModule shader = nullptr;
	WGPUShaderModule worldShader = nullptr;              // kWorldCommonWgsl + this file's shader at group 1
	WGPUBindGroupLayout worldBindGroupLayout = nullptr;  // group 0 of the world-lit pipelines
	WGPUPipelineLayout worldPipelineLayout = nullptr;
	WGPUBindGroup worldBindGroup = nullptr;              // the world pass's frame uniforms + occluder grid
	WGPUBuffer worldFrameBuffer = nullptr;
	WGPUBuffer worldOccluderBuffer = nullptr;
	bool rendererRegistered = false;
	WGPUBindGroupLayout bindGroupLayout = nullptr;
	WGPUPipelineLayout pipelineLayout = nullptr;
	WGPUBindGroup bindGroup = nullptr;
	std::vector<std::pair<PipelineKey, WGPURenderPipeline>> pipelines;
	uint32_t slotsUsed = 0;
	bool loggedSlots = false;
	/** SkinnedReady() was asked: create the GPU resources at the next SkinnedPrepareFrame even without imposters. */
	bool wanted = false;
	/** The imposter pipeline compiling in the background from the first presented frame (title screen). */
	hd_gpu::AsyncPipeline imposterWarmup;
	bool warmupStarted = false;

	RenderTargets targets;                  // imposters (size x size)
	std::array<ImposterSlot, MAX_PLRS> slots;
	std::array<SkinnedImposterOutput, MAX_PLRS> outputs;
} G;

constexpr uint32_t ImposterSamples = 4;
constexpr WGPUTextureFormat ImposterFormat = WGPUTextureFormat_RGBA8Unorm;
constexpr WGPUTextureFormat ImposterDepthFormat = WGPUTextureFormat_Depth24Plus;

WGPUStringView View(const char *text)
{
	return { text, WGPU_STRLEN };
}

/**
 * Validation errors while creating the shader, the pipelines or the resources are caught in an error scope instead of
 * reaching the presenter's uncaptured-error handler (which treats them as a renderer failure): the warrior then falls
 * back to its prerendered sprites for the rest of the session.
 */
void PushScope()
{
	wgpuDevicePushErrorScope(G.device, WGPUErrorFilter_Validation);
}

void PopScope(const char *what)
{
	WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
	callback.mode = WGPUCallbackMode_AllowSpontaneous;
	callback.userdata1 = const_cast<char *>(what);
	callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void *userdata1, void *) {
		if (status != WGPUPopErrorScopeStatus_Success || type == WGPUErrorType_NoError)
			return;
		LogError("3D warrior: {} failed ({}); using the prerendered sprites", static_cast<const char *>(userdata1),
		    std::string_view(message.data != nullptr ? message.data : "", message.data != nullptr && message.length != WGPU_STRLEN ? message.length : 0));
		G.failed = true;
		GpuBroken = true;
	};
	wgpuDevicePopErrorScope(G.device, callback);
}

void ReleaseTexture(WGPUTexture &texture, WGPUTextureView &view)
{
	if (view != nullptr)
		wgpuTextureViewRelease(view);
	if (texture != nullptr) {
		wgpuTextureDestroy(texture);
		wgpuTextureRelease(texture);
	}
	texture = nullptr;
	view = nullptr;
}

void ReleaseBuffer(WGPUBuffer &buffer)
{
	if (buffer != nullptr) {
		wgpuBufferDestroy(buffer);
		wgpuBufferRelease(buffer);
	}
	buffer = nullptr;
}

void ReleaseTargets(RenderTargets &t)
{
	ReleaseTexture(t.msaaColor, t.msaaColorView);
	ReleaseTexture(t.msaaNormal, t.msaaNormalView);
	ReleaseTexture(t.msaaDepth, t.msaaDepthView);
	ReleaseTexture(t.resolveColor, t.resolveColorView);
	ReleaseTexture(t.resolveNormal, t.resolveNormalView);
	t.width = 0;
	t.height = 0;
}

WGPUBuffer CreateBuffer(const char *label, WGPUBufferUsage usage, uint64_t size, const void *data)
{
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage | WGPUBufferUsage_CopyDst;
	descriptor.size = (size + 3) & ~uint64_t { 3 };
	WGPUBuffer buffer = wgpuDeviceCreateBuffer(G.device, &descriptor);
	if (buffer != nullptr && data != nullptr)
		wgpuQueueWriteBuffer(G.queue, buffer, 0, data, size & ~uint64_t { 3 });
	return buffer;
}

WGPUTexture CreateTexture(const char *label, int width, int height, WGPUTextureFormat format, WGPUTextureUsage usage, uint32_t samples = 1, uint32_t mips = 1)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
	descriptor.format = format;
	descriptor.sampleCount = samples;
	descriptor.mipLevelCount = mips;
	return wgpuDeviceCreateTexture(G.device, &descriptor);
}

bool CreateModelResources()
{
	G.vertexBuffer = CreateBuffer("DamnationX skinned vertices", WGPUBufferUsage_Vertex, Model.vertices.size() * sizeof(SkinnedVertex), Model.vertices.data());
	G.indexBuffer = CreateBuffer("DamnationX skinned indices", WGPUBufferUsage_Index, Model.indices.size() * sizeof(uint32_t), Model.indices.data());
	std::vector<GpuMaterial> materials;
	for (const GltfMaterial &material : Model.materials) {
		float baseTexture = 0.0F;
		if (material.textured && material.baseColorImage == Textures.body && Textures.body >= 0)
			baseTexture = 1.0F;
		else if (material.textured && material.baseColorImage == Textures.equipBase && Textures.equipBase >= 0)
			baseTexture = 2.0F;
		const bool normalMap = material.normalImage >= 0 && material.normalImage == Textures.equipNormal;
		const bool mrMap = material.metallicRoughnessImage >= 0 && material.metallicRoughnessImage == Textures.equipMr;
		materials.push_back({ { material.baseColor[0], material.baseColor[1], material.baseColor[2], material.baseColor[3] },
		    { material.metallic, material.roughness, material.specular, baseTexture },
		    { normalMap ? 1.0F : 0.0F, mrMap ? 1.0F : 0.0F, material.normalScale, 0.0F } });
	}
	G.materialBuffer = CreateBuffer("DamnationX skinned materials", WGPUBufferUsage_Storage, materials.size() * sizeof(GpuMaterial), materials.data());
	G.drawBuffer = CreateBuffer("DamnationX skinned draws", WGPUBufferUsage_Uniform, DrawSlotStride * DrawSlots, nullptr);

	// Textures with their mips; 1 x 1 stand-ins (white, flat normal, white metallic-roughness) for the missing ones.
	const auto upload = [](const char *label, int image, std::array<uint8_t, 4> fallbackTexel, WGPUTextureFormat format, WGPUTexture &texture,
	                        WGPUTextureView &view) {
		MipChain fallback;
		const MipChain *levels = image >= 0 ? &Images[static_cast<size_t>(image)] : nullptr;
		if (levels == nullptr || levels->empty()) {
			fallback.push_back({ 1, 1, { fallbackTexel[0], fallbackTexel[1], fallbackTexel[2], fallbackTexel[3] } });
			levels = &fallback;
		}
		texture = CreateTexture(label, (*levels)[0].width, (*levels)[0].height, format, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1,
		    static_cast<uint32_t>(levels->size()));
		if (texture == nullptr)
			return;
		for (size_t level = 0; level < levels->size(); ++level) {
			const ImageLevel &source = (*levels)[level];
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = texture;
			destination.mipLevel = static_cast<uint32_t>(level);
			WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			layout.bytesPerRow = static_cast<uint32_t>(source.width * 4);
			layout.rowsPerImage = static_cast<uint32_t>(source.height);
			const WGPUExtent3D extent = { static_cast<uint32_t>(source.width), static_cast<uint32_t>(source.height), 1 };
			wgpuQueueWriteTexture(G.queue, &destination, source.pixels.data(), source.pixels.size(), &layout, &extent);
		}
		view = wgpuTextureCreateView(texture, nullptr);
	};
	upload("DamnationX skinned base colour", Textures.body, { 255, 255, 255, 255 }, WGPUTextureFormat_RGBA8UnormSrgb, G.baseColor, G.baseColorView);
	upload("DamnationX equipment base colour", Textures.equipBase, { 255, 255, 255, 255 }, WGPUTextureFormat_RGBA8UnormSrgb, G.equipTextures[0], G.equipViews[0]);
	upload("DamnationX equipment normal", Textures.equipNormal, { 128, 128, 255, 255 }, WGPUTextureFormat_RGBA8Unorm, G.equipTextures[1], G.equipViews[1]);
	upload("DamnationX equipment metallic-roughness", Textures.equipMr, { 255, 255, 255, 255 }, WGPUTextureFormat_RGBA8Unorm, G.equipTextures[2], G.equipViews[2]);
	if (G.baseColorView == nullptr || G.equipViews[0] == nullptr || G.equipViews[1] == nullptr || G.equipViews[2] == nullptr)
		return false;

	WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
	samplerDescriptor.addressModeU = WGPUAddressMode_Repeat;
	samplerDescriptor.addressModeV = WGPUAddressMode_Repeat;
	samplerDescriptor.magFilter = WGPUFilterMode_Linear;
	samplerDescriptor.minFilter = WGPUFilterMode_Linear;
	samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Linear;
	samplerDescriptor.maxAnisotropy = 4;
	G.sampler = wgpuDeviceCreateSampler(G.device, &samplerDescriptor);

	WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	wgsl.code = { ShaderSource, WGPU_STRLEN };
	WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	moduleDescriptor.nextInChain = &wgsl.chain;
	moduleDescriptor.label = View("DamnationX skinned mesh shader");
	G.shader = wgpuDeviceCreateShaderModule(G.device, &moduleDescriptor);

	std::array<WGPUBindGroupLayoutEntry, 7> layoutEntries {};
	layoutEntries.fill(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
	layoutEntries[0].binding = 0;
	layoutEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	layoutEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
	layoutEntries[0].buffer.hasDynamicOffset = true;
	layoutEntries[0].buffer.minBindingSize = sizeof(GpuDraw);
	layoutEntries[1].binding = 1;
	layoutEntries[1].visibility = WGPUShaderStage_Fragment;
	layoutEntries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
	layoutEntries[2].binding = 2;
	layoutEntries[2].visibility = WGPUShaderStage_Fragment;
	layoutEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
	layoutEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
	layoutEntries[3].binding = 3;
	layoutEntries[3].visibility = WGPUShaderStage_Fragment;
	layoutEntries[3].sampler.type = WGPUSamplerBindingType_Filtering;
	for (uint32_t i = 4; i < 7; ++i) {
		layoutEntries[i].binding = i;
		layoutEntries[i].visibility = WGPUShaderStage_Fragment;
		layoutEntries[i].texture.sampleType = WGPUTextureSampleType_Float;
		layoutEntries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
	}
	WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	layoutDescriptor.label = View("DamnationX skinned layout");
	layoutDescriptor.entryCount = layoutEntries.size();
	layoutDescriptor.entries = layoutEntries.data();
	G.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(G.device, &layoutDescriptor);
	WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
	pipelineLayoutDescriptor.bindGroupLayouts = &G.bindGroupLayout;
	G.pipelineLayout = wgpuDeviceCreatePipelineLayout(G.device, &pipelineLayoutDescriptor);

	std::array<WGPUBindGroupEntry, 7> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	entries[0].binding = 0;
	entries[0].buffer = G.drawBuffer;
	entries[0].size = sizeof(GpuDraw);
	entries[1].binding = 1;
	entries[1].buffer = G.materialBuffer;
	entries[1].size = materials.size() * sizeof(GpuMaterial);
	entries[2].binding = 2;
	entries[2].textureView = G.baseColorView;
	entries[3].binding = 3;
	entries[3].sampler = G.sampler;
	for (uint32_t i = 4; i < 7; ++i) {
		entries[i].binding = i;
		entries[i].textureView = G.equipViews[i - 4];
	}
	WGPUBindGroupDescriptor bindGroupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bindGroupDescriptor.label = View("DamnationX skinned bind group");
	bindGroupDescriptor.layout = G.bindGroupLayout;
	bindGroupDescriptor.entryCount = entries.size();
	bindGroupDescriptor.entries = entries.data();
	G.bindGroup = wgpuDeviceCreateBindGroup(G.device, &bindGroupDescriptor);

	// World-lit variant: the world pass's frame uniforms and occluder grid at group 0, this shader at group 1.
	std::string worldSource = ShaderSource;
	for (size_t at = worldSource.find("@group(0)"); at != std::string::npos; at = worldSource.find("@group(0)", at))
		worldSource.replace(at, 9, "@group(1)");
	worldSource = std::string(kWorldCommonWgsl) + worldSource + WorldLitFragmentSource;
	WGPUShaderSourceWGSL worldWgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	worldWgsl.code = { worldSource.c_str(), WGPU_STRLEN };
	WGPUShaderModuleDescriptor worldModuleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	worldModuleDescriptor.nextInChain = &worldWgsl.chain;
	worldModuleDescriptor.label = View("DamnationX skinned mesh shader (world lit)");
	G.worldShader = wgpuDeviceCreateShaderModule(G.device, &worldModuleDescriptor);
	std::array<WGPUBindGroupLayoutEntry, 2> worldEntries {};
	worldEntries.fill(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
	worldEntries[0].binding = 0;
	worldEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	worldEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
	worldEntries[1].binding = 1;
	worldEntries[1].visibility = WGPUShaderStage_Fragment;
	worldEntries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
	WGPUBindGroupLayoutDescriptor worldLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	worldLayoutDescriptor.label = View("DamnationX skinned world frame layout");
	worldLayoutDescriptor.entryCount = worldEntries.size();
	worldLayoutDescriptor.entries = worldEntries.data();
	G.worldBindGroupLayout = wgpuDeviceCreateBindGroupLayout(G.device, &worldLayoutDescriptor);
	const std::array<WGPUBindGroupLayout, 2> worldLayouts { G.worldBindGroupLayout, G.bindGroupLayout };
	WGPUPipelineLayoutDescriptor worldPipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	worldPipelineLayoutDescriptor.bindGroupLayoutCount = worldLayouts.size();
	worldPipelineLayoutDescriptor.bindGroupLayouts = worldLayouts.data();
	G.worldPipelineLayout = wgpuDeviceCreatePipelineLayout(G.device, &worldPipelineLayoutDescriptor);

	return G.worldShader != nullptr && G.worldPipelineLayout != nullptr && G.vertexBuffer != nullptr && G.indexBuffer != nullptr && G.materialBuffer != nullptr && G.drawBuffer != nullptr
	    && G.baseColorView != nullptr && G.sampler != nullptr && G.shader != nullptr && G.pipelineLayout != nullptr && G.bindGroup != nullptr;
}

/** GPU resources for the current device; false when they cannot exist (logged once). */
bool EnsureGpu(WGPUDevice device, WGPUQueue queue)
{
	if (device == nullptr)
		return false;
	if (G.device != device) {
		SkinnedReleaseGpu();
		G.device = device;
		G.queue = queue;
	}
	if (G.failed)
		return false;
	if (G.bindGroup != nullptr)
		return true;
	if (!SkinnedModelLoaded())
		return false;
	const auto start = std::chrono::steady_clock::now();
	PushScope();
	const bool created = CreateModelResources();
	PopScope("creating the GPU resources");
	if (!created) {
		LogError("3D warrior: could not create the GPU resources");
		G.failed = true;
		GpuBroken = true;
		return false;
	}
	Log("3D warrior: GPU resources ready ({} ms)", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	return true;
}

/** Builds the pipeline descriptor of `key` and hands it to `use` (the descriptor points at locals). */
template <typename Use>
void WithPipelineDescriptor(const PipelineKey &key, Use &&use)
{
	std::array<WGPUVertexAttribute, 6> attributes {};
	attributes.fill(WGPU_VERTEX_ATTRIBUTE_INIT);
	attributes[0] = { nullptr, WGPUVertexFormat_Float32x3, offsetof(SkinnedVertex, position), 0 };
	attributes[1] = { nullptr, WGPUVertexFormat_Float32x3, offsetof(SkinnedVertex, normal), 1 };
	attributes[2] = { nullptr, WGPUVertexFormat_Float32x2, offsetof(SkinnedVertex, uv), 2 };
	attributes[3] = { nullptr, WGPUVertexFormat_Uint8x4, offsetof(SkinnedVertex, joints), 3 };
	attributes[4] = { nullptr, WGPUVertexFormat_Float32x4, offsetof(SkinnedVertex, weights), 4 };
	attributes[5] = { nullptr, WGPUVertexFormat_Uint32, offsetof(SkinnedVertex, packed), 5 };
	WGPUVertexBufferLayout vertexLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vertexLayout.stepMode = WGPUVertexStepMode_Vertex;
	vertexLayout.arrayStride = sizeof(SkinnedVertex);
	vertexLayout.attributeCount = attributes.size();
	vertexLayout.attributes = attributes.data();

	std::array<WGPUColorTargetState, 2> targets {};
	targets.fill(WGPU_COLOR_TARGET_STATE_INIT);
	targets[0].format = key.color;
	targets[1].format = key.color;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = key.worldLit ? G.worldShader : G.shader;
	fragment.entryPoint = View(key.imposter ? "fragmentImposter" : (key.worldLit ? "fragmentWorldLit" : "fragmentWorld"));
	fragment.targetCount = key.imposter ? 2 : 1;
	fragment.targets = targets.data();

	WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
	depth.format = key.depth;
	depth.depthWriteEnabled = WGPUOptionalBool_True;
	depth.depthCompare = WGPUCompareFunction_Less;

	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = View(key.imposter ? "DamnationX skinned imposter pipeline" : "DamnationX skinned world pipeline");
	descriptor.layout = key.worldLit ? G.worldPipelineLayout : G.pipelineLayout;
	descriptor.vertex.module = key.worldLit ? G.worldShader : G.shader;
	descriptor.vertex.entryPoint = View("vertexMain");
	descriptor.vertex.bufferCount = 1;
	descriptor.vertex.buffers = &vertexLayout;
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	descriptor.primitive.cullMode = WGPUCullMode_None; // the materials are double-sided
	descriptor.depthStencil = key.depth != WGPUTextureFormat_Undefined ? &depth : nullptr;
	descriptor.multisample.count = key.samples;
	descriptor.fragment = &fragment;
	use(descriptor);
}

constexpr PipelineKey ImposterKey { WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_Depth24Plus, 4, true, false };

WGPURenderPipeline GetPipeline(const PipelineKey &key)
{
	for (const auto &[existing, pipeline] : G.pipelines) {
		if (existing == key)
			return pipeline;
	}
	if (key == ImposterKey && G.imposterWarmup.pending()) {
		WGPURenderPipeline pipeline = nullptr;
		if (G.imposterWarmup.take(pipeline) && pipeline != nullptr) {
			G.pipelines.emplace_back(key, pipeline);
			return pipeline;
		}
		G.imposterWarmup.abandon(); // still compiling: create it now
	}
	const auto start = std::chrono::steady_clock::now();
	WGPURenderPipeline pipeline = nullptr;
	WithPipelineDescriptor(key, [&](const WGPURenderPipelineDescriptor &descriptor) {
		PushScope();
		pipeline = wgpuDeviceCreateRenderPipeline(G.device, &descriptor);
		PopScope(key.imposter ? "creating the imposter pipeline" : "creating the world pipeline");
	});
	if (pipeline == nullptr) {
		LogError("3D warrior: pipeline creation failed");
		GpuBroken = true;
	} else {
		Log("3D warrior: {} pipeline ({} ms)", key.imposter ? "imposter" : "world",
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	}
	G.pipelines.emplace_back(key, pipeline);
	return pipeline;
}

/** Next per-frame uniform slot, or -1 when the frame used them all. */
int AllocateSlot()
{
	if (G.slotsUsed >= DrawSlots) {
		if (!G.loggedSlots)
			LogError("3D warrior: more than {} skinned draws in a frame", DrawSlots);
		G.loggedSlots = true;
		return -1;
	}
	return static_cast<int>(G.slotsUsed++);
}

void CopyMatrix(std::array<float, 16> &out, const Mat4 &matrix)
{
	out = matrix.m;
}

/** Common uniform fields: matrices, palette, visibility. */
void FillDraw(GpuDraw &draw, const Pose &pose, const Mat4 &viewProj, const Mat4 &model)
{
	ComputePosePalette(pose);
	CopyMatrix(draw.viewProj, viewProj);
	CopyMatrix(draw.model, model);
	CopyMatrix(draw.normalToOut, Mat4::Identity());
	for (size_t i = 0; i < draw.palette.size(); ++i)
		draw.palette[i] = i < ScratchPalette.size() ? ScratchPalette[i].m : Mat4::Identity().m;
	draw.visible = { pose.visibleParts, 0, 0, 0 };
}

void EncodeDraw(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline, int slot, WGPUBindGroup worldGroup = nullptr)
{
	const uint32_t offset = static_cast<uint32_t>(slot * DrawSlotStride);
	wgpuRenderPassEncoderSetPipeline(pass, pipeline);
	if (worldGroup != nullptr) {
		wgpuRenderPassEncoderSetBindGroup(pass, 0, worldGroup, 0, nullptr);
		wgpuRenderPassEncoderSetBindGroup(pass, 1, G.bindGroup, 1, &offset);
	} else {
		wgpuRenderPassEncoderSetBindGroup(pass, 0, G.bindGroup, 1, &offset);
	}
	wgpuRenderPassEncoderSetVertexBuffer(pass, 0, G.vertexBuffer, 0, Model.vertices.size() * sizeof(SkinnedVertex));
	wgpuRenderPassEncoderSetIndexBuffer(pass, G.indexBuffer, WGPUIndexFormat_Uint32, 0, Model.indices.size() * sizeof(uint32_t));
	wgpuRenderPassEncoderDrawIndexed(pass, static_cast<uint32_t>(Model.indices.size()), 1, 0, 0, 0);
}

// ---- Imposters ----

bool EnsureTargets(RenderTargets &t, int width, int height)
{
	if (t.width == width && t.height == height && t.resolveNormalView != nullptr)
		return true;
	ReleaseTargets(t);
	const auto make = [&](const char *label, WGPUTextureFormat format, WGPUTextureUsage usage, uint32_t samples, WGPUTexture &texture, WGPUTextureView &view) {
		texture = CreateTexture(label, width, height, format, usage, samples);
		view = texture != nullptr ? wgpuTextureCreateView(texture, nullptr) : nullptr;
	};
	make("DamnationX imposter MSAA colour", ImposterFormat, WGPUTextureUsage_RenderAttachment, ImposterSamples, t.msaaColor, t.msaaColorView);
	make("DamnationX imposter MSAA normal", ImposterFormat, WGPUTextureUsage_RenderAttachment, ImposterSamples, t.msaaNormal, t.msaaNormalView);
	make("DamnationX imposter MSAA depth", ImposterDepthFormat, WGPUTextureUsage_RenderAttachment, ImposterSamples, t.msaaDepth, t.msaaDepthView);
	make("DamnationX imposter colour", ImposterFormat, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc, 1, t.resolveColor, t.resolveColorView);
	make("DamnationX imposter normal", ImposterFormat, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc, 1, t.resolveNormal, t.resolveNormalView);
	t.width = width;
	t.height = height;
	return t.msaaColorView != nullptr && t.msaaNormalView != nullptr && t.msaaDepthView != nullptr && t.resolveColorView != nullptr && t.resolveNormalView != nullptr;
}

bool EnsureSlot(ImposterSlot &slot, int width, int height)
{
	if (slot.width == width && slot.height == height && slot.view != nullptr)
		return true;
	ReleaseTexture(slot.texture, slot.view);
	slot.texture = CreateTexture("DamnationX imposter", width * 2, height, ImposterFormat, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
	slot.view = slot.texture != nullptr ? wgpuTextureCreateView(slot.texture, nullptr) : nullptr;
	slot.width = width;
	slot.height = height;
	return slot.view != nullptr;
}

/** One MSAA pass into `t` (colour + normal + depth cleared), `encode` records its draws; then [colour | normal] -> slot. */
template <typename Encode>
void RenderToSlot(WGPUCommandEncoder encoder, const RenderTargets &t, const ImposterSlot &slot, Encode &&encode)
{
	std::array<WGPURenderPassColorAttachment, 2> colors {};
	colors.fill(WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT);
	colors[0].view = t.msaaColorView;
	colors[0].resolveTarget = t.resolveColorView;
	colors[1].view = t.msaaNormalView;
	colors[1].resolveTarget = t.resolveNormalView;
	for (WGPURenderPassColorAttachment &color : colors) {
		color.loadOp = WGPULoadOp_Clear;
		color.storeOp = WGPUStoreOp_Discard;
		color.clearValue = { 0.0, 0.0, 0.0, 0.0 };
	}
	WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	depth.view = t.msaaDepthView;
	depth.depthLoadOp = WGPULoadOp_Clear;
	depth.depthStoreOp = WGPUStoreOp_Discard;
	depth.depthClearValue = 1.0F;
	WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	passDescriptor.label = View("DamnationX imposter pass");
	passDescriptor.colorAttachmentCount = colors.size();
	passDescriptor.colorAttachments = colors.data();
	passDescriptor.depthStencilAttachment = &depth;
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
	encode(pass);
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	const WGPUExtent3D extent = { static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height), 1 };
	for (int half = 0; half < 2; ++half) {
		WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		source.texture = half == 0 ? t.resolveColor : t.resolveNormal;
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = slot.texture;
		destination.origin = { static_cast<uint32_t>(half * t.width), 0, 0 };
		wgpuCommandEncoderCopyTextureToTexture(encoder, &source, &destination, &extent);
	}
}

/** The bake camera: orthographic, 35 degrees down, looking at -z, the ground origin on pixel row 424 of 640. */
Mat4 ImposterViewProj()
{
	const float elevation = BakeElevationDeg * Pi / 180.0F;
	const float s = std::sin(elevation);
	const float c = std::cos(elevation);
	const Vec3 right { 1.0F, 0.0F, 0.0F };
	const Vec3 up { 0.0F, c, -s };
	const Vec3 back { 0.0F, s, c };
	const Mat4 view = skm::ViewFromBasis(right, up, back, back * 10.0F);
	const float half = BakeOrthoWidth * 0.5F;
	const float centreUp = static_cast<float>(kImposterPivotY - kImposterCellSize / 2) / kImposterCellSize * BakeOrthoWidth;
	return skm::Orthographic(-half, half, centreUp - half, centreUp + half, 0.1F, 20.0F) * view;
}

/** Normals of the bake frame (x screen right, y up, z towards the camera = south) in tile space (x, y tiles, z up). */
Mat4 BakeToTile()
{
	constexpr float a = 0.70710678F;
	Mat4 m = Mat4::Zero();
	m(0, 0) = a;
	m(1, 0) = -a;
	m(2, 1) = 1.0F;
	m(0, 2) = a;
	m(1, 2) = a;
	m(3, 3) = 1.0F;
	return m;
}

void RenderImposter(WGPUCommandEncoder encoder, WGPURenderPipeline pipeline, const ImposterRequest &request, ImposterSlot &slot)
{
	const int drawSlot = AllocateSlot();
	if (drawSlot < 0)
		return;
	GpuDraw draw {};
	// Bake facing (s = 0, e = +90 degrees about +y) = world yaw - 45 degrees (the bake's +z is the screen's south).
	const float bakeYaw = request.pose.worldYaw - Pi / 4.0F;
	FillDraw(draw, request.pose, ImposterViewProj(), skm::RotationY(bakeYaw));
	CopyMatrix(draw.normalToOut, BakeToTile());
	const float elevation = BakeElevationDeg * Pi / 180.0F;
	draw.eye = { 0.0F, std::sin(elevation), std::cos(elevation), 1.0F };
	draw.params = { BakeExposure, static_cast<float>(BakeLights.size()), 0.15F, 0.0F };
	draw.ambientUp = { BakeAmbient[0], BakeAmbient[1], BakeAmbient[2], 0.0F };
	draw.ambientDown = draw.ambientUp;
	for (size_t i = 0; i < BakeLights.size(); ++i) {
		const BakeLight &light = BakeLights[i];
		const float intensity = light.power / Pi * BakeLightScale;
		draw.lights[i] = { { light.position.x, light.position.y, light.position.z, 0.0F }, { intensity, intensity, intensity, 0.0F } };
	}
	wgpuQueueWriteBuffer(G.queue, G.drawBuffer, static_cast<uint64_t>(drawSlot) * DrawSlotStride, &draw, sizeof(draw));
	RenderToSlot(encoder, G.targets, slot, [&](WGPURenderPassEncoder pass) { EncodeDraw(pass, pipeline, drawSlot); });
}

void LightsToGpu(GpuDraw &draw, const WebGpuHybridLight *lights, int lightCount, float scale)
{
	int count = 0;
	for (int i = 0; i < lightCount && count < 16; ++i) {
		const WebGpuHybridLight &light = lights[i];
		if (light.radius <= 0.0F || light.intensity <= 0.0F)
			continue;
		// Tile space (x, y, height) -> world (x, height, y); the shader's Lambert divides by pi, the hybrid pass does not.
		const float intensity = light.intensity * scale * Pi;
		draw.lights[static_cast<size_t>(count++)] = { { light.worldX, light.worldZ, light.worldY, light.radius },
			{ light.red * intensity, light.green * intensity, light.blue * intensity, 0.0F } };
	}
	draw.params[1] = static_cast<float>(count);
}

bool IsSrgb(WGPUTextureFormat format)
{
	return format == WGPUTextureFormat_RGBA8UnormSrgb || format == WGPUTextureFormat_BGRA8UnormSrgb;
}

// ---- First-person weapon viewmodel ----

/** Principal axes of a point set: `axes[0]` the largest spread, `axes[2]` the smallest. */
struct PrincipalAxes {
	Vec3 centroid;
	std::array<Vec3, 3> axes;
	bool valid = false;
};

PrincipalAxes ComputePrincipalAxes(const std::vector<Vec3> &points)
{
	PrincipalAxes result;
	if (points.size() < 3)
		return result;
	Vec3 centroid;
	for (const Vec3 &p : points)
		centroid = centroid + p;
	centroid = centroid * (1.0F / static_cast<float>(points.size()));
	double a[3][3] {};
	for (const Vec3 &p : points) {
		const double d[3] { p.x - centroid.x, p.y - centroid.y, p.z - centroid.z };
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				a[r][c] += d[r] * d[c];
	}
	// Jacobi eigenvalue iteration (3x3 symmetric).
	double v[3][3] { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	for (int sweep = 0; sweep < 32; ++sweep) {
		for (int p = 0; p < 2; ++p) {
			for (int q = p + 1; q < 3; ++q) {
				if (std::abs(a[p][q]) < 1e-12)
					continue;
				const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
				const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
				const double c = 1.0 / std::sqrt(t * t + 1.0);
				const double s = t * c;
				for (int k = 0; k < 3; ++k) {
					const double akp = a[k][p];
					const double akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 3; ++k) {
					const double apk = a[p][k];
					const double aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 3; ++k) {
					const double vkp = v[k][p];
					const double vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
		}
	}
	std::array<int, 3> order { 0, 1, 2 };
	std::sort(order.begin(), order.end(), [&](int x, int y) { return a[x][x] > a[y][y]; });
	result.centroid = centroid;
	for (int i = 0; i < 3; ++i) {
		const int column = order[static_cast<size_t>(i)];
		result.axes[static_cast<size_t>(i)] = skm::Normalize(Vec3 { static_cast<float>(v[0][column]), static_cast<float>(v[1][column]), static_cast<float>(v[2][column]) });
	}
	result.valid = true;
	return result;
}

/** A rigid frame: origin plus orthonormal axes (right-handed: z = x cross y). */
struct Frame {
	Vec3 origin;
	Vec3 x { 1.0F, 0.0F, 0.0F };
	Vec3 y { 0.0F, 1.0F, 0.0F };
	Vec3 z { 0.0F, 0.0F, 1.0F };
};

Mat4 FrameMatrix(const Frame &f)
{
	Mat4 m;
	m(0, 0) = f.x.x;
	m(1, 0) = f.x.y;
	m(2, 0) = f.x.z;
	m(0, 1) = f.y.x;
	m(1, 1) = f.y.y;
	m(2, 1) = f.y.z;
	m(0, 2) = f.z.x;
	m(1, 2) = f.z.y;
	m(2, 2) = f.z.z;
	m(0, 3) = f.origin.x;
	m(1, 3) = f.origin.y;
	m(2, 3) = f.origin.z;
	return m;
}

/** Frame whose z axis is `forward` and whose y axis is as close to `upHint` as possible. */
Frame MakeFrame(Vec3 origin, Vec3 forward, Vec3 upHint)
{
	Frame f;
	f.origin = origin;
	f.z = skm::Normalize(forward);
	Vec3 x = skm::Cross(upHint, f.z);
	if (skm::Dot(x, x) < 1e-6F)
		x = skm::Cross(Vec3 { 1.0F, 0.0F, 0.0F }, f.z);
	f.x = skm::Normalize(x);
	f.y = skm::Cross(f.z, f.x);
	return f;
}

/** An arm of the skeleton: palette entries (joints) and nodes from the clavicle to the hand. */
struct ArmChain {
	int shoulderJoint = -1, armJoint = -1, foreArmJoint = -1;
	int shoulderNode = -1, armNode = -1, foreArmNode = -1, handNode = -1;
};

/** Rotation taking unit direction `from` to unit direction `to` (the shortest arc). */
Mat4 RotationBetween(Vec3 from, Vec3 to)
{
	const Vec3 axis = skm::Cross(from, to);
	const float sine = std::sqrt(skm::Dot(axis, axis));
	if (sine < 1e-5F)
		return Mat4::Identity();
	const float angle = std::atan2(sine, skm::Dot(from, to));
	return skm::ComposeTrs({}, skm::AxisAngle(axis * (1.0F / sine), angle), { 1.0F, 1.0F, 1.0F });
}

/**
 * Two-bone IK for a viewmodel arm. `rigid` maps the arm pose (model space, ScratchGlobals) to camera meters with the
 * hand on its weapon; the hand stays there while the upper arm is re-aimed at a shoulder placed towards `shoulderHint`
 * (always within reach, slightly bent) and the elbow bends towards `pole`. Writes the upper arm, forearm and clavicle
 * palette entries (camera meters).
 */
void SolveArm(const ArmChain &c, const Mat4 &rigid, Vec3 shoulderHint, Vec3 pole, std::array<std::array<float, 16>, 64> &palette)
{
	if (c.armJoint < 0 || c.foreArmJoint < 0 || c.armNode < 0 || c.foreArmNode < 0 || c.handNode < 0)
		return;
	const Mat4 armOld = rigid * ScratchGlobals[static_cast<size_t>(c.armNode)];
	const Mat4 foreOld = rigid * ScratchGlobals[static_cast<size_t>(c.foreArmNode)];
	const Vec3 a0 = skm::TransformPoint(armOld, {});
	const Vec3 e0 = skm::TransformPoint(foreOld, {});
	const Vec3 w = skm::TransformPoint(rigid * ScratchGlobals[static_cast<size_t>(c.handNode)], {});
	const Vec3 upper = e0 - a0;
	const Vec3 lower = w - e0;
	const float l1 = std::sqrt(skm::Dot(upper, upper));
	const float l2 = std::sqrt(skm::Dot(lower, lower));
	if (l1 < 1e-4F || l2 < 1e-4F)
		return;
	// The shoulder goes towards the hint, as far as the arm reaches slightly bent; when the hint is further away the
	// upper arm stretches (up to 1.7 times the whole arm), which keeps its cut end out of the view for raised hands.
	const Vec3 toShoulder = skm::Normalize(shoulderHint - w);
	const float wanted = std::clamp(std::sqrt(skm::Dot(shoulderHint - w, shoulderHint - w)), (l1 + l2) * 0.93F, (l1 + l2) * 1.7F);
	const float stretchedUpper = std::max(l1, wanted / 0.93F - l2);
	const float stretch = stretchedUpper / l1;
	const float reach = (stretchedUpper + l2) * 0.93F;
	const Vec3 s = w + toShoulder * reach;
	const Vec3 u = toShoulder * -1.0F; // shoulder -> wrist
	const float cosA = std::clamp((stretchedUpper * stretchedUpper + reach * reach - l2 * l2) / (2.0F * stretchedUpper * reach), -1.0F, 1.0F);
	const float sinA = std::sqrt(std::max(0.0F, 1.0F - cosA * cosA));
	Vec3 bend = pole - u * skm::Dot(pole, u);
	if (skm::Dot(bend, bend) < 1e-6F)
		bend = Vec3 { 0.0F, -1.0F, 0.0F } - u * (-u.y);
	bend = skm::Normalize(bend);
	const Vec3 e = s + u * (stretchedUpper * cosA) + bend * (stretchedUpper * sinA);
	// Stretch along the old bone (about the shoulder joint), then turn the bone onto shoulder -> elbow.
	const Vec3 d0 = upper * (1.0F / l1);
	Mat4 alongBone = Mat4::Identity();
	for (int r = 0; r < 3; ++r) {
		const float dr = r == 0 ? d0.x : (r == 1 ? d0.y : d0.z);
		for (int col = 0; col < 3; ++col) {
			const float dc = col == 0 ? d0.x : (col == 1 ? d0.y : d0.z);
			alongBone(r, col) += (stretch - 1.0F) * dr * dc;
		}
	}
	const Mat4 armNew = skm::Translation(s) * RotationBetween(d0, skm::Normalize(e - s)) * alongBone * skm::Translation(a0 * -1.0F) * armOld;
	const Mat4 foreNew = skm::Translation(e) * RotationBetween(lower * (1.0F / l2), skm::Normalize(w - e)) * skm::Translation(e0 * -1.0F) * foreOld;
	palette[static_cast<size_t>(c.armJoint)] = (armNew * Model.inverseBind[static_cast<size_t>(c.armJoint)]).m;
	palette[static_cast<size_t>(c.foreArmJoint)] = (foreNew * Model.inverseBind[static_cast<size_t>(c.foreArmJoint)]).m;
	Mat4 armOldInverse;
	if (c.shoulderJoint >= 0 && c.shoulderNode >= 0 && skm::Inverse(armOld, armOldInverse)) {
		// The clavicle follows the upper arm, so vertices partly weighted to it stay on the shoulder.
		const Mat4 shoulderOld = rigid * ScratchGlobals[static_cast<size_t>(c.shoulderNode)];
		palette[static_cast<size_t>(c.shoulderJoint)] = (armNew * armOldInverse * shoulderOld * Model.inverseBind[static_cast<size_t>(c.shoulderJoint)]).m;
	}
}

/**
 * The viewmodel rig, built once from the model: which palette entries each hand keeps, and the weapon frames in their
 * own node space (sword: z along the blade, from the grip to the tip; shield: z along the face normal, away from the
 * arm, y "up" as held in the stand pose), plus the arm pose (stand clip, first key) the hands are drawn in.
 */
struct ViewModelRig {
	bool built = false;
	bool valid = false;
	std::array<uint32_t, 2> rightMask {};
	std::array<uint32_t, 2> leftMask {};
	ArmChain rightArm;
	ArmChain leftArm;
	int swordNode = -1;
	int shieldNode = -1;
	int leftHandNode = -1;
	Frame swordLocal;  // grip at the origin
	Frame shieldLocal; // face centre at the origin
	/** Distances along the blade from the grip centre (meters): where the blade leaves the guard, and the tip. */
	float bladeRoot = 0.15F;
	float bladeTip = 0.85F;
	Pose armPose;
} Rig;

void SetMaskBit(std::array<uint32_t, 2> &mask, size_t entry)
{
	if (entry < 64)
		mask[entry / 32] |= 1U << (entry % 32);
}

void BuildViewModelRig()
{
	Rig.built = true;
	const auto joint = [](std::string_view name) {
		const int node = Model.findNode(name);
		for (size_t j = 0; j < Model.joints.size(); ++j) {
			if (Model.joints[j] == node)
				return static_cast<int>(j);
		}
		return -1;
	};
	// Whole arms (upper arm included, bent by SolveArm towards a shoulder off screen): the cut sits at the shoulder,
	// outside the view, instead of a visible stump mid-forearm.
	for (const char *name : { "mixamorig:RightArm", "mixamorig:RightForeArm", "mixamorig:RightHand", "mixamorig:RightHandIndex1",
	         "mixamorig:RightHandIndex2", "mixamorig:RightHandIndex3", "mixamorig:RightHandIndex4" }) {
		if (const int j = joint(name); j >= 0)
			SetMaskBit(Rig.rightMask, static_cast<size_t>(j));
	}
	for (const char *name : { "mixamorig:LeftArm", "mixamorig:LeftForeArm", "mixamorig:LeftHand", "mixamorig:LeftHandIndex1",
	         "mixamorig:LeftHandIndex2", "mixamorig:LeftHandIndex3", "mixamorig:LeftHandIndex4" }) {
		if (const int j = joint(name); j >= 0)
			SetMaskBit(Rig.leftMask, static_cast<size_t>(j));
	}
	const auto chain = [&](const char *shoulder, const char *arm, const char *foreArm, const char *hand) {
		ArmChain c;
		c.shoulderJoint = joint(shoulder);
		c.armJoint = joint(arm);
		c.foreArmJoint = joint(foreArm);
		c.shoulderNode = Model.findNode(shoulder);
		c.armNode = Model.findNode(arm);
		c.foreArmNode = Model.findNode(foreArm);
		c.handNode = Model.findNode(hand);
		return c;
	};
	Rig.rightArm = chain("mixamorig:RightShoulder", "mixamorig:RightArm", "mixamorig:RightForeArm", "mixamorig:RightHand");
	Rig.leftArm = chain("mixamorig:LeftShoulder", "mixamorig:LeftArm", "mixamorig:LeftForeArm", "mixamorig:LeftHand");
	Rig.swordNode = Model.findNode("Sword_RIGHT");
	Rig.shieldNode = Model.findNode("Shield_LEFT");
	Rig.leftHandNode = Model.findNode("mixamorig:LeftHand");
	for (size_t r = 0; r < Model.rigidNodes.size(); ++r) {
		if (Model.rigidNodes[r] == Rig.swordNode)
			SetMaskBit(Rig.rightMask, Model.joints.size() + r);
		if (Model.rigidNodes[r] == Rig.shieldNode)
			SetMaskBit(Rig.leftMask, Model.joints.size() + r);
	}
	const int swordPart = Model.findPart("Sword_RIGHT");
	const int shieldPart = Model.findPart("Shield_LEFT");
	if (Rig.swordNode < 0 || Rig.shieldNode < 0 || Rig.leftHandNode < 0 || swordPart < 0 || shieldPart < 0 || Clips.stand < 0)
		return;

	// The arm pose: the dungeon stand clip's first key (sword and shield gripped).
	Rig.armPose.clip = Clips.stand;
	Rig.armPose.time = 0.0F;
	Rig.armPose.loop = true;
	ComputePosePalette(Rig.armPose);

	// Sword: blade = principal axis, oriented from the leather grip towards the tip.
	std::vector<Vec3> swordPoints;
	std::vector<Vec3> gripPoints;
	for (const SkinnedVertex &v : Model.vertices) {
		if (static_cast<int>((v.packed >> 8) & 255) != swordPart)
			continue;
		const Vec3 p { v.position[0], v.position[1], v.position[2] };
		swordPoints.push_back(p);
		const int material = static_cast<int>(v.packed & 255);
		if (Model.materials[static_cast<size_t>(material)].name.find("Grip") != std::string::npos)
			gripPoints.push_back(p);
	}
	const PrincipalAxes sword = ComputePrincipalAxes(swordPoints);
	const PrincipalAxes grip = ComputePrincipalAxes(gripPoints);
	if (!sword.valid)
		return;
	const Vec3 gripCentre = grip.valid ? grip.centroid : sword.centroid;
	Vec3 blade = sword.axes[0];
	if (skm::Dot(blade, sword.centroid - gripCentre) < 0.0F)
		blade = blade * -1.0F;
	Rig.swordLocal = MakeFrame(gripCentre, blade, sword.axes[1]);
	float tip = 0.0F;
	for (const Vec3 &p : swordPoints)
		tip = std::max(tip, skm::Dot(p - gripCentre, blade));
	float gripTop = 0.0F;
	for (const Vec3 &p : gripPoints)
		gripTop = std::max(gripTop, skm::Dot(p - gripCentre, blade));
	if (tip > 0.1F) {
		Rig.bladeTip = tip;
		Rig.bladeRoot = std::clamp(gripTop + 0.06F, 0.05F, tip * 0.5F);
	}

	// Shield: normal = smallest spread, pointing away from the hand; up = model +y in the stand pose.
	std::vector<Vec3> shieldPoints;
	for (const SkinnedVertex &v : Model.vertices) {
		if (static_cast<int>((v.packed >> 8) & 255) == shieldPart)
			shieldPoints.push_back({ v.position[0], v.position[1], v.position[2] });
	}
	const PrincipalAxes shield = ComputePrincipalAxes(shieldPoints);
	if (!shield.valid)
		return;
	Mat4 shieldToModel = ScratchGlobals[static_cast<size_t>(Rig.shieldNode)];
	Mat4 modelToShield;
	if (!skm::Inverse(shieldToModel, modelToShield))
		return;
	const Vec3 handInShield = skm::TransformPoint(modelToShield, skm::TransformPoint(ScratchGlobals[static_cast<size_t>(Rig.leftHandNode)], {}));
	Vec3 normal = shield.axes[2];
	if (skm::Dot(normal, shield.centroid - handInShield) < 0.0F)
		normal = normal * -1.0F;
	const Vec3 upInShield = skm::TransformDirection(modelToShield, { 0.0F, 1.0F, 0.0F });
	Rig.shieldLocal = MakeFrame(shield.centroid, normal, upInShield);
	Rig.valid = true;
}

/**
 * A hand-authored weapon pose in view space (camera at the origin looking down -z, +y up, x right). `position` is
 * multiplied by ViewModelDistance (see WeaponModelMatrix); the weapon itself keeps its real size.
 */
struct WeaponPose {
	Vec3 position;  // sword: grip; shield: face centre
	Vec3 direction; // sword: blade; shield: outer face normal
	Vec3 up;        // sword: across the blade (its width); shield: its top
};

enum class Ease : uint8_t { Smooth, In, Out };

float Eased(Ease ease, float t)
{
	t = std::clamp(t, 0.0F, 1.0F);
	switch (ease) {
	case Ease::In: return t * t;
	case Ease::Out: return 1.0F - (1.0F - t) * (1.0F - t);
	default: return t * t * (3.0F - 2.0F * t);
	}
}

WeaponPose Mix(const WeaponPose &a, const WeaponPose &b, float t)
{
	return { skm::Lerp(a.position, b.position, t), skm::SlerpDirection(a.direction, b.direction, t), skm::Normalize(skm::Lerp(a.up, b.up, t)) };
}

struct PoseKey {
	float time;
	WeaponPose pose;
	Ease ease = Ease::Smooth; // easing of the segment that ends at this key
};

/** Piecewise interpolation through `keys`, times ascending in [0, 1]. */
WeaponPose Animate(std::initializer_list<PoseKey> keys, float t)
{
	const PoseKey *previous = keys.begin();
	for (const PoseKey *key = keys.begin(); key != keys.end(); ++key) {
		if (t <= key->time) {
			if (key == keys.begin())
				return key->pose;
			const float span = key->time - previous->time;
			return Mix(previous->pose, key->pose, Eased(key->ease, span > 0.0F ? (t - previous->time) / span : 1.0F));
		}
		previous = key;
	}
	return (keys.end() - 1)->pose;
}

Vec3 N(float x, float y, float z) { return skm::Normalize(Vec3 { x, y, z }); }

/** Distance multiplier of every authored weapon position (see WeaponModelMatrix). */
constexpr float ViewModelDistance = 1.9F;

// Rest poses, framed in the 3D view above the control panel (ViewModelParams::frameFraction): sword lower right with
// the blade angled up and to the left, its flat towards the viewer; shield lower left, its outer face turned towards
// the centre so the boss and the rim read, mostly off screen.
const WeaponPose SwordIdle { { 0.27F, -0.215F, -0.50F }, N(-0.42F, 0.80F, -0.42F), N(0.85F, 0.35F, 0.25F) };
const WeaponPose SwordLowered { { 0.34F, -0.36F, -0.48F }, N(-0.25F, 0.55F, -0.80F), N(1.0F, 0.2F, 0.0F) };
const WeaponPose ShieldIdle { { -0.40F, -0.30F, -0.62F }, N(0.62F, 0.22F, 0.75F), N(0.10F, 1.0F, -0.15F) };
const WeaponPose ShieldLowered { { -0.46F, -0.48F, -0.60F }, N(0.55F, 0.45F, 0.70F), N(0.10F, 1.0F, -0.30F) };

/** One sword swing: the blade turns in a single plane (edge leading), fastest exactly at the hit. */
struct SwingPlan {
	WeaponPose windUp;    // raised, the anticipation pose
	WeaponPose hit;       // the blade crosses the screen centre
	Vec3 followGrip;      // where the grip ends the follow-through
	Vec3 axis;            // rotation axis of the blade: windUp -> hit
	float hitAngle = 0.0F;
	float followAngle = 1.0F;
};

SwingPlan MakeSwing(bool backhand)
{
	SwingPlan plan;
	if (!backhand) {
		// Forehand: from over the right shoulder, diagonally down across the view to the lower left.
		plan.windUp = { { 0.34F, -0.06F, -0.62F }, N(0.22F, 0.90F, 0.38F), {} };
		plan.hit = { { 0.16F, -0.10F, -0.50F }, N(-0.86F, 0.12F, -0.50F), {} };
		plan.followGrip = { -0.16F, -0.20F, -0.52F };
	} else {
		// Backhand: from over the left shoulder, the arm across the body, down to the lower right.
		plan.windUp = { { -0.30F, 0.0F, -0.62F }, N(-0.45F, 0.85F, 0.30F), {} };
		plan.hit = { { -0.02F, -0.10F, -0.50F }, N(0.86F, 0.08F, -0.50F), {} };
		plan.followGrip = { 0.32F, -0.24F, -0.52F };
	}
	plan.axis = skm::Normalize(skm::Cross(plan.windUp.direction, plan.hit.direction));
	plan.hitAngle = std::acos(std::clamp(skm::Dot(plan.windUp.direction, plan.hit.direction), -1.0F, 1.0F));
	plan.followAngle = 0.75F; // ~45 degrees past the centre
	plan.windUp.up = plan.axis;
	plan.hit.up = plan.axis;
	return plan;
}

/**
 * Sword pose at attack progress `p` (0..1) whose hit is at `hit`: raise and pull back (anticipation), hold, a fast
 * planar swing accelerating into the hit (ease in) and decelerating through the follow-through (ease out, matched
 * speed), then a slower recovery to the rest pose.
 */
WeaponPose SwordAttackPose(float p, float hit, bool backhand)
{
	static const SwingPlan Plans[2] { MakeSwing(false), MakeSwing(true) };
	const SwingPlan &plan = Plans[backhand ? 1 : 0];
	const float raised = hit * 0.50F;
	const float release = hit * 0.74F;
	// Follow-through length chosen so the angular speed is continuous at the hit.
	const float follow = std::min(hit + plan.followAngle / std::max(plan.hitAngle, 0.1F) * (hit - release), 0.9F);
	// Anticipation: the raised pose creeps a little further back while it holds.
	WeaponPose cocked = plan.windUp;
	cocked.direction = skm::RotateAround(plan.windUp.direction, plan.axis, -0.12F);
	cocked.position = plan.windUp.position + Vec3 { 0.02F, 0.02F, 0.03F };
	if (p <= raised)
		return Mix(SwordIdle, plan.windUp, Eased(Ease::Smooth, p / raised));
	if (p <= release)
		return Mix(plan.windUp, cocked, Eased(Ease::Out, (p - raised) / (release - raised)));
	const auto planar = [&](float angle, Vec3 grip) {
		return WeaponPose { grip, skm::RotateAround(cocked.direction, plan.axis, angle), plan.axis };
	};
	const float startAngle = plan.hitAngle + 0.12F;
	if (p <= hit) {
		const float u = Eased(Ease::In, (p - release) / (hit - release));
		return planar(startAngle * u, skm::Lerp(cocked.position, plan.hit.position, u));
	}
	if (p <= follow) {
		const float u = Eased(Ease::Out, (p - hit) / (follow - hit));
		return planar(startAngle + plan.followAngle * u, skm::Lerp(plan.hit.position, plan.followGrip, u));
	}
	const WeaponPose end = planar(startAngle + plan.followAngle, plan.followGrip);
	return Mix(end, SwordIdle, Eased(Ease::Smooth, (p - follow) / (1.0F - follow)));
}

enum class SpellElement : uint8_t { Fire, Lightning, Magic };

struct ViewModelState {
	WeaponPose sword = SwordIdle;
	WeaponPose shield = ShieldIdle;
	float glow = 0.0F;
	Vec3 glowColor { 1.0F, 0.45F, 0.12F };
	bool hideShield = false;
	/** Cast: place the left hand itself (not the shield) at this view-space point (before ViewModelDistance). */
	bool anchorHand = false;
	Vec3 handTarget;
	/** Attack: the swing's progress, hit and direction, for the motion trail. */
	bool swinging = false;
	float progress = 0.0F;
	float hit = 0.5F;
	bool backhand = false;
};

/** The weapon poses for the player's current action (progress from AnimInfo, as the imposter's clip time). */
ViewModelState ViewModelForPlayer(const Player &player, bool mirroredSwing)
{
	ViewModelState state;
	float p = AnimationProgress(player.AnimInfo);
	player_graphic graphic = player.getGraphic();
	std::optional<SpellElement> forcedElement;
	// Developer knob: DVL_VM_POSE=attack|backhand|block|hit|cast|cast_fire|cast_lightning|death|idle loops that
	// viewmodel action every DVL_VM_PERIOD seconds (default 1.6); DVL_VM_PROGRESS=<p>[,<p>...] holds it at those
	// progress values instead, one per period.
	{
		static const char *Forced = std::getenv("DVL_VM_POSE");
		static const char *Period = std::getenv("DVL_VM_PERIOD");
		static const char *Frozen = std::getenv("DVL_VM_PROGRESS");
		if (Forced != nullptr) {
			const std::string_view pose = Forced;
			const float period = Period != nullptr ? std::max(0.1F, static_cast<float>(std::atof(Period))) : 1.6F;
			const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
			p = std::fmod(seconds, period) / period;
			if (Frozen != nullptr) {
				// A comma-separated list steps through its values, one per period.
				std::vector<float> steps;
				for (const char *at = Frozen; *at != '\0';) {
					char *end = nullptr;
					steps.push_back(std::clamp(std::strtof(at, &end), 0.0F, 1.0F));
					if (end == at)
						break;
					at = *end == ',' ? end + 1 : end;
				}
				// Wall-clock based, so a capture script can tell which value is on screen.
				const double wallSeconds = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
				if (!steps.empty())
					p = steps[static_cast<size_t>(wallSeconds / period) % steps.size()];
			}
			if (pose == "attack" || pose == "backhand") {
				graphic = player_graphic::Attack;
				mirroredSwing = pose == "backhand";
			} else if (pose == "block") {
				graphic = player_graphic::Block;
			} else if (pose == "hit") {
				graphic = player_graphic::Hit;
			} else if (pose.starts_with("cast")) {
				graphic = player_graphic::Magic;
				forcedElement = pose == "cast_fire" ? SpellElement::Fire : (pose == "cast_lightning" ? SpellElement::Lightning : SpellElement::Magic);
			} else if (pose == "death") {
				graphic = player_graphic::Death;
			} else {
				graphic = player_graphic::Stand;
			}
		}
	}
	switch (graphic) {
	case player_graphic::Attack: {
		// The blade crosses the screen centre exactly at the hit (AnimInfo frame _pAFNum - 1).
		const float frames = static_cast<float>(std::max<int>(player.AnimInfo.numberOfFrames, 2));
		const float hit = player._pmode == PM_ATTACK || player._pmode == PM_RATTACK ? std::clamp(static_cast<float>(player._pAFNum - 1) / frames, 0.25F, 0.8F) : 0.5F;
		state.sword = SwordAttackPose(p, hit, mirroredSwing);
		state.shield = Animate({ { 0.0F, ShieldIdle }, { hit, ShieldLowered }, { 1.0F, ShieldIdle } }, p);
		state.swinging = true;
		state.progress = p;
		state.hit = hit;
		state.backhand = mirroredSwing;
		break;
	}
	case player_graphic::Block: {
		// Shield up to the centre, outer face to the viewer, the sword pulled back low.
		const WeaponPose raised { { -0.06F, -0.13F, -0.44F }, N(0.10F, 0.12F, 1.0F), N(0.05F, 1.0F, -0.1F) };
		state.shield = Animate({ { 0.0F, ShieldIdle }, { 0.22F, raised, Ease::Out }, { 0.78F, raised }, { 1.0F, ShieldIdle } }, p);
		state.sword = Animate({ { 0.0F, SwordIdle }, { 0.22F, SwordLowered, Ease::Out }, { 0.78F, SwordLowered }, { 1.0F, SwordIdle } }, p);
		break;
	}
	case player_graphic::Hit: {
		// Recoil: a hard knock back and down, then a decaying shake.
		const float knock = std::sin(std::clamp(p / 0.25F, 0.0F, 1.0F) * Pi * 0.5F) * std::pow(1.0F - std::clamp((p - 0.25F) / 0.75F, 0.0F, 1.0F), 2.0F);
		const float shake = std::pow(1.0F - p, 2.0F);
		const Vec3 offset { 0.035F * std::sin(p * 47.0F) * shake, -0.05F * knock + 0.02F * std::sin(p * 61.0F) * shake, 0.10F * knock };
		state.sword.position = state.sword.position + offset;
		state.shield.position = state.shield.position + offset * 0.8F;
		state.sword.direction = skm::Normalize(state.sword.direction + Vec3 { 0.25F * knock, -0.1F * knock, 0.3F * knock });
		state.shield.direction = skm::Normalize(state.shield.direction + Vec3 { -0.2F * knock, 0.15F * knock, 0.0F });
		break;
	}
	case player_graphic::Lightning:
	case player_graphic::Fire:
	case player_graphic::Magic: {
		// The off hand comes forward to the centre with a glow of the spell's element; the shield is put aside and the
		// sword lowered.
		SpellElement element = graphic == player_graphic::Fire ? SpellElement::Fire : (graphic == player_graphic::Lightning ? SpellElement::Lightning : SpellElement::Magic);
		if (forcedElement)
			element = *forcedElement;
		switch (element) {
		case SpellElement::Fire: state.glowColor = { 1.0F, 0.42F, 0.10F }; break;
		case SpellElement::Lightning: state.glowColor = { 0.55F, 0.75F, 1.0F }; break;
		case SpellElement::Magic: state.glowColor = { 0.62F, 0.40F, 1.0F }; break;
		}
		state.sword = Animate({ { 0.0F, SwordIdle }, { 0.25F, SwordLowered }, { 0.75F, SwordLowered }, { 1.0F, SwordIdle } }, p);
		const float reach = Eased(Ease::Smooth, std::clamp(p / 0.3F, 0.0F, 1.0F)) * (1.0F - Eased(Ease::Smooth, std::clamp((p - 0.75F) / 0.25F, 0.0F, 1.0F)));
		state.glow = 1.4F * reach * (0.85F + 0.15F * std::sin(p * 40.0F));
		state.hideShield = true;
		state.anchorHand = true;
		state.handTarget = skm::Lerp(Vec3 { -0.34F, -0.46F, -0.50F }, Vec3 { -0.07F, -0.05F, -0.46F }, reach);
		break;
	}
	case player_graphic::Death: {
		const float drop = Eased(Ease::In, std::min(p * 1.6F, 1.0F));
		state.sword.position = state.sword.position + Vec3 { 0.1F * drop, -0.9F * drop, 0.0F };
		state.shield.position = state.shield.position + Vec3 { -0.1F * drop, -0.9F * drop, 0.0F };
		state.sword.direction = skm::Normalize(skm::Lerp(state.sword.direction, N(0.6F, -0.2F, -0.7F), drop));
		break;
	}
	default:
		break;
	}
	return state;
}

/** Weapon frame (view space) -> the draw's model matrix: view = pose * inverse(weapon node global * local frame). */
bool WeaponModelMatrix(const WeaponPose &pose, const Frame &local, int node, Mat4 &out)
{
	const Mat4 weaponFrameInModel = ScratchGlobals[static_cast<size_t>(node)] * FrameMatrix(local);
	Mat4 inverse;
	if (!skm::Inverse(weaponFrameInModel, inverse))
		return false;
	// The authored positions are directions from the eye; pushing them out keeps the screen layout and shrinks the
	// weapons to Hexen-like proportions (the real 0.9 m sword at arm's length fills the screen).
	out = FrameMatrix(MakeFrame(pose.position * ViewModelDistance, pose.direction, pose.up)) * inverse;
	return true;
}

/** Walk bob, breathing and mouselook lag (per process; the viewmodel belongs to the local player). */
struct ViewModelMotion {
	bool valid = false;
	std::chrono::steady_clock::time_point last;
	float phase = 0.0F;
	float bobAmount = 0.0F;
	float strafe = 0.0F;
	float lagYaw = 0.0F;
	float lagPitch = 0.0F;
	float time = 0.0F;
} Motion;

Vec3 ViewModelSway(const Player &player, float moveSpeed, float yaw, float pitch)
{
	const auto now = std::chrono::steady_clock::now();
	if (!Motion.valid) {
		Motion.valid = true;
		Motion.last = now;
		Motion.lagYaw = yaw;
		Motion.lagPitch = pitch;
	}
	const float dt = std::clamp(std::chrono::duration<float>(now - Motion.last).count(), 0.0F, 0.1F);
	Motion.last = now;
	Motion.time += dt;
	// First-person gameplay (Source/fpv) supplies the movement and bob phase; otherwise derive them from the player.
	const bool fpvInputs = moveSpeed < 0.0F && fpv::IsActive();
	const fpv::ViewmodelInputs *inputs = fpvInputs ? &fpv::GetViewmodelInputs() : nullptr;
	float speed = moveSpeed >= 0.0F ? moveSpeed / 2.5F : (player.isWalking() ? 1.0F : 0.0F);
	if (inputs != nullptr)
		speed = inputs->moveSpeed;
	const float targetBob = std::clamp(speed, 0.0F, 1.5F);
	const float blend = std::min(dt * 8.0F, 1.0F);
	Motion.bobAmount += (targetBob - Motion.bobAmount) * blend;
	Motion.strafe += ((inputs != nullptr ? inputs->strafe : 0.0F) - Motion.strafe) * blend;
	if (inputs != nullptr)
		Motion.phase = inputs->bobPhase;
	else
		Motion.phase += dt * Pi * 2.0F * (0.9F + 0.9F * speed);
	// Look lag: the weapons trail a fast turn a little.
	const float follow = std::min(dt * 12.0F, 1.0F);
	Motion.lagYaw += std::remainder(yaw - Motion.lagYaw, 2.0F * Pi) * follow;
	Motion.lagPitch += (pitch - Motion.lagPitch) * follow;
	const float lagX = std::clamp(std::remainder(Motion.lagYaw - yaw, 2.0F * Pi) * 0.30F, -0.05F, 0.05F);
	const float lagY = std::clamp((Motion.lagPitch - pitch) * 0.30F, -0.04F, 0.04F);
	// Bob: a figure-eight (sideways at the step rate, down twice per cycle), plus slow breathing while standing.
	return {
		0.014F * std::sin(Motion.phase) * Motion.bobAmount + lagX - 0.012F * Motion.strafe,
		-0.016F * std::abs(std::sin(Motion.phase)) * Motion.bobAmount + 0.004F * std::sin(Motion.time * 1.7F) + lagY,
		0.006F * Motion.bobAmount * std::abs(std::cos(Motion.phase)),
	};
}

/** The viewmodel's projection: camera meters -> clip, framed in the top `frameFraction` of the viewport. */
Mat4 ViewModelProjection(float fovYDeg, float aspect, float frameFraction)
{
	const float s = std::clamp(frameFraction, 0.3F, 1.0F);
	Mat4 frame = Mat4::Identity();
	// Clip y' = s y + (1 - s) w: NDC [-1, 1] -> [1 - 2 s, 1], the part of the view above the control panel.
	frame(1, 1) = s;
	frame(1, 3) = 1.0F - s;
	return frame * skm::Perspective(fovYDeg * Pi / 180.0F, aspect / s, 0.03F, 30.0F);
}

/** The effects pipeline for a pass (created with its module, layout and buffer on first use); null if it failed. */
WGPURenderPipeline GetFxPipeline(WGPUTextureFormat color, WGPUTextureFormat depth)
{
	for (const auto &[key, pipeline] : G.fxPipelines) {
		if (key.first == color && key.second == depth)
			return pipeline;
	}
	PushScope();
	if (G.fxShader == nullptr) {
		WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl.code = { EffectsShaderSource, WGPU_STRLEN };
		WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		moduleDescriptor.nextInChain = &wgsl.chain;
		moduleDescriptor.label = View("DamnationX viewmodel effects shader");
		G.fxShader = wgpuDeviceCreateShaderModule(G.device, &moduleDescriptor);
		WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		entry.binding = 0;
		entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entry.buffer.type = WGPUBufferBindingType_Uniform;
		entry.buffer.hasDynamicOffset = true;
		entry.buffer.minBindingSize = sizeof(GpuFx);
		WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.label = View("DamnationX viewmodel effects layout");
		layoutDescriptor.entryCount = 1;
		layoutDescriptor.entries = &entry;
		G.fxBindGroupLayout = wgpuDeviceCreateBindGroupLayout(G.device, &layoutDescriptor);
		WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
		pipelineLayoutDescriptor.bindGroupLayouts = &G.fxBindGroupLayout;
		G.fxPipelineLayout = wgpuDeviceCreatePipelineLayout(G.device, &pipelineLayoutDescriptor);
		G.fxBuffer = CreateBuffer("DamnationX viewmodel effects", WGPUBufferUsage_Uniform, FxSlotStride * FxSlots, nullptr);
		WGPUBindGroupEntry bind = WGPU_BIND_GROUP_ENTRY_INIT;
		bind.binding = 0;
		bind.buffer = G.fxBuffer;
		bind.size = sizeof(GpuFx);
		WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindDescriptor.label = View("DamnationX viewmodel effects bind group");
		bindDescriptor.layout = G.fxBindGroupLayout;
		bindDescriptor.entryCount = 1;
		bindDescriptor.entries = &bind;
		G.fxBindGroup = wgpuDeviceCreateBindGroup(G.device, &bindDescriptor);
	}
	WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
	blend.color = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
	blend.alpha = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format = color;
	target.blend = &blend;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = G.fxShader;
	fragment.entryPoint = View("fxFragment");
	fragment.targetCount = 1;
	fragment.targets = &target;
	WGPUDepthStencilState depthState = WGPU_DEPTH_STENCIL_STATE_INIT;
	depthState.format = depth;
	depthState.depthWriteEnabled = WGPUOptionalBool_False;
	depthState.depthCompare = WGPUCompareFunction_Always;
	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX viewmodel effects pipeline");
	descriptor.layout = G.fxPipelineLayout;
	descriptor.vertex.module = G.fxShader;
	descriptor.vertex.entryPoint = View("fxVertex");
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
	descriptor.primitive.cullMode = WGPUCullMode_None;
	descriptor.depthStencil = depth != WGPUTextureFormat_Undefined ? &depthState : nullptr;
	descriptor.multisample.count = 1;
	descriptor.fragment = &fragment;
	WGPURenderPipeline pipeline = G.fxShader != nullptr && G.fxPipelineLayout != nullptr && G.fxBindGroup != nullptr
	    ? wgpuDeviceCreateRenderPipeline(G.device, &descriptor)
	    : nullptr;
	PopScope("creating the viewmodel effects pipeline");
	G.fxPipelines.push_back({ { color, depth }, pipeline });
	return pipeline;
}

void EncodeFx(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline, const GpuFx &fx, uint32_t vertices)
{
	if (pipeline == nullptr || G.fxSlotsUsed >= FxSlots)
		return;
	const uint32_t offset = static_cast<uint32_t>(G.fxSlotsUsed++ * FxSlotStride);
	wgpuQueueWriteBuffer(G.queue, G.fxBuffer, offset, &fx, sizeof(fx));
	wgpuRenderPassEncoderSetPipeline(pass, pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, G.fxBindGroup, 1, &offset);
	wgpuRenderPassEncoderDraw(pass, vertices, 1, 0, 0);
}

/** The lights of a viewmodel draw (world lit): a key from the player's own light over the shoulder, fill and rim. */
struct ViewModelLighting {
	bool worldLit = false;
	bool replacesFirstWorldLight = false; // world light 0 is the player's own: lit by the key instead
	Vec3 keyColor { 0.0F, 0.0F, 0.0F };    // world units of light (albedo * light, like the world)
	Vec3 fill { 0.05F, 0.05F, 0.055F };
	Vec3 rimColor { 0.30F, 0.33F, 0.40F };
	float rimStrength = 1.0F;
};

/**
 * Encodes the viewmodel's draws into `pass` with `pipeline`: the sword hand, the shield hand, then the effects (the
 * swing's motion trail, the cast glow). `params.view` maps fpv world -> camera (world units). The authored poses live in
 * camera space (meters); they are moved into the world (camera -> world, meters -> world units) so the same draw can be
 * lit by the world pass (`worldGroup`, world-lit pipeline) or by this file's shader.
 */
void EncodeViewModelDraws(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline, const ViewModelParams &params, bool srgb,
    WGPUBindGroup worldGroup = nullptr, const ViewModelLighting *lighting = nullptr)
{
	if (!Rig.built)
		BuildViewModelRig();
	if (!Rig.valid || params.player == nullptr || params.view == nullptr)
		return;
	const Player &player = *params.player;
	const Mat4 view = Mat4::FromArray(params.view);
	Mat4 cameraToWorld;
	if (!skm::Inverse(view, cameraToWorld))
		return;
	// Camera yaw / pitch for the look lag: the camera's forward is -z of the view rotation (its third row).
	const Vec3 forward { -view(2, 0), -view(2, 1), -view(2, 2) };
	const float yaw = std::atan2(forward.x, forward.z);
	const float pitch = std::asin(std::clamp(forward.y, -1.0F, 1.0F));
	const Vec3 sway = ViewModelSway(player, params.moveSpeed, yaw, pitch);

	const bool mirrored = fpv::IsActive() && fpv::GetViewmodelInputs().swingMirrored;
	ViewModelState state = ViewModelForPlayer(player, mirrored);
	state.sword.position = state.sword.position + sway;
	state.shield.position = state.shield.position + sway * 0.8F;
	ComputePosePalette(Rig.armPose); // ScratchGlobals / ScratchPalette: the arms gripping their weapons

	Mat4 rightModel;
	Mat4 leftModel;
	if (!WeaponModelMatrix(state.sword, Rig.swordLocal, Rig.swordNode, rightModel) || !WeaponModelMatrix(state.shield, Rig.shieldLocal, Rig.shieldNode, leftModel))
		return;
	Vec3 handView = skm::TransformPoint(leftModel * ScratchGlobals[static_cast<size_t>(Rig.leftHandNode)], {});
	if (state.anchorHand) {
		leftModel = skm::Translation(state.handTarget * ViewModelDistance + sway - handView) * leftModel;
		handView = state.handTarget * ViewModelDistance + sway;
	}
	const bool inTown = leveltype == DTYPE_TOWN;
	uint32_t parts = VisibleParts(AppearanceOf(player), inTown, /*dead=*/false);
	parts &= ~(Parts.swordStowed | Parts.scabbard | Parts.frog | Parts.chape);
	if (state.hideShield)
		parts &= ~Parts.shield;

	// camera meters -> world units -> world; the projection is the viewmodel's own (narrower FOV than the world's).
	const float scale = fpv::kWarriorMetersToWorld;
	const Mat4 cameraMetersToWorld = cameraToWorld * skm::Scale(scale);
	const float aspect = params.aspect > 0.0F ? params.aspect : 16.0F / 9.0F;
	const Mat4 projection = ViewModelProjection(params.fovYDeg, aspect, params.frameFraction);
	const Mat4 viewProj = projection * skm::Scale(1.0F / scale) * view;
	GpuDraw draw {};
	FillDraw(draw, Rig.armPose, viewProj, Mat4::Identity()); // palette and matrices; the model matrix is set per hand
	const Vec3 eye = skm::TransformPoint(cameraToWorld, {});
	draw.eye = { eye.x, eye.y, eye.z, 0.0F };
	draw.params = { params.exposure, 0.0F, 0.3F, srgb ? 1.0F : 0.0F };
	draw.ambientUp = { params.ambientUp[0], params.ambientUp[1], params.ambientUp[2], 0.0F };
	draw.ambientDown = { params.ambientDown[0], params.ambientDown[1], params.ambientDown[2], 0.0F };
	uint32_t mode = 1;
	if (lighting != nullptr && lighting->worldLit) {
		// Own lights: the key over the right shoulder, slightly behind the eye (camera meters -> world).
		const Vec3 keyPosition = skm::TransformPoint(cameraMetersToWorld, { 0.35F, 0.55F, 0.25F });
		draw.lights[0] = { { keyPosition.x, keyPosition.y, keyPosition.z, 0.0F }, { lighting->keyColor.x, lighting->keyColor.y, lighting->keyColor.z, 0.0F } };
		draw.params[1] = 1.0F;
		draw.ambientDown = { lighting->fill.x, lighting->fill.y, lighting->fill.z, 0.0F };
		draw.ambientUp = { lighting->rimColor.x, lighting->rimColor.y, lighting->rimColor.z, 0.0F };
		const Vec3 rimDirection = skm::Normalize(skm::TransformDirection(cameraToWorld, N(-0.35F, 0.65F, -0.70F)));
		draw.rim = { rimDirection.x, rimDirection.y, rimDirection.z, lighting->rimStrength };
		mode = lighting->replacesFirstWorldLight ? 2U : 1U;
	} else {
		LightsToGpu(draw, params.lights, params.lights != nullptr ? params.lightCount : 0, params.lightScale);
	}
	// The cast glow sits a little in front of the off hand's palm.
	const Vec3 glowView = handView + Vec3 { 0.04F, 0.05F, -0.10F };
	const Vec3 glowWorld = skm::TransformPoint(cameraMetersToWorld, glowView);

	// Each hand's palette in camera meters: everything rigid with its weapon, the arm bent by IK towards a shoulder
	// off screen (lower corner), so the arm always leaves the view instead of ending in a visible cut.
	const std::vector<Mat4> restPalette = ScratchPalette;
	const auto encodeHand = [&](const Mat4 &model, const std::array<uint32_t, 2> &mask, float glow, const ArmChain &arm, Vec3 shoulder, Vec3 pole) {
		const int slot = AllocateSlot();
		if (slot < 0)
			return;
		for (size_t i = 0; i < draw.palette.size(); ++i)
			draw.palette[i] = i < restPalette.size() ? (model * restPalette[i]).m : model.m;
		SolveArm(arm, model, shoulder, pole, draw.palette);
		CopyMatrix(draw.model, cameraMetersToWorld);
		draw.visible = { parts, mask[0], mask[1], mode };
		draw.glow = { state.glowColor.x, state.glowColor.y, state.glowColor.z, glow };
		draw.glowCentre = { glowWorld.x, glowWorld.y, glowWorld.z, 0.35F * scale };
		wgpuQueueWriteBuffer(G.queue, G.drawBuffer, static_cast<uint64_t>(slot) * DrawSlotStride, &draw, sizeof(draw));
		EncodeDraw(pass, pipeline, slot, worldGroup);
	};
	// The sword arm rises from below and to the right of the hand (so a backhand across the body does not cover the
	// crosshair with a forearm); the shield arm from the lower left corner.
	const float swordShoulderX = std::clamp(state.sword.position.x * ViewModelDistance + 0.5F, -0.15F, 0.75F);
	encodeHand(rightModel, Rig.rightMask, 0.0F, Rig.rightArm, { swordShoulderX, -1.6F, 0.3F }, { 0.8F, -1.0F, 0.1F });
	encodeHand(leftModel, Rig.leftMask, state.glow, Rig.leftArm, { -0.75F, -1.6F, 0.3F }, { -0.8F, -1.0F, 0.1F });

	// Effects: the swept motion trail of the blade, and the glow orb in the casting hand.
	const bool trail = state.swinging && (parts & Parts.swordRight) != 0;
	if (!trail && state.glow <= 0.0F)
		return;
	WGPURenderPipeline fxPipeline = GetFxPipeline(params.colorFormat, params.depthFormat);
	if (fxPipeline == nullptr)
		return;
	GpuFx fx {};
	fx.viewProj = projection.m;
	const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (trail) {
		// Sample the swing backwards in time: root and tip of the blade per sample, faded with age and by the blade's
		// angular speed (no trail while raising the sword or recovering).
		constexpr float Span = 0.09F; // of the attack's progress
		const auto bladeAt = [&](float p, Vec3 &root, Vec3 &tip) {
			WeaponPose pose = SwordAttackPose(std::clamp(p, 0.0F, 1.0F), state.hit, state.backhand);
			const Vec3 grip = (pose.position + sway) * ViewModelDistance;
			root = grip + pose.direction * Rig.bladeRoot;
			tip = grip + pose.direction * Rig.bladeTip;
			return pose.direction;
		};
		float visible = 0.0F;
		for (int k = 0; k < TrailSamples; ++k) {
			const float p = state.progress - Span * static_cast<float>(k) / (TrailSamples - 1);
			Vec3 root;
			Vec3 tip;
			const Vec3 direction = bladeAt(p, root, tip);
			Vec3 earlierRoot;
			Vec3 earlierTip;
			const Vec3 earlier = bladeAt(p - 0.01F, earlierRoot, earlierTip);
			const float angularSpeed = std::acos(std::clamp(skm::Dot(direction, earlier), -1.0F, 1.0F)) / 0.01F; // rad per progress
			const float age = static_cast<float>(k) / (TrailSamples - 1);
			const float alpha = std::pow(1.0F - age, 1.6F) * std::clamp((angularSpeed - 6.0F) / 14.0F, 0.0F, 1.0F) * (p >= 0.0F ? 1.0F : 0.0F);
			visible = std::max(visible, alpha);
			fx.samples[static_cast<size_t>(k * 2)] = { root.x, root.y, root.z, alpha };
			fx.samples[static_cast<size_t>(k * 2 + 1)] = { tip.x, tip.y, tip.z, alpha };
		}
		if (visible > 0.01F) {
			fx.color = { 0.80F, 0.84F, 0.92F, 0.55F };
			fx.params = { 0.0F, static_cast<float>(TrailSamples), 0.0F, seconds };
			EncodeFx(pass, fxPipeline, fx, TrailSamples * 2);
		}
	}
	if (state.glow > 0.0F) {
		fx.color = { state.glowColor.x, state.glowColor.y, state.glowColor.z, std::min(state.glow, 1.0F) * 1.1F };
		fx.params = { 1.0F, 0.0F, 0.22F + 0.025F * std::sin(seconds * 9.0F), seconds };
		fx.centre = { glowView.x, glowView.y, glowView.z, 0.0F };
		EncodeFx(pass, fxPipeline, fx, 4);
	}
}

// ---- World renderer hooks (render3d/world_renderer.hpp) ----

std::array<float, 16> ToArray(const render3d::Mat4 &m)
{
	std::array<float, 16> out;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			out[static_cast<size_t>(c * 4 + r)] = m.at(r, c);
	return out;
}

/** The world pass's frame uniforms + occluder grid as this renderer's group 0 (rebuilt when the buffers change). */
WGPUBindGroup WorldBindGroup(const SkinnedDrawContext &context)
{
	if (context.frameUniforms == nullptr || context.occluderGrid == nullptr || G.worldBindGroupLayout == nullptr)
		return nullptr;
	if (G.worldBindGroup != nullptr && G.worldFrameBuffer == context.frameUniforms && G.worldOccluderBuffer == context.occluderGrid)
		return G.worldBindGroup;
	if (G.worldBindGroup != nullptr)
		wgpuBindGroupRelease(G.worldBindGroup);
	std::array<WGPUBindGroupEntry, 2> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	entries[0].binding = 0;
	entries[0].buffer = context.frameUniforms;
	entries[0].size = context.frameUniformsSize;
	entries[1].binding = 1;
	entries[1].buffer = context.occluderGrid;
	entries[1].size = context.occluderGridSize;
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX skinned world frame");
	descriptor.layout = G.worldBindGroupLayout;
	descriptor.entryCount = entries.size();
	descriptor.entries = entries.data();
	PushScope();
	G.worldBindGroup = wgpuDeviceCreateBindGroup(G.device, &descriptor);
	PopScope("binding the world frame");
	G.worldFrameBuffer = context.frameUniforms;
	G.worldOccluderBuffer = context.occluderGrid;
	return G.worldBindGroup;
}

bool WorldWantsPlayer(int playerId)
{
	if (playerId < 0 || static_cast<size_t>(playerId) >= Players.size() || !SkinnedReady())
		return false;
	return SkinnedPlayerWanted(Players[static_cast<size_t>(playerId)]);
}

/** Other warriors in the first-person 3D pass, lit by the world's lights and shadows. */
void WorldDrawActors(const SkinnedDrawContext &context)
{
	if (context.pass == nullptr || context.device != G.device || !SkinnedReady())
		return;
	WGPUBindGroup worldGroup = WorldBindGroup(context);
	if (worldGroup == nullptr)
		return;
	WGPURenderPipeline pipeline = nullptr;
	const std::array<float, 16> viewProj = ToArray(context.viewProjection);
	for (const Player &player : Players) {
		if (&player == MyPlayer || !player.plractive || !player.isOnActiveLevel() || !SkinnedPlayerWanted(player))
			continue;
		if (pipeline == nullptr)
			pipeline = GetPipeline({ context.colorFormat, context.depthFormat, 1, false, true });
		if (pipeline == nullptr)
			return;
		const int slot = AllocateSlot();
		if (slot < 0)
			return;
		Pose pose;
		BuildPose(player, pose);
		float model[16];
		SkinnedPlayerModelMatrix(player, model);
		GpuDraw draw {};
		FillDraw(draw, pose, Mat4::FromArray(viewProj.data()), Mat4::FromArray(model));
		draw.eye = { context.cameraPosition.x, context.cameraPosition.y, context.cameraPosition.z, 0.0F };
		wgpuQueueWriteBuffer(G.queue, G.drawBuffer, static_cast<uint64_t>(slot) * DrawSlotStride, &draw, sizeof(draw));
		EncodeDraw(context.pass, pipeline, slot, worldGroup);
	}
}

/**
 * Fraction of the 3D view's height (rows [0, GetViewportHeight()) of the screen) above the control panel: the
 * viewmodel is framed in that part. 1 when the panel is hidden or already below the view.
 */
float ViewFractionAbovePanel()
{
	const float viewportRows = static_cast<float>(GetViewportHeight());
	const float uiHeight = static_cast<float>(GetUiHeight());
	if (viewportRows <= 0.0F || uiHeight <= 0.0F)
		return 1.0F;
	const float panelTop = static_cast<float>(GetMainPanel().position.y) * static_cast<float>(GetScreenHeight()) / uiHeight;
	return std::clamp(panelTop / viewportRows, 0.5F, 1.0F);
}

/** The local player's Hexen-style weapon, in the world renderer's viewmodel pass (depth cleared). */
void WorldDrawViewModel(const ViewModelDrawContext &context)
{
	if (context.pass == nullptr || context.device != G.device || MyPlayer == nullptr || !SkinnedReady() || !*GetOptions().Graphics.fpvWeapon
	    || MyPlayer->_pClass != HeroClass::Warrior)
		return;
	WGPUBindGroup worldGroup = WorldBindGroup(context);
	if (worldGroup == nullptr)
		return;
	WGPURenderPipeline pipeline = GetPipeline({ context.colorFormat, context.depthFormat, 1, false, true });
	if (pipeline == nullptr)
		return;
	const std::array<float, 16> view = ToArray(context.view);
	ViewModelParams params;
	params.player = MyPlayer;
	params.view = view.data();
	params.aspect = context.aspect;
	params.colorFormat = context.colorFormat;
	params.depthFormat = context.depthFormat;
	params.exposure = 1.0F;
	params.frameFraction = ViewFractionAbovePanel();
	// The player's own light keys the weapon from over the shoulder; the other lights of the frame light it in place.
	ViewModelLighting lighting;
	lighting.worldLit = true;
	if (context.hasPlayerLight && context.playerLight.radius > 0.0F) {
		lighting.replacesFirstWorldLight = true;
		const float intensity = std::max(context.playerLight.intensity, 0.0F) * 0.85F;
		// Slightly warm, like the torch-lit world around it.
		lighting.keyColor = { context.playerLight.color.x * intensity, context.playerLight.color.y * intensity * 0.88F, context.playerLight.color.z * intensity * 0.72F };
	} else {
		lighting.keyColor = { 0.10F, 0.095F, 0.09F };
	}
	if (context.ambient > 0.3F) { // town: moonlight
		lighting.rimColor = { 0.35F, 0.40F, 0.52F };
		lighting.fill = { 0.08F, 0.085F, 0.10F };
	}
	EncodeViewModelDraws(context.pass, pipeline, params, IsSrgb(context.colorFormat), worldGroup);
}

/**
 * Registers the hooks with the world renderer once the GPU side is usable (unregisters if it broke). The viewmodel
 * hook is registered only while the local player is a warrior with the option on: registering it hides the 2D slash.
 */
void RegisterWorldRenderers()
{
	const bool usable = State == LoadState::Loaded && !GpuBroken && G.bindGroup != nullptr && G.worldPipelineLayout != nullptr;
	if (usable != G.rendererRegistered) {
		SetSkinnedActorRenderer(usable ? &WorldDrawActors : nullptr, usable ? &WorldWantsPlayer : nullptr);
		G.rendererRegistered = usable;
	}
	const bool viewModel = usable && MyPlayer != nullptr && MyPlayer->_pClass == HeroClass::Warrior && *GetOptions().Graphics.fpvWeapon;
	static bool ViewModelRegistered = false;
	if (viewModel != ViewModelRegistered) {
		SetViewModelRenderer(viewModel ? &WorldDrawViewModel : nullptr);
		ViewModelRegistered = viewModel;
	}
	if (!usable && *GetOptions().Graphics.firstPersonView)
		(void)SkinnedReady(); // request the GPU resources even before the first imposter
}

} // namespace

const SkinnedImposterOutput *SkinnedPrepareFrame(WGPUDevice device, WGPUQueue queue, float texelsPerPixel, int &count)
{
	RegisterWorldRenderers();
	count = 0;
	G.slotsUsed = 0;
	G.fxSlotsUsed = 0;
	bool any = false;
	for (const ImposterRequest &request : Requests)
		any = any || request.active;
	if (G.wanted && G.bindGroup == nullptr && SkinnedModelLoaded())
		EnsureGpu(device, queue);
	// Load the model and compile the imposter pipeline in the background on the first presented frames (title screen)
	// rather than with the first in-game frame (~50 ms load + ~150 ms compile).
	if (!G.warmupStarted && device != nullptr && *GetOptions().Graphics.warrior3dModel && !GpuBroken) {
		G.warmupStarted = true;
		if (EnsureGpu(device, queue)) {
			WithPipelineDescriptor(ImposterKey, [](const WGPURenderPipelineDescriptor &descriptor) { G.imposterWarmup.start(G.device, descriptor); });
		}
	}
	if (!any || !EnsureGpu(device, queue))
		return G.outputs.data();
	const auto start = std::chrono::steady_clock::now();
	// The cell is 160 back-buffer pixels across (640 HD pixels / 4); render it at the screen's texel density.
	const int wanted = static_cast<int>(std::ceil(160.0F * std::clamp(texelsPerPixel, 1.0F, 4.0F) / 32.0F)) * 32;
	const int size = std::clamp(wanted, 160, kImposterCellSize);
	if (!EnsureTargets(G.targets, size, size))
		return G.outputs.data();
	WGPURenderPipeline pipeline = GetPipeline(ImposterKey);
	if (pipeline == nullptr)
		return G.outputs.data();
	WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
	encoderDescriptor.label = View("DamnationX imposters");
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(G.device, &encoderDescriptor);
	for (size_t i = 0; i < Requests.size(); ++i) {
		const ImposterRequest &request = Requests[i];
		if (!request.active || !EnsureSlot(G.slots[i], size, size))
			continue;
		RenderImposter(encoder, pipeline, request, G.slots[i]);
		G.outputs[static_cast<size_t>(count++)] = { request.clip, G.slots[i].view };
	}
	WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
	wgpuQueueSubmit(G.queue, 1, &commands);
	wgpuCommandBufferRelease(commands);
	wgpuCommandEncoderRelease(encoder);
	static int logged = 0;
	if (logged < 3) {
		++logged;
		Log("3D warrior: {} imposter(s) at {} px, CPU {} us", count, any ? size : 0,
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
	}
	return G.outputs.data();
}

void SkinnedReleaseGpu()
{
	for (auto &[key, pipeline] : G.pipelines) {
		if (pipeline != nullptr)
			wgpuRenderPipelineRelease(pipeline);
	}
	G.pipelines.clear();
	G.imposterWarmup.abandon();
	G.warmupStarted = false;
	ReleaseTargets(G.targets);
	for (ImposterSlot &slot : G.slots) {
		ReleaseTexture(slot.texture, slot.view);
		slot = {};
	}
	if (G.bindGroup != nullptr) wgpuBindGroupRelease(G.bindGroup);
	if (G.pipelineLayout != nullptr) wgpuPipelineLayoutRelease(G.pipelineLayout);
	if (G.bindGroupLayout != nullptr) wgpuBindGroupLayoutRelease(G.bindGroupLayout);
	if (G.shader != nullptr) wgpuShaderModuleRelease(G.shader);
	if (G.worldShader != nullptr) wgpuShaderModuleRelease(G.worldShader);
	if (G.worldBindGroup != nullptr) wgpuBindGroupRelease(G.worldBindGroup);
	if (G.worldPipelineLayout != nullptr) wgpuPipelineLayoutRelease(G.worldPipelineLayout);
	if (G.worldBindGroupLayout != nullptr) wgpuBindGroupLayoutRelease(G.worldBindGroupLayout);
	G.worldShader = nullptr;
	G.worldBindGroup = nullptr;
	G.worldPipelineLayout = nullptr;
	G.worldBindGroupLayout = nullptr;
	G.worldFrameBuffer = nullptr;
	G.worldOccluderBuffer = nullptr;
	if (G.sampler != nullptr) wgpuSamplerRelease(G.sampler);
	G.bindGroup = nullptr;
	G.pipelineLayout = nullptr;
	G.bindGroupLayout = nullptr;
	G.shader = nullptr;
	G.sampler = nullptr;
	ReleaseTexture(G.baseColor, G.baseColorView);
	for (size_t i = 0; i < G.equipTextures.size(); ++i)
		ReleaseTexture(G.equipTextures[i], G.equipViews[i]);
	for (auto &[key, pipeline] : G.fxPipelines) {
		if (pipeline != nullptr)
			wgpuRenderPipelineRelease(pipeline);
	}
	G.fxPipelines.clear();
	if (G.fxBindGroup != nullptr) wgpuBindGroupRelease(G.fxBindGroup);
	if (G.fxPipelineLayout != nullptr) wgpuPipelineLayoutRelease(G.fxPipelineLayout);
	if (G.fxBindGroupLayout != nullptr) wgpuBindGroupLayoutRelease(G.fxBindGroupLayout);
	if (G.fxShader != nullptr) wgpuShaderModuleRelease(G.fxShader);
	G.fxBindGroup = nullptr;
	G.fxPipelineLayout = nullptr;
	G.fxBindGroupLayout = nullptr;
	G.fxShader = nullptr;
	ReleaseBuffer(G.fxBuffer);
	G.fxSlotsUsed = 0;
	ReleaseBuffer(G.vertexBuffer);
	ReleaseBuffer(G.indexBuffer);
	ReleaseBuffer(G.materialBuffer);
	ReleaseBuffer(G.drawBuffer);
	G.device = nullptr;
	G.queue = nullptr;
	G.failed = false;
	G.slotsUsed = 0;
}

bool SkinnedReady()
{
	G.wanted = true;
	return State == LoadState::Loaded && G.device != nullptr && G.bindGroup != nullptr && !G.failed;
}

void SkinnedPlayerModelMatrix(const Player &player, float outModel[16])
{
	// Drawn screen offset (free-form sub-tile movement) back to tiles: screen x = 32 (tx - ty), screen y = 16 (tx + ty).
	const Displacement offset = player.position.GetDrawnScreenOffset(player.isWalking(), player._pdir, player.AnimInfo);
	const float sx = static_cast<float>(offset.deltaX) / 32.0F;
	const float sy = static_cast<float>(offset.deltaY) / 16.0F;
	const float tileX = static_cast<float>(player.position.tile.x) + 0.5F + (sx + sy) * 0.5F;
	const float tileY = static_cast<float>(player.position.tile.y) + 0.5F + (sy - sx) * 0.5F;
	const skm::Mat4 model = skm::Translation({ tileX, 0.0F, tileY }) * skm::RotationY(SkinnedPlayerYaw(player)) * skm::Scale(fpv::kWarriorMetersToWorld);
	std::copy(model.m.begin(), model.m.end(), outModel);
}

void SkinnedEncodePlayer(WGPURenderPassEncoder pass, const float viewProj[16], const float model[16], const Player &player,
    const WebGpuHybridLight *lights, int lightCount, WGPUTextureFormat colorFormat, WGPUTextureFormat depthFormat)
{
	SkinnedDrawParams params;
	params.viewProj = viewProj;
	params.model = model;
	params.lights = lights;
	params.lightCount = lightCount;
	params.colorFormat = colorFormat;
	params.depthFormat = depthFormat;
	// The eye is the point the camera projects to clip (0, 0, z, 0): inverse(viewProj) * (0, 0, 1, 0), homogenised.
	skm::Mat4 inverse;
	if (skm::Inverse(skm::Mat4::FromArray(viewProj), inverse) && std::abs(inverse(3, 2)) > 1e-8F) {
		params.eye[0] = inverse(0, 2) / inverse(3, 2);
		params.eye[1] = inverse(1, 2) / inverse(3, 2);
		params.eye[2] = inverse(2, 2) / inverse(3, 2);
	} else {
		params.eye[0] = model[12]; // orthographic or degenerate: a point above and in front of the model
		params.eye[1] = model[13] + 1.0F;
		params.eye[2] = model[14] + 2.0F;
	}
	SkinnedEncodePlayerEx(pass, player, params);
}

void SkinnedEncodePlayerEx(WGPURenderPassEncoder pass, const Player &player, const SkinnedDrawParams &params)
{
	if (pass == nullptr || params.viewProj == nullptr || params.model == nullptr || !SkinnedReady() || !SkinnedModelLoaded())
		return;
	WGPURenderPipeline pipeline = GetPipeline({ params.colorFormat, params.depthFormat, std::max(params.sampleCount, 1U), false });
	if (pipeline == nullptr)
		return;
	const int slot = AllocateSlot();
	if (slot < 0)
		return;
	Pose pose;
	BuildPose(player, pose);
	GpuDraw draw {};
	FillDraw(draw, pose, skm::Mat4::FromArray(params.viewProj), skm::Mat4::FromArray(params.model));
	draw.eye = { params.eye[0], params.eye[1], params.eye[2], 0.0F };
	draw.params = { params.exposure, 0.0F, 0.3F, IsSrgb(params.colorFormat) ? 1.0F : 0.0F };
	draw.ambientUp = { params.ambientUp[0], params.ambientUp[1], params.ambientUp[2], 0.0F };
	draw.ambientDown = { params.ambientDown[0], params.ambientDown[1], params.ambientDown[2], 0.0F };
	LightsToGpu(draw, params.lights, params.lights != nullptr ? params.lightCount : 0, params.lightScale);
	wgpuQueueWriteBuffer(G.queue, G.drawBuffer, static_cast<uint64_t>(slot) * DrawSlotStride, &draw, sizeof(draw));
	EncodeDraw(pass, pipeline, slot);
}

void SkinnedEncodeViewModel(WGPURenderPassEncoder pass, const ViewModelParams &params)
{
	if (pass == nullptr || params.player == nullptr || params.view == nullptr || !SkinnedReady() || !*GetOptions().Graphics.fpvWeapon
	    || params.player->_pClass != HeroClass::Warrior)
		return;
	WGPURenderPipeline pipeline = GetPipeline({ params.colorFormat, params.depthFormat, std::max(params.sampleCount, 1U), false });
	if (pipeline != nullptr)
		EncodeViewModelDraws(pass, pipeline, params, IsSrgb(params.colorFormat));
}

} // namespace devilution::render3d

#endif // DEVILUTIONX_WEBGPU
