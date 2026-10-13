/**
 * @file hd_ui_webgpu.cpp
 *
 * GPU half of the opt-in HD UI (hd_ui.hpp, `?hdUi=1`).
 *
 * UI sheets (hd/ui.txt) are fetched and decoded by the browser on first use (hd_sprites_loader.hpp) and kept under a
 * 160 MiB LRU budget; sheets drawn this frame are never evicted. True-colour sheets load at the resolution the canvas
 * can show (SelectUiTextureDivisor); indexed sheets (the bitmap fonts) always load at full size since their texels
 * are palette indices.
 *
 * Each visible layer is a quad in back-buffer pixels (UI pixels mapped through the UI scale, ui_scale.hpp) drawn after the HD sprites inside the presenter's composite
 * pass, so it lands at canvas resolution. The fragment shader keeps the classic painter order through the
 * visibility plane (hd_ui_core.hpp: a texel of layer i shows iff i >= plane[pixel]) and applies:
 *  - true-colour art: the palette fade, and a colour translation through a 32^3 lookup table built from the TRN
 *    (hd_ui::BuildTrnLut: the red tint of unusable items, the spell type colours);
 *  - indexed art: index -> TRN -> the live system palette, exactly as the classic blit (fonts keep their colours);
 *  - tints: a flat translucent colour (the inventory slot background of magic and unique items).
 *
 * Nothing HD is shown before it is known to work, since an HD layer hides the classic pixels below it:
 *  - the pipelines are created asynchronously on the first frame (the main menu); until the layer pipeline exists, or
 *    if it fails (an unsupported shader), every layer stays classic;
 *  - every sheet upload runs inside a validation error scope, and an indexed sheet is read back and checked against
 *    the classic glyphs it replaces (hd_ui::CheckIndexedSheet): a sheet becomes drawable only once that passed, and a
 *    failed one keeps its frames classic (the font is never hidden behind an empty or colour-shifted texture).
 * The TRN lookup tables are computed by a compute shader when the palette or the TRN pool changes (integer maths,
 * identical to hd_ui::BuildTrnLut); the CPU builds them, a few per frame, only until that pipeline is ready.
 */
#include "engine/render/hd_ui_webgpu.hpp"

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "engine/palette.h"
#include "engine/render/hd_sprites_core.hpp"
#include "engine/render/hd_sprites_loader.hpp"
#include "engine/render/hd_ui.hpp"
#include "engine/render/ui_scale.hpp"
#include "engine/render/webgpu_platform.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "utils/log.hpp"

namespace devilution {
namespace {

constexpr size_t TextureBudgetBytes = 160U * 1024U * 1024U;
constexpr int MaxLoadsInFlight = 4;
constexpr int LutSize = hd_ui::TrnLutSize;
constexpr int LutSlots = static_cast<int>(hd_ui::TrnPool::Capacity);
constexpr uint32_t LutCells = LutSize * LutSize * LutSize;
/** Lookup tables computed per submit; the staging buffer holds one 256-byte aligned copy source per table. */
constexpr uint32_t LutBatch = 8;
constexpr uint32_t LutStagingRowBytes = 256;
constexpr uint32_t LutStagingSlotBytes = LutStagingRowBytes * LutSize * LutSize;

/**
 * Asynchronous results (pipelines, error scopes, readbacks) arrive on the main thread: from the browser's event loop,
 * or natively from wgpuInstanceProcessEvents, which the platform calls after every present (never from Dawn's threads).
 */
#ifdef __EMSCRIPTEN__
constexpr WGPUCallbackMode AsyncCallbackMode = WGPUCallbackMode_AllowSpontaneous;
#else
constexpr WGPUCallbackMode AsyncCallbackMode = WGPUCallbackMode_AllowProcessEvents;
#endif

constexpr char UiShaderSource[] = R"(
struct Globals {
  sourceSize: vec4f,
  // The layers are in UI pixels (ui_scale.hpp): screen = ui * uiMap.x + uiMap.yz; uiMap.w = quarters (4 = 1x).
  uiMap: vec4f,
  uiSize: vec4f,
};

struct Instance {
  rect: vec4f,     // x, y, width, height in layer (UI) pixels
  uvRect: vec4f,   // u0, v0, u1, v1 in the sheet
  clipRect: vec4f, // x0, y0, x1, y1 visible layer (UI) pixels
  params: vec4f,   // layer index, opacity (x palette fade for true colour), TRN slot (-1 none), mode (0 rgba, 1 indexed, 2 tint)
  tint: vec4f,     // tint colour (straight)
};

@group(0) @binding(0) var<uniform> globals: Globals;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(0) @binding(2) var uiSampler: sampler;
@group(0) @binding(3) var visibility: texture_2d<u32>;
@group(0) @binding(4) var trnLut: texture_3d<f32>;
@group(0) @binding(5) var<storage, read> tables: array<u32>; // 256 palette entries (rgba8), then 256 per TRN slot
@group(1) @binding(0) var sheet: texture_2d<f32>;

const LUT_SIZE: f32 = 32.0;
const LUT_SLOTS: f32 = 64.0;

struct VertexOutput {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
  @location(1) screen: vec2f,
  @location(2) @interpolate(flat) instance: u32,
};

@vertex fn vertexMain(@builtin(vertex_index) vertex: u32, @builtin(instance_index) instanceIndex: u32) -> VertexOutput {
  let corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
    vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  let corner = corners[vertex];
  let item = instances[instanceIndex];
  let screen = (item.rect.xy + corner * item.rect.zw) * globals.uiMap.x + globals.uiMap.yz;
  var output: VertexOutput;
  output.position = vec4f(screen.x / globals.sourceSize.x * 2.0 - 1.0, 1.0 - screen.y / globals.sourceSize.y * 2.0, 0.0, 1.0);
  output.uv = mix(item.uvRect.xy, item.uvRect.zw, corner);
  output.screen = screen;
  output.instance = instanceIndex;
  return output;
}

fn frameUv(item: Instance, uv: vec2f) -> vec2f {
  let halfTexel = vec2f(0.5) / vec2f(textureDimensions(sheet));
  return clamp(uv, item.uvRect.xy + halfTexel, item.uvRect.zw - halfTexel);
}

@fragment fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
  let item = instances[input.instance];
  let uv = frameUv(item, input.uv);
  let sampled = textureSampleLevel(sheet, uiSampler, uv, 0.0);
  // The UI pixel under this screen pixel, exactly as ScreenToUi maps the mouse (the first UI pixel takes the remainder).
  let quarters = i32(globals.uiMap.w);
  let fromOrigin = vec2i(floor(input.screen)) - vec2i(globals.uiMap.yz);
  let pixel = vec2f(clamp(max(fromOrigin * 4 / quarters, vec2i(0)), vec2i(0), vec2i(globals.uiSize.xy) - vec2i(1)));
  if (any(pixel < item.clipRect.xy) || any(pixel >= item.clipRect.zw)) {
    discard;
  }
  let first = textureLoad(visibility, vec2i(pixel), 0).r;
  if (f32(first) > item.params.x + 0.5) {
    discard;
  }
  let mode = item.params.w;
  let opacity = item.params.y;
  if (mode > 1.5) {
    let a = item.tint.a * opacity;
    return vec4f(item.tint.rgb * a, a);
  }
  if (mode > 0.5) {
    let dims = vec2f(textureDimensions(sheet));
    let texel = textureLoad(sheet, vec2i(clamp(floor(uv * dims), vec2f(0.0), dims - vec2f(1.0))), 0);
    let coverage = sampled.g;
    if (coverage < 0.02 || texel.a < 0.5) {
      discard;
    }
    var index = u32(texel.r * 255.0 + 0.5);
    if (item.params.z >= 0.0) {
      index = tables[256u + u32(item.params.z) * 256u + index] & 255u;
    }
    let colour = unpack4x8unorm(tables[index]);
    let a = coverage * opacity;
    return vec4f(colour.rgb * a, a);
  }
  if (sampled.a < 0.004) {
    discard;
  }
  var rgb = sampled.rgb;
  if (item.params.z >= 0.0) {
    let straight = clamp(sampled.rgb / sampled.a, vec3f(0.0), vec3f(1.0));
    let lutCoord = (straight * (LUT_SIZE - 1.0) + vec3f(0.5)) / LUT_SIZE;
    let z = (item.params.z * LUT_SIZE + straight.b * (LUT_SIZE - 1.0) + 0.5) / (LUT_SIZE * LUT_SLOTS);
    rgb = textureSampleLevel(trnLut, uiSampler, vec3f(lutCoord.x, lutCoord.y, z), 0.0).rgb * sampled.a;
  }
  return vec4f(rgb, sampled.a) * opacity;
}
)";

