/**
 * @file hd_sprites_webgpu.cpp
 *
 * GPU half of the opt-in HD sprites (hd_sprites.hpp, `?hdSprites=1`).
 *
 * Clip textures are loaded lazily per clip (the browser fetches and decodes the PNG grid and copies it straight into
 * a texture, see hd_sprites_loader.hpp) and kept under an LRU budget (256 MiB in the browser, more on a desktop GPU); clips drawn this frame are never
 * evicted, nor are pinned clips (the player's walk and idle clips, HdSpritesGpuSetPinnedClips). The texture
 * resolution follows the canvas: a 2x canvas keeps the grids at half their 4x size (a quarter of the memory) since
 * that is all the screen can show.
 *
 * Each instance is a quad in back-buffer pixels drawn inside the presenter's composite pass after the full-screen
 * triangle, so it lands at canvas resolution with sub-pixel placement. The fragment shader discards texels hidden by
 * later classic draws (the last-writer plane, hd_sprites_core.hpp) and applies the same lighting as the classic body:
 * the classic light table level (ambient curve) times the legacy ray traced light mask times the palette fade.
 * Behind a see-through wall (TranslucentBit in the plane) the texels are drawn at half coverage, the ghost classic
 * shows through its blended walls. Every instance also draws a soft drop shadow (HdSpriteShadow) before all sprites,
 * standing in for the shadow baked into the classic art.
 *
 * Hybrid frames (2.5D lighting) are full-bright and lit on the GPU from the
 * G-buffer. The skipped classic blit wrote no card there, so the HD texels are lit by a second pipeline that evaluates
 * the hybrid light pass itself: each texel is a point on the camera-facing card the classic sprite would have been
 * (anchored at the classic frame's bottom row, height = rows above it), run through the same `accumulate()` as K_CARD
 * mask texels (lights, ray-marched wall shadows, bilinear classic ambient, no gate). The card normal is tilted outward
 * near the HD silhouette (alpha gradient), as the classic generated normal planes inflate the sprite outline.
 */
#include "engine/render/hd_sprites_webgpu.hpp"

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_sprites_internal.hpp"
#include "engine/render/hd_sprites_loader.hpp"
#include "engine/render/hd_webgpu_async.hpp"
#include "engine/render/webgpu_platform.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "engine/render/webgpu_shaders.hpp"
#include "render3d/skinned_renderer.hpp"
#include "utils/log.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// A copy the GPU rejected after hd_loader::UploadImage returned (an asynchronous validation or out-of-memory error):
// the browser loader pushes the texture handle to Module.hdFailedUploads. Returns one such handle, or 0.
EM_JS(int, dx_hd_take_failed_upload, (), {
	const failed = Module.hdFailedUploads;
	return failed && failed.length ? failed.shift() : 0;
});
#endif

namespace devilution {
namespace {

/** Clip texture budget in the browser (and on a desktop adapter of unknown kind); see ChooseTextureBudget. */
constexpr size_t WebTextureBudgetBytes = 256U * 1024U * 1024U;
constexpr size_t IntegratedTextureBudgetBytes = 768U * 1024U * 1024U;
constexpr size_t DiscreteTextureBudgetBytes = 1024U * 1024U * 1024U;
/** Clips decoding or uploading at once. */
constexpr int MaxLoadsInFlight = 3;
/** Clip texels a frame writes to the GPU (see ContinueUpload), and the largest single write on the desktop. */
constexpr size_t UploadBudgetBytes = 4U * 1024U * 1024U;
constexpr size_t UploadSliceBytes = 2U * 1024U * 1024U;

/**
 * Shared by both sprite pipelines (each declares the `sprite*` bindings at its own group numbers). An instance is
 * either the sprite itself or, drawn before all sprites, its drop shadow (params.w = 1): the same quad sheared onto
 * the ground around the pivot row (HdSpriteShadow), which is an affine map, so the cell UVs still interpolate exactly.
 */
constexpr char SpriteCoreShaderSource[] = R"(
// ---- HD sprite core (hd_sprites_webgpu.cpp) ----
struct Globals {
  sourceSize: vec4f,
  hybrid: vec4f,     // hybrid only: exposure, silhouette normal radius (back-buffer px), silhouette normal strength, world px per back-buffer px (1 / zoom)
  cardRight: vec4f,  // hybrid only: card basis in tile space (gbuffer::CardToTile)
  cardUp: vec4f,
  cardNormal: vec4f,
  shadow: vec4f,     // shear x, shear y, opacity, blur radius (back-buffer px)
};

struct Instance {
  rect: vec4f,     // x, y, width, height in back-buffer pixels
  uvRect: vec4f,   // u0, v0, u1, v1 of the frame cell
  clipRect: vec4f, // x0, y0, x1, y1: visible (snapshotted) back-buffer pixels
  params: vec4f,   // light (legacy) or palette fade (hybrid), instance index, card anchor row (sprite) / pivot row (shadow) / outline radius (outline), kind
  tint: vec4f,     // HdSpriteTint: per-channel multiplier (rgb), saturation (w); identity (1, 1, 1, 1). Outline: colour (rgb), quad padding (w).
};

// params.w: 0 sprite, 1 drop shadow, 2 self-lit sprite (classic draws it without a light table), 3 hover outline,
// 4 sprite with a normal map (the 3D warrior imposter, render3d/skinned_renderer.hpp: the texture is [colour | tile-space
// normal], the cell in the left half, its normals 0.5 to the right).
fn isShadow(item: Instance) -> bool { return item.params.w > 0.5 && item.params.w < 1.5; }
fn isSelfLit(item: Instance) -> bool { return item.params.w > 1.5 && item.params.w < 2.5; }
fn isOutline(item: Instance) -> bool { return item.params.w > 2.5 && item.params.w < 3.5; }
fn isNormalMapped(item: Instance) -> bool { return item.params.w > 3.5; }

// HdSpriteTint on (premultiplied or straight) colour: desaturate towards the luma, then scale per channel.
fn applyTint(item: Instance, color: vec3f) -> vec3f {
  let luma = dot(color, vec3f(0.299, 0.587, 0.114));
  return mix(vec3f(luma), color, item.tint.w) * item.tint.rgb;
}

const TRANSLUCENT_BIT: u32 = 0x8000u;  // OcclusionTracker::TranslucentBit
const WRITER_MASK: u32 = 0x7fffu;

struct VertexOutput {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
  @location(1) screen: vec2f,
  @location(2) @interpolate(flat) instance: u32,
  @location(3) height: f32,  // shadow: rows above the pivot of the sheared texel
};

@vertex fn vertexMain(@builtin(vertex_index) vertex: u32, @builtin(instance_index) instanceIndex: u32) -> VertexOutput {
  let corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
    vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  let corner = corners[vertex];
  let item = spriteInstances[instanceIndex];
  var screen = item.rect.xy + corner * item.rect.zw;
  var uv = mix(item.uvRect.xy, item.uvRect.zw, corner);
  if (isOutline(item)) {
    // The outline reaches past the sprite quad by its radius plus a soft edge (tint.w); the UVs run past the cell with it.
    let pad = item.tint.w;
    let uvPerPixel = (item.uvRect.zw - item.uvRect.xy) / item.rect.zw;
    screen = item.rect.xy - vec2f(pad) + corner * (item.rect.zw + vec2f(2.0 * pad));
    uv = item.uvRect.xy + (screen - item.rect.xy) * uvPerPixel;
  }
  var height = 0.0;
  if (isShadow(item)) {
    height = item.params.z - screen.y;
    screen = vec2f(screen.x - spriteGlobals.shadow.x * height, item.params.z - spriteGlobals.shadow.y * height);
  }
  var output: VertexOutput;
  output.position = vec4f(screen.x / spriteGlobals.sourceSize.x * 2.0 - 1.0, 1.0 - screen.y / spriteGlobals.sourceSize.y * 2.0, 0.0, 1.0);
  output.uv = uv;
  output.screen = screen;
  output.instance = instanceIndex;
  output.height = height;
  return output;
}

// Keeps the bilinear footprint inside the frame cell so neighbouring frames never bleed in.
fn cellUv(item: Instance, uv: vec2f) -> vec2f {
  let halfTexel = vec2f(0.5) / vec2f(textureDimensions(spriteTexture));
  return clamp(uv, item.uvRect.xy + halfTexel, item.uvRect.zw - halfTexel);
}

// The cell's alpha at `uv`, 0 outside the cell (the outline quad reaches past it).
fn cellAlpha(item: Instance, uv: vec2f) -> f32 {
  if (any(uv < item.uvRect.xy) || any(uv > item.uvRect.zw)) {
    return 0.0;
  }
  return textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, uv), 0.0).a;
}

