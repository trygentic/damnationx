/**
 * @file hd_tiles_webgpu.cpp
 *
 * GPU half of the opt-in HD level tiles (hd_tiles.hpp, `?hdTiles=1`).
 *
 * Atlas pages (hd/tiles-<tileset>-<N>.webp|png, up to 4096 x 4096) are fetched and decoded by the browser with straight
 * (not premultiplied) alpha, so the colour stored under transparent texels survives: the converter keeps the composite
 * colour around each piece there, which lets the colour be filtered bilinearly across piece borders without dark
 * fringes. Pages load lazily as pieces ask for them and are evicted least-recently-used over a byte budget (the town
 * packs its pages by map area, so a view needs one to three of them).
 *
 * Each instance is a quad in back-buffer pixels drawn inside the presenter's composite pass after the full-screen
 * triangle and before the HD sprites. Coverage is the atlas alpha of the nearest texel: the masks are exact classic
 * pixel masks, so neighbouring pieces meet on the 1x pixel grid without seams. A texel is discarded where the dynamic
 * plane (hd_tiles_core.hpp) says a character, item, missile or the UI was drawn over the piece after it.
 *
 * Legacy composite: the classic light-table brightness of the piece (ambient curve) x the legacy light mask x the
 * palette fade, like the HD sprites. Hybrid composite: the frame is full-bright and lit from the G-buffer; each texel
 * takes the hybrid light of the classic pixel under it exactly as the hybrid present shader computes it (kind, height,
 * bilateral upsample of the light mask, palette emissive of the classic index, exposure). In the Cathedral the albedo also
 * takes the local classic hue and chroma (hd_tiles_core.hpp ClassicTint): the HD stone art is near-neutral grey.
 */
#include "engine/render/hd_tiles_webgpu.hpp"

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_sprites_core.hpp"
#include "engine/render/hd_sprites_loader.hpp"
#include "engine/render/hd_tiles.hpp"
#include "engine/render/hd_webgpu_async.hpp"
#include "engine/render/webgpu_platform.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "engine/render/webgpu_shaders.hpp"
#include "utils/log.hpp"

#ifdef __EMSCRIPTEN__
// Straight-alpha variant of hd_sprites_loader's image jobs (own job table): the tile atlases keep colour under alpha 0.
EM_JS(int, dx_hd_tile_request_image, (const char *urlPtr), {
	const state = Module.hdTileJobs || (Module.hdTileJobs = { next: 1, jobs: new Map() });
	const id = state.next++;
	const job = { state: 0, bitmap: null };
	state.jobs.set(id, job);
	const url = UTF8ToString(urlPtr);
	// Revalidated like the sprite grids: a page cached from an earlier build of the pack must not meet a newer index.
	fetch(url, { cache: 'no-cache' })
	    .then(response => {
		    if (!response.ok)
			    throw new Error('HTTP ' + response.status);
		    return response.blob();
	    })
	    .then(blob => createImageBitmap(blob, { premultiplyAlpha: 'none', colorSpaceConversion: 'none' }))
	    .then(bitmap => {
		    if (state.jobs.get(id) !== job) {
			    bitmap.close();
			    return;
		    }
		    job.bitmap = bitmap;
		    job.state = 1;
	    })
	    .catch(error => {
		    console.warn('HD tile page load failed: ' + url + ': ' + error);
		    job.state = 2;
	    });
	return id;
});

EM_JS(int, dx_hd_tile_poll_image, (int id), {
	const state = Module.hdTileJobs;
	const job = state && state.jobs.get(id);
	return job ? job.state : 2;
});

// Copies rows [firstRow, firstRow + rowCount) of a decoded page into the same rows of `texture` (one slice of a page
// upload, see ContinueUpload); the job stays until dx_hd_tile_cancel_image.
EM_JS(int, dx_hd_tile_upload_rows, (int id, void *devicePtr, void *texturePtr, int width, int height, int firstRow, int rowCount), {
	const state = Module.hdTileJobs;
	const job = state && state.jobs.get(id);
	if (!job || !job.bitmap)
		return 0;
	const bitmap = job.bitmap;
	const copyWidth = Math.min(width, bitmap.width);
	const rows = Math.min(firstRow + rowCount, height, bitmap.height) - firstRow;
	if (copyWidth <= 0)
		return 0;
	if (rows <= 0)
		return 1;
	try {
		const device = WebGPU.getJsObject(devicePtr);
		const texture = WebGPU.getJsObject(texturePtr);
		device.queue.copyExternalImageToTexture(
		    { source: bitmap, origin: { x: 0, y: firstRow } },
		    { texture: texture, origin: { x: 0, y: firstRow }, premultipliedAlpha: false },
		    [ copyWidth, rows ]);
		return 1;
	} catch (error) {
		console.warn('HD tile page upload failed: ' + error);
		return 0;
	}
});

EM_JS(void, dx_hd_tile_cancel_image, (int id), {
	const state = Module.hdTileJobs;
	const job = state && state.jobs.get(id);
	if (!job)
		return;
	if (job.bitmap)
		job.bitmap.close();
	state.jobs.delete(id);
});
#else
// Desktop: the shared loader's straight-alpha jobs (hd_sprites_loader_native.cpp).
int dx_hd_tile_request_image(const char *url) { return devilution::hd_loader::RequestStraightImage(url); }
int dx_hd_tile_poll_image(int id) { return static_cast<int>(devilution::hd_loader::PollImage(id)); }
int dx_hd_tile_upload_rows(int id, void *device, void *texture, int width, int height, int firstRow, int rowCount)
{
	return devilution::hd_loader::UploadImageRows(id, static_cast<WGPUDevice>(device), static_cast<WGPUTexture>(texture), width, height, firstRow, rowCount) ? 1 : 0;
}
void dx_hd_tile_cancel_image(int id) { devilution::hd_loader::CancelImage(id); }
#endif