/**
 * The TRN lookup tables (hd_ui::BuildNearestGrid + hd_ui::BuildTrnLut) on the GPU. Every step is integer maths, so the
 * texels are bit-identical to the CPU tables; they are written to a staging buffer with 256-byte rows and copied into
 * the 3D texture (a storage-texture write would go through a float -> unorm conversion).
 */
constexpr char LutShaderSource[] = R"(
struct LutParams {
  firstSlot: u32,
  slotCount: u32,
  pad0: u32,
  pad1: u32,
};

@group(0) @binding(0) var<storage, read> palette: array<u32>;        // logical palette, 0x00BBGGRR
@group(0) @binding(1) var<storage, read_write> nearest: array<u32>; // 32^3 nearest palette indices
@group(0) @binding(2) var<storage, read> tables: array<u32>;         // system palette, then 256 entries per TRN slot
@group(0) @binding(3) var<storage, read_write> staging: array<u32>; // per batch slot: 32 x 32 rows of 64 words
@group(0) @binding(4) var<uniform> params: LutParams;

fn channel(colour: u32, k: u32) -> i32 {
  return i32((colour >> (8u * k)) & 255u);
}

fn gridColour(cell: u32) -> vec3i {
  return vec3i(i32(cell & 31u), i32((cell >> 5u) & 31u), i32(cell >> 10u)) * 255 / 31;
}

@compute @workgroup_size(64) fn nearestMain(@builtin(global_invocation_id) id: vec3u) {
  let cell = id.x;
  if (cell >= 32768u) {
    return;
  }
  let c = gridColour(cell);
  var best = 0u;
  var bestDistance = 2147483647;
  for (var i = 0u; i < 256u; i = i + 1u) {
    let p = palette[i];
    let dr = channel(p, 0u) - c.x;
    let dg = channel(p, 1u) - c.y;
    let db = channel(p, 2u) - c.z;
    let distance = 4 * dg * dg + 3 * dr * dr + 2 * db * db;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  nearest[cell] = best;
}

@compute @workgroup_size(64) fn lutMain(@builtin(global_invocation_id) id: vec3u) {
  let cell = id.x;
  if (cell >= 32768u || id.y >= params.slotCount) {
    return;
  }
  let slot = params.firstSlot + id.y;
  let source = nearest[cell];
  let to = tables[256u + slot * 256u + source] & 255u;
  let pf = palette[source];
  let pt = palette[to];
  let shift = vec3i(channel(pt, 0u) - channel(pf, 0u), channel(pt, 1u) - channel(pf, 1u), channel(pt, 2u) - channel(pf, 2u));
  let o = clamp(gridColour(cell) + shift, vec3i(0), vec3i(255));
  let r = cell & 31u;
  let gb = cell >> 5u; // g + 32 * b: the staging row
  staging[id.y * 65536u + gb * 64u + r] = u32(o.x) | (u32(o.y) << 8u) | (u32(o.z) << 16u) | 0xFF000000u;
}
)";

struct GpuGlobals {
	std::array<float, 4> sourceSize;
	std::array<float, 4> uiMap;
	std::array<float, 4> uiSize;
};

struct GpuInstance {
	std::array<float, 4> rect;
	std::array<float, 4> uvRect;
	std::array<float, 4> clipRect;
	std::array<float, 4> params;
	std::array<float, 4> tint;
};

struct GpuLutParams {
	uint32_t firstSlot;
	uint32_t slotCount;
	uint32_t pad[2];
};

/** Unrequested -> Queued -> Loading (fetch, decode, then the upload checks) -> Resident or Failed. */
enum class SheetState : uint8_t { Unrequested, Queued, Loading, Resident, Failed };

enum class PipelineState : uint8_t { None, Pending, Ready, Failed };

/**
 * An uploaded sheet texture waiting for its checks: the validation error scope of the upload and, for an indexed
 * sheet, the readback of its texels. Owned by HdUiGpu::checks; the callbacks only set flags. `sheet` < 0: abandoned
 * (evicted or device teardown), released once its callbacks have run.
 */
struct UploadCheck {
	int sheet = -1;
	int divisor = 1;
	bool upgrade = false;
	bool indexed = false;
	int width = 0;
	int height = 0;
	WGPUTexture texture = nullptr;
	WGPUTextureView view = nullptr;
	WGPUBindGroup bindGroup = nullptr;
	WGPUBuffer readback = nullptr;
	uint32_t readbackPitch = 0;
	int pendingCallbacks = 0;
	bool scopeFailed = false;
	bool mapFailed = false;
	std::string error;
};

struct SheetSlot {
	SheetState state = SheetState::Unrequested;
	int job = 0;
	int divisor = 1;
	int textureWidth = 0;
	int textureHeight = 0;
	WGPUTexture texture = nullptr;
	WGPUTextureView view = nullptr;
	WGPUBindGroup bindGroup = nullptr;
	int upgradeJob = 0;
	int upgradeDivisor = 0;
	UploadCheck *check = nullptr;        // first upload being checked (state Loading)
	UploadCheck *upgradeCheck = nullptr; // sharper copy being checked (the coarse one stays resident)
};

struct HdUiGpu {
	WGPUDevice device = nullptr;
	WGPUQueue gpuQueue = nullptr;
	WGPUTextureFormat pipelineFormat = WGPUTextureFormat_Undefined;
	WGPUBindGroupLayout frameLayout = nullptr;
	WGPUBindGroupLayout sheetLayout = nullptr;
	WGPUPipelineLayout pipelineLayout = nullptr;
	WGPURenderPipeline pipeline = nullptr;
	PipelineState pipelineState = PipelineState::None;
	/** Bumped whenever the GPU objects are released: async results of an older generation are discarded. */
	uintptr_t generation = 1;
	WGPUSampler sampler = nullptr;
	WGPUBuffer globalsBuffer = nullptr;
	WGPUBuffer instanceBuffer = nullptr;
	size_t instanceCapacity = 0;
	WGPUBuffer tableBuffer = nullptr;
	WGPUTexture lutTexture = nullptr;
	WGPUTextureView lutView = nullptr;
	WGPUTexture planeTexture = nullptr;
	WGPUTextureView planeView = nullptr;
	int planeWidth = 0;
	int planeHeight = 0;
	WGPUTexture blankTexture = nullptr;
	WGPUTextureView blankView = nullptr;
	WGPUBindGroup blankBindGroup = nullptr;
	WGPUBindGroup frameBindGroup = nullptr;

	// TRN lookup tables on the GPU (LutShaderSource).
	WGPUBindGroupLayout lutLayout = nullptr;
	WGPUPipelineLayout lutPipelineLayout = nullptr;
	WGPUComputePipeline nearestPipeline = nullptr;
	WGPUComputePipeline lutPipeline = nullptr;
	PipelineState nearestState = PipelineState::None;
	PipelineState lutState = PipelineState::None;
	WGPUBuffer lutPaletteBuffer = nullptr;
	WGPUBuffer nearestBuffer = nullptr;
	WGPUBuffer lutStaging = nullptr;
	WGPUBuffer lutParamsBuffer = nullptr;
	WGPUBindGroup lutBindGroup = nullptr;
	bool gpuNearestValid = false; // nearestBuffer holds the grid of lutPalette

	std::vector<SheetSlot> sheets;
	std::vector<UploadCheck *> checks;
	std::deque<int> queue;
	int loadsInFlight = 0;
	hd_sprites::TextureLru lru { TextureBudgetBytes };
	uint64_t frame = 1;
	int textureDivisor = 1;
	bool loggedBudget = false;

	/** TRN slots whose lookup tables are uploaded, and the palette they were built from. */
	size_t lutTables = 0;
	uint32_t lutGeneration = UINT32_MAX;
	std::array<uint8_t, 768> lutPalette {};
	std::vector<uint8_t> lutBuilt;
	std::vector<uint8_t> nearestGrid; // BuildNearestGrid(lutPalette), built on the first CPU table of a palette

	std::vector<GpuInstance> instances;
	std::vector<int> drawSheets; // sheet per instance (-1: tint)
	std::vector<hd_ui::Rect> uploadRects; // plane rectangles uploaded this frame (UploadRects)
	std::vector<uint16_t> uploadStaging;  // one rectangle, rows packed (PackRect)
	std::vector<uint32_t> tables;         // what tableBuffer holds (empty: nothing yet)
	uint32_t tablesTrnGeneration = UINT32_MAX;
};

HdUiGpu Gpu;

WGPUStringView View(const char *text)
{
	return { text, WGPU_STRLEN };
}