// Hover outline coverage (params.w = 3): the classic 1-pixel ClxDrawOutline, as the cell's alpha dilated by the radius
// (params.z back-buffer pixels) over two rings of taps. It is drawn right before its sprite, which covers the inside.
fn outlineAlpha(item: Instance, uv: vec2f) -> f32 {
  let radius = (item.uvRect.zw - item.uvRect.xy) / item.rect.zw * item.params.z;
  var alpha = cellAlpha(item, uv);
  for (var i = 0; i < 16; i++) {
    let angle = f32(i) * 0.39269908; // 2 pi / 16
    let offset = vec2f(cos(angle), sin(angle)) * radius;
    alpha = max(alpha, max(cellAlpha(item, uv + offset), cellAlpha(item, uv + offset * 0.5)));
  }
  return smoothstep(0.1, 0.5, alpha);
}

// Coverage left by the classic draws after the instance (the last-writer plane): 0 hidden, 0.5 behind a see-through
// wall (classic draws the entity through it as a ghost), 1 visible.
fn occlusionCoverage(item: Instance, screen: vec2f) -> f32 {
  let pixel = floor(screen);
  if (any(pixel < item.clipRect.xy) || any(pixel >= item.clipRect.zw)) {
    return 0.0;
  }
  let plane = textureLoad(spriteOcclusion, vec2i(pixel), 0).r;
  if (f32(plane & WRITER_MASK) <= item.params.y) {
    return 1.0;
  }
  if ((plane & TRANSLUCENT_BIT) != 0u) {
    return 0.5;
  }
  return 0.0;
}

// Drop shadow alpha: the silhouette's alpha, softened by four taps (explicit level: any control flow is fine).
fn shadowAlpha(item: Instance, input: VertexOutput, centre: f32) -> f32 {
  if (input.height < 0.0) {
    return 0.0;
  }
  let tap = (item.uvRect.zw - item.uvRect.xy) / item.rect.zw * spriteGlobals.shadow.w;
  let a = textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv + vec2f(tap.x, 0.0)), 0.0).a
        + textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv - vec2f(tap.x, 0.0)), 0.0).a
        + textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv + vec2f(0.0, tap.y)), 0.0).a
        + textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv - vec2f(0.0, tap.y)), 0.0).a;
  return (centre * 2.0 + a) / 6.0 * spriteGlobals.shadow.z;
}
)";

/** Legacy composite: bindings and fragment stage (the classic light level, the legacy light mask and the fade). */
constexpr char SpriteShaderSource[] = R"(
@group(0) @binding(0) var<uniform> spriteGlobals: Globals;
@group(0) @binding(1) var<storage, read> spriteInstances: array<Instance>;
@group(0) @binding(2) var spriteSampler: sampler;
@group(0) @binding(3) var lightMask: texture_2d<f32>;
@group(0) @binding(4) var spriteOcclusion: texture_2d<u32>;
@group(1) @binding(0) var spriteTexture: texture_2d<f32>;

@fragment fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
  let item = spriteInstances[input.instance];
  let texel = textureSample(spriteTexture, spriteSampler, cellUv(item, input.uv)); // premultiplied alpha
  let coverage = occlusionCoverage(item, input.screen);
  if (coverage <= 0.0) {
    discard;
  }
  if (isShadow(item)) {
    let alpha = shadowAlpha(item, input, texel.a) * coverage;
    if (alpha < 0.004) {
      discard;
    }
    return vec4f(0.0, 0.0, 0.0, alpha);
  }
  let maskSize = vec2f(textureDimensions(lightMask));
  let maskPixel = vec2i(clamp(input.screen * maskSize / spriteGlobals.sourceSize.xy, vec2f(0.0), maskSize - vec2f(1.0)));
  if (isOutline(item)) {
    // A palette colour without a light table, like the classic outline pixel (the light mask still applies).
    let alpha = outlineAlpha(item, input.uv) * coverage;
    if (alpha < 0.004) {
      discard;
    }
    return vec4f(item.tint.rgb * textureLoad(lightMask, maskPixel, 0).rgb * alpha, alpha);
  }
  if (texel.a < 0.004) {
    discard;
  }
  let lighting = textureLoad(lightMask, maskPixel, 0).rgb * item.params.x;
  let tinted = clamp(applyTint(item, texel.rgb), vec3f(0.0), vec3f(texel.a));
  return vec4f(tinted * lighting, texel.a) * coverage;
}
)";

/**
 * Hybrid variant, compiled after HybridPreludeWgsl + HybridLightWgsl (webgpu_shaders.hpp) so it calls the light
 * pass's own screenToWorld() / accumulate() with its group 0 (HybridParams, occluder volume). The compute entry point
 * in the same module is never used; the render pipeline's auto layout only holds what these stages reference.
 */
constexpr char HybridSpriteShaderSource[] = R"(
// ---- HD sprite card pass (hd_sprites_webgpu.cpp) ----
@group(1) @binding(0) var<uniform> spriteGlobals: Globals;
@group(1) @binding(1) var<storage, read> spriteInstances: array<Instance>;
@group(1) @binding(2) var spriteSampler: sampler;
@group(1) @binding(3) var spriteOcclusion: texture_2d<u32>;
@group(2) @binding(0) var spriteTexture: texture_2d<f32>;

// The hybrid composite's transfer functions (HybridPresentWgsl), renamed: that string is not part of this module.
fn hdSrgbToLinear1(c: f32) -> f32 {
  if (c > 0.04045) { return pow((c + 0.055) / 1.055, 2.4); }
  return c / 12.92;
}
fn hdLinearToSrgb1(c: f32) -> f32 {
  if (c > 0.0031308) { return 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
  return c * 12.92;
}
fn hdSrgbToLinear(c: vec3f) -> vec3f { return vec3f(hdSrgbToLinear1(c.x), hdSrgbToLinear1(c.y), hdSrgbToLinear1(c.z)); }
fn hdLinearToSrgb(c: vec3f) -> vec3f { return vec3f(hdLinearToSrgb1(c.x), hdLinearToSrgb1(c.y), hdLinearToSrgb1(c.z)); }

@fragment fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
  let item = spriteInstances[input.instance];
  let uv = cellUv(item, input.uv);
  let texel = textureSample(spriteTexture, spriteSampler, uv); // premultiplied alpha
  let coverage = occlusionCoverage(item, input.screen);
  if (coverage <= 0.0) {
    discard;
  }
  // G-buffer debug views show the planes, which hold no card for an HD figure.
  if (((params.counts.w >> 8u) & 15u) != 0u) {
    discard;
  }
  if (isShadow(item)) {
    // Hybrid sprites cast no ray-marched shadows (the occluder volume is static geometry): keep the classic-style one.
    let alpha = shadowAlpha(item, input, texel.a) * coverage;
    if (alpha < 0.004) {
      discard;
    }
    return vec4f(0.0, 0.0, 0.0, alpha);
  }
  if (isOutline(item)) {
    // The hover highlight is unlit, like the classic outline's palette colour.
    let alpha = outlineAlpha(item, input.uv) * coverage;
    if (alpha < 0.004) {
      discard;
    }
    return vec4f(item.tint.rgb * alpha, alpha);
  }
  if (texel.a < 0.004) {
    discard;
  }
  // Unlit objects (torches, fires) pass through the hybrid composite like the classic emissive card.
  if (isSelfLit(item)) {
    return vec4f(clamp(applyTint(item, texel.rgb), vec3f(0.0), vec3f(texel.a)) * item.params.x, texel.a) * coverage;
  }

  // Silhouette normal: tilt the card normal outward where the alpha falls off (screen x right = card right, screen y
  // down = card down), as the classic generated normal planes inflate the sprite outline.
  let tap = (item.uvRect.zw - item.uvRect.xy) / item.rect.zw * spriteGlobals.hybrid.y;
  let alphaLeft = textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv - vec2f(tap.x, 0.0)), 0.0).a;
  let alphaRight = textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv + vec2f(tap.x, 0.0)), 0.0).a;
  let alphaUp = textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv - vec2f(0.0, tap.y)), 0.0).a;
  let alphaDown = textureSampleLevel(spriteTexture, spriteSampler, cellUv(item, input.uv + vec2f(0.0, tap.y)), 0.0).a;
  let gradient = vec2f(alphaRight - alphaLeft, alphaDown - alphaUp) * 0.5 * spriteGlobals.hybrid.z;
  let cardSpace = normalize(vec3f(-gradient.x, gradient.y, 1.0));
  var n = normalize(spriteGlobals.cardRight.xyz * cardSpace.x + spriteGlobals.cardUp.xyz * cardSpace.y + spriteGlobals.cardNormal.xyz * cardSpace.z);
  if (isNormalMapped(item)) {
    // The imposter's rendered tile-space normal (premultiplied by coverage like the colour).
    let encoded = textureSampleLevel(spriteTexture, spriteSampler, uv + vec2f(0.5, 0.0), 0.0);
    if (encoded.a > 0.01) {
      n = normalize(encoded.rgb / encoded.a * 2.0 - vec3f(1.0));
    }
  }

  // The classic card: ground under the texel's column at the anchor row, height = rows above the anchor (K_CARD).
  let anchorY = item.params.z;
  let ground = screenToWorld(vec2f(input.screen.x, anchorY));
  let z = max(anchorY - input.screen.y, 0.0) * spriteGlobals.hybrid.w / params.shading.y;   // screen rows -> world rows (zoom)
  let cell = vec2i(floor(ground));
  let everywhere = Footprint(vec2f(-1e9, -1e9), vec2f(1e9, 1e9)); // no workgroup light culling in a fragment shader
  let lighting = accumulate(vec3f(ground, z), n, ground, cell, cell, params.shading.w, false, everywhere);

  let albedo = hdSrgbToLinear(clamp(applyTint(item, texel.rgb / texel.a) * item.params.x, vec3f(0.0), vec3f(1.0)));
  let color = clamp(albedo * lighting * spriteGlobals.hybrid.x, vec3f(0.0), vec3f(1.0));
  return vec4f(hdLinearToSrgb(color) * texel.a, texel.a) * coverage;
}
)";