namespace devilution {
namespace {

constexpr size_t PageBudgetBytes = 320U * 1024U * 1024U;
/** Pages decoding or uploading at once (each holds its decoded pixels until its upload is complete). */
constexpr int MaxLoadsInFlight = 2;
/**
 * Page texels a frame writes to the GPU. A 4096 x 4096 page is 64 MiB: written in one go it held the frame for 15-41 ms
 * (desktop) or 2-23 ms (browser). It is written in row slices of at most UploadSliceBytes instead (small enough for
 * Dawn's 4 MiB upload ring) until the frame's budget is used; the page draws (and its pieces leave the classic art) only
 * once every row is in.
 */
constexpr size_t UploadBudgetBytes = 8U * 1024U * 1024U;
constexpr size_t UploadSliceBytes = 2U * 1024U * 1024U;

constexpr char TileCoreShaderSource[] = R"(
// ---- HD tile core (hd_tiles_webgpu.cpp) ----
struct TileGlobals {
  sourceSize: vec4f, // width, height, classic tint strength (hybrid, hd_tiles::ClassicTintStrength), 0
  viewport: vec4f,  // x0, y0, x1, y1: the world viewport (panels and the space below it stay classic)
};

struct TileInstance {
  rect: vec4f,    // x, y, width, height in back-buffer pixels
  uvRect: vec4f,  // u0, v0, u1, v1 in atlas texels
  params: vec4f,  // brightness (legacy: light x fade, hybrid: fade), instance index, opacity, 0
};

struct TileVertexOutput {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,      // atlas texels
  @location(1) screen: vec2f,  // back-buffer pixels
  @location(2) @interpolate(flat) instance: u32,
};

@vertex fn vertexMain(@builtin(vertex_index) vertex: u32, @builtin(instance_index) instanceIndex: u32) -> TileVertexOutput {
  let corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
    vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  let corner = corners[vertex];
  let item = tileInstances[instanceIndex];
  let screen = item.rect.xy + corner * item.rect.zw;
  var output: TileVertexOutput;
  output.position = vec4f(screen.x / tileGlobals.sourceSize.x * 2.0 - 1.0, 1.0 - screen.y / tileGlobals.sourceSize.y * 2.0, 0.0, 1.0);
  output.uv = mix(item.uvRect.xy, item.uvRect.zw, corner);
  output.screen = screen;
  output.instance = instanceIndex;
  return output;
}

// Straight colour filtered bilinearly inside the image, coverage from the nearest texel (exact classic pixel mask).
fn tileTexel(item: TileInstance, uv: vec2f) -> vec4f {
  let dims = vec2f(textureDimensions(tileAtlas));
  let nearest = clamp(uv, item.uvRect.xy, item.uvRect.zw - vec2f(0.01));
  let alpha = textureLoad(tileAtlas, vec2i(floor(nearest)), 0).a;
  let filtered = clamp(uv, item.uvRect.xy + vec2f(0.5), item.uvRect.zw - vec2f(0.5));
  let rgb = textureSampleLevel(tileAtlas, tileSampler, filtered / dims, 0.0).rgb;
  return vec4f(rgb, alpha * item.params.z);
}

// A character, item, missile or the UI was drawn over this pixel after the piece (DynamicPlane: hidden iff plane > i).
fn tileHidden(item: TileInstance, screen: vec2f) -> bool {
  if (any(screen < tileGlobals.viewport.xy) || any(screen >= tileGlobals.viewport.zw)) {
    return true;
  }
  let dims = vec2i(textureDimensions(tileDynamic));
  let pixel = clamp(vec2i(floor(screen)), vec2i(0), dims - vec2i(1));
  return f32(textureLoad(tileDynamic, pixel, 0).r) > item.params.y;
}
)";

constexpr char TileLegacyShaderSource[] = R"(
@group(0) @binding(0) var<uniform> tileGlobals: TileGlobals;
@group(0) @binding(1) var<storage, read> tileInstances: array<TileInstance>;
@group(0) @binding(2) var tileSampler: sampler;
@group(0) @binding(3) var lightMask: texture_2d<f32>;
@group(0) @binding(4) var tileDynamic: texture_2d<u32>;
@group(1) @binding(0) var tileAtlas: texture_2d<f32>;
)";

constexpr char TileLegacyFragmentSource[] = R"(
@fragment fn fragmentMain(input: TileVertexOutput) -> @location(0) vec4f {
  let item = tileInstances[input.instance];
  if (tileHidden(item, input.screen)) {
    discard;
  }
  let texel = tileTexel(item, input.uv);
  if (texel.a < 0.004) {
    discard;
  }
  let maskSize = vec2f(textureDimensions(lightMask));
  let maskPixel = vec2i(clamp(input.screen * maskSize / tileGlobals.sourceSize.xy, vec2f(0.0), maskSize - vec2f(1.0)));
  let lighting = textureLoad(lightMask, maskPixel, 0).rgb * item.params.x;
  return vec4f(texel.rgb * lighting * texel.a, texel.a);
}
)";