std::string ToString(WGPUStringView text)
{
	if (text.data == nullptr)
		return {};
	return text.length == WGPU_STRLEN ? std::string(text.data) : std::string(text.data, text.length);
}

void ReleaseTexture(WGPUTexture &texture, WGPUTextureView &view)
{
	if (view != nullptr) wgpuTextureViewRelease(view);
	if (texture != nullptr) {
		wgpuTextureDestroy(texture);
		wgpuTextureRelease(texture);
	}
	texture = nullptr;
	view = nullptr;
}

void ReleaseBindGroup(WGPUBindGroup &group)
{
	if (group != nullptr) wgpuBindGroupRelease(group);
	group = nullptr;
}

void ReleaseSheet(SheetSlot &slot)
{
	ReleaseBindGroup(slot.bindGroup);
	ReleaseTexture(slot.texture, slot.view);
}

void ReleaseBuffer(WGPUBuffer &buffer)
{
	if (buffer != nullptr) {
		wgpuBufferDestroy(buffer);
		wgpuBufferRelease(buffer);
	}
	buffer = nullptr;
}

void ReleaseCheckTexture(UploadCheck &check)
{
	ReleaseBindGroup(check.bindGroup);
	ReleaseTexture(check.texture, check.view);
}

/** Drops a check whose sheet went away; its callbacks may still run (it is freed in ProcessChecks). */
void AbandonCheck(UploadCheck *&check)
{
	if (check == nullptr)
		return;
	check->sheet = -1;
	ReleaseCheckTexture(*check);
	check = nullptr;
}

WGPUBindGroup CreateSheetBindGroup(WGPUTextureView view)
{
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.binding = 0;
	entry.textureView = view;
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.layout = Gpu.sheetLayout;
	descriptor.entryCount = 1;
	descriptor.entries = &entry;
	return wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
}

WGPUTexture CreateTexture(const char *label, WGPUTextureDimension dimension, uint32_t width, uint32_t height, uint32_t depth,
    WGPUTextureFormat format, WGPUTextureUsage usage)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage;
	descriptor.dimension = dimension;
	descriptor.size = { width, height, depth };
	descriptor.format = format;
	return wgpuDeviceCreateTexture(Gpu.device, &descriptor);
}

WGPUBuffer CreateBuffer(const char *label, WGPUBufferUsage usage, uint64_t size)
{
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage;
	descriptor.size = size;
	return wgpuDeviceCreateBuffer(Gpu.device, &descriptor);
}

WGPUBindGroupLayoutEntry BufferLayoutEntry(uint32_t binding, WGPUShaderStage visibility, WGPUBufferBindingType type)
{
	WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	entry.binding = binding;
	entry.visibility = visibility;
	entry.buffer.type = type;
	return entry;
}

WGPUBindGroupLayoutEntry TextureLayoutEntry(uint32_t binding, WGPUTextureSampleType sampleType, WGPUTextureViewDimension dimension)
{
	WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	entry.binding = binding;
	entry.visibility = WGPUShaderStage_Fragment;
	entry.texture.sampleType = sampleType;
	entry.texture.viewDimension = dimension;
	return entry;
}

WGPUBindGroupLayout CreateBindGroupLayout(const char *label, const WGPUBindGroupLayoutEntry *entries, size_t count)
{
	WGPUBindGroupLayoutDescriptor descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.entryCount = count;
	descriptor.entries = entries;
	return wgpuDeviceCreateBindGroupLayout(Gpu.device, &descriptor);
}

void OnShaderScopePopped(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void *userdata1, void * /*userdata2*/)
{
	if (status == WGPUPopErrorScopeStatus_Success && type != WGPUErrorType_NoError)
		LogError("HD UI: {} does not compile here: {}", static_cast<const char *>(userdata1), ToString(message));
}

/**
 * A shader this browser cannot compile must not take the renderer down: its error is caught by a scope (logged), and
 * the pipelines built from it fail, which keeps the UI classic (or the lookup tables on the CPU).
 */
WGPUShaderModule CreateShader(const char *label, const char *source)
{
	WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	wgsl.code = View(source);
	WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	descriptor.nextInChain = &wgsl.chain;
	descriptor.label = View(label);
	wgpuDevicePushErrorScope(Gpu.device, WGPUErrorFilter_Validation);
	WGPUShaderModule shader = wgpuDeviceCreateShaderModule(Gpu.device, &descriptor);
	WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
	callback.mode = AsyncCallbackMode;
	callback.callback = OnShaderScopePopped;
	callback.userdata1 = const_cast<char *>(label);
	wgpuDevicePopErrorScope(Gpu.device, callback);
	return shader;
}

// ---- Asynchronous pipeline creation ----

void OnRenderPipelineCreated(WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message, void *userdata1, void * /*userdata2*/)
{
	if (reinterpret_cast<uintptr_t>(userdata1) != Gpu.generation) {
		if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
		return;
	}
	if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline != nullptr) {
		Gpu.pipeline = pipeline;
		Gpu.pipelineState = PipelineState::Ready;
		return;
	}
	if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
	Gpu.pipelineState = PipelineState::Failed;
	LogError("HD UI: the layer pipeline could not be created ({}); the UI stays classic", ToString(message));
}

struct ComputeTarget {
	WGPUComputePipeline *pipeline;
	PipelineState *state;
	const char *name;
};

void OnComputePipelineCreated(WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline, WGPUStringView message, void *userdata1, void *userdata2)
{
	const auto *target = static_cast<const ComputeTarget *>(userdata2);
	if (reinterpret_cast<uintptr_t>(userdata1) != Gpu.generation) {
		if (pipeline != nullptr) wgpuComputePipelineRelease(pipeline);
		return;
	}
	if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline != nullptr) {
		*target->pipeline = pipeline;
		*target->state = PipelineState::Ready;
		return;
	}
	if (pipeline != nullptr) wgpuComputePipelineRelease(pipeline);
	*target->state = PipelineState::Failed;
	LogError("HD UI: the {} pipeline could not be created ({}); lookup tables are built on the CPU", target->name, ToString(message));
}

ComputeTarget NearestTarget { &Gpu.nearestPipeline, &Gpu.nearestState, "TRN nearest-colour" };
ComputeTarget LutTarget { &Gpu.lutPipeline, &Gpu.lutState, "TRN lookup" };

void StartRenderPipeline(WGPUTextureFormat format)
{
	WGPUShaderModule shader = CreateShader("DamnationX HD UI shader", UiShaderSource);
	if (shader == nullptr) {
		Gpu.pipelineState = PipelineState::Failed;
		return;
	}
	WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
	blend.color = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
	blend.alpha = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
	WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
	colorTarget.format = format;
	colorTarget.blend = &blend;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = shader;
	fragment.entryPoint = View("fragmentMain");
	fragment.targetCount = 1;
	fragment.targets = &colorTarget;
	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD UI pipeline");
	descriptor.layout = Gpu.pipelineLayout;
	descriptor.vertex.module = shader;
	descriptor.vertex.entryPoint = View("vertexMain");
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	descriptor.fragment = &fragment;
	WGPUCreateRenderPipelineAsyncCallbackInfo callback = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
	callback.mode = AsyncCallbackMode;
	callback.callback = OnRenderPipelineCreated;
	callback.userdata1 = reinterpret_cast<void *>(Gpu.generation);
	Gpu.pipelineState = PipelineState::Pending;
	wgpuDeviceCreateRenderPipelineAsync(Gpu.device, &descriptor, callback);
	wgpuShaderModuleRelease(shader);
}

void StartComputePipelines()
{
	WGPUShaderModule shader = CreateShader("DamnationX HD UI TRN lookup shader", LutShaderSource);
	if (shader == nullptr) {
		Gpu.nearestState = PipelineState::Failed;
		Gpu.lutState = PipelineState::Failed;
		return;
	}
	for (ComputeTarget *target : { &NearestTarget, &LutTarget }) {
		WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		descriptor.label = View(target->name);
		descriptor.layout = Gpu.lutPipelineLayout;
		descriptor.compute.module = shader;
		descriptor.compute.entryPoint = View(target == &NearestTarget ? "nearestMain" : "lutMain");
		WGPUCreateComputePipelineAsyncCallbackInfo callback = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
		callback.mode = AsyncCallbackMode;
		callback.callback = OnComputePipelineCreated;
		callback.userdata1 = reinterpret_cast<void *>(Gpu.generation);
		callback.userdata2 = target;
		*target->state = PipelineState::Pending;
		wgpuDeviceCreateComputePipelineAsync(Gpu.device, &descriptor, callback);
	}
	wgpuShaderModuleRelease(shader);
}