/** Back-buffer pixels between the silhouette normal's alpha taps, and how strongly the alpha gradient tilts the card normal. */
constexpr float SilhouetteNormalRadius = 3.0F;
constexpr float SilhouetteNormalStrength = 1.5F;

struct alignas(16) GpuInstance {
	std::array<float, 4> rect;
	std::array<float, 4> uvRect;
	std::array<float, 4> clipRect;
	std::array<float, 4> params;
	std::array<float, 4> tint;
};
static_assert(sizeof(GpuInstance) == 80);

struct alignas(16) GpuGlobals {
	std::array<float, 4> sourceSize;
	// hybrid .. cardNormal are read by the hybrid shader only.
	std::array<float, 4> hybrid;
	std::array<float, 4> cardRight;
	std::array<float, 4> cardUp;
	std::array<float, 4> cardNormal;
	std::array<float, 4> shadow;
};

enum class ClipState : uint8_t { Unrequested, Queued, Loading, Resident, Failed };

/** A finished decode being written to a new texture (see ContinueUpload). */
struct ClipUpload {
	WGPUTexture texture = nullptr;
	int width = 0;
	int height = 0;
	int rows = 0; // rows [0, rows) are written
};

struct ClipSlot {
	ClipState state = ClipState::Unrequested;
	int job = 0;
	int textureWidth = 0;
	int textureHeight = 0;
	WGPUTexture texture = nullptr;
	WGPUTextureView view = nullptr;
	WGPUBindGroup bindGroup = nullptr;       // legacy pipeline (group 1), created when first drawn
	WGPUBindGroup hybridBindGroup = nullptr; // the same view for the hybrid pipeline's auto layout (group 2)
	int divisor = 0; // the texture holds the 4x grid / divisor
	// A sharper copy loading while the resident texture keeps drawing (the view was zoomed in or the canvas grew).
	int upgradeJob = 0;
	int upgradeDivisor = 0;
	bool upgradeQueued = false;
	/** The decode of `job` (Loading) or `upgradeJob` being uploaded. */
	ClipUpload upload;
	/** Listed in HdSpritesGpu::inFlight. */
	bool inFlight = false;
	/** Queued by HdSpritesGpuPrefetchClipsIfRoom only: loads only into free texture space. */
	bool ifRoom = false;
	/** The texture is owned by the 3D warrior renderer (an imposter, render3d::SkinnedPrepareFrame), never loaded or evicted. */
	bool external = false;
};

struct HdSpritesGpu {
	WGPUDevice device = nullptr;
	WGPUTextureFormat pipelineFormat = WGPUTextureFormat_Undefined;
	hd_gpu::QuadPipeline legacyPipeline { std::string(SpriteCoreShaderSource) + SpriteShaderSource, "DamnationX HD sprite pipeline" };
	// Hybrid pipeline: group 0 = HybridParams + volume, group 1 = frame, group 2 = clip.
	hd_gpu::QuadPipeline hybridPipeline { std::string(HybridPreludeWgsl) + HybridLightWgsl + SpriteCoreShaderSource + HybridSpriteShaderSource,
		"DamnationX HD sprite hybrid pipeline" };
	WGPURenderPipeline framePipeline = nullptr; // the pipeline the prepared frame draws with
	WGPUSampler sampler = nullptr;
	WGPUBuffer globalsBuffer = nullptr;
	WGPUBuffer instanceBuffer = nullptr;
	size_t instanceCapacity = 0;
	WGPUTexture occlusionTexture = nullptr;
	WGPUTextureView occlusionView = nullptr;
	int occlusionWidth = 0;
	int occlusionHeight = 0;
	WGPUBindGroup frameBindGroup = nullptr;
	WGPUBindGroup hybridLightBindGroup = nullptr;
	bool hybridFrame = false;

	std::vector<ClipSlot> clips;
	std::deque<int> queue;
	std::deque<int> upgradeQueue; // resident clips to reload at the current (finer) textureDivisor
	std::vector<int> pinnedClips; // HdSpritesGpuSetPinnedClips: (re)loaded by PumpLoads, never evicted
	/** Clips with a decode or an upload in flight (Loading, or an upgrade), in request order: PumpLoads polls only these. */
	std::vector<int> inFlight;
	int loadsInFlight = 0;
	/** Texture bytes of the clips Loading (decoding or uploading), for HdSpritesGpuPrefetchClipsIfRoom. */
	size_t loadingBytes = 0;
	hd_sprites::TextureLru lru { WebTextureBudgetBytes };
	uint64_t frame = 1;
	int textureDivisor = 2; // 4x grid / divisor, chosen from the canvas scale times the world scale (zoom)
	bool loggedBudget = false;

	std::vector<GpuInstance> gpuInstances;
	std::vector<int> drawClips;
	std::vector<int> shadowClips; // clip of each shadow instance (they follow the sprites in the instance buffer)
	std::vector<int> outlineSlots; // per sprite: instance buffer index of its hover outline (after the shadows), or -1
	std::vector<hd_sprites::PixelRect> uploadRects; // occlusion plane rectangles uploaded this frame
	std::vector<uint16_t> uploadStaging;             // one rectangle, rows packed
	std::array<float, 16> ambientCurve {};
	bool ambientValid = false;
};

HdSpritesGpu Gpu;

WGPUStringView View(const char *text)
{
	return { text, WGPU_STRLEN };
}

void ReleaseTexture(WGPUTexture &texture)
{
	if (texture != nullptr) {
		wgpuTextureDestroy(texture);
		wgpuTextureRelease(texture);
	}
	texture = nullptr;
}

void ReleaseClipTexture(ClipSlot &slot)
{
	if (slot.bindGroup != nullptr) wgpuBindGroupRelease(slot.bindGroup);
	if (slot.hybridBindGroup != nullptr) wgpuBindGroupRelease(slot.hybridBindGroup);
	if (slot.external) {
		slot.texture = nullptr; // owned by the skinned renderer
	} else {
		if (slot.view != nullptr) wgpuTextureViewRelease(slot.view);
		ReleaseTexture(slot.texture);
	}
	slot.bindGroup = nullptr;
	slot.hybridBindGroup = nullptr;
	slot.view = nullptr;
}

/** Drops a half-written texture (its decode was abandoned or failed). */
void ReleaseUpload(ClipSlot &slot)
{
	ReleaseTexture(slot.upload.texture);
	slot.upload = {};
}

void ReleaseFrameBindGroup()
{
	if (Gpu.frameBindGroup != nullptr) wgpuBindGroupRelease(Gpu.frameBindGroup);
	Gpu.frameBindGroup = nullptr;
	if (Gpu.hybridLightBindGroup != nullptr) wgpuBindGroupRelease(Gpu.hybridLightBindGroup);
	Gpu.hybridLightBindGroup = nullptr;
	Gpu.framePipeline = nullptr;
}

bool EnsureClipSlots()
{
	const size_t count = HdSpritesIndex().clips.size();
	if (Gpu.clips.size() != count)
		Gpu.clips.resize(count);
	return count > 0;
}

/**
 * The clip texture budget. The browser keeps 256 MiB (a tab shares the GPU process with everything else). The desktop
 * sizes it from the adapter: the Catacombs' pinned and prefetched monster clips plus the actions drawn on demand did
 * not fit 256 MiB and evicted and reloaded 100+ MiB every few seconds.
 */
size_t ChooseTextureBudget([[maybe_unused]] WGPUDevice device)
{
#ifdef __EMSCRIPTEN__
	return WebTextureBudgetBytes;
#else
	WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
	if (wgpuDeviceGetAdapterInfo(device, &info) != WGPUStatus_Success)
		return WebTextureBudgetBytes;
	const WGPUAdapterType type = info.adapterType;
	wgpuAdapterInfoFreeMembers(info);
	switch (type) {
	case WGPUAdapterType_DiscreteGPU:
		return DiscreteTextureBudgetBytes;
	case WGPUAdapterType_IntegratedGPU:
		return IntegratedTextureBudgetBytes;
	default:
		return WebTextureBudgetBytes;
	}
#endif
}