/** Hybrid variant, compiled after HybridPreludeWgsl (kinds, heightRows, baseTexelAt, passThrough). */
constexpr char TileHybridShaderSource[] = R"(
struct PresentParams { config: vec4u, values: vec4f, config2: vec4u, paletteEmissive: array<vec4f, 64>, normalLut: array<vec4f, 32> };
@group(0) @binding(0) var<uniform> tileGlobals: TileGlobals;
@group(0) @binding(1) var<storage, read> tileInstances: array<TileInstance>;
@group(0) @binding(2) var tileSampler: sampler;
@group(0) @binding(4) var tileDynamic: texture_2d<u32>;
@group(0) @binding(5) var hybridMask: texture_2d<f32>;
@group(0) @binding(6) var auxTex: texture_2d<u32>;
@group(0) @binding(7) var baseTex: texture_2d<u32>;
@group(0) @binding(8) var indexTex: texture_2d<u32>;
@group(0) @binding(9) var<uniform> present: PresentParams;
@group(0) @binding(10) var tilePalette: texture_2d<f32>;   // rgba8unorm 256x1: the frame's SDL palette (classic colour match)
@group(1) @binding(0) var tileAtlas: texture_2d<f32>;
)";

constexpr char TileHybridFragmentSource[] = R"(
fn tileSrgbToLinear1(c: f32) -> f32 {
  if (c > 0.04045) { return pow((c + 0.055) / 1.055, 2.4); }
  return c / 12.92;
}
fn tileLinearToSrgb1(c: f32) -> f32 {
  if (c > 0.0031308) { return 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
  return c * 12.92;
}
fn tileSrgbToLinear(c: vec3f) -> vec3f { return vec3f(tileSrgbToLinear1(c.x), tileSrgbToLinear1(c.y), tileSrgbToLinear1(c.z)); }
fn tileLinearToSrgb(c: vec3f) -> vec3f { return vec3f(tileLinearToSrgb1(c.x), tileLinearToSrgb1(c.y), tileLinearToSrgb1(c.z)); }
fn tilePaletteEmissive(idx: u32) -> f32 { var v = present.paletteEmissive[idx >> 2u]; return v[idx & 3u]; }

// Classic colour match (hd_tiles_core.hpp ClassicTint / ApplyClassicTint, the C++ reference): the mean linear palette colour
// of the classic pixels on a 3x3 grid, 3 px apart, around this texel (same G-buffer kind, not hidden by a later draw, not
// index 0), divided by its luminance and clamped to [0.4, 2].
fn tileClassicTint(item: TileInstance, screen: vec2f, kind: u32) -> vec3f {
  let dims = vec2i(textureDimensions(indexTex));
  var sum = vec3f(0.0); var count = 0.0;
  for (var j = -1; j <= 1; j++) { for (var i = -1; i <= 1; i++) {
    let s = screen + vec2f(f32(i), f32(j)) * 3.0;
    if (tileHidden(item, s)) { continue; }
    let p = clamp(vec2i(floor(s)), vec2i(0), dims - vec2i(1));
    if ((textureLoad(auxTex, p, 0).r >> 5u) != kind) { continue; }
    let idx = textureLoad(indexTex, p, 0).r;
    if (idx == 0u) { continue; }
    sum += tileSrgbToLinear(textureLoad(tilePalette, vec2i(i32(idx), 0), 0).rgb); count += 1.0;
  } }
  if (count < 1.0) { return vec3f(1.0); }
  let mean = sum / count;
  let luminance = dot(mean, vec3f(0.2126, 0.7152, 0.0722));
  if (luminance < 0.002) { return vec3f(1.0); }
  return clamp(mean / luminance, vec3f(0.4), vec3f(2.0));
}

// Copy of the hybrid present pass's sampleLighting (webgpu_shaders.hpp HybridPresentWgsl).
fn tileSampleLighting(px: vec2i, kind: u32, h: u32) -> vec3f {
  let s = i32(present.config.w);
  if (s == 1) { return textureLoad(hybridMask, px, 0).rgb; }
  let mf = (vec2f(px) - vec2f(1.0)) / 2.0; let m0 = vec2i(floor(mf)); let f = mf - vec2f(m0);
  let zoom = present.config.y; let zoomOffsetX = i32(present.config2.x);
  let viewportH = i32(present.values.x);
  var sum = vec3f(0.0); var wsum = 0.0;
  for (var j = 0; j < 2; j++) { for (var i = 0; i < 2; i++) {
    let m = m0 + vec2i(i, j);
    let sp = m * 2 + 1;
    if (sp.y >= viewportH) { continue; }
    let ta = textureLoad(auxTex, sp, 0).r;
    let tb = baseTexelAt(baseTex, sp, zoom, zoomOffsetX);
    let th = heightRows(ta >> 5u, tb, u32(max(sp.y, 0)), zoom);
    var wx = 1.0 - f.x; if (i == 1) { wx = f.x; }
    var wy = 1.0 - f.y; if (j == 1) { wy = f.y; }
    let w = wx * wy;
    let ok = ((ta >> 5u) == kind) && (abs(i32(th) - i32(h)) < 8);
    if (ok) { sum += textureLoad(hybridMask, m, 0).rgb * w; wsum += w; }
  } }
  if (wsum <= 0.0) { return textureLoad(hybridMask, m0, 0).rgb; }
  return sum / wsum;
}

@fragment fn fragmentMain(input: TileVertexOutput) -> @location(0) vec4f {
  let item = tileInstances[input.instance];
  if (tileHidden(item, input.screen)) {
    discard;
  }
  // G-buffer debug views show the planes.
  if (present.config.z != 0u) {
    discard;
  }
  let texel = tileTexel(item, input.uv);
  if (texel.a < 0.004) {
    discard;
  }
  let dims = vec2i(textureDimensions(auxTex));
  let px = clamp(vec2i(floor(input.screen)), vec2i(0), dims - vec2i(1));
  let a = textureLoad(auxTex, px, 0).r;
  let kind = a >> 5u;
  var lighting = vec3f(0.0);
  var emissive = 0.0;
  if (!passThrough(kind)) {
    let b = baseTexelAt(baseTex, px, present.config.y, i32(present.config2.x));
    var h = heightRows(kind, b, u32(px.y), present.config.y);
    var k = kind;
    if (isWallKind(k) && h > MAX_WALL_ROWS) { k = K_FLOOR; h = 0u; }
    lighting = tileSampleLighting(px, k, h);
    emissive = tilePaletteEmissive(textureLoad(indexTex, px, 0).r);
  }
  var albedo = tileSrgbToLinear(clamp(texel.rgb * item.params.x, vec3f(0.0), vec3f(1.0)));
  let tintStrength = tileGlobals.sourceSize.z;
  if (tintStrength > 0.0) {
    // The texel keeps its luminance; its chromaticity becomes the classic tint plus half its own deviation (ApplyClassicTint).
    let tint = tileClassicTint(item, input.screen, kind);
    let luminance = dot(albedo, vec3f(0.2126, 0.7152, 0.0722));
    var recoloured = vec3f(0.0);
    if (luminance > 0.0) {
      recoloured = luminance * max(tint + (albedo / luminance - vec3f(1.0)) * 0.5, vec3f(0.0));
    }
    albedo = clamp(mix(albedo, recoloured, tintStrength), vec3f(0.0), vec3f(1.0));
  }
  let color = (albedo * lighting + albedo * emissive * present.values.y) * present.values.z;
  return vec4f(tileLinearToSrgb(clamp(color, vec3f(0.0), vec3f(1.0))) * texel.a, texel.a);
}
)";