bool CreateLayouts()
{
	const std::array<WGPUBindGroupLayoutEntry, 6> frameEntries {
		BufferLayoutEntry(0, WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform),
		BufferLayoutEntry(1, WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
		[] {
			WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			entry.binding = 2;
			entry.visibility = WGPUShaderStage_Fragment;
			entry.sampler.type = WGPUSamplerBindingType_Filtering;
			return entry;
		}(),
		TextureLayoutEntry(3, WGPUTextureSampleType_Uint, WGPUTextureViewDimension_2D),
		TextureLayoutEntry(4, WGPUTextureSampleType_Float, WGPUTextureViewDimension_3D),
		BufferLayoutEntry(5, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage),
	};
	Gpu.frameLayout = CreateBindGroupLayout("DamnationX HD UI frame layout", frameEntries.data(), frameEntries.size());
	const WGPUBindGroupLayoutEntry sheetEntry = TextureLayoutEntry(0, WGPUTextureSampleType_Float, WGPUTextureViewDimension_2D);
	Gpu.sheetLayout = CreateBindGroupLayout("DamnationX HD UI sheet layout", &sheetEntry, 1);
	if (Gpu.frameLayout == nullptr || Gpu.sheetLayout == nullptr)
		return false;
	const std::array<WGPUBindGroupLayout, 2> groups { Gpu.frameLayout, Gpu.sheetLayout };
	WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	layout.label = View("DamnationX HD UI pipeline layout");
	layout.bindGroupLayoutCount = groups.size();
	layout.bindGroupLayouts = groups.data();
	Gpu.pipelineLayout = wgpuDeviceCreatePipelineLayout(Gpu.device, &layout);

	const std::array<WGPUBindGroupLayoutEntry, 5> lutEntries {
		BufferLayoutEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
		BufferLayoutEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
		BufferLayoutEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage),
		BufferLayoutEntry(3, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage),
		BufferLayoutEntry(4, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform),
	};
	Gpu.lutLayout = CreateBindGroupLayout("DamnationX HD UI TRN lookup layout", lutEntries.data(), lutEntries.size());
	if (Gpu.lutLayout != nullptr) {
		WGPUPipelineLayoutDescriptor lutLayout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		lutLayout.label = View("DamnationX HD UI TRN lookup pipeline layout");
		lutLayout.bindGroupLayoutCount = 1;
		lutLayout.bindGroupLayouts = &Gpu.lutLayout;
		Gpu.lutPipelineLayout = wgpuDeviceCreatePipelineLayout(Gpu.device, &lutLayout);
	}
	return Gpu.pipelineLayout != nullptr;
}

void CreateLutResources()
{
	Gpu.lutPaletteBuffer = CreateBuffer("DamnationX HD UI logical palette", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage, 256 * sizeof(uint32_t));
	Gpu.nearestBuffer = CreateBuffer("DamnationX HD UI nearest colours", WGPUBufferUsage_Storage, LutCells * sizeof(uint32_t));
	Gpu.lutStaging = CreateBuffer("DamnationX HD UI TRN lookup staging", WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
	    static_cast<uint64_t>(LutStagingSlotBytes) * LutBatch);
	Gpu.lutParamsBuffer = CreateBuffer("DamnationX HD UI TRN lookup params", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, sizeof(GpuLutParams));
	if (Gpu.lutLayout == nullptr || Gpu.lutPaletteBuffer == nullptr || Gpu.nearestBuffer == nullptr || Gpu.lutStaging == nullptr
	    || Gpu.lutParamsBuffer == nullptr || Gpu.tableBuffer == nullptr)
		return;
	std::array<WGPUBindGroupEntry, 5> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	const std::array<std::pair<WGPUBuffer, uint64_t>, 5> buffers { {
	    { Gpu.lutPaletteBuffer, 256 * sizeof(uint32_t) },
	    { Gpu.nearestBuffer, LutCells * sizeof(uint32_t) },
	    { Gpu.tableBuffer, sizeof(uint32_t) * 256 * (1 + LutSlots) },
	    { Gpu.lutStaging, static_cast<uint64_t>(LutStagingSlotBytes) * LutBatch },
	    { Gpu.lutParamsBuffer, sizeof(GpuLutParams) },
	} };
	for (size_t i = 0; i < entries.size(); ++i) {
		entries[i].binding = static_cast<uint32_t>(i);
		entries[i].buffer = buffers[i].first;
		entries[i].size = buffers[i].second;
	}
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD UI TRN lookup bind group");
	descriptor.layout = Gpu.lutLayout;
	descriptor.entryCount = entries.size();
	descriptor.entries = entries.data();
	Gpu.lutBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
}