/**
 * Starts compiling both sprite pipelines in the background on the first presented frame (title screen), so the first
 * town and dungeon frames do not wait 250-350 ms for them; a device or surface format change starts over.
 */
void StartPipelines(WGPUDevice device, WGPUTextureFormat format)
{
	if (Gpu.device != device || Gpu.pipelineFormat != format) {
		if (Gpu.device != nullptr)
			HdSpritesGpuRelease();
		Gpu.device = device;
		Gpu.pipelineFormat = format;
		const size_t budget = ChooseTextureBudget(device);
		if (budget != Gpu.lru.budget()) {
			Gpu.lru.setBudget(budget);
			Log("HD sprites: {} MiB clip texture budget", budget >> 20);
		}
	}
	Gpu.legacyPipeline.start(device, format);
	Gpu.hybridPipeline.start(device, format);
}

/** The sampler and the globals buffer (cheap; created with the first drawn frame). */
bool EnsureFrameResources()
{
	if (Gpu.sampler == nullptr) {
		WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
		samplerDescriptor.addressModeU = WGPUAddressMode_ClampToEdge;
		samplerDescriptor.addressModeV = WGPUAddressMode_ClampToEdge;
		samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
		samplerDescriptor.magFilter = WGPUFilterMode_Linear;
		samplerDescriptor.minFilter = WGPUFilterMode_Linear;
		samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		Gpu.sampler = wgpuDeviceCreateSampler(Gpu.device, &samplerDescriptor);
	}
	if (Gpu.globalsBuffer == nullptr) {
		WGPUBufferDescriptor globalsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
		globalsDescriptor.label = View("DamnationX HD sprite globals");
		globalsDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform;
		globalsDescriptor.size = sizeof(GpuGlobals);
		Gpu.globalsBuffer = wgpuDeviceCreateBuffer(Gpu.device, &globalsDescriptor);
	}
	return Gpu.sampler != nullptr && Gpu.globalsBuffer != nullptr;
}

/** The legacy pipeline (nullptr: creation failed, logged once). */
WGPURenderPipeline LegacyPipeline()
{
	WGPURenderPipeline pipeline = Gpu.legacyPipeline.get(Gpu.device, Gpu.pipelineFormat);
	static bool logged = false;
	if (pipeline == nullptr && !logged) {
		LogError("HD sprites: pipeline creation failed");
		logged = true;
	}
	return pipeline;
}

/** The hybrid card pipeline (nullptr: creation failed, logged once). */
WGPURenderPipeline HybridPipeline()
{
	WGPURenderPipeline pipeline = Gpu.hybridPipeline.get(Gpu.device, Gpu.pipelineFormat);
	static bool logged = false;
	if (pipeline == nullptr && !logged) {
		LogError("HD sprites: hybrid pipeline creation failed; HD sprites are hidden under hybrid lighting");
		logged = true;
	}
	return pipeline;
}

/** Bind group of one clip texture for `pipeline`'s auto layout at `group`. */
WGPUBindGroup CreateClipBindGroup(WGPURenderPipeline pipeline, uint32_t group, WGPUTextureView view)
{
	WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, group);
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.binding = 0;
	entry.textureView = view;
	WGPUBindGroupDescriptor bindGroupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bindGroupDescriptor.layout = layout;
	bindGroupDescriptor.entryCount = 1;
	bindGroupDescriptor.entries = &entry;
	WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &bindGroupDescriptor);
	wgpuBindGroupLayoutRelease(layout);
	return bindGroup;
}

bool EnsureInstanceBuffer(size_t count)
{
	if (Gpu.instanceBuffer != nullptr && Gpu.instanceCapacity >= count)
		return true;
	if (Gpu.instanceBuffer != nullptr) {
		wgpuBufferDestroy(Gpu.instanceBuffer);
		wgpuBufferRelease(Gpu.instanceBuffer);
	}
	Gpu.instanceCapacity = std::max<size_t>(16, std::bit_ceil(count));
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD sprite instances");
	descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage;
	descriptor.size = Gpu.instanceCapacity * sizeof(GpuInstance);
	Gpu.instanceBuffer = wgpuDeviceCreateBuffer(Gpu.device, &descriptor);
	return Gpu.instanceBuffer != nullptr;
}

/**
 * Copies a `width` x `height` rectangle of a plane (`pitch` values per row) into `out`, rows packed: the browser copies
 * the whole data range of a texture write, so a write straight from the plane would copy every full row it spans.
 */
void PackRect(const uint16_t *plane, int pitch, int x, int y, int width, int height, std::vector<uint16_t> &out)
{
	out.resize(static_cast<size_t>(width) * height);
	for (int row = 0; row < height; ++row)
		std::copy_n(plane + static_cast<size_t>(y + row) * pitch + x, width, out.data() + static_cast<size_t>(row) * width);
}

/**
 * The occlusion plane rectangles to upload: the clip rectangles of the first `spriteCount` instances (the shadow and
 * outline instances share them), nearby ones merged while that adds little area.
 */
void MergeUploadRects(size_t spriteCount, std::vector<hd_sprites::PixelRect> &rects)
{
	constexpr int64_t MergeSlack = 16384; // texels a merge may add (each upload has a fixed cost)
	const auto area = [](const hd_sprites::PixelRect &r) { return static_cast<int64_t>(r.width) * r.height; };
	const auto merge = [](const hd_sprites::PixelRect &a, const hd_sprites::PixelRect &b) {
		const int x0 = std::min(a.x, b.x);
		const int y0 = std::min(a.y, b.y);
		return hd_sprites::PixelRect { x0, y0, std::max(a.x + a.width, b.x + b.width) - x0, std::max(a.y + a.height, b.y + b.height) - y0 };
	};
	rects.clear();
	for (size_t i = 0; i < spriteCount; ++i) {
		const std::array<float, 4> &clip = Gpu.gpuInstances[i].clipRect;
		hd_sprites::PixelRect rect { static_cast<int>(clip[0]), static_cast<int>(clip[1]), static_cast<int>(clip[2] - clip[0]),
			static_cast<int>(clip[3] - clip[1]) };
		if (rect.empty())
			continue;
		for (size_t j = 0; j < rects.size();) {
			const hd_sprites::PixelRect merged = merge(rects[j], rect);
			if (area(merged) <= area(rects[j]) + area(rect) + MergeSlack) {
				rect = merged;
				rects.erase(rects.begin() + static_cast<ptrdiff_t>(j));
				j = 0; // the grown rectangle may now merge with an earlier one
				continue;
			}
			++j;
		}
		rects.push_back(rect);
	}
}

bool EnsureOcclusionTexture(int width, int height)
{
	if (Gpu.occlusionTexture != nullptr && Gpu.occlusionWidth == width && Gpu.occlusionHeight == height)
		return true;
	if (Gpu.occlusionView != nullptr) wgpuTextureViewRelease(Gpu.occlusionView);
	ReleaseTexture(Gpu.occlusionTexture);
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD sprite last-writer plane");
	descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
	descriptor.format = WGPUTextureFormat_R16Uint;
	Gpu.occlusionTexture = wgpuDeviceCreateTexture(Gpu.device, &descriptor);
	Gpu.occlusionView = Gpu.occlusionTexture != nullptr ? wgpuTextureCreateView(Gpu.occlusionTexture, nullptr) : nullptr;
	Gpu.occlusionWidth = width;
	Gpu.occlusionHeight = height;
	return Gpu.occlusionView != nullptr;
}

/** Texture size of a clip grid loaded at `divisor` (the 4x grid / divisor, rounded up). */
void ClipTextureSize(const hd_sprites::ClipInfo &clip, int divisor, int &width, int &height)
{
	width = (clip.sheetWidth() + divisor - 1) / divisor;
	height = (clip.sheetHeight() + divisor - 1) / divisor;
}

/** Starts the browser decode of a clip grid at `divisor`; returns the job. */
int RequestClipImage(const hd_sprites::ClipInfo &clip, int divisor)
{
	const hd_sprites::ImageRequest request = hd_sprites::ClipImageRequest(clip, divisor);
	const std::string url = "hd/" + clip.png;
	return hd_loader::RequestImage(url.c_str(), request.width, request.height, request.expectedWidth, request.expectedHeight);
}

WGPUTexture CreateClipTexture(int width, int height)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD sprite clip");
	descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
	descriptor.format = WGPUTextureFormat_RGBA8Unorm;
	return wgpuDeviceCreateTexture(Gpu.device, &descriptor);
}

/** Makes `texture` the clip's resident texture and registers its size with the LRU. */
void InstallClipTexture(int clipIndex, ClipSlot &slot, WGPUTexture texture, int width, int height, int divisor)
{
	ReleaseClipTexture(slot);
	slot.texture = texture;
	slot.textureWidth = width;
	slot.textureHeight = height;
	slot.divisor = divisor;
	slot.view = wgpuTextureCreateView(slot.texture, nullptr);
	slot.state = ClipState::Resident;
	Gpu.lru.insert(clipIndex, static_cast<size_t>(width) * height * 4, Gpu.frame);
}