struct alignas(16) GpuInstance {
	std::array<float, 4> rect;
	std::array<float, 4> uvRect;
	std::array<float, 4> params;
};
static_assert(sizeof(GpuInstance) == 48);

struct alignas(16) GpuGlobals {
	std::array<float, 4> sourceSize;
	std::array<float, 4> viewport;
};

enum class PageState : uint8_t { Unrequested, Queued, Loading, Uploading, Resident, Failed };

struct PageSlot {
	PageState state = PageState::Unrequested;
	int job = 0;
	int width = 0;
	int height = 0;
	int uploadedRows = 0; // Uploading: rows [0, uploadedRows) of the texture are written
	WGPUTexture texture = nullptr;
	WGPUTextureView view = nullptr;
	WGPUBindGroup bindGroup = nullptr;       // legacy pipeline, group 1 (created when first drawn)
	WGPUBindGroup hybridBindGroup = nullptr; // hybrid pipeline, group 1 (created when first drawn)
	uint64_t touchedFrame = 0;               // last Gpu.frame TouchPage marked it used in the LRU
};

struct DrawRun {
	int page;
	uint32_t first;
	uint32_t count;
};

struct HdTilesGpu {
	WGPUDevice device = nullptr;
	WGPUTextureFormat format = WGPUTextureFormat_Undefined;
	// The instance/globals bindings are declared before the core helpers that read them.
	hd_gpu::QuadPipeline legacyPipeline { std::string(TileLegacyShaderSource) + TileCoreShaderSource + TileLegacyFragmentSource,
		"DamnationX HD tile pipeline" };
	hd_gpu::QuadPipeline hybridPipeline { std::string(HybridPreludeWgsl) + TileHybridShaderSource + TileCoreShaderSource + TileHybridFragmentSource,
		"DamnationX HD tile hybrid pipeline" };
	WGPUSampler sampler = nullptr;
	WGPUBuffer globalsBuffer = nullptr;
	WGPUBuffer instanceBuffer = nullptr;
	size_t instanceCapacity = 0;
	WGPUTexture dynamicTexture = nullptr;
	WGPUTextureView dynamicView = nullptr;
	int dynamicWidth = 0;
	int dynamicHeight = 0;
	WGPUBindGroup frameBindGroup = nullptr;
	WGPURenderPipeline framePipeline = nullptr; // the pipeline the prepared frame draws with
	bool hybridFrame = false;

	int tileset = -1;
	const hd_tiles::Manifest *manifest = nullptr;
	std::vector<PageSlot> pages;
	std::deque<int> queue;
	/** Pages Loading or Uploading, in request order (PumpLoads polls only these). */
	std::vector<int> inFlight;
	hd_sprites::TextureLru lru { PageBudgetBytes };
	uint64_t frame = 1;

	std::vector<GpuInstance> gpuInstances;
	std::vector<DrawRun> runs;
	/** Per row of dynamicTexture, a span outside of which it holds 0 (what UploadDynamicPlane last wrote). */
	std::vector<hd_tiles::DynamicPlane::RowSpan> uploadedSpans;
	std::vector<uint16_t> uploadStaging; // one band, rows packed
	std::array<float, 16> ambientCurve {};
	bool ambientValid = false;
};

HdTilesGpu Gpu;

WGPUStringView View(const char *text)
{
	return { text, WGPU_STRLEN };
}

void ReleasePage(PageSlot &slot)
{
	if (slot.state == PageState::Loading || slot.state == PageState::Uploading)
		dx_hd_tile_cancel_image(slot.job);
	if (slot.bindGroup != nullptr) wgpuBindGroupRelease(slot.bindGroup);
	if (slot.hybridBindGroup != nullptr) wgpuBindGroupRelease(slot.hybridBindGroup);
	if (slot.view != nullptr) wgpuTextureViewRelease(slot.view);
	if (slot.texture != nullptr) {
		wgpuTextureDestroy(slot.texture);
		wgpuTextureRelease(slot.texture);
	}
	slot = {};
}

void ReleasePages()
{
	for (PageSlot &slot : Gpu.pages)
		ReleasePage(slot);
	Gpu.pages.clear();
	Gpu.queue.clear();
	Gpu.inFlight.clear();
	Gpu.lru.clear();
}