/** Creates the device objects and starts the pipelines (asynchronously) for a new device or surface format. */
bool EnsureResources(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat format)
{
	if (Gpu.device == device && Gpu.pipelineFormat == format && Gpu.frameLayout != nullptr)
		return true;
	if (Gpu.device != nullptr)
		HdUiGpuRelease();
	Gpu.device = device;
	Gpu.gpuQueue = queue;
	Gpu.pipelineFormat = format;
	if (!CreateLayouts())
		return false;

	WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
	samplerDescriptor.addressModeU = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.addressModeV = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
	samplerDescriptor.magFilter = WGPUFilterMode_Linear;
	samplerDescriptor.minFilter = WGPUFilterMode_Linear;
	samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
	Gpu.sampler = wgpuDeviceCreateSampler(device, &samplerDescriptor);

	Gpu.globalsBuffer = CreateBuffer("DamnationX HD UI globals", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform, sizeof(GpuGlobals));
	Gpu.tableBuffer = CreateBuffer("DamnationX HD UI palette and TRN tables", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage,
	    sizeof(uint32_t) * 256 * (1 + LutSlots));
	Gpu.lutTexture = CreateTexture("DamnationX HD UI TRN lookup", WGPUTextureDimension_3D, LutSize, LutSize, LutSize * LutSlots,
	    WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
	Gpu.lutView = Gpu.lutTexture != nullptr ? wgpuTextureCreateView(Gpu.lutTexture, nullptr) : nullptr;
	Gpu.blankTexture = CreateTexture("DamnationX HD UI blank sheet", WGPUTextureDimension_2D, 1, 1, 1, WGPUTextureFormat_RGBA8Unorm,
	    WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
	Gpu.blankView = Gpu.blankTexture != nullptr ? wgpuTextureCreateView(Gpu.blankTexture, nullptr) : nullptr;
	if (Gpu.blankView != nullptr)
		Gpu.blankBindGroup = CreateSheetBindGroup(Gpu.blankView);
	Gpu.lutTables = 0;
	Gpu.lutGeneration = UINT32_MAX;
	Gpu.lutBuilt.clear();
	Gpu.gpuNearestValid = false;
	if (Gpu.sampler == nullptr || Gpu.globalsBuffer == nullptr || Gpu.tableBuffer == nullptr || Gpu.lutView == nullptr || Gpu.blankBindGroup == nullptr)
		return false;
	StartRenderPipeline(format);
	if (Gpu.lutPipelineLayout != nullptr) {
		CreateLutResources();
		if (Gpu.lutBindGroup != nullptr)
			StartComputePipelines();
	}
	return true;
}

bool PipelineReady()
{
	return Gpu.pipelineState == PipelineState::Ready;
}

bool EnsureSheetSlots()
{
	const size_t count = HdUiIndex().sheets.size();
	if (Gpu.sheets.size() != count)
		Gpu.sheets.resize(count);
	return count > 0;
}

int SheetDivisor(const hd_ui::SheetInfo &sheet)
{
	return sheet.indexed ? 1 : Gpu.textureDivisor;
}

void SheetTextureSize(const hd_ui::SheetInfo &sheet, int divisor, int &width, int &height)
{
	width = (sheet.width + divisor - 1) / divisor;
	height = (sheet.height + divisor - 1) / divisor;
}

int RequestSheet(const hd_ui::SheetInfo &sheet, int divisor)
{
	int width = 0;
	int height = 0;
	SheetTextureSize(sheet, divisor, width, height);
	const std::string url = "hd/" + sheet.png;
	// The index's sheet size guards against a cached PNG from another build of the pack (hd_loader::RequestImage).
	return divisor == 1 ? hd_loader::RequestImage(url.c_str(), 0, 0, sheet.width, sheet.height)
	                    : hd_loader::RequestImage(url.c_str(), width, height, sheet.width, sheet.height);
}

// ---- Upload checks ----

void FinishCallback(UploadCheck *check)
{
	--check->pendingCallbacks;
}

void OnUploadScopePopped(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void *userdata1, void * /*userdata2*/)
{
	auto *check = static_cast<UploadCheck *>(userdata1);
	if (check == nullptr)
		return;
	if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
		check->scopeFailed = true;
		check->error = ToString(message);
	}
	FinishCallback(check);
}

void OnReadbackMapped(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void * /*userdata2*/)
{
	auto *check = static_cast<UploadCheck *>(userdata1);
	if (status != WGPUMapAsyncStatus_Success) {
		check->mapFailed = true;
		if (check->error.empty())
			check->error = ToString(message);
	}
	FinishCallback(check);
}

/** Copies an indexed sheet's texels into a mappable buffer (validated by ProcessChecks). */
bool StartReadback(UploadCheck &check)
{
	check.readbackPitch = (static_cast<uint32_t>(check.width) * 4 + 255) & ~255U;
	const uint64_t size = static_cast<uint64_t>(check.readbackPitch) * check.height;
	check.readback = CreateBuffer("DamnationX HD UI sheet check", WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead, size);
	if (check.readback == nullptr)
		return false;
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(Gpu.device, nullptr);
	WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	source.texture = check.texture;
	source.aspect = WGPUTextureAspect_All;
	WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	destination.buffer = check.readback;
	destination.layout.bytesPerRow = check.readbackPitch;
	destination.layout.rowsPerImage = static_cast<uint32_t>(check.height);
	const WGPUExtent3D extent = { static_cast<uint32_t>(check.width), static_cast<uint32_t>(check.height), 1 };
	wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
	WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
	wgpuQueueSubmit(Gpu.gpuQueue, 1, &commands);
	wgpuCommandBufferRelease(commands);
	wgpuCommandEncoderRelease(encoder);
	WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	callback.mode = AsyncCallbackMode;
	callback.callback = OnReadbackMapped;
	callback.userdata1 = &check;
	++check.pendingCallbacks;
	wgpuBufferMapAsync(check.readback, WGPUMapMode_Read, 0, static_cast<size_t>(size), callback);
	return true;
}

/**
 * Uploads a Ready job into a new texture and starts its checks; the sheet becomes drawable in ProcessChecks.
 * Returns false when the upload failed outright.
 */
bool InstallSheet(int index, SheetSlot &slot, int job, int divisor, bool upgrade)
{
	const hd_ui::SheetInfo &sheet = HdUiIndex().sheets[index];
	int width = 0;
	int height = 0;
	SheetTextureSize(sheet, divisor, width, height);
	WGPUTextureUsage usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment;
	if (sheet.indexed)
		usage |= WGPUTextureUsage_CopySrc;
	WGPUTexture texture = CreateTexture("DamnationX HD UI sheet", WGPUTextureDimension_2D, width, height, 1, WGPUTextureFormat_RGBA8Unorm, usage);
	if (texture == nullptr) {
		hd_loader::CancelImage(job);
		return false;
	}
	auto *check = new UploadCheck;
	check->sheet = index;
	check->divisor = divisor;
	check->upgrade = upgrade;
	check->indexed = sheet.indexed;
	check->width = width;
	check->height = height;
	check->texture = texture;
	// A failed copy is a validation error, not a failed call: the scope catches it (and keeps it off the uncaptured
	// error log). The callbacks may run inside the calls below, so the count is set first.
	check->pendingCallbacks = 1;
	wgpuDevicePushErrorScope(Gpu.device, WGPUErrorFilter_Validation);
	const bool uploaded = hd_loader::UploadImage(job, Gpu.device, texture, width, height);
	WGPUPopErrorScopeCallbackInfo scopeCallback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
	scopeCallback.mode = AsyncCallbackMode;
	scopeCallback.callback = OnUploadScopePopped;
	scopeCallback.userdata1 = check;
	wgpuDevicePopErrorScope(Gpu.device, scopeCallback);
	Gpu.checks.push_back(check);
	if (!uploaded) {
		check->sheet = -1;
		ReleaseCheckTexture(*check);
		return false;
	}
	check->view = wgpuTextureCreateView(texture, nullptr);
	check->bindGroup = CreateSheetBindGroup(check->view);
	if (sheet.indexed && !StartReadback(*check))
		check->mapFailed = true;
	(upgrade ? slot.upgradeCheck : slot.check) = check;
	return true;
}

/** True when the indexed sheet a check read back holds the classic glyphs' palette indices. */
bool ValidateIndexedSheet(UploadCheck &check)
{
	const size_t size = static_cast<size_t>(check.readbackPitch) * check.height;
	const auto *texels = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(check.readback, 0, size));
	if (texels == nullptr) {
		check.error = "the readback could not be mapped";
		return false;
	}
	const std::vector<hd_ui::IndexedSheetFrame> frames = HdUiClassicFramesOfSheet(check.sheet);
	const hd_ui::IndexedSheetReport report = hd_ui::CheckIndexedSheet(texels, check.readbackPitch, check.width, check.height,
	    HdUiIndex().scale, frames);
	if (!report.ok) {
		check.error = "indices do not match the classic glyphs (" + std::to_string(report.matched) + " of " + std::to_string(report.compared)
		    + " sampled, " + std::to_string(report.badTexels) + " texels with a partial alpha)";
	}
	return report.ok;
}

/** Applies the finished upload checks: a sheet that passed becomes drawable, one that failed stays classic. */
void ProcessChecks()
{
	const auto &sheets = HdUiIndex().sheets;
	std::erase_if(Gpu.checks, [&](UploadCheck *check) {
		if (check->pendingCallbacks > 0)
			return false;
		const int index = check->sheet;
		bool ok = index >= 0 && !check->scopeFailed && !check->mapFailed;
		if (ok && check->indexed)
			ok = ValidateIndexedSheet(*check);
		if (check->readback != nullptr) {
			if (!check->mapFailed)
				wgpuBufferUnmap(check->readback);
			ReleaseBuffer(check->readback);
		}
		if (index >= 0 && static_cast<size_t>(index) < Gpu.sheets.size()) {
			SheetSlot &slot = Gpu.sheets[index];
			(check->upgrade ? slot.upgradeCheck : slot.check) = nullptr;
			if (ok) {
				ReleaseSheet(slot);
				slot.texture = check->texture;
				slot.view = check->view;
				slot.bindGroup = check->bindGroup;
				check->texture = nullptr;
				check->view = nullptr;
				check->bindGroup = nullptr;
				slot.textureWidth = check->width;
				slot.textureHeight = check->height;
				slot.divisor = check->divisor;
				slot.state = SheetState::Resident;
				Gpu.lru.insert(index, static_cast<size_t>(check->width) * check->height * 4, Gpu.frame);
			} else if (check->upgrade) {
				slot.divisor = std::min(slot.divisor, check->divisor); // keep the coarse copy, do not retry
			} else {
				LogError("HD UI: {} failed its upload check ({}); its frames stay classic", sheets[index].png, check->error);
				slot.state = SheetState::Failed;
			}
		}
		ReleaseCheckTexture(*check);
		delete check;
		return true;
	});
}

void PumpLoads()
{
	ProcessChecks();
	const auto &sheets = HdUiIndex().sheets;
	for (size_t i = 0; i < Gpu.sheets.size(); ++i) {
		SheetSlot &slot = Gpu.sheets[i];
		if (slot.upgradeJob != 0) {
			const hd_loader::JobState state = hd_loader::PollImage(slot.upgradeJob);
			if (state != hd_loader::JobState::Pending) {
				--Gpu.loadsInFlight;
				if (state != hd_loader::JobState::Ready || !InstallSheet(static_cast<int>(i), slot, slot.upgradeJob, slot.upgradeDivisor, /*upgrade=*/true))
					slot.divisor = std::min(slot.divisor, slot.upgradeDivisor); // keep the coarse copy, do not retry
				slot.upgradeJob = 0;
			}
		}
		if (slot.state != SheetState::Loading || slot.job == 0)
			continue;
		const hd_loader::JobState state = hd_loader::PollImage(slot.job);
		if (state == hd_loader::JobState::Pending)
			continue;
		--Gpu.loadsInFlight;
		if (state != hd_loader::JobState::Ready || !InstallSheet(static_cast<int>(i), slot, slot.job, slot.divisor, /*upgrade=*/false)) {
			LogError("HD UI: {} failed to load; its frames stay classic", sheets[i].png);
			slot.state = SheetState::Failed;
		}
		slot.job = 0;
	}
	while (Gpu.loadsInFlight < MaxLoadsInFlight && !Gpu.queue.empty()) {
		const int index = Gpu.queue.front();
		Gpu.queue.pop_front();
		SheetSlot &slot = Gpu.sheets[index];
		if (slot.state != SheetState::Queued)
			continue;
		slot.divisor = SheetDivisor(sheets[index]);
		slot.job = RequestSheet(sheets[index], slot.divisor);
		slot.state = SheetState::Loading;
		++Gpu.loadsInFlight;
	}
	// Sharper copies of sheets loaded for a smaller canvas.
	for (size_t i = 0; i < Gpu.sheets.size() && Gpu.loadsInFlight < MaxLoadsInFlight; ++i) {
		SheetSlot &slot = Gpu.sheets[i];
		const int wanted = SheetDivisor(sheets[i]);
		if (slot.state != SheetState::Resident || slot.upgradeJob != 0 || slot.upgradeCheck != nullptr || slot.divisor <= wanted
		    || !Gpu.lru.contains(static_cast<int>(i)))
			continue;
		slot.upgradeDivisor = wanted;
		slot.upgradeJob = RequestSheet(sheets[i], wanted);
		++Gpu.loadsInFlight;
	}
}