enum class UploadStep : uint8_t { Waiting, Done, Failed };

/**
 * Writes the finished decode `job` into `slot.upload` (a `width` x `height` texture, created on the first step) as far
 * as the frame's upload budget (`spent` bytes so far) allows. Desktop: row slices of at most UploadSliceBytes (a
 * single 1 MiB clip write once took 11 ms while Dawn churned its staging buffers). Browser: the loader copies a
 * decoded image in one call, so a whole clip per step, and at least one clip per frame.
 */
UploadStep ContinueUpload(ClipSlot &slot, int job, int width, int height, size_t &spent)
{
	ClipUpload &upload = slot.upload;
	const size_t rowBytes = static_cast<size_t>(width) * 4;
#ifdef __EMSCRIPTEN__
	const size_t bytes = rowBytes * static_cast<size_t>(height);
	if (spent > 0 && spent + bytes > UploadBudgetBytes)
		return UploadStep::Waiting;
	WGPUTexture texture = CreateClipTexture(width, height);
	if (texture == nullptr || !hd_loader::UploadImage(job, Gpu.device, texture, width, height)) {
		ReleaseTexture(texture);
		return UploadStep::Failed;
	}
	spent += bytes;
	upload = ClipUpload { texture, width, height, height };
	return UploadStep::Done;
#else
	if (spent >= UploadBudgetBytes)
		return UploadStep::Waiting;
	if (upload.texture == nullptr) {
		upload = ClipUpload { CreateClipTexture(width, height), width, height, 0 };
		if (upload.texture == nullptr)
			return UploadStep::Failed;
	}
	const int sliceRows = static_cast<int>(std::max<size_t>(1, UploadSliceBytes / rowBytes));
	while (upload.rows < height && spent < UploadBudgetBytes) {
		const int rows = std::min(sliceRows, height - upload.rows);
		if (!hd_loader::UploadImageRows(job, Gpu.device, upload.texture, width, height, upload.rows, rows))
			return UploadStep::Failed;
		upload.rows += rows;
		spent += rowBytes * static_cast<size_t>(rows);
	}
	if (upload.rows < height)
		return UploadStep::Waiting;
	hd_loader::CancelImage(job); // the decoded pixels are no longer needed
	return UploadStep::Done;
#endif
}

/** Abandons a clip's pending upgrade (eviction, teardown). */
void CancelUpgrade(ClipSlot &slot)
{
	if (slot.upgradeJob != 0) {
		hd_loader::CancelImage(slot.upgradeJob);
		ReleaseUpload(slot);
		--Gpu.loadsInFlight;
	}
	slot.upgradeJob = 0;
	slot.upgradeDivisor = 0;
	slot.upgradeQueued = false;
}

/** Lists a clip whose decode was just started. */
void TrackInFlight(int clipIndex)
{
	ClipSlot &slot = Gpu.clips[clipIndex];
	++Gpu.loadsInFlight;
	if (!slot.inFlight) {
		slot.inFlight = true;
		Gpu.inFlight.push_back(clipIndex);
	}
}

/**
 * Queues an unrequested clip; `urgent` puts it (or moves an already queued one) ahead of every queued load. `ifRoom`:
 * see HdSpritesGpuPrefetchClipsIfRoom (any other request for the clip lifts it).
 */
void QueueClip(int clip, bool urgent, bool ifRoom = false)
{
	ClipSlot &slot = Gpu.clips[clip];
	if (slot.state == ClipState::Queued && !ifRoom)
		slot.ifRoom = false;
	if (slot.state == ClipState::Queued && urgent) {
		const auto it = std::find(Gpu.queue.begin(), Gpu.queue.end(), clip);
		if (it != Gpu.queue.begin()) {
			if (it != Gpu.queue.end())
				Gpu.queue.erase(it);
			Gpu.queue.push_front(clip);
		}
		return;
	}
	if (slot.state != ClipState::Unrequested)
		return;
	slot.state = ClipState::Queued;
	slot.ifRoom = ifRoom;
	if (urgent)
		Gpu.queue.push_front(clip);
	else
		Gpu.queue.push_back(clip);
}

bool ValidClip(int clip)
{
	return EnsureClipSlots() && clip >= 0 && static_cast<size_t>(clip) < Gpu.clips.size();
}

/** A Loading clip's decode and upload are over (installed or failed). */
void EndLoading(const ClipSlot &slot)
{
	--Gpu.loadsInFlight;
	const size_t bytes = static_cast<size_t>(slot.textureWidth) * slot.textureHeight * 4;
	Gpu.loadingBytes = Gpu.loadingBytes > bytes ? Gpu.loadingBytes - bytes : 0;
}

/** Polls (and uploads) one in-flight clip; false once it has nothing in flight any more. */
bool PumpClip(int clipIndex, size_t &spent)
{
	const auto &clips = HdSpritesIndex().clips;
	ClipSlot &slot = Gpu.clips[clipIndex];
	if (slot.state == ClipState::Resident && slot.upgradeJob != 0) {
		if (slot.upload.texture == nullptr) {
			const hd_loader::JobState state = hd_loader::PollImage(slot.upgradeJob);
			if (state == hd_loader::JobState::Pending)
				return true;
			if (state != hd_loader::JobState::Ready) {
				--Gpu.loadsInFlight;
				hd_loader::CancelImage(slot.upgradeJob);
				slot.divisor = std::min(slot.divisor, slot.upgradeDivisor); // keep the coarse copy, do not retry
				slot.upgradeJob = 0;
				slot.upgradeDivisor = 0;
				return false;
			}
		}
		int width = 0;
		int height = 0;
		ClipTextureSize(clips[clipIndex], slot.upgradeDivisor, width, height);
		const UploadStep step = ContinueUpload(slot, slot.upgradeJob, width, height, spent);
		if (step == UploadStep::Waiting)
			return true;
		--Gpu.loadsInFlight;
		if (step == UploadStep::Done) {
			InstallClipTexture(clipIndex, slot, slot.upload.texture, width, height, slot.upgradeDivisor);
			slot.upload = {};
		} else {
			hd_loader::CancelImage(slot.upgradeJob);
			ReleaseUpload(slot);
			slot.divisor = std::min(slot.divisor, slot.upgradeDivisor); // keep the coarse copy, do not retry
		}
		slot.upgradeJob = 0;
		slot.upgradeDivisor = 0;
		return false;
	}
	if (slot.state != ClipState::Loading)
		return false; // evicted or released since
	if (slot.upload.texture == nullptr) {
		const hd_loader::JobState state = hd_loader::PollImage(slot.job);
		if (state == hd_loader::JobState::Pending)
			return true;
		if (state != hd_loader::JobState::Ready) {
			EndLoading(slot);
			LogError("HD sprites: {} failed to load; it stays classic", clips[clipIndex].key);
			hd_loader::CancelImage(slot.job);
			slot.state = ClipState::Failed;
			slot.job = 0;
			return false;
		}
	}
	const UploadStep step = ContinueUpload(slot, slot.job, slot.textureWidth, slot.textureHeight, spent);
	if (step == UploadStep::Waiting)
		return true;
	EndLoading(slot);
	if (step == UploadStep::Done) {
		InstallClipTexture(clipIndex, slot, slot.upload.texture, slot.textureWidth, slot.textureHeight, slot.divisor);
		slot.upload = {};
	} else {
		LogError("HD sprites: could not upload {}", clips[clipIndex].key);
		hd_loader::CancelImage(slot.job);
		ReleaseUpload(slot);
		ReleaseClipTexture(slot);
		slot.state = ClipState::Failed;
	}
	slot.job = 0;
	return false;
}