void ReleaseFrameBindGroup()
{
	if (Gpu.frameBindGroup != nullptr) wgpuBindGroupRelease(Gpu.frameBindGroup);
	Gpu.frameBindGroup = nullptr;
	Gpu.framePipeline = nullptr;
}

/**
 * Starts compiling both pipelines in the background on the first presented frame (title screen), so the first town or
 * dungeon frame does not wait for them; a device or surface format change starts over.
 */
void StartPipelines(WGPUDevice device, WGPUTextureFormat format)
{
	if (Gpu.device != device || Gpu.format != format) {
		if (Gpu.device != nullptr)
			HdTilesGpuRelease();
		Gpu.device = device;
		Gpu.format = format;
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
		globalsDescriptor.label = View("DamnationX HD tile globals");
		globalsDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform;
		globalsDescriptor.size = sizeof(GpuGlobals);
		Gpu.globalsBuffer = wgpuDeviceCreateBuffer(Gpu.device, &globalsDescriptor);
	}
	return Gpu.sampler != nullptr && Gpu.globalsBuffer != nullptr;
}

/** The legacy pipeline (nullptr: creation failed, logged once). */
WGPURenderPipeline LegacyPipeline()
{
	WGPURenderPipeline pipeline = Gpu.legacyPipeline.get(Gpu.device, Gpu.format);
	static bool logged = false;
	if (pipeline == nullptr && !logged) {
		LogError("HD tiles: pipeline creation failed");
		logged = true;
	}
	return pipeline;
}

/** The hybrid pipeline (nullptr: creation failed, logged once; the HD tiles are hidden under hybrid lighting). */
WGPURenderPipeline HybridPipeline()
{
	WGPURenderPipeline pipeline = Gpu.hybridPipeline.get(Gpu.device, Gpu.format);
	static bool logged = false;
	if (pipeline == nullptr && !logged) {
		LogError("HD tiles: hybrid pipeline creation failed; HD tiles are hidden under hybrid lighting");
		logged = true;
	}
	return pipeline;
}

WGPUBindGroup CreatePageBindGroup(WGPURenderPipeline pipeline, WGPUTextureView view)
{
	if (pipeline == nullptr)
		return nullptr;
	WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 1);
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.binding = 0;
	entry.textureView = view;
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.layout = layout;
	descriptor.entryCount = 1;
	descriptor.entries = &entry;
	WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
	wgpuBindGroupLayoutRelease(layout);
	return bindGroup;
}

void FailUpload(int pageIndex, PageSlot &slot)
{
	LogError("HD tiles: could not upload {}", Gpu.manifest->pages[pageIndex].path);
	ReleasePage(slot);
	slot.state = PageState::Failed;
}

/** A decoded page: creates its texture; its rows are written by ContinueUpload over the next frames. */
void BeginUpload(int pageIndex, PageSlot &slot)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD tile atlas page");
	descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { static_cast<uint32_t>(slot.width), static_cast<uint32_t>(slot.height), 1 };
	descriptor.format = WGPUTextureFormat_RGBA8Unorm;
	slot.texture = wgpuDeviceCreateTexture(Gpu.device, &descriptor);
	slot.uploadedRows = 0;
	slot.state = PageState::Uploading;
	if (slot.texture == nullptr)
		FailUpload(pageIndex, slot);
}

/** The page's rows are all in: it becomes drawable. */
void FinishUpload(int pageIndex, PageSlot &slot)
{
	dx_hd_tile_cancel_image(slot.job); // releases the decoded pixels
	slot.job = 0;
	slot.view = wgpuTextureCreateView(slot.texture, nullptr);
	slot.state = PageState::Resident;
	Gpu.lru.insert(pageIndex, static_cast<size_t>(slot.width) * slot.height * 4, Gpu.frame);
	Log("HD tiles: page {} resident ({} MiB of pages)", Gpu.manifest->pages[pageIndex].path, Gpu.lru.totalBytes() >> 20);
}

/** Writes row slices of an uploading page while `budget` lasts; true when the page is complete (or failed). */
bool ContinueUpload(int pageIndex, PageSlot &slot, size_t &budget)
{
	const size_t rowBytes = static_cast<size_t>(slot.width) * 4;
	const int sliceRows = static_cast<int>(std::max<size_t>(1, UploadSliceBytes / rowBytes));
	while (slot.uploadedRows < slot.height && budget > 0) {
		const int rows = std::min(sliceRows, slot.height - slot.uploadedRows);
		if (dx_hd_tile_upload_rows(slot.job, Gpu.device, slot.texture, slot.width, slot.height, slot.uploadedRows, rows) == 0) {
			FailUpload(pageIndex, slot);
			return true;
		}
		slot.uploadedRows += rows;
		const size_t bytes = rowBytes * static_cast<size_t>(rows);
		budget = bytes < budget ? budget - bytes : 0;
	}
	if (slot.uploadedRows < slot.height)
		return false;
	FinishUpload(pageIndex, slot);
	return true;
}