void EvictOverBudget()
{
	for (const int index : Gpu.lru.evict(Gpu.frame)) {
		SheetSlot &slot = Gpu.sheets[index];
		if (slot.upgradeJob != 0) {
			hd_loader::CancelImage(slot.upgradeJob);
			--Gpu.loadsInFlight;
			slot.upgradeJob = 0;
		}
		AbandonCheck(slot.upgradeCheck);
		ReleaseSheet(slot);
		slot.state = SheetState::Unrequested;
	}
	if (Gpu.lru.totalBytes() > Gpu.lru.budget() && !Gpu.loggedBudget) {
		LogError("HD UI: this frame needs {} MiB of UI sheets (budget {} MiB)", Gpu.lru.totalBytes() >> 20, Gpu.lru.budget() >> 20);
		Gpu.loggedBudget = true;
	}
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

uint32_t PackColour(const SDL_Color &c)
{
	return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) | (static_cast<uint32_t>(c.b) << 16);
}

/** Uploads the live palette and the TRN tables when they changed; notes when the lookup tables must be rebuilt. */
void UploadTables()
{
	const hd_ui::TrnPool &trns = HdUiTrns();
	std::vector<uint32_t> &tables = Gpu.tables;
	const size_t count = static_cast<size_t>(256) * (1 + LutSlots);
	bool changed = tables.size() != count || trns.generation() != Gpu.tablesTrnGeneration;
	if (changed) {
		tables.assign(count, 0);
		for (size_t t = 0; t < trns.size() && t < static_cast<size_t>(LutSlots); ++t) {
			const auto &table = trns.table(static_cast<int>(t));
			for (size_t i = 0; i < 256; ++i)
				tables[256 * (1 + t) + i] = table[i];
		}
		Gpu.tablesTrnGeneration = trns.generation();
	}
	for (size_t i = 0; i < 256; ++i) {
		const uint32_t colour = PackColour(system_palette[i]) | 0xFF000000U;
		if (tables[i] != colour) {
			tables[i] = colour;
			changed = true;
		}
	}
	if (changed)
		wgpuQueueWriteBuffer(Gpu.gpuQueue, Gpu.tableBuffer, 0, tables.data(), tables.size() * sizeof(uint32_t));

	std::array<uint8_t, 768> palette;
	for (size_t i = 0; i < 256; ++i) {
		palette[i * 3] = logical_palette[i].r;
		palette[i * 3 + 1] = logical_palette[i].g;
		palette[i * 3 + 2] = logical_palette[i].b;
	}
	if (palette != Gpu.lutPalette || trns.generation() != Gpu.lutGeneration) {
		// A new level palette or a changed pool: every lookup table is rebuilt on its next use.
		if (palette != Gpu.lutPalette || trns.size() < Gpu.lutTables) {
			Gpu.lutBuilt.assign(LutSlots, 0);
			Gpu.nearestGrid.clear();
			Gpu.gpuNearestValid = false;
		}
		Gpu.lutPalette = palette;
		Gpu.lutGeneration = trns.generation();
		Gpu.lutTables = trns.size();
	}
}

/**
 * Computes every lookup table of the pool that is not built yet on the GPU (all of them after a palette change):
 * no CPU work, ready for this frame's draw (queue order). False until the compute pipelines exist.
 */
bool BuildLutsOnGpu()
{
	if (Gpu.nearestState != PipelineState::Ready || Gpu.lutState != PipelineState::Ready || Gpu.lutBindGroup == nullptr)
		return false;
	if (Gpu.lutBuilt.size() != static_cast<size_t>(LutSlots))
		Gpu.lutBuilt.assign(LutSlots, 0);
	const uint32_t tableCount = static_cast<uint32_t>(std::min<size_t>(HdUiTrns().size(), LutSlots));
	uint32_t first = 0;
	while (first < tableCount && Gpu.lutBuilt[first] != 0)
		++first;
	if (first == tableCount)
		return true;
	if (!Gpu.gpuNearestValid) {
		std::array<uint32_t, 256> palette;
		for (size_t i = 0; i < 256; ++i)
			palette[i] = static_cast<uint32_t>(Gpu.lutPalette[i * 3]) | (static_cast<uint32_t>(Gpu.lutPalette[i * 3 + 1]) << 8)
			    | (static_cast<uint32_t>(Gpu.lutPalette[i * 3 + 2]) << 16);
		wgpuQueueWriteBuffer(Gpu.gpuQueue, Gpu.lutPaletteBuffer, 0, palette.data(), sizeof(palette));
	}
	// Batches of consecutive unbuilt slots (each submit carries its own parameters).
	while (first < tableCount) {
		uint32_t count = 0;
		while (first + count < tableCount && count < LutBatch && Gpu.lutBuilt[first + count] == 0)
			++count;
		const GpuLutParams params { first, count, { 0, 0 } };
		wgpuQueueWriteBuffer(Gpu.gpuQueue, Gpu.lutParamsBuffer, 0, &params, sizeof(params));
		WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(Gpu.device, nullptr);
		WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
		wgpuComputePassEncoderSetBindGroup(pass, 0, Gpu.lutBindGroup, 0, nullptr);
		if (!Gpu.gpuNearestValid) {
			wgpuComputePassEncoderSetPipeline(pass, Gpu.nearestPipeline);
			wgpuComputePassEncoderDispatchWorkgroups(pass, LutCells / 64, 1, 1);
			Gpu.gpuNearestValid = true;
		}
		wgpuComputePassEncoderSetPipeline(pass, Gpu.lutPipeline);
		wgpuComputePassEncoderDispatchWorkgroups(pass, LutCells / 64, count, 1);
		wgpuComputePassEncoderEnd(pass);
		wgpuComputePassEncoderRelease(pass);
		for (uint32_t k = 0; k < count; ++k) {
			WGPUTexelCopyBufferInfo source = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
			source.buffer = Gpu.lutStaging;
			source.layout.offset = static_cast<uint64_t>(k) * LutStagingSlotBytes;
			source.layout.bytesPerRow = LutStagingRowBytes;
			source.layout.rowsPerImage = LutSize;
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = Gpu.lutTexture;
			destination.aspect = WGPUTextureAspect_All;
			destination.origin = { 0, 0, (first + k) * LutSize };
			const WGPUExtent3D extent = { LutSize, LutSize, LutSize };
			wgpuCommandEncoderCopyBufferToTexture(encoder, &source, &destination, &extent);
			Gpu.lutBuilt[first + k] = 1;
		}
		WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
		wgpuQueueSubmit(Gpu.gpuQueue, 1, &commands);
		wgpuCommandBufferRelease(commands);
		wgpuCommandEncoderRelease(encoder);
		first += count;
		while (first < tableCount && Gpu.lutBuilt[first] != 0)
			++first;
	}
	return true;
}

/**
 * Copies `rect` of a plane (`pitch` values per row) into `out`, rows packed: the browser copies the whole data range of
 * a texture write, so a write straight from the plane would copy every full row it spans.
 */
void PackRect(const uint16_t *plane, int pitch, const hd_ui::Rect &rect, std::vector<uint16_t> &out)
{
	out.resize(static_cast<size_t>(rect.w) * rect.h);
	for (int row = 0; row < rect.h; ++row)
		std::copy_n(plane + static_cast<size_t>(rect.y + row) * pitch + rect.x, rect.w, out.data() + static_cast<size_t>(row) * rect.w);
}

/**
 * The plane rectangles to upload: the shader reads the plane only inside the clip rectangle of an instance it draws.
 * Nearby clips are merged (fewer uploads) as long as that adds little area: the layers are spread over the screen, so
 * their union is several times larger than the clips.
 */