void PumpLoads()
{
	const auto &clips = HdSpritesIndex().clips;
	// Pinned clips come back after a device loss (HdSpritesGpuRelease) or a clip-index change.
	for (const int clip : Gpu.pinnedClips) {
		if (static_cast<size_t>(clip) < Gpu.clips.size())
			QueueClip(clip, false);
	}
	// Only the clips with work in flight are visited (the index holds ~1800 clips); finished decodes are written to
	// their textures in request order within the frame's upload budget.
	size_t spent = 0;
	for (size_t i = 0; i < Gpu.inFlight.size();) {
		const int clipIndex = Gpu.inFlight[i];
		if (static_cast<size_t>(clipIndex) < Gpu.clips.size() && PumpClip(clipIndex, spent)) {
			++i;
			continue;
		}
		if (static_cast<size_t>(clipIndex) < Gpu.clips.size())
			Gpu.clips[clipIndex].inFlight = false;
		Gpu.inFlight.erase(Gpu.inFlight.begin() + static_cast<ptrdiff_t>(i));
	}
	while (Gpu.loadsInFlight < MaxLoadsInFlight && !Gpu.queue.empty()) {
		const int clipIndex = Gpu.queue.front();
		Gpu.queue.pop_front();
		ClipSlot &slot = Gpu.clips[clipIndex];
		if (slot.state != ClipState::Queued)
			continue;
		const hd_sprites::ClipInfo &clip = clips[clipIndex];
		const int divisor = Gpu.textureDivisor;
		int width = 0;
		int height = 0;
		ClipTextureSize(clip, divisor, width, height);
		const size_t bytes = static_cast<size_t>(width) * height * 4;
		if (slot.ifRoom && Gpu.lru.totalBytes() + Gpu.loadingBytes + bytes > Gpu.lru.budget()) {
			slot.state = ClipState::Unrequested; // no room: it loads on first use instead
			slot.ifRoom = false;
			continue;
		}
		slot.ifRoom = false;
		slot.divisor = divisor;
		slot.textureWidth = width;
		slot.textureHeight = height;
		slot.job = RequestClipImage(clip, slot.divisor);
		slot.state = ClipState::Loading;
		Gpu.loadingBytes += bytes;
		TrackInFlight(clipIndex);
	}
	// Sharper copies of resident clips after missing ones: the coarse texture keeps drawing until the new one is in.
	while (Gpu.loadsInFlight < MaxLoadsInFlight && !Gpu.upgradeQueue.empty()) {
		const int clipIndex = Gpu.upgradeQueue.front();
		Gpu.upgradeQueue.pop_front();
		ClipSlot &slot = Gpu.clips[clipIndex];
		if (!slot.upgradeQueued)
			continue;
		slot.upgradeQueued = false;
		if (slot.state != ClipState::Resident || slot.divisor <= Gpu.textureDivisor)
			continue;
		slot.upgradeDivisor = Gpu.textureDivisor;
		slot.upgradeJob = RequestClipImage(clips[clipIndex], slot.upgradeDivisor);
		TrackInFlight(clipIndex);
	}
}

/** Clips whose texture the GPU failed to fill after the upload call returned (browser) stay classic from now on. */
void DropFailedUploads()
{
#ifdef __EMSCRIPTEN__
	for (int texture = dx_hd_take_failed_upload(); texture != 0; texture = dx_hd_take_failed_upload()) {
		for (size_t i = 0; i < Gpu.clips.size(); ++i) {
			ClipSlot &slot = Gpu.clips[i];
			if (slot.texture == nullptr || reinterpret_cast<intptr_t>(slot.texture) != texture)
				continue;
			LogError("HD sprites: the GPU rejected {}; it stays classic", HdSpritesIndex().clips[i].key);
			CancelUpgrade(slot);
			Gpu.lru.erase(static_cast<int>(i));
			ReleaseClipTexture(slot);
			slot.state = ClipState::Failed;
			break;
		}
	}
#endif
}

/** Queues a sharper copy of a resident clip drawn coarser than the current textureDivisor (zoom, canvas growth). */
void RequestUpgrade(int clipIndex)
{
	ClipSlot &slot = Gpu.clips[clipIndex];
	if (slot.state != ClipState::Resident || slot.divisor <= Gpu.textureDivisor || slot.upgradeQueued || slot.upgradeJob != 0)
		return;
	slot.upgradeQueued = true;
	Gpu.upgradeQueue.push_back(clipIndex);
}

void EvictOverBudget()
{
	for (const int clipIndex : Gpu.lru.evict(Gpu.frame)) {
		ClipSlot &slot = Gpu.clips[clipIndex];
		CancelUpgrade(slot);
		ReleaseClipTexture(slot);
		slot.state = ClipState::Unrequested;
	}
	if (Gpu.lru.totalBytes() > Gpu.lru.budget() && !Gpu.loggedBudget) {
		LogError("HD sprites: this frame needs {} MiB of clip textures (budget {} MiB)", Gpu.lru.totalBytes() >> 20, Gpu.lru.budget() >> 20);
		Gpu.loggedBudget = true;
	}
}

/** Points the external clips of this frame's 3D warrior imposters at their freshly rendered textures. */
void InstallImposters(const render3d::SkinnedImposterOutput *outputs, int count)
{
	for (int i = 0; i < count; ++i) {
		const render3d::SkinnedImposterOutput &output = outputs[i];
		if (!ValidClip(output.clip) || output.view == nullptr)
			continue;
		ClipSlot &slot = Gpu.clips[output.clip];
		if (!slot.external && slot.state != ClipState::Unrequested)
			continue; // a clip of the pack: never replaced
		if (slot.view != output.view) {
			slot.external = true;
			ReleaseClipTexture(slot);
			slot.view = output.view;
		}
		slot.external = true;
		slot.state = ClipState::Resident;
		slot.divisor = 1;
	}
}

float LightFactor(int lightTableIndex)
{
	if (lightTableIndex <= 0)
		return 1.0F;
	// Light tables change per level and the curve is cheap (16 x 256 lookups): rebuild it once per prepared frame.
	if (!Gpu.ambientValid) {
		gbuffer::BuildClassicAmbientCurve(std::span<const SDL_Color, 256>(logical_palette.data(), 256), LightTables, Gpu.ambientCurve);
		Gpu.ambientValid = true;
	}
	return Gpu.ambientCurve[static_cast<size_t>(std::min(lightTableIndex, 15))];
}

/** The palette fade (system vs logical palette) the classic frame goes through, as one brightness factor. */
float PaletteFade()
{
	double logical = 0.0;
	double system = 0.0;
	for (size_t i = 0; i < 256; ++i) {
		logical += logical_palette[i].r + logical_palette[i].g + logical_palette[i].b;
		system += system_palette[i].r + system_palette[i].g + system_palette[i].b;
	}
	return logical > 0.0 ? static_cast<float>(std::clamp(system / logical, 0.0, 1.0)) : 1.0F;
}

} // namespace

bool HdSpritesGpuAvailable()
{
	static const bool UserEnabled = GetWebGpuPlatform().queryFlag("hdSprites");
	return UserEnabled && HdGraphicsEnabled() && WebGpuIsReady();
}

bool HdSpritesGpuRequestClip(int clip)
{
	if (!EnsureClipSlots() || clip < 0 || static_cast<size_t>(clip) >= Gpu.clips.size())
		return false;
	ClipSlot &slot = Gpu.clips[clip];
	switch (slot.state) {
	case ClipState::Resident:
		Gpu.lru.touch(clip, Gpu.frame);
		return true;
	case ClipState::Unrequested:
		slot.state = ClipState::Queued;
		Gpu.queue.push_back(clip);
		return false;
	default:
		return false;
	}
}

hd_sprites::ClipStatus HdSpritesGpuClipStatus(int clip)
{
	if (!HdSpritesGpuAvailable() || !ValidClip(clip))
		return hd_sprites::ClipStatus::Unavailable;
	switch (Gpu.clips[clip].state) {
	case ClipState::Resident:
		Gpu.lru.touch(clip, Gpu.frame);
		return hd_sprites::ClipStatus::Resident;
	case ClipState::Failed:
		return hd_sprites::ClipStatus::Unavailable;
	default:
		return hd_sprites::ClipStatus::Loading;
	}
}

void HdSpritesGpuRequestClipUrgent(int clip)
{
	if (ValidClip(clip))
		QueueClip(clip, true);
}

void HdSpritesGpuSetPinnedClips(std::span<const int> clips)
{
	if (std::equal(clips.begin(), clips.end(), Gpu.pinnedClips.begin(), Gpu.pinnedClips.end()))
		return;
	for (const int clip : Gpu.pinnedClips)
		Gpu.lru.setPinned(clip, false);
	Gpu.pinnedClips.assign(clips.begin(), clips.end());
	for (const int clip : Gpu.pinnedClips) {
		Gpu.lru.setPinned(clip, true);
		if (ValidClip(clip))
			QueueClip(clip, false);
	}
}

void HdSpritesGpuPrefetchClips(std::span<const int> clips)
{
	for (const int clip : clips) {
		if (ValidClip(clip))
			QueueClip(clip, false);
	}
}

void HdSpritesGpuPrefetchClipsIfRoom(std::span<const int> clips)
{
	for (const int clip : clips) {
		if (ValidClip(clip))
			QueueClip(clip, false, /*ifRoom=*/true);
	}
}

size_t HdSpritesGpuClipBytes(int clip)
{
	const auto &clips = HdSpritesIndex().clips;
	if (clip < 0 || static_cast<size_t>(clip) >= clips.size())
		return 0;
	int width = 0;
	int height = 0;
	ClipTextureSize(clips[clip], Gpu.textureDivisor, width, height);
	return static_cast<size_t>(width) * height * 4;
}

size_t HdSpritesGpuTextureBudget()
{
	return Gpu.lru.budget();
}

bool HdSpritesGpuFetchIndex(std::string &text, bool &failed)
{
	return hd_loader::FetchText("hd/sprites.txt", text, failed);
}

bool HdSpritesGpuFetchText(const char *url, std::string &text, bool &failed)
{
	return hd_loader::FetchText(url, text, failed);
}