void PumpLoads()
{
	if (Gpu.manifest == nullptr)
		return;
	// Finished decodes start uploading; uploads continue in request order within the frame's budget.
	size_t budget = UploadBudgetBytes;
	for (size_t i = 0; i < Gpu.inFlight.size();) {
		const int pageIndex = Gpu.inFlight[i];
		PageSlot &slot = Gpu.pages[pageIndex];
		bool done = false;
		if (slot.state == PageState::Loading) {
			const int state = dx_hd_tile_poll_image(slot.job);
			if (state == 1) {
				BeginUpload(pageIndex, slot);
			} else if (state != 0) {
				LogError("HD tiles: {} failed to load; its pieces stay classic", Gpu.manifest->pages[pageIndex].path);
				slot.state = PageState::Failed;
				slot.job = 0;
				done = true;
			}
		}
		if (slot.state == PageState::Uploading && budget > 0)
			done = ContinueUpload(pageIndex, slot, budget);
		else if (slot.state != PageState::Loading && slot.state != PageState::Uploading)
			done = true;
		if (done)
			Gpu.inFlight.erase(Gpu.inFlight.begin() + static_cast<std::ptrdiff_t>(i));
		else
			++i;
	}
	while (static_cast<int>(Gpu.inFlight.size()) < MaxLoadsInFlight && !Gpu.queue.empty()) {
		const int pageIndex = Gpu.queue.front();
		Gpu.queue.pop_front();
		PageSlot &slot = Gpu.pages[pageIndex];
		if (slot.state != PageState::Queued)
			continue;
		const hd_tiles::Page &page = Gpu.manifest->pages[pageIndex];
		std::string url = page.path;
		std::replace(url.begin(), url.end(), '\\', '/');
		slot.width = page.width;
		slot.height = page.height;
		slot.job = dx_hd_tile_request_image(url.c_str());
		slot.state = PageState::Loading;
		Gpu.inFlight.push_back(pageIndex);
	}
}

void EvictOverBudget()
{
	for (const int pageIndex : Gpu.lru.evict(Gpu.frame)) {
		ReleasePage(Gpu.pages[pageIndex]);
		Log("HD tiles: evicted page {}", pageIndex);
	}
}

float LightFactor(int lightTableIndex)
{
	if (lightTableIndex <= 0)
		return 1.0F;
	if (!Gpu.ambientValid) {
		gbuffer::BuildClassicAmbientCurve(std::span<const SDL_Color, 256>(logical_palette.data(), 256), LightTables, Gpu.ambientCurve);
		Gpu.ambientValid = true;
	}
	return Gpu.ambientCurve[static_cast<size_t>(std::min(lightTableIndex, 15))];
}

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

bool EnsureInstanceBuffer(size_t count)
{
	if (Gpu.instanceBuffer != nullptr && Gpu.instanceCapacity >= count)
		return true;
	if (Gpu.instanceBuffer != nullptr) {
		wgpuBufferDestroy(Gpu.instanceBuffer);
		wgpuBufferRelease(Gpu.instanceBuffer);
	}
	Gpu.instanceCapacity = std::max<size_t>(256, std::bit_ceil(count));
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD tile instances");
	descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage;
	descriptor.size = Gpu.instanceCapacity * sizeof(GpuInstance);
	Gpu.instanceBuffer = wgpuDeviceCreateBuffer(Gpu.device, &descriptor);
	return Gpu.instanceBuffer != nullptr;
}

/** Marks `page` used this frame; a page is drawn by many instances, so the LRU lookup runs once per page and frame. */
void TouchPage(int page)
{
	PageSlot &slot = Gpu.pages[page];
	if (slot.touchedFrame == Gpu.frame)
		return;
	slot.touchedFrame = Gpu.frame;
	Gpu.lru.touch(page, Gpu.frame);
}

bool EnsureDynamicTexture(int width, int height)
{
	if (Gpu.dynamicTexture != nullptr && Gpu.dynamicWidth == width && Gpu.dynamicHeight == height)
		return true;
	if (Gpu.dynamicView != nullptr) wgpuTextureViewRelease(Gpu.dynamicView);
	if (Gpu.dynamicTexture != nullptr) {
		wgpuTextureDestroy(Gpu.dynamicTexture);
		wgpuTextureRelease(Gpu.dynamicTexture);
	}
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD tile dynamic plane");
	descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
	descriptor.format = WGPUTextureFormat_R16Uint;
	Gpu.dynamicTexture = wgpuDeviceCreateTexture(Gpu.device, &descriptor);
	Gpu.dynamicView = Gpu.dynamicTexture != nullptr ? wgpuTextureCreateView(Gpu.dynamicTexture, nullptr) : nullptr;
	Gpu.dynamicWidth = width;
	Gpu.dynamicHeight = height;
	Gpu.uploadedSpans.assign(static_cast<size_t>(height), {}); // a new texture is all 0
	return Gpu.dynamicView != nullptr;
}

/**
 * Brings dynamicTexture up to date with the plane. Only the rows' non-zero spans differ from 0, in the plane and in the
 * texture, so bands of rows are written over the union of both spans (most of the frame stays 0: no write).
 */
void UploadDynamicPlane(WGPUQueue queue, const hd_tiles::DynamicPlane &plane)
{
	constexpr int BandRows = 64;
	const std::vector<uint16_t> &values = plane.plane();
	const auto &spans = plane.rowSpans();
	for (int y0 = 0; y0 < plane.height(); y0 += BandRows) {
		const int y1 = std::min(y0 + BandRows, plane.height());
		int first = plane.width();
		int end = 0;
		for (int y = y0; y < y1; ++y) {
			for (const hd_tiles::DynamicPlane::RowSpan &span : { spans[y], Gpu.uploadedSpans[y] }) {
				if (span.empty())
					continue;
				first = std::min(first, span.first);
				end = std::max(end, span.end);
			}
		}
		if (end <= first)
			continue;
		// Rows packed: the browser copies the whole data range of a texture write, every full plane row it spans.
		const int width = end - first;
		Gpu.uploadStaging.resize(static_cast<size_t>(width) * (y1 - y0));
		for (int y = y0; y < y1; ++y)
			std::copy_n(values.data() + static_cast<size_t>(y) * plane.width() + first, width, Gpu.uploadStaging.data() + static_cast<size_t>(y - y0) * width);
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = Gpu.dynamicTexture;
		destination.aspect = WGPUTextureAspect_All;
		destination.origin = { static_cast<uint32_t>(first), static_cast<uint32_t>(y0), 0 };
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = static_cast<uint32_t>(width * sizeof(uint16_t));
		layout.rowsPerImage = static_cast<uint32_t>(y1 - y0);
		const WGPUExtent3D extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(y1 - y0), 1 };
		wgpuQueueWriteTexture(queue, &destination, Gpu.uploadStaging.data(), Gpu.uploadStaging.size() * sizeof(uint16_t), &layout, &extent);
	}
	Gpu.uploadedSpans = spans;
}

} // namespace