void UploadRects(const hd_ui::LayerTracker &layers, std::vector<hd_ui::Rect> &rects)
{
	constexpr int MergeSlack = 16384; // texels a merge may add (each upload has a fixed cost)
	const auto area = [](const hd_ui::Rect &r) { return static_cast<int64_t>(r.w) * r.h; };
	rects.clear();
	for (const GpuInstance &instance : Gpu.instances) {
		const auto &layer = layers.layers()[static_cast<size_t>(instance.params[0])];
		hd_ui::Rect rect = layer.placed.clip.intersect(hd_ui::Rect { 0, 0, layers.width(), layers.height() });
		if (rect.empty())
			continue;
		for (size_t j = 0; j < rects.size();) {
			const hd_ui::Rect merged = hd_ui::Union(rects[j], rect);
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

/**
 * CPU fallback until the compute pipelines are ready: builds the lookup table of TRN slot `slot` if needed; at most a
 * few per frame (a level change rebuilds them).
 */
bool EnsureLut(int slot, int &budget)
{
	if (Gpu.lutBuilt.size() != static_cast<size_t>(LutSlots))
		Gpu.lutBuilt.assign(LutSlots, 0);
	if (Gpu.lutBuilt[slot] != 0)
		return true;
	if (budget <= 0)
		return false;
	--budget;
	const auto &table = HdUiTrns().table(slot);
	const std::span<const uint8_t, 768> palette(Gpu.lutPalette.data(), 768);
	if (Gpu.nearestGrid.empty())
		Gpu.nearestGrid = hd_ui::BuildNearestGrid(palette);
	const std::vector<uint8_t> lut = hd_ui::BuildTrnLut(std::span<const uint8_t, 256>(table.data(), 256), palette, Gpu.nearestGrid);
	WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture = Gpu.lutTexture;
	destination.aspect = WGPUTextureAspect_All;
	destination.origin = { 0, 0, static_cast<uint32_t>(slot * LutSize) };
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = LutSize * 4;
	layout.rowsPerImage = LutSize;
	const WGPUExtent3D extent = { LutSize, LutSize, LutSize };
	wgpuQueueWriteTexture(Gpu.gpuQueue, &destination, lut.data(), lut.size(), &layout, &extent);
	Gpu.lutBuilt[slot] = 1;
	return true;
}

bool EnsureInstanceBuffer(size_t count)
{
	if (Gpu.instanceBuffer != nullptr && Gpu.instanceCapacity >= count)
		return true;
	ReleaseBuffer(Gpu.instanceBuffer);
	Gpu.instanceCapacity = std::max<size_t>(64, std::bit_ceil(count));
	Gpu.instanceBuffer = CreateBuffer("DamnationX HD UI layers", WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage, Gpu.instanceCapacity * sizeof(GpuInstance));
	return Gpu.instanceBuffer != nullptr;
}

bool EnsurePlaneTexture(int width, int height)
{
	if (Gpu.planeTexture != nullptr && Gpu.planeWidth == width && Gpu.planeHeight == height)
		return true;
	ReleaseTexture(Gpu.planeTexture, Gpu.planeView);
	Gpu.planeTexture = CreateTexture("DamnationX HD UI visibility plane", WGPUTextureDimension_2D, width, height, 1, WGPUTextureFormat_R16Uint,
	    WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding);
	Gpu.planeView = Gpu.planeTexture != nullptr ? wgpuTextureCreateView(Gpu.planeTexture, nullptr) : nullptr;
	Gpu.planeWidth = width;
	Gpu.planeHeight = height;
	return Gpu.planeView != nullptr;
}

void ReleaseFrameBindGroup()
{
	ReleaseBindGroup(Gpu.frameBindGroup);
}

} // namespace

bool HdUiGpuWanted()
{
	static const bool UserEnabled = GetWebGpuPlatform().queryFlag("hdUi");
	return UserEnabled;
}

bool HdUiGpuFetchIndex(std::string &text, bool &failed)
{
	return hd_loader::FetchText("hd/ui.txt", text, failed);
}

bool HdUiGpuSheetReady(int sheet)
{
	if (Gpu.device == nullptr || !EnsureSheetSlots() || sheet < 0 || static_cast<size_t>(sheet) >= Gpu.sheets.size())
		return false;
	SheetSlot &slot = Gpu.sheets[sheet];
	switch (slot.state) {
	case SheetState::Resident:
		Gpu.lru.touch(sheet, Gpu.frame);
		// Until the layer pipeline exists (or when it failed) nothing HD is drawn: the classic pixels stay.
		return PipelineReady();
	case SheetState::Unrequested:
		slot.state = SheetState::Queued;
		Gpu.queue.push_back(sheet);
		return false;
	default:
		return false;
	}
}

void HdUiGpuPrepare(const HdUiGpuFrame &frame)
{
	Gpu.instances.clear();
	Gpu.drawSheets.clear();
	ReleaseFrameBindGroup();
	if (!HdUiActive() || frame.device == nullptr)
		return;
	if (!EnsureResources(frame.device, frame.queue, frame.surfaceFormat)) {
		LogError("HD UI: GPU resources could not be created");
		return;
	}
	if (!EnsureSheetSlots())
		return;
	// Under a UI scale the layers are in UI pixels, each covering several back-buffer pixels (ui_scale.hpp).
	const HdWorldTransform &uiTransform = GetUiTransform();
	const bool uiScaled = GetUiLayerSurface() != nullptr; // hd_ui.cpp records in UI pixels then
	const int layerWidth = uiScaled ? uiTransform.world.width : frame.sourceWidth;
	const int layerHeight = uiScaled ? uiTransform.world.height : frame.sourceHeight;
	const float uiScale = uiScaled ? uiTransform.Scale() : 1.0F;
	const float canvasScale = uiScale * std::min(static_cast<float>(frame.drawableWidth) / std::max(frame.sourceWidth, 1),
	    static_cast<float>(frame.drawableHeight) / std::max(frame.sourceHeight, 1));
	Gpu.textureDivisor = hd_ui::SelectUiTextureDivisor(canvasScale);

	const hd_ui::LayerTracker &layers = HdUiLayers();
	const auto &list = layers.layers();
	const auto &drawable = layers.drawableFlags();
	for (size_t i = 0; i < list.size(); ++i) {
		const hd_ui::FrameInfo *info = HdUiLayerFrame(list[i].placed);
		if (info != nullptr)
			Gpu.lru.touch(info->sheet, Gpu.frame);
	}
	PumpLoads();
	EvictOverBudget();
	++Gpu.frame;

	const hd_ui::Rect bounds = layers.bounds();
	if (!PipelineReady() || list.empty() || bounds.empty() || layers.width() != layerWidth || layers.height() != layerHeight)
		return;
	const auto &sheets = HdUiIndex().sheets;
	const float fade = PaletteFade();
	UploadTables();
	BuildLutsOnGpu();
	int lutBudget = 4;
	for (size_t i = 0; i < list.size(); ++i) {
		if (i >= drawable.size() || drawable[i] == 0)
			continue;
		const hd_ui::Placed &placed = list[i].placed;
		GpuInstance instance {};
		instance.rect = { static_cast<float>(placed.rect.x), static_cast<float>(placed.rect.y), static_cast<float>(placed.rect.w),
			static_cast<float>(placed.rect.h) };
		instance.clipRect = { static_cast<float>(placed.clip.x), static_cast<float>(placed.clip.y), static_cast<float>(placed.clip.right()),
			static_cast<float>(placed.clip.bottom()) };
		if (placed.isTint()) {
			instance.params = { static_cast<float>(i), placed.opacity, -1.0F, 2.0F };
			instance.tint = { (placed.tint & 0xFF) / 255.0F, ((placed.tint >> 8) & 0xFF) / 255.0F, ((placed.tint >> 16) & 0xFF) / 255.0F,
				((placed.tint >> 24) & 0xFF) / 255.0F };
			Gpu.instances.push_back(instance);
			Gpu.drawSheets.push_back(-1);
			continue;
		}
		const hd_ui::FrameInfo *info = HdUiLayerFrame(placed);
		if (info == nullptr || static_cast<size_t>(info->sheet) >= Gpu.sheets.size() || Gpu.sheets[info->sheet].state != SheetState::Resident)
			continue;
		const hd_ui::SheetInfo &sheet = sheets[info->sheet];
		const auto sw = static_cast<float>(sheet.width);
		const auto sh = static_cast<float>(sheet.height);
		instance.uvRect = { info->x / sw, info->y / sh, (info->x + info->w) / sw, (info->y + info->h) / sh };
		const bool indexed = sheet.indexed;
		float trn = placed.trn >= 0 && placed.trn < LutSlots ? static_cast<float>(placed.trn) : -1.0F;
		if (trn >= 0.0F && !indexed && !EnsureLut(placed.trn, lutBudget))
			trn = -1.0F;
		instance.params = { static_cast<float>(i), indexed ? placed.opacity : placed.opacity * fade, trn, indexed ? 1.0F : 0.0F };
		Gpu.instances.push_back(instance);
		Gpu.drawSheets.push_back(info->sheet);
	}
	if (Gpu.instances.empty())
		return;
	if (!EnsureInstanceBuffer(Gpu.instances.size()) || !EnsurePlaneTexture(layerWidth, layerHeight)) {
		Gpu.instances.clear();
		return;
	}
	wgpuQueueWriteBuffer(Gpu.gpuQueue, Gpu.instanceBuffer, 0, Gpu.instances.data(), Gpu.instances.size() * sizeof(GpuInstance));
	const GpuGlobals globals {
		{ static_cast<float>(frame.sourceWidth), static_cast<float>(frame.sourceHeight), 0.0F, 0.0F },
		{ uiScale, uiScaled ? static_cast<float>(uiTransform.offsetX) : 0.0F, uiScaled ? static_cast<float>(uiTransform.offsetY) : 0.0F,
		    uiScaled ? static_cast<float>(uiTransform.quarters) : 4.0F },
		{ static_cast<float>(layerWidth), static_cast<float>(layerHeight), 0.0F, 0.0F },
	};
	wgpuQueueWriteBuffer(Gpu.gpuQueue, Gpu.globalsBuffer, 0, &globals, sizeof(globals));

	const std::vector<uint16_t> &plane = layers.plane();
	UploadRects(layers, Gpu.uploadRects);
	for (const hd_ui::Rect &rect : Gpu.uploadRects) {
		PackRect(plane.data(), layers.width(), rect, Gpu.uploadStaging);
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = Gpu.planeTexture;
		destination.aspect = WGPUTextureAspect_All;
		destination.origin = { static_cast<uint32_t>(rect.x), static_cast<uint32_t>(rect.y), 0 };
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = static_cast<uint32_t>(rect.w * sizeof(uint16_t));
		layout.rowsPerImage = static_cast<uint32_t>(rect.h);
		const WGPUExtent3D extent = { static_cast<uint32_t>(rect.w), static_cast<uint32_t>(rect.h), 1 };
		wgpuQueueWriteTexture(Gpu.gpuQueue, &destination, Gpu.uploadStaging.data(), Gpu.uploadStaging.size() * sizeof(uint16_t), &layout, &extent);
	}

	std::array<WGPUBindGroupEntry, 6> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	entries[0].binding = 0;
	entries[0].buffer = Gpu.globalsBuffer;
	entries[0].size = sizeof(GpuGlobals);
	entries[1].binding = 1;
	entries[1].buffer = Gpu.instanceBuffer;
	entries[1].size = Gpu.instanceCapacity * sizeof(GpuInstance);
	entries[2].binding = 2;
	entries[2].sampler = Gpu.sampler;
	entries[3].binding = 3;
	entries[3].textureView = Gpu.planeView;
	entries[4].binding = 4;
	entries[4].textureView = Gpu.lutView;
	entries[5].binding = 5;
	entries[5].buffer = Gpu.tableBuffer;
	entries[5].size = sizeof(uint32_t) * 256 * (1 + LutSlots);
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX HD UI frame bind group");
	descriptor.layout = Gpu.frameLayout;
	descriptor.entryCount = entries.size();
	descriptor.entries = entries.data();
	Gpu.frameBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
	if (Gpu.frameBindGroup == nullptr)
		Gpu.instances.clear();
}

void HdUiGpuEncode(WGPURenderPassEncoder pass)
{
	if (Gpu.instances.empty() || Gpu.frameBindGroup == nullptr || !PipelineReady())
		return;
	wgpuRenderPassEncoderSetPipeline(pass, Gpu.pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, Gpu.frameBindGroup, 0, nullptr);
	// One draw per run of consecutive instances on the same sheet (instances still draw in order).
	WGPUBindGroup bound = nullptr;
	const auto groupOf = [](int sheet) { return sheet < 0 ? Gpu.blankBindGroup : Gpu.sheets[sheet].bindGroup; };
	for (size_t i = 0; i < Gpu.instances.size();) {
		WGPUBindGroup group = groupOf(Gpu.drawSheets[i]);
		size_t end = i + 1;
		while (end < Gpu.instances.size() && groupOf(Gpu.drawSheets[end]) == group)
			++end;
		if (group != nullptr) {
			if (group != bound) {
				wgpuRenderPassEncoderSetBindGroup(pass, 1, group, 0, nullptr);
				bound = group;
			}
			wgpuRenderPassEncoderDraw(pass, 6, static_cast<uint32_t>(end - i), 0, static_cast<uint32_t>(i));
		}
		i = end;
	}
}

void HdUiGpuRelease()
{
	++Gpu.generation; // pipelines still being created for the old device are released when they arrive
	ReleaseFrameBindGroup();
	for (SheetSlot &slot : Gpu.sheets) {
		if (slot.state == SheetState::Loading && slot.job != 0)
			hd_loader::CancelImage(slot.job);
		if (slot.upgradeJob != 0)
			hd_loader::CancelImage(slot.upgradeJob);
		AbandonCheck(slot.check);
		AbandonCheck(slot.upgradeCheck);
		ReleaseSheet(slot);
		slot = {};
	}
	// Checks still waiting for their callbacks stay in Gpu.checks (abandoned) and are freed once those ran.
	for (UploadCheck *check : Gpu.checks) {
		check->sheet = -1;
		ReleaseCheckTexture(*check);
	}
	Gpu.queue.clear();
	Gpu.loadsInFlight = 0;
	Gpu.lru.clear();
	Gpu.instances.clear();
	Gpu.drawSheets.clear();
	ReleaseTexture(Gpu.planeTexture, Gpu.planeView);
	Gpu.planeWidth = 0;
	Gpu.planeHeight = 0;
	ReleaseTexture(Gpu.lutTexture, Gpu.lutView);
	ReleaseBindGroup(Gpu.blankBindGroup);
	ReleaseTexture(Gpu.blankTexture, Gpu.blankView);
	ReleaseBuffer(Gpu.instanceBuffer);
	Gpu.instanceCapacity = 0;
	ReleaseBuffer(Gpu.globalsBuffer);
	ReleaseBuffer(Gpu.tableBuffer);
	Gpu.tables.clear();
	Gpu.tablesTrnGeneration = UINT32_MAX;
	ReleaseBindGroup(Gpu.lutBindGroup);
	ReleaseBuffer(Gpu.lutPaletteBuffer);
	ReleaseBuffer(Gpu.nearestBuffer);
	ReleaseBuffer(Gpu.lutStaging);
	ReleaseBuffer(Gpu.lutParamsBuffer);
	if (Gpu.nearestPipeline != nullptr) wgpuComputePipelineRelease(Gpu.nearestPipeline);
	if (Gpu.lutPipeline != nullptr) wgpuComputePipelineRelease(Gpu.lutPipeline);
	Gpu.nearestPipeline = nullptr;
	Gpu.lutPipeline = nullptr;
	Gpu.nearestState = PipelineState::None;
	Gpu.lutState = PipelineState::None;
	Gpu.gpuNearestValid = false;
	if (Gpu.sampler != nullptr) wgpuSamplerRelease(Gpu.sampler);
	Gpu.sampler = nullptr;
	if (Gpu.pipeline != nullptr) wgpuRenderPipelineRelease(Gpu.pipeline);
	Gpu.pipeline = nullptr;
	Gpu.pipelineState = PipelineState::None;
	if (Gpu.pipelineLayout != nullptr) wgpuPipelineLayoutRelease(Gpu.pipelineLayout);
	if (Gpu.lutPipelineLayout != nullptr) wgpuPipelineLayoutRelease(Gpu.lutPipelineLayout);
	Gpu.pipelineLayout = nullptr;
	Gpu.lutPipelineLayout = nullptr;
	for (WGPUBindGroupLayout *layout : { &Gpu.frameLayout, &Gpu.sheetLayout, &Gpu.lutLayout }) {
		if (*layout != nullptr) wgpuBindGroupLayoutRelease(*layout);
		*layout = nullptr;
	}
	Gpu.pipelineFormat = WGPUTextureFormat_Undefined;
	Gpu.device = nullptr;
	Gpu.lutTables = 0;
	Gpu.lutBuilt.clear();
}

} // namespace devilution

#else

#include <string>

#include "engine/render/hd_ui.hpp"

namespace devilution {

bool HdUiGpuWanted() { return false; }
bool HdUiGpuFetchIndex(std::string &, bool &failed)
{
	failed = true;
	return false;
}
bool HdUiGpuSheetReady(int) { return false; }

} // namespace devilution

#endif // DEVILUTIONX_WEBGPU