void HdSpritesGpuPrepare(const HdSpritesGpuFrame &frame)
{
	Gpu.gpuInstances.clear();
	Gpu.drawClips.clear();
	Gpu.shadowClips.clear();
	Gpu.outlineSlots.clear();
	Gpu.ambientValid = false;
	ReleaseFrameBindGroup();
	// Texel density the screen can show: canvas pixels per back-buffer pixel times back-buffer pixels per world pixel
	// (2 under zoom, hd_view_transform.hpp). The grids are 4 texels per world pixel.
	const float canvasScale = std::min(static_cast<float>(frame.drawableWidth) / std::max(frame.sourceWidth, 1),
	    static_cast<float>(frame.drawableHeight) / std::max(frame.sourceHeight, 1));
	const float worldScale = std::max(HdSpritesFrameScale(), 1.0F);
	// The 3D warrior imposters requested by the world pass are rendered now (and the skinned renderer's frame resets).
	int imposterCount = 0;
	const render3d::SkinnedImposterOutput *imposters = render3d::SkinnedPrepareFrame(frame.device, frame.queue, canvasScale * worldScale, imposterCount);
	if (!HdSpritesGpuAvailable() || !EnsureClipSlots() || frame.device == nullptr)
		return;
	StartPipelines(frame.device, frame.surfaceFormat);
	InstallImposters(imposters, imposterCount);

	Gpu.textureDivisor = hd_sprites::SelectTextureDivisor(canvasScale * worldScale);

	const std::span<const HdSpriteInstance> instances = HdSpritesFrameInstances();
	// Clips drawn this frame (also by a re-present without a world pass) must survive eviction; resident clips loaded
	// for a coarser view (zoomed in since) reload sharper in the background.
	for (const HdSpriteInstance &instance : instances) {
		Gpu.lru.touch(instance.clip, Gpu.frame);
		if (static_cast<size_t>(instance.clip) < Gpu.clips.size())
			RequestUpgrade(instance.clip);
	}
	DropFailedUploads();
	PumpLoads();
	EvictOverBudget();

	const hd_sprites::OcclusionTracker &occlusion = HdSpritesOcclusion();
	const hd_sprites::PixelRect bounds = HdSpritesOcclusionBounds();
	// Legacy frames multiply by the legacy light mask; hybrid frames light the texels as cards (HybridSpriteShaderSource).
	Gpu.hybridFrame = !frame.legacyComposite;
	const bool canLight = Gpu.hybridFrame
	    ? frame.hybridParamsBuffer != nullptr && frame.hybridVolumeView != nullptr
	    : frame.lightMaskView != nullptr;
	// The pipeline is taken (or, if its background compile is still running, created) only by a frame that draws.
	WGPURenderPipeline pipeline = nullptr;
	if (canLight && !instances.empty() && !bounds.empty()
	    && occlusion.width() == frame.sourceWidth && occlusion.height() == frame.sourceHeight)
		pipeline = Gpu.hybridFrame ? HybridPipeline() : LegacyPipeline();
	if (pipeline != nullptr) {
		const auto &clips = HdSpritesIndex().clips;
		const float fade = PaletteFade();
		for (size_t i = 0; i < instances.size(); ++i) {
			const HdSpriteInstance &instance = instances[i];
			if (!occlusion.visible(i) || Gpu.clips[instance.clip].state != ClipState::Resident)
				continue;
			ClipSlot &slot = Gpu.clips[instance.clip];
			// Clip bind groups are made for the pipeline that first draws the clip.
			WGPUBindGroup &clipBindGroup = Gpu.hybridFrame ? slot.hybridBindGroup : slot.bindGroup;
			if (clipBindGroup == nullptr)
				clipBindGroup = CreateClipBindGroup(pipeline, Gpu.hybridFrame ? 2 : 1, slot.view);
			const hd_sprites::ClipInfo &clip = clips[instance.clip];
			const int column = instance.frame % clip.cols;
			const int row = instance.frame / clip.cols;
			const auto sheetWidth = static_cast<float>(clip.sheetWidth());
			const auto sheetHeight = static_cast<float>(clip.sheetHeight());
			const hd_sprites::PixelRect &visible = instance.clipRect;
			Gpu.gpuInstances.push_back(GpuInstance {
			    .rect = { instance.x, instance.y, instance.width, instance.height },
			    .uvRect = { column * clip.cellWidth / sheetWidth, row * clip.cellHeight / sheetHeight,
			        (column + 1) * clip.cellWidth / sheetWidth, (row + 1) * clip.cellHeight / sheetHeight },
			    .clipRect = { static_cast<float>(visible.x), static_cast<float>(visible.y),
			        static_cast<float>(visible.x + visible.width), static_cast<float>(visible.y + visible.height) },
			    // Hybrid: the ambient comes from the light pass, so only the palette fade scales the albedo.
			    .params = { Gpu.hybridFrame ? fade : LightFactor(instance.lightTableIndex) * fade, static_cast<float>(i),
			        Gpu.hybridFrame ? instance.anchorY : 0.0F, instance.selfLit ? 2.0F : (slot.external ? 4.0F : 0.0F) },
			    .tint = { instance.tint.rgb[0], instance.tint.rgb[1], instance.tint.rgb[2], instance.tint.saturation },
			});
			Gpu.drawClips.push_back(instance.clip);
		}
	}
	++Gpu.frame;
	if (Gpu.gpuInstances.empty())
		return;
	// Shadow instances follow the sprites in the buffer (same quad, flag set, pivot row instead of the card anchor).
	// Objects carry their shadow in the art and get none.
	const size_t spriteCount = Gpu.gpuInstances.size();
	Gpu.shadowClips.clear();
	for (size_t i = 0; i < spriteCount; ++i) {
		const HdSpriteInstance &instance = instances[static_cast<size_t>(Gpu.gpuInstances[i].params[1])];
		if (!instance.dropShadow)
			continue;
		GpuInstance shadow = Gpu.gpuInstances[i];
		shadow.params[2] = instance.pivotY;
		shadow.params[3] = 1.0F;
		Gpu.gpuInstances.push_back(shadow);
		Gpu.shadowClips.push_back(Gpu.drawClips[i]);
	}
	// Hover outlines follow the shadows (same quad, expanded by the shader; colour from the current, faded palette).
	Gpu.outlineSlots.assign(spriteCount, -1);
	for (size_t i = 0; i < spriteCount; ++i) {
		const HdSpriteInstance &instance = instances[static_cast<size_t>(Gpu.gpuInstances[i].params[1])];
		if (instance.outlineColor < 0 || instance.outlineColor > 255)
			continue;
		const SDL_Color &color = system_palette[static_cast<size_t>(instance.outlineColor)];
		GpuInstance outline = Gpu.gpuInstances[i];
		outline.params = { 1.0F, outline.params[1], hd_sprites::OutlineRadius(worldScale), 3.0F };
		outline.tint = { color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, hd_sprites::OutlineQuadPadding(worldScale) };
		Gpu.outlineSlots[i] = static_cast<int>(Gpu.gpuInstances.size());
		Gpu.gpuInstances.push_back(outline);
	}

	if (!EnsureFrameResources() || !EnsureInstanceBuffer(Gpu.gpuInstances.size()) || !EnsureOcclusionTexture(frame.sourceWidth, frame.sourceHeight)) {
		Gpu.gpuInstances.clear();
		return;
	}
	wgpuQueueWriteBuffer(frame.queue, Gpu.instanceBuffer, 0, Gpu.gpuInstances.data(), Gpu.gpuInstances.size() * sizeof(GpuInstance));
	GpuGlobals globals {};
	globals.sourceSize = { static_cast<float>(frame.sourceWidth), static_cast<float>(frame.sourceHeight), 0.0F, 0.0F };
	// Lengths in back-buffer pixels scale with the zoom (the instances are in screen pixels, sized for world pixels).
	const float scale = worldScale;
	globals.hybrid = { frame.hybridExposure, SilhouetteNormalRadius * scale, SilhouetteNormalStrength, 1.0F / scale };
	globals.shadow = { HdSpriteShadow::ShearX, HdSpriteShadow::ShearY, HdSpriteShadow::Opacity, HdSpriteShadow::BlurRadius * scale };
	gbuffer::CardToTile(1.0F, 0.0F, 0.0F, globals.cardRight[0], globals.cardRight[1], globals.cardRight[2]);
	gbuffer::CardToTile(0.0F, 1.0F, 0.0F, globals.cardUp[0], globals.cardUp[1], globals.cardUp[2]);
	gbuffer::CardToTile(0.0F, 0.0F, 1.0F, globals.cardNormal[0], globals.cardNormal[1], globals.cardNormal[2]);
	wgpuQueueWriteBuffer(frame.queue, Gpu.globalsBuffer, 0, &globals, sizeof(globals));

	// Only the instance rectangles of the drawn sprites are read by the shader (every quad is clipped to its rectangle);
	// the instances are often spread over the screen, so nearby rectangles are merged rather than taking their union.
	const std::vector<uint16_t> &plane = occlusion.plane();
	MergeUploadRects(spriteCount, Gpu.uploadRects);
	for (const hd_sprites::PixelRect &rect : Gpu.uploadRects) {
		PackRect(plane.data(), occlusion.width(), rect.x, rect.y, rect.width, rect.height, Gpu.uploadStaging);
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = Gpu.occlusionTexture;
		destination.aspect = WGPUTextureAspect_All;
		destination.origin = { static_cast<uint32_t>(rect.x), static_cast<uint32_t>(rect.y), 0 };
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = static_cast<uint32_t>(rect.width * sizeof(uint16_t));
		layout.rowsPerImage = static_cast<uint32_t>(rect.height);
		const WGPUExtent3D extent = { static_cast<uint32_t>(rect.width), static_cast<uint32_t>(rect.height), 1 };
		wgpuQueueWriteTexture(frame.queue, &destination, Gpu.uploadStaging.data(), Gpu.uploadStaging.size() * sizeof(uint16_t), &layout, &extent);
	}

	std::array<WGPUBindGroupEntry, 5> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD sprite frame bind group");
	if (Gpu.hybridFrame) {
		// group 0: the light pass's own HybridParams + occluder volume; group 1: this pass's frame data.
		WGPUBindGroupLayout lightLayout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
		std::array<WGPUBindGroupEntry, 2> lightEntries {};
		lightEntries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
		lightEntries[0].binding = 0;
		lightEntries[0].buffer = frame.hybridParamsBuffer;
		lightEntries[0].size = frame.hybridParamsSize;
		lightEntries[1].binding = 1;
		lightEntries[1].textureView = frame.hybridVolumeView;
		WGPUBindGroupDescriptor lightDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		lightDescriptor.label = View("DamnationX HD sprite hybrid light bind group");
		lightDescriptor.layout = lightLayout;
		lightDescriptor.entryCount = lightEntries.size();
		lightDescriptor.entries = lightEntries.data();
		Gpu.hybridLightBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &lightDescriptor);
		wgpuBindGroupLayoutRelease(lightLayout);

		entries[0].binding = 0;
		entries[0].buffer = Gpu.globalsBuffer;
		entries[0].size = sizeof(GpuGlobals);
		entries[1].binding = 1;
		entries[1].buffer = Gpu.instanceBuffer;
		entries[1].size = Gpu.instanceCapacity * sizeof(GpuInstance);
		entries[2].binding = 2;
		entries[2].sampler = Gpu.sampler;
		entries[3].binding = 3;
		entries[3].textureView = Gpu.occlusionView;
		descriptor.entryCount = 4;
		descriptor.layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 1);
	} else {
		entries[0].binding = 0;
		entries[0].buffer = Gpu.globalsBuffer;
		entries[0].size = sizeof(GpuGlobals);
		entries[1].binding = 1;
		entries[1].buffer = Gpu.instanceBuffer;
		entries[1].size = Gpu.instanceCapacity * sizeof(GpuInstance);
		entries[2].binding = 2;
		entries[2].sampler = Gpu.sampler;
		entries[3].binding = 3;
		entries[3].textureView = frame.lightMaskView;
		entries[4].binding = 4;
		entries[4].textureView = Gpu.occlusionView;
		descriptor.entryCount = entries.size();
		descriptor.layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
	}
	descriptor.entries = entries.data();
	Gpu.frameBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
	wgpuBindGroupLayoutRelease(descriptor.layout);
	if (Gpu.frameBindGroup == nullptr || (Gpu.hybridFrame && Gpu.hybridLightBindGroup == nullptr)) {
		Gpu.gpuInstances.clear();
		return;
	}
	Gpu.framePipeline = pipeline;
}