bool HdTilesGpuAvailable()
{
	static const bool UserEnabled = GetWebGpuPlatform().queryFlag("hdTiles");
	return UserEnabled && HdGraphicsEnabled() && WebGpuIsReady();
}

bool HdTilesGpuFlag(const char *name)
{
	return GetWebGpuPlatform().queryFlag(name);
}

bool HdTilesGpuFetchText(const char *url, std::string &text, bool &failed)
{
	return hd_loader::FetchText(url, text, failed);
}

void HdTilesGpuUseManifest(int tileset, const hd_tiles::Manifest &manifest)
{
	if (Gpu.tileset == tileset && Gpu.manifest == &manifest)
		return;
	ReleasePages();
	Gpu.tileset = tileset;
	Gpu.manifest = &manifest;
	Gpu.pages.resize(manifest.pages.size());
}

bool HdTilesGpuRequestPage(int page)
{
	if (page < 0 || static_cast<size_t>(page) >= Gpu.pages.size())
		return false;
	PageSlot &slot = Gpu.pages[page];
	switch (slot.state) {
	case PageState::Resident:
		TouchPage(page);
		return true;
	case PageState::Unrequested:
		slot.state = PageState::Queued;
		Gpu.queue.push_back(page);
		return false;
	default:
		return false;
	}
}

void HdTilesGpuPrepare(const HdTilesGpuFrame &frame)
{
	Gpu.gpuInstances.clear();
	Gpu.runs.clear();
	Gpu.ambientValid = false;
	ReleaseFrameBindGroup();
	if (!HdTilesGpuAvailable() || frame.device == nullptr)
		return;
	StartPipelines(frame.device, frame.surfaceFormat);

	const std::span<const HdTileInstance> instances = HdTilesFrameInstances();
	const hd_tiles::Manifest *manifest = HdTilesFrameManifest();
	// Pages drawn this frame (also by a re-present without a world pass) must survive eviction.
	if (manifest != nullptr && manifest == Gpu.manifest) {
		for (const HdTileInstance &instance : instances)
			TouchPage(manifest->images[instance.image].page);
	}
	PumpLoads();
	EvictOverBudget();

	const hd_tiles::DynamicPlane &plane = HdTilesDynamicPlane();
	Gpu.hybridFrame = !frame.legacyComposite;
	const bool drawable = manifest != nullptr && manifest == Gpu.manifest && !instances.empty()
	    && plane.width() == frame.sourceWidth && plane.height() == frame.sourceHeight;
	// The pipeline is taken (or, if its background compile is still running, created) only by a frame that draws.
	WGPURenderPipeline pipeline = nullptr;
	if (drawable) {
		if (Gpu.hybridFrame) {
			if (frame.hybridMaskView != nullptr && frame.auxView != nullptr && frame.baseView != nullptr && frame.indexView != nullptr
			    && frame.paletteView != nullptr && frame.presentParamsBuffer != nullptr)
				pipeline = HybridPipeline();
		} else if (frame.lightMaskView != nullptr) {
			pipeline = LegacyPipeline();
		}
	}
	if (pipeline != nullptr) {
		// The painted town tiles are brighter than the classic night palette. Lower their
		// exposure together (ground, buildings and trees) to soften the HD toggle.
		constexpr float TownExposure = 0.8F;
		const float exposure = HdTilesFrameTileset() == 0 ? TownExposure : 1.0F;
		const float fade = PaletteFade();
		for (size_t i = 0; i < instances.size(); ++i) {
			const HdTileInstance &instance = instances[i];
			const hd_tiles::Image &image = manifest->images[instance.image];
			const PageSlot &slot = Gpu.pages[image.page];
			if (slot.state != PageState::Resident)
				continue;
			const uint32_t index = static_cast<uint32_t>(Gpu.gpuInstances.size());
			Gpu.gpuInstances.push_back(GpuInstance {
			    .rect = { instance.x, instance.y, instance.width, instance.height },
			    .uvRect = { static_cast<float>(image.u), static_cast<float>(image.v), static_cast<float>(image.u + image.width),
			        static_cast<float>(image.v + image.height) },
			    // Hybrid: the light comes from the G-buffer; exposure and palette fade scale the albedo.
			    .params = { (Gpu.hybridFrame ? fade : LightFactor(instance.lightTableIndex) * fade) * exposure, static_cast<float>(i),
			        instance.translucent ? 0.5F : 1.0F, 0.0F },
			});
			if (!Gpu.runs.empty() && Gpu.runs.back().page == image.page && Gpu.runs.back().first + Gpu.runs.back().count == index)
				++Gpu.runs.back().count;
			else
				Gpu.runs.push_back(DrawRun { image.page, index, 1 });
		}
	}
	++Gpu.frame;
	if (Gpu.gpuInstances.empty())
		return;
	if (!EnsureFrameResources() || !EnsureInstanceBuffer(Gpu.gpuInstances.size()) || !EnsureDynamicTexture(frame.sourceWidth, frame.sourceHeight)) {
		Gpu.gpuInstances.clear();
		Gpu.runs.clear();
		return;
	}
	// Page bind groups are made for the pipeline that first draws the page.
	for (const DrawRun &run : Gpu.runs) {
		PageSlot &slot = Gpu.pages[run.page];
		WGPUBindGroup &pageBindGroup = Gpu.hybridFrame ? slot.hybridBindGroup : slot.bindGroup;
		if (pageBindGroup == nullptr)
			pageBindGroup = CreatePageBindGroup(pipeline, slot.view);
	}
	wgpuQueueWriteBuffer(frame.queue, Gpu.instanceBuffer, 0, Gpu.gpuInstances.data(), Gpu.gpuInstances.size() * sizeof(GpuInstance));
	const hd_tiles::PixelRect viewport = HdTilesFrameViewport();
	const GpuGlobals globals {
		{ static_cast<float>(frame.sourceWidth), static_cast<float>(frame.sourceHeight),
		    Gpu.hybridFrame ? hd_tiles::ClassicTintStrength(HdTilesFrameTileset()) : 0.0F, 0.0F },
		{ static_cast<float>(viewport.x), static_cast<float>(viewport.y), static_cast<float>(viewport.x + viewport.width),
		    static_cast<float>(viewport.y + viewport.height) },
	};
	wgpuQueueWriteBuffer(frame.queue, Gpu.globalsBuffer, 0, &globals, sizeof(globals));

	UploadDynamicPlane(frame.queue, plane);

	std::array<WGPUBindGroupEntry, 10> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	entries[0].binding = 0;
	entries[0].buffer = Gpu.globalsBuffer;
	entries[0].size = sizeof(GpuGlobals);
	entries[1].binding = 1;
	entries[1].buffer = Gpu.instanceBuffer;
	entries[1].size = Gpu.instanceCapacity * sizeof(GpuInstance);
	entries[2].binding = 2;
	entries[2].sampler = Gpu.sampler;
	entries[3].binding = 4;
	entries[3].textureView = Gpu.dynamicView;
	size_t entryCount = 4;
	if (Gpu.hybridFrame) {
		entries[4].binding = 5;
		entries[4].textureView = frame.hybridMaskView;
		entries[5].binding = 6;
		entries[5].textureView = frame.auxView;
		entries[6].binding = 7;
		entries[6].textureView = frame.baseView;
		entries[7].binding = 8;
		entries[7].textureView = frame.indexView;
		entries[8].binding = 9;
		entries[8].buffer = frame.presentParamsBuffer;
		entries[8].size = frame.presentParamsSize;
		entries[9].binding = 10;
		entries[9].textureView = frame.paletteView;
		entryCount = 10;
	} else {
		entries[4].binding = 3;
		entries[4].textureView = frame.lightMaskView;
		entryCount = 5;
	}
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD tile frame bind group");
	descriptor.layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
	descriptor.entryCount = entryCount;
	descriptor.entries = entries.data();
	Gpu.frameBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
	wgpuBindGroupLayoutRelease(descriptor.layout);
	if (Gpu.frameBindGroup == nullptr) {
		Gpu.gpuInstances.clear();
		Gpu.runs.clear();
		return;
	}
	Gpu.framePipeline = pipeline;
}