void HdSpritesGpuEncode(WGPURenderPassEncoder pass)
{
	if (Gpu.gpuInstances.empty() || Gpu.frameBindGroup == nullptr || Gpu.framePipeline == nullptr)
		return;
	// Legacy: group 0 frame, group 1 clip. Hybrid: group 0 light pass data, group 1 frame, group 2 clip.
	const uint32_t clipGroup = Gpu.hybridFrame ? 2 : 1;
	if (Gpu.hybridFrame) {
		wgpuRenderPassEncoderSetPipeline(pass, Gpu.framePipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, Gpu.hybridLightBindGroup, 0, nullptr);
		wgpuRenderPassEncoderSetBindGroup(pass, 1, Gpu.frameBindGroup, 0, nullptr);
	} else {
		wgpuRenderPassEncoderSetPipeline(pass, Gpu.framePipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, Gpu.frameBindGroup, 0, nullptr);
	}
	// All drop shadows first (they sit on the floor under every figure), then the sprites in draw order.
	const size_t spriteCount = Gpu.drawClips.size();
	const auto draw = [&](int clip, size_t instance) {
		const ClipSlot &slot = Gpu.clips[clip];
		WGPUBindGroup clipBindGroup = Gpu.hybridFrame ? slot.hybridBindGroup : slot.bindGroup;
		if (clipBindGroup == nullptr)
			return;
		wgpuRenderPassEncoderSetBindGroup(pass, clipGroup, clipBindGroup, 0, nullptr);
		wgpuRenderPassEncoderDraw(pass, 6, 1, 0, static_cast<uint32_t>(instance));
	};
	for (size_t i = 0; i < Gpu.shadowClips.size(); ++i)
		draw(Gpu.shadowClips[i], spriteCount + i);
	// A hover outline right before its sprite: the sprite covers its inside, later sprites cover it as they cover the sprite.
	for (size_t i = 0; i < spriteCount; ++i) {
		if (i < Gpu.outlineSlots.size() && Gpu.outlineSlots[i] >= 0)
			draw(Gpu.drawClips[i], static_cast<size_t>(Gpu.outlineSlots[i]));
		draw(Gpu.drawClips[i], i);
	}
}

void HdSpritesGpuRelease()
{
	ReleaseFrameBindGroup();
	render3d::SkinnedReleaseGpu();
	for (ClipSlot &slot : Gpu.clips) {
		if (slot.state == ClipState::Loading)
			hd_loader::CancelImage(slot.job);
		if (slot.upgradeJob != 0)
			hd_loader::CancelImage(slot.upgradeJob);
		ReleaseUpload(slot);
		ReleaseClipTexture(slot);
		slot = {};
	}
	Gpu.queue.clear();
	Gpu.upgradeQueue.clear();
	Gpu.inFlight.clear();
	Gpu.loadsInFlight = 0;
	Gpu.loadingBytes = 0;
	Gpu.lru.clear();
	Gpu.gpuInstances.clear();
	Gpu.drawClips.clear();
	Gpu.shadowClips.clear();
	Gpu.outlineSlots.clear();
	if (Gpu.occlusionView != nullptr) wgpuTextureViewRelease(Gpu.occlusionView);
	ReleaseTexture(Gpu.occlusionTexture);
	Gpu.occlusionView = nullptr;
	Gpu.occlusionWidth = 0;
	Gpu.occlusionHeight = 0;
	if (Gpu.instanceBuffer != nullptr) {
		wgpuBufferDestroy(Gpu.instanceBuffer);
		wgpuBufferRelease(Gpu.instanceBuffer);
	}
	Gpu.instanceBuffer = nullptr;
	Gpu.instanceCapacity = 0;
	if (Gpu.globalsBuffer != nullptr) {
		wgpuBufferDestroy(Gpu.globalsBuffer);
		wgpuBufferRelease(Gpu.globalsBuffer);
	}
	Gpu.globalsBuffer = nullptr;
	if (Gpu.sampler != nullptr) wgpuSamplerRelease(Gpu.sampler);
	Gpu.sampler = nullptr;
	Gpu.hybridPipeline.release();
	Gpu.legacyPipeline.release();
	Gpu.hybridFrame = false;
	Gpu.pipelineFormat = WGPUTextureFormat_Undefined;
	Gpu.device = nullptr;
}

} // namespace devilution

#else

#include <string>

#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_sprites_internal.hpp"

namespace devilution {

bool HdSpritesGpuAvailable() { return false; }
bool HdSpritesGpuRequestClip(int) { return false; }
hd_sprites::ClipStatus HdSpritesGpuClipStatus(int) { return hd_sprites::ClipStatus::Unavailable; }
void HdSpritesGpuRequestClipUrgent(int) { }
void HdSpritesGpuSetPinnedClips(std::span<const int>) { }
void HdSpritesGpuPrefetchClips(std::span<const int>) { }
void HdSpritesGpuPrefetchClipsIfRoom(std::span<const int>) { }
size_t HdSpritesGpuClipBytes(int) { return 0; }
size_t HdSpritesGpuTextureBudget() { return 0; }
bool HdSpritesGpuFetchIndex(std::string &, bool &failed)
{
	failed = true;
	return false;
}
bool HdSpritesGpuFetchText(const char *, std::string &, bool &failed)
{
	failed = true;
	return false;
}

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