void HdTilesGpuEncode(WGPURenderPassEncoder pass)
{
	if (Gpu.runs.empty() || Gpu.frameBindGroup == nullptr || Gpu.framePipeline == nullptr)
		return;
	wgpuRenderPassEncoderSetPipeline(pass, Gpu.framePipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, Gpu.frameBindGroup, 0, nullptr);
	int boundPage = -1;
	for (const DrawRun &run : Gpu.runs) {
		const PageSlot &slot = Gpu.pages[run.page];
		WGPUBindGroup pageBindGroup = Gpu.hybridFrame ? slot.hybridBindGroup : slot.bindGroup;
		if (pageBindGroup == nullptr)
			continue;
		if (run.page != boundPage) {
			wgpuRenderPassEncoderSetBindGroup(pass, 1, pageBindGroup, 0, nullptr);
			boundPage = run.page;
		}
		wgpuRenderPassEncoderDraw(pass, 6, run.count, 0, run.first);
	}
}

void HdTilesGpuRelease()
{
	ReleaseFrameBindGroup();
	ReleasePages();
	Gpu.tileset = -1;
	Gpu.manifest = nullptr;
	Gpu.gpuInstances.clear();
	Gpu.runs.clear();
	if (Gpu.dynamicView != nullptr) wgpuTextureViewRelease(Gpu.dynamicView);
	if (Gpu.dynamicTexture != nullptr) {
		wgpuTextureDestroy(Gpu.dynamicTexture);
		wgpuTextureRelease(Gpu.dynamicTexture);
	}
	Gpu.dynamicView = nullptr;
	Gpu.dynamicTexture = nullptr;
	Gpu.dynamicWidth = 0;
	Gpu.dynamicHeight = 0;
	Gpu.uploadedSpans.clear();
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
	Gpu.format = WGPUTextureFormat_Undefined;
	Gpu.device = nullptr;
}

} // namespace devilution

#else

#include <string>

#include "engine/render/hd_tiles.hpp"

namespace devilution {

bool HdTilesGpuAvailable() { return false; }
bool HdTilesGpuFlag(const char *) { return false; }
bool HdTilesGpuFetchText(const char *, std::string &, bool &failed)
{
	failed = true;
	return false;
}
void HdTilesGpuUseManifest(int, const hd_tiles::Manifest &) { }
bool HdTilesGpuRequestPage(int) { return false; }

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
