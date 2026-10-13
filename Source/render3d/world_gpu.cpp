/**
 * @file render3d/world_gpu.cpp
 *
 * WebGPU side of the first-person 3D world (see world_renderer.hpp): resources, uniforms, the 3D pass and the blit.
 */
#include "render3d/world_renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif
#include <string>
#include <vector>

#include "engine/palette.h"
#include "engine/render/gbuffer.hpp"
#include "engine/surface.hpp"
#include "fpv/fpv.hpp"
#include "fpv/fpv_viewmodel_inputs.hpp"
#include "game_mode.hpp"
#include "levels/gendung.h"
#include "player.h"
#include "utils/display.h"
#include "utils/ui_fwd.h"
#include "utils/log.hpp"

#if DEVILUTIONX_WEBGPU
#include "engine/render/webgpu_presenter.hpp"
#include "render3d/world_level.hpp"
#include "render3d/world_shaders.hpp"
#include "render3d/world_sprites.hpp"
#endif

namespace devilution {

namespace render3d {

bool WorldPassActive()
{
#if DEVILUTIONX_WEBGPU
	// _pLvlChanging: a level change is in progress; the load thread rebuilds the dungeon globals meanwhile.
	return gbRunGame && MyPlayer != nullptr && !MyPlayer->_pLvlChanging && fpv::IsActive() && WebGpuIsReady();
#else
	return false;
#endif
}

static_assert(kUiTransparentIndex == fpv::kTransparentWorldIndex);

namespace {
/** Aux byte of world-row pixels no UI has drawn over this frame (any non-Unlit kind). */
constexpr uint8_t kUiUntouchedAux = gbuffer::PackAux(gbuffer::Kind::Floor, 0);
bool UiCoverageArmed = false;
} // namespace

void BeginUiCoverage(const Surface &out, int rows)
{
	UiCoverageArmed = false;
	gbuffer::Planes &planes = gbuffer::CurrentPlanes;
	if (planes.aux == nullptr || !planes.covers(out) || out.begin() != planes.outBase)
		return;
	const int clamped = std::clamp(rows, 0, static_cast<int>(planes.height));
	std::memset(planes.aux, kUiUntouchedAux, static_cast<size_t>(planes.pitch) * clamped);
	// The hybrid latch is off in first person, so nothing else stamps: arm it for the UI drawn after the world pass
	// (gbuffer::EndFrame resets it after the present).
	gbuffer::CurrentMode = gbuffer::Mode::StampUnlit;
	UiCoverageArmed = true;
}

void ResolveUiCoverage(uint8_t *pixels, int pitch, int width, int rows)
{
	const gbuffer::Planes &planes = gbuffer::CurrentPlanes;
	if (!UiCoverageArmed || pixels == nullptr || planes.aux == nullptr || pixels != planes.outBase || pitch != planes.pitch)
		return;
	// The darkest neutral palette entry other than the transparent index stands in for UI black.
	const SDL_Color *palette = logical_palette.data();
	uint8_t black = 0;
	int best = 1 << 30;
	for (int i = 0; i < 256; ++i) {
		if (i == kUiTransparentIndex)
			continue;
		const SDL_Color &c = palette[i];
		const int chroma = std::max({ c.r, c.g, c.b }) - std::min({ c.r, c.g, c.b });
		const int luma = c.r * 3 + c.g * 6 + c.b + chroma * 40;
		if (luma < best) {
			best = luma;
			black = static_cast<uint8_t>(i);
		}
	}
	const int clampedRows = std::min(rows, static_cast<int>(planes.height));
	const int clampedWidth = std::min(width, static_cast<int>(planes.width));
	for (int y = 0; y < clampedRows; ++y) {
		uint8_t *row = pixels + static_cast<size_t>(y) * pitch;
		const uint8_t *aux = planes.aux + static_cast<size_t>(y) * pitch;
		for (int x = 0; x < clampedWidth; ++x) {
			if (row[x] == kUiTransparentIndex && aux[x] != kUiUntouchedAux)
				row[x] = black;
		}
	}
}

#if DEVILUTIONX_WEBGPU

namespace {

struct alignas(16) GpuWorldLight {
	float positionRadius[4];
	float colorIntensity[4];
};

struct alignas(16) GpuWorldFrame {
	float viewProjection[16];
	float cameraPosition[4];
	float cameraForward[4];
	float params[4]; // ambient, exposure, town, wall height
	float sky[4];
	uint32_t counts[4]; // lights, grid size, flags, frame counter
	GpuWorldLight lights[32];
};
static_assert(sizeof(GpuWorldFrame) == 1168);

WGPUStringView View(const char *text)
{
	return WGPUStringView { text, std::strlen(text) };
}

struct WorldGpu {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUTextureFormat colorFormat = WGPUTextureFormat_Undefined;

	WGPUBindGroupLayout meshGroupLayout = nullptr;
	WGPURenderPipeline meshPipeline = nullptr;
	WGPURenderPipeline glowPipeline = nullptr;
	WGPURenderPipeline blitPipeline = nullptr;
	WGPUBuffer frameBuffer = nullptr;
	WGPUBuffer occluderBuffer = nullptr;
	WGPUBuffer levelVertices = nullptr;
	WGPUBuffer levelIndices = nullptr;
	uint32_t levelIndexCount = 0;
	WGPUBuffer spriteVertices = nullptr;
	uint64_t spriteVertexCapacity = 0;
	WGPUBuffer spriteIndices = nullptr;
	uint64_t spriteIndexCapacity = 0;
	uint32_t spriteIndexCount = 0;
	WGPUBuffer glowVertices = nullptr;
	uint64_t glowVertexCapacity = 0;
	WGPUBuffer glowIndices = nullptr;
	uint64_t glowIndexCapacity = 0;
	uint32_t glowIndexCount = 0;
	WGPUTexture levelAtlas = nullptr;
	WGPUTextureView levelAtlasView = nullptr;
	uint32_t levelAtlasWidth = 0;
	uint32_t levelAtlasHeight = 0;
	uint32_t levelAtlasLevels = 0;
	WGPUTexture spriteAtlas = nullptr;
	WGPUTextureView spriteAtlasView = nullptr;
	WGPUTexture colorTarget = nullptr;
	WGPUTextureView colorTargetView = nullptr;
	WGPUTexture depthTarget = nullptr;
	WGPUTextureView depthTargetView = nullptr;
	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;
	WGPUBindGroup meshBindGroup = nullptr;
	WGPUBindGroup blitBindGroup = nullptr;
	uint32_t uploadedGeneration = 0;
	bool failed = false;

	bool ready = false;     // a pass is encoded this frame
	float viewportRowsFraction = 1.0F;
	Mat4 view;
	Mat4 projection;
	Mat4 viewProjection;
	Vec3 cameraPosition;
	std::vector<WorldLight> lights;
	float ambient = 0.0F;
	uint32_t frameCounter = 0;
	bool hasPlayerLight = false;
	WorldLight playerLight;
	Vec3 cameraForward;
	Vec3 cameraRight;
	Vec3 cameraUp;
	float fovYRadians = 1.0F;
	float aspect = 1.0F;
};

WorldGpu Gpu;
LevelGeometry Level;
BillboardFrame Billboards;
SkinnedDrawFn SkinnedDraw = nullptr;
SkinnedWantsPlayerFn SkinnedWantsPlayer = nullptr;
ViewModelDrawFn ViewModelDraw = nullptr;

template <typename T>
void Release(T &handle, void (*release)(T))
{
	if (handle != nullptr)
		release(handle);
	handle = nullptr;
}

void ReleaseBuffer(WGPUBuffer &buffer)
{
	if (buffer != nullptr) {
		wgpuBufferDestroy(buffer);
		wgpuBufferRelease(buffer);
	}
	buffer = nullptr;
}

void ReleaseTexture(WGPUTexture &texture, WGPUTextureView &view)
{
	Release(view, wgpuTextureViewRelease);
	if (texture != nullptr) {
		wgpuTextureDestroy(texture);
		wgpuTextureRelease(texture);
	}
	texture = nullptr;
}

void ReleaseBindGroups()
{
	Release(Gpu.meshBindGroup, wgpuBindGroupRelease);
	Release(Gpu.blitBindGroup, wgpuBindGroupRelease);
}

void ReleaseAll()
{
	ReleaseBindGroups();
	Release(Gpu.meshPipeline, wgpuRenderPipelineRelease);
	Release(Gpu.glowPipeline, wgpuRenderPipelineRelease);
	Release(Gpu.meshGroupLayout, wgpuBindGroupLayoutRelease);
	Release(Gpu.blitPipeline, wgpuRenderPipelineRelease);
	ReleaseBuffer(Gpu.frameBuffer);
	ReleaseBuffer(Gpu.occluderBuffer);
	ReleaseBuffer(Gpu.levelVertices);
	ReleaseBuffer(Gpu.levelIndices);
	ReleaseBuffer(Gpu.spriteVertices);
	ReleaseBuffer(Gpu.spriteIndices);
	ReleaseBuffer(Gpu.glowVertices);
	ReleaseBuffer(Gpu.glowIndices);
	Gpu.glowVertexCapacity = 0;
	Gpu.glowIndexCapacity = 0;
	Gpu.glowIndexCount = 0;
	ReleaseTexture(Gpu.levelAtlas, Gpu.levelAtlasView);
	ReleaseTexture(Gpu.spriteAtlas, Gpu.spriteAtlasView);
	ReleaseTexture(Gpu.colorTarget, Gpu.colorTargetView);
	ReleaseTexture(Gpu.depthTarget, Gpu.depthTargetView);
	Gpu.levelIndexCount = 0;
	Gpu.spriteVertexCapacity = 0;
	Gpu.spriteIndexCapacity = 0;
	Gpu.levelAtlasWidth = Gpu.levelAtlasHeight = 0;
	Gpu.levelAtlasLevels = 0;
	Gpu.targetWidth = Gpu.targetHeight = 0;
	Gpu.uploadedGeneration = 0;
	Gpu.device = nullptr;
	Gpu.queue = nullptr;
	Gpu.failed = false;
	ResetSpriteCache();
}

WGPUBuffer CreateBuffer(const char *label, WGPUBufferUsage usage, uint64_t size)
{
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage;
	descriptor.size = (size + 3) & ~uint64_t { 3 };
	return wgpuDeviceCreateBuffer(Gpu.device, &descriptor);
}

bool CreateTexture(const char *label, WGPUTextureUsage usage, WGPUTextureFormat format, uint32_t width, uint32_t height, WGPUTexture &texture,
    WGPUTextureView &view, uint32_t mipLevels = 1)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { width, height, 1 };
	descriptor.format = format;
	descriptor.mipLevelCount = mipLevels;
	texture = wgpuDeviceCreateTexture(Gpu.device, &descriptor);
	if (texture == nullptr)
		return false;
	view = wgpuTextureCreateView(texture, nullptr);
	return view != nullptr;
}

WGPUShaderModule CreateShader(const char *label, const std::string &code)
{
	WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
	source.code = WGPUStringView { code.data(), code.size() };
	WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	descriptor.nextInChain = &source.chain;
	descriptor.label = View(label);
	return wgpuDeviceCreateShaderModule(Gpu.device, &descriptor);
}

bool CreatePipelines()
{
	const std::string meshSource = std::string(kWorldCommonWgsl) + kWorldMeshWgsl;
	WGPUShaderModule meshShader = CreateShader("DamnationX 3D world shader", meshSource);
	if (meshShader == nullptr)
		return false;
	std::array<WGPUVertexAttribute, 4> attributes {};
	const WGPUVertexFormat formats[4] = { WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x2, WGPUVertexFormat_Float32x3, WGPUVertexFormat_Uint32 };
	const uint64_t offsets[4] = { 0, 12, 20, 32 };
	for (uint32_t i = 0; i < 4; ++i) {
		attributes[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
		attributes[i].format = formats[i];
		attributes[i].offset = offsets[i];
		attributes[i].shaderLocation = i;
	}
	WGPUVertexBufferLayout layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	layout.stepMode = WGPUVertexStepMode_Vertex;
	layout.arrayStride = sizeof(WorldVertex);
	layout.attributeCount = attributes.size();
	layout.attributes = attributes.data();

	WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
	colorTarget.format = Gpu.colorFormat;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = meshShader;
	fragment.entryPoint = View("worldFragment");
	fragment.targetCount = 1;
	fragment.targets = &colorTarget;
	WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
	depth.format = kWorldDepthFormat;
	depth.depthWriteEnabled = WGPUOptionalBool_True;
	depth.depthCompare = WGPUCompareFunction_Less;
	// Explicit layout shared by the mesh and glow pipelines (and their one bind group).
	std::array<WGPUBindGroupLayoutEntry, 4> layoutEntries {};
	for (WGPUBindGroupLayoutEntry &entry : layoutEntries)
		entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	layoutEntries[0].binding = 0;
	layoutEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	layoutEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
	layoutEntries[1].binding = 1;
	layoutEntries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	layoutEntries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
	for (uint32_t i = 2; i < 4; ++i) {
		layoutEntries[i].binding = i;
		layoutEntries[i].visibility = WGPUShaderStage_Fragment;
		layoutEntries[i].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
		layoutEntries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
	}
	WGPUBindGroupLayoutDescriptor groupLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	groupLayoutDescriptor.label = View("DamnationX 3D world bind group layout");
	groupLayoutDescriptor.entryCount = layoutEntries.size();
	groupLayoutDescriptor.entries = layoutEntries.data();
	Gpu.meshGroupLayout = wgpuDeviceCreateBindGroupLayout(Gpu.device, &groupLayoutDescriptor);
	WGPUPipelineLayoutDescriptor layoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	layoutDescriptor.bindGroupLayoutCount = 1;
	layoutDescriptor.bindGroupLayouts = &Gpu.meshGroupLayout;
	WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(Gpu.device, &layoutDescriptor);

	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX 3D world pipeline");
	descriptor.layout = pipelineLayout;
	descriptor.vertex.module = meshShader;
	descriptor.vertex.entryPoint = View("worldVertex");
	descriptor.vertex.bufferCount = 1;
	descriptor.vertex.buffers = &layout;
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	descriptor.primitive.cullMode = WGPUCullMode_None;
	descriptor.depthStencil = &depth;
	descriptor.fragment = &fragment;
	Gpu.meshPipeline = wgpuDeviceCreateRenderPipeline(Gpu.device, &descriptor);
	if (Gpu.meshPipeline == nullptr) {
		wgpuPipelineLayoutRelease(pipelineLayout);
		wgpuShaderModuleRelease(meshShader);
		return false;
	}

	// Additive glow (missiles): same vertex layout and bind group layout, no depth write.
	{
		WGPUBlendState additive = WGPU_BLEND_STATE_INIT;
		additive.color = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One };
		additive.alpha = { WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One };
		WGPUColorTargetState glowTarget = colorTarget;
		glowTarget.blend = &additive;
		WGPUFragmentState glowFragment = fragment;
		glowFragment.entryPoint = View("glowFragment");
		glowFragment.targets = &glowTarget;
		WGPUDepthStencilState glowDepth = depth;
		glowDepth.depthWriteEnabled = WGPUOptionalBool_False;
		WGPURenderPipelineDescriptor glowDescriptor = descriptor;
		glowDescriptor.label = View("DamnationX 3D glow pipeline");
		glowDescriptor.layout = pipelineLayout;
		glowDescriptor.depthStencil = &glowDepth;
		glowDescriptor.fragment = &glowFragment;
		Gpu.glowPipeline = wgpuDeviceCreateRenderPipeline(Gpu.device, &glowDescriptor);
		wgpuPipelineLayoutRelease(pipelineLayout);
	}
	wgpuShaderModuleRelease(meshShader);
	if (Gpu.glowPipeline == nullptr)
		return false;

	WGPUShaderModule blitShader = CreateShader("DamnationX 3D blit shader", kWorldBlitWgsl);
	if (blitShader == nullptr)
		return false;
	WGPUColorTargetState blitTarget = WGPU_COLOR_TARGET_STATE_INIT;
	blitTarget.format = Gpu.colorFormat;
	WGPUFragmentState blitFragment = WGPU_FRAGMENT_STATE_INIT;
	blitFragment.module = blitShader;
	blitFragment.entryPoint = View("blitFragment");
	blitFragment.targetCount = 1;
	blitFragment.targets = &blitTarget;
	WGPURenderPipelineDescriptor blitDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	blitDescriptor.label = View("DamnationX 3D blit pipeline");
	blitDescriptor.vertex.module = blitShader;
	blitDescriptor.vertex.entryPoint = View("blitVertex");
	blitDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	blitDescriptor.fragment = &blitFragment;
	Gpu.blitPipeline = wgpuDeviceCreateRenderPipeline(Gpu.device, &blitDescriptor);
	wgpuShaderModuleRelease(blitShader);
	return Gpu.blitPipeline != nullptr;
}

bool EnsureStaticResources(const WorldGpuFrame &frame)
{
	if (Gpu.device != frame.device || Gpu.colorFormat != frame.colorFormat) {
		ReleaseAll();
		Gpu.device = frame.device;
		Gpu.queue = frame.queue;
		Gpu.colorFormat = frame.colorFormat;
	}
	if (Gpu.failed)
		return false;
	if (Gpu.meshPipeline != nullptr)
		return true;
	if (!CreatePipelines()) {
		LogError("render3d: pipeline creation failed");
		Gpu.failed = true;
		return false;
	}
	Gpu.frameBuffer = CreateBuffer("DamnationX 3D frame uniforms", WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, sizeof(GpuWorldFrame));
	Gpu.occluderBuffer = CreateBuffer("DamnationX 3D occluders", WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, sizeof(Level.occluders));
	if (!CreateTexture("DamnationX 3D sprite atlas", WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, WGPUTextureFormat_RGBA8Unorm,
	        kSpriteAtlasSize, kSpriteAtlasSize, Gpu.spriteAtlas, Gpu.spriteAtlasView)
	    || Gpu.frameBuffer == nullptr || Gpu.occluderBuffer == nullptr) {
		Gpu.failed = true;
		return false;
	}
	ResetSpriteCache();
	return true;
}

void WriteTexture(WGPUTexture texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t mipLevel = 0)
{
	WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture = texture;
	destination.mipLevel = mipLevel;
	destination.origin = { x, y, 0 };
	destination.aspect = WGPUTextureAspect_All;
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = width * 4;
	layout.rowsPerImage = height;
	const WGPUExtent3D extent { width, height, 1 };
	wgpuQueueWriteTexture(Gpu.queue, &destination, rgba, static_cast<size_t>(width) * height * 4, &layout, &extent);
}

bool UploadLevel()
{
	if (Gpu.uploadedGeneration == Level.generation)
		return Gpu.levelVertices != nullptr;
	ReleaseBuffer(Gpu.levelVertices);
	ReleaseBuffer(Gpu.levelIndices);
	Gpu.levelIndexCount = 0;
	if (Level.vertices.empty() || Level.indices.empty())
		return false;
	Gpu.levelVertices = CreateBuffer("DamnationX 3D level vertices", WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, Level.vertices.size() * sizeof(WorldVertex));
	Gpu.levelIndices = CreateBuffer("DamnationX 3D level indices", WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst, Level.indices.size() * sizeof(uint32_t));
	if (Gpu.levelVertices == nullptr || Gpu.levelIndices == nullptr)
		return false;
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.levelVertices, 0, Level.vertices.data(), Level.vertices.size() * sizeof(WorldVertex));
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.levelIndices, 0, Level.indices.data(), Level.indices.size() * sizeof(uint32_t));
	Gpu.levelIndexCount = static_cast<uint32_t>(Level.indices.size());
	const auto levels = static_cast<uint32_t>(1 + Level.atlasMips.size());
	if (Gpu.levelAtlasWidth != Level.atlasWidth || Gpu.levelAtlasHeight != Level.atlasHeight || Gpu.levelAtlasLevels != levels) {
		ReleaseBindGroups();
		ReleaseTexture(Gpu.levelAtlas, Gpu.levelAtlasView);
		if (!CreateTexture("DamnationX 3D level atlas", WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, WGPUTextureFormat_RGBA8Unorm,
		        Level.atlasWidth, Level.atlasHeight, Gpu.levelAtlas, Gpu.levelAtlasView, levels))
			return false;
		Gpu.levelAtlasWidth = Level.atlasWidth;
		Gpu.levelAtlasHeight = Level.atlasHeight;
		Gpu.levelAtlasLevels = levels;
	}
	// Every mip level, in bands (Dawn stages writes over 4 MiB in their own buffers).
	for (uint32_t level = 0; level < levels; ++level) {
		const uint32_t width = std::max<uint32_t>(1, Level.atlasWidth >> level);
		const uint32_t height = std::max<uint32_t>(1, Level.atlasHeight >> level);
		const std::vector<uint8_t> &pixels = level == 0 ? Level.atlasRgba : Level.atlasMips[level - 1];
		const uint32_t bandRows = std::max<uint32_t>(1, (4U << 20) / (width * 4));
		for (uint32_t y = 0; y < height; y += bandRows) {
			const uint32_t rows = std::min(bandRows, height - y);
			WriteTexture(Gpu.levelAtlas, 0, y, width, rows, &pixels[static_cast<size_t>(y) * width * 4], level);
		}
	}
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.occluderBuffer, 0, Level.occluders.data(), sizeof(Level.occluders));
	Gpu.uploadedGeneration = Level.generation;
	return true;
}

bool EnsureTargets(uint32_t width, uint32_t height)
{
	if (Gpu.colorTargetView != nullptr && Gpu.targetWidth == width && Gpu.targetHeight == height)
		return true;
	Release(Gpu.blitBindGroup, wgpuBindGroupRelease);
	ReleaseTexture(Gpu.colorTarget, Gpu.colorTargetView);
	ReleaseTexture(Gpu.depthTarget, Gpu.depthTargetView);
	if (!CreateTexture("DamnationX 3D colour", WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, Gpu.colorFormat, width, height,
	        Gpu.colorTarget, Gpu.colorTargetView)
	    || !CreateTexture("DamnationX 3D depth", WGPUTextureUsage_RenderAttachment, kWorldDepthFormat, width, height, Gpu.depthTarget, Gpu.depthTargetView))
		return false;
	Gpu.targetWidth = width;
	Gpu.targetHeight = height;
	return true;
}

bool EnsureBindGroups()
{
	if (Gpu.meshBindGroup == nullptr) {
		std::array<WGPUBindGroupEntry, 4> entries {};
		for (WGPUBindGroupEntry &entry : entries)
			entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entries[0].binding = 0;
		entries[0].buffer = Gpu.frameBuffer;
		entries[0].size = sizeof(GpuWorldFrame);
		entries[1].binding = 1;
		entries[1].buffer = Gpu.occluderBuffer;
		entries[1].size = sizeof(Level.occluders);
		entries[2].binding = 2;
		entries[2].textureView = Gpu.levelAtlasView;
		entries[3].binding = 3;
		entries[3].textureView = Gpu.spriteAtlasView;
		WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		descriptor.label = View("DamnationX 3D world bind group");
		descriptor.layout = Gpu.meshGroupLayout;
		descriptor.entryCount = entries.size();
		descriptor.entries = entries.data();
		Gpu.meshBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
	}
	if (Gpu.blitBindGroup == nullptr) {
		WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entry.binding = 0;
		entry.textureView = Gpu.colorTargetView;
		WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(Gpu.blitPipeline, 0);
		WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		descriptor.label = View("DamnationX 3D blit bind group");
		descriptor.layout = layout;
		descriptor.entryCount = 1;
		descriptor.entries = &entry;
		Gpu.blitBindGroup = wgpuDeviceCreateBindGroup(Gpu.device, &descriptor);
		wgpuBindGroupLayoutRelease(layout);
	}
	return Gpu.meshBindGroup != nullptr && Gpu.blitBindGroup != nullptr;
}

void UploadBillboards()
{
	for (const SpriteUpload &upload : Billboards.uploads)
		WriteTexture(Gpu.spriteAtlas, static_cast<uint32_t>(upload.x), static_cast<uint32_t>(upload.y), static_cast<uint32_t>(upload.width),
		    static_cast<uint32_t>(upload.height), upload.rgba.data());
	Gpu.spriteIndexCount = 0;
	if (Billboards.indices.empty())
		return;
	const uint64_t vertexBytes = Billboards.vertices.size() * sizeof(WorldVertex);
	const uint64_t indexBytes = Billboards.indices.size() * sizeof(uint32_t);
	if (vertexBytes > Gpu.spriteVertexCapacity) {
		ReleaseBuffer(Gpu.spriteVertices);
		Gpu.spriteVertexCapacity = std::max<uint64_t>(vertexBytes * 2, 64 * 1024);
		Gpu.spriteVertices = CreateBuffer("DamnationX 3D billboard vertices", WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, Gpu.spriteVertexCapacity);
	}
	if (indexBytes > Gpu.spriteIndexCapacity) {
		ReleaseBuffer(Gpu.spriteIndices);
		Gpu.spriteIndexCapacity = std::max<uint64_t>(indexBytes * 2, 32 * 1024);
		Gpu.spriteIndices = CreateBuffer("DamnationX 3D billboard indices", WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst, Gpu.spriteIndexCapacity);
	}
	if (Gpu.spriteVertices == nullptr || Gpu.spriteIndices == nullptr)
		return;
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.spriteVertices, 0, Billboards.vertices.data(), vertexBytes);
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.spriteIndices, 0, Billboards.indices.data(), indexBytes);
	Gpu.spriteIndexCount = static_cast<uint32_t>(Billboards.indices.size());
}

void UploadGlows()
{
	Gpu.glowIndexCount = 0;
	if (Billboards.glowIndices.empty())
		return;
	const uint64_t vertexBytes = Billboards.glowVertices.size() * sizeof(WorldVertex);
	const uint64_t indexBytes = Billboards.glowIndices.size() * sizeof(uint32_t);
	if (vertexBytes > Gpu.glowVertexCapacity) {
		ReleaseBuffer(Gpu.glowVertices);
		Gpu.glowVertexCapacity = std::max<uint64_t>(vertexBytes * 2, 16 * 1024);
		Gpu.glowVertices = CreateBuffer("DamnationX 3D glow vertices", WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, Gpu.glowVertexCapacity);
	}
	if (indexBytes > Gpu.glowIndexCapacity) {
		ReleaseBuffer(Gpu.glowIndices);
		Gpu.glowIndexCapacity = std::max<uint64_t>(indexBytes * 2, 8 * 1024);
		Gpu.glowIndices = CreateBuffer("DamnationX 3D glow indices", WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst, Gpu.glowIndexCapacity);
	}
	if (Gpu.glowVertices == nullptr || Gpu.glowIndices == nullptr)
		return;
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.glowVertices, 0, Billboards.glowVertices.data(), vertexBytes);
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.glowIndices, 0, Billboards.glowIndices.data(), indexBytes);
	Gpu.glowIndexCount = static_cast<uint32_t>(Billboards.glowIndices.size());
}

void FillContext(SkinnedDrawContext &context, WGPURenderPassEncoder pass)
{
	context.device = Gpu.device;
	context.queue = Gpu.queue;
	context.pass = pass;
	context.colorFormat = Gpu.colorFormat;
	context.depthFormat = kWorldDepthFormat;
	context.view = Gpu.view;
	context.projection = Gpu.projection;
	context.viewProjection = Gpu.viewProjection;
	context.cameraPosition = Gpu.cameraPosition;
	context.lights = Gpu.lights.data();
	context.lightCount = static_cast<uint32_t>(Gpu.lights.size());
	context.ambient = Gpu.ambient;
	context.frameUniforms = Gpu.frameBuffer;
	context.frameUniformsSize = sizeof(GpuWorldFrame);
	context.occluderGrid = Gpu.occluderBuffer;
	context.occluderGridSize = sizeof(Level.occluders);
}

bool EnvFloat(const char *name, float &out)
{
	const char *value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
		return false;
	out = std::strtof(value, nullptr);
	return true;
}

/** Developer test knobs (see world_renderer.hpp). */
fpv::CameraState ApplyTestOverrides(fpv::CameraState camera)
{
	static const auto Start = std::chrono::steady_clock::now();
	const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - Start).count();
	constexpr float DegToRad = 3.14159265F / 180.0F;
	float value = 0.0F;
	if (EnvFloat("DVL_W3D_YAW", value))
		camera.yaw = value * DegToRad;
	if (EnvFloat("DVL_W3D_SPIN", value))
		camera.yaw += value * seconds * DegToRad;
	if (EnvFloat("DVL_W3D_PITCH", value))
		camera.pitch = value * DegToRad;
	if (const char *pos = std::getenv("DVL_W3D_POS"); pos != nullptr && pos[0] != '\0') {
		char *end = nullptr;
		camera.x = std::strtof(pos, &end);
		if (end != nullptr && *end == ',')
			camera.z = std::strtof(end + 1, nullptr);
	}
	static bool DemoStopSent = false;
	if (!DemoStopSent && EnvFloat("DVL_W3D_STOP_DEMO", value) && seconds > value) {
		DemoStopSent = true;
		SDL_Event event {};
#ifdef USE_SDL3
		event.type = SDL_EVENT_KEY_DOWN;
		event.key.key = SDLK_ESCAPE;
		event.key.down = true;
#else
		event.type = SDL_KEYDOWN;
		event.key.keysym.sym = SDLK_ESCAPE;
		event.key.state = SDL_PRESSED;
#endif
		SDL_PushEvent(&event);
	}
	return camera;
}

bool SkipPlayerForSkinned(int playerId)
{
	return SkinnedWantsPlayer != nullptr && SkinnedWantsPlayer(playerId);
}

} // namespace

bool WorldGpuPrepare(const WorldGpuFrame &frame)
{
	Gpu.ready = false;
	if (frame.device == nullptr || frame.sourceWidth <= 0 || frame.viewportRows <= 0)
		return false;
	if (!EnsureStaticResources(frame))
		return false;

	RefreshArtPalette();
	UpdateLevelGeometry(Level);
	if (!UploadLevel())
		return false;
	// The 3D image is rendered at the back buffer's resolution (pixelated like the original art), times the optional
	// developer scale DVL_W3D_SCALE (e.g. 2.5 to measure a 1080p-sized pass in a 768 x 480 demo window).
	float renderScale = 1.0F;
	if (EnvFloat("DVL_W3D_SCALE", renderScale))
		renderScale = std::clamp(renderScale, 0.25F, 4.0F);
	const auto rows = std::min(frame.viewportRows, frame.sourceHeight);
	const auto width = static_cast<uint32_t>(std::max(1.0F, static_cast<float>(frame.sourceWidth) * renderScale));
	const auto height = static_cast<uint32_t>(std::max(1.0F, static_cast<float>(rows) * renderScale));
	if (!EnsureTargets(width, height) || !EnsureBindGroups())
		return false;
	Gpu.viewportRowsFraction = static_cast<float>(rows) / static_cast<float>(std::max(1, frame.sourceHeight));

	// Camera.
	const fpv::CameraState camera = ApplyTestOverrides(fpv::GetCamera());
	Gpu.cameraPosition = { camera.x, camera.y, camera.z };
	const Vec3 forward = YawPitchForward(camera.yaw, camera.pitch);
	Gpu.view = LookAt(Gpu.cameraPosition, Gpu.cameraPosition + forward, { 0.0F, 1.0F, 0.0F });
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	Gpu.projection = Perspective(camera.fovYDeg * 3.14159265F / 180.0F, aspect, 0.03F, 160.0F);
	Gpu.viewProjection = Gpu.projection * Gpu.view;
	Gpu.cameraForward = forward;
	Gpu.cameraRight = Normalize(Cross(forward, { 0.0F, 1.0F, 0.0F }));
	Gpu.cameraUp = Cross(Gpu.cameraRight, forward);
	Gpu.fovYRadians = camera.fovYDeg * 3.14159265F / 180.0F;
	Gpu.aspect = aspect;

	// Lights (tile space -> world: y is the height).
	Gpu.lights.clear();
	for (uint32_t i = 0; i < frame.lightCount && i < 32 && frame.lights != nullptr; ++i) {
		const WebGpuHybridLight &light = frame.lights[i];
		WorldLight world;
		world.position = { light.worldX, std::max(0.05F, light.worldZ * Level.wallHeight), light.worldY };
		world.radius = light.radius;
		world.color = { light.red, light.green, light.blue };
		world.intensity = light.intensity;
		Gpu.lights.push_back(world);
	}
	// dx.cpp sorts the player's own light first (priority 0); it is the one at the camera.
	Gpu.hasPlayerLight = false;
	if (!Gpu.lights.empty()) {
		const WorldLight &first = Gpu.lights.front();
		const float dx = first.position.x - camera.x;
		const float dz = first.position.z - camera.z;
		if (dx * dx + dz * dz < 1.5F * 1.5F) {
			Gpu.hasPlayerLight = true;
			Gpu.playerLight = first;
		}
	}

	GpuWorldFrame uniforms {};
	std::memcpy(uniforms.viewProjection, Gpu.viewProjection.m.data(), sizeof(uniforms.viewProjection));
	uniforms.cameraPosition[0] = camera.x;
	uniforms.cameraPosition[1] = camera.y;
	uniforms.cameraPosition[2] = camera.z;
	uniforms.cameraPosition[3] = static_cast<float>(Gpu.frameCounter) / 60.0F;
	uniforms.cameraForward[0] = forward.x;
	uniforms.cameraForward[1] = forward.y;
	uniforms.cameraForward[2] = forward.z;
	uniforms.cameraForward[3] = fpv::kEyeHeight;
	// Town: fully lit like the classic (night ambient plus moonlight, lifted in worldShade); dungeons: a faint ambient so
	// the edge of the light radius fades to near black like the original light tables.
	Gpu.ambient = Level.isTown ? 0.85F : 0.03F;
	uniforms.params[0] = Gpu.ambient;
	uniforms.params[1] = Level.isTown ? 1.15F : 1.35F;
	uniforms.params[2] = Level.isTown ? 1.0F : 0.0F;
	uniforms.params[3] = Level.wallHeight;
	const Vec3 moon = Normalize({ -0.45F, 0.8F, -0.35F });
	uniforms.sky[0] = moon.x;
	uniforms.sky[1] = moon.y;
	uniforms.sky[2] = moon.z;
	uniforms.sky[3] = Level.isTown ? 0.6F : 0.0F;
	uniforms.counts[0] = static_cast<uint32_t>(Gpu.lights.size());
	uniforms.counts[1] = kGridSize;
	uniforms.counts[2] = 1U | (std::getenv("DVL_W3D_NO_SHADOWS") != nullptr ? 2U : 0U); // soft shadows
	uniforms.counts[3] = Gpu.frameCounter++;
	for (size_t i = 0; i < Gpu.lights.size(); ++i) {
		const WorldLight &light = Gpu.lights[i];
		uniforms.lights[i] = {
			{ light.position.x, light.position.y, light.position.z, light.radius },
			{ light.color.x, light.color.y, light.color.z, light.intensity },
		};
	}
	wgpuQueueWriteBuffer(Gpu.queue, Gpu.frameBuffer, 0, &uniforms, sizeof(uniforms));

	BuildBillboards(CameraView { Gpu.cameraPosition, camera.yaw }, Level, SkipPlayerForSkinned, Billboards);
	UploadBillboards();
	UploadGlows();
	Gpu.ready = true;
	return true;
}

void WorldGpuEncode(WGPUCommandEncoder encoder, WGPUTextureView target, float x, float y, float width, float height)
{
	if (!Gpu.ready)
		return;
	// 1. The 3D world into the offscreen colour + depth target.
	WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	color.view = Gpu.colorTargetView;
	color.loadOp = WGPULoadOp_Clear;
	color.storeOp = WGPUStoreOp_Store;
	color.clearValue = { 0.0, 0.0, 0.0, 1.0 };
	WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	depth.view = Gpu.depthTargetView;
	depth.depthLoadOp = WGPULoadOp_Clear;
	depth.depthStoreOp = WGPUStoreOp_Store;
	depth.depthClearValue = 1.0F;
	WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	passDescriptor.label = View("DamnationX 3D world pass");
	passDescriptor.colorAttachmentCount = 1;
	passDescriptor.colorAttachments = &color;
	passDescriptor.depthStencilAttachment = &depth;
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
	wgpuRenderPassEncoderSetPipeline(pass, Gpu.meshPipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, Gpu.meshBindGroup, 0, nullptr);
	if (Gpu.levelIndexCount != 0) {
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, Gpu.levelVertices, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetIndexBuffer(pass, Gpu.levelIndices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderDrawIndexed(pass, Gpu.levelIndexCount, 1, 0, 0, 0);
	}
	if (Gpu.spriteIndexCount != 0) {
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, Gpu.spriteVertices, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetIndexBuffer(pass, Gpu.spriteIndices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderDrawIndexed(pass, Gpu.spriteIndexCount, 1, 0, 0, 0);
	}
	if (SkinnedDraw != nullptr) {
		SkinnedDrawContext context;
		FillContext(context, pass);
		SkinnedDraw(context);
	}
	if (Gpu.glowIndexCount != 0) {
		wgpuRenderPassEncoderSetPipeline(pass, Gpu.glowPipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, Gpu.meshBindGroup, 0, nullptr);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, Gpu.glowVertices, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetIndexBuffer(pass, Gpu.glowIndices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderDrawIndexed(pass, Gpu.glowIndexCount, 1, 0, 0, 0);
	}
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	// 1b. The weapon viewmodel over the world: same colour target, depth cleared.
	if (ViewModelDraw != nullptr) {
		WGPURenderPassColorAttachment keep = color;
		keep.loadOp = WGPULoadOp_Load;
		WGPURenderPassDepthStencilAttachment clearedDepth = depth;
		WGPURenderPassDescriptor viewModelDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		viewModelDescriptor.label = View("DamnationX 3D viewmodel pass");
		viewModelDescriptor.colorAttachmentCount = 1;
		viewModelDescriptor.colorAttachments = &keep;
		viewModelDescriptor.depthStencilAttachment = &clearedDepth;
		WGPURenderPassEncoder viewModelPass = wgpuCommandEncoderBeginRenderPass(encoder, &viewModelDescriptor);
		ViewModelDrawContext context;
		FillContext(context, viewModelPass);
		context.hasPlayerLight = Gpu.hasPlayerLight;
		context.playerLight = Gpu.playerLight;
		context.cameraForward = Gpu.cameraForward;
		context.cameraRight = Gpu.cameraRight;
		context.cameraUp = Gpu.cameraUp;
		context.fovYRadians = Gpu.fovYRadians;
		context.aspect = Gpu.aspect;
		ViewModelDraw(context);
		wgpuRenderPassEncoderEnd(viewModelPass);
		wgpuRenderPassEncoderRelease(viewModelPass);
	}

	// 2. Nearest blit into the swapchain (the world rows of the letterboxed back buffer rectangle).
	WGPURenderPassColorAttachment swap = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	swap.view = target;
	swap.loadOp = WGPULoadOp_Clear;
	swap.storeOp = WGPUStoreOp_Store;
	swap.clearValue = { 0.0, 0.0, 0.0, 1.0 };
	WGPURenderPassDescriptor blitDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	blitDescriptor.label = View("DamnationX 3D blit pass");
	blitDescriptor.colorAttachmentCount = 1;
	blitDescriptor.colorAttachments = &swap;
	WGPURenderPassEncoder blit = wgpuCommandEncoderBeginRenderPass(encoder, &blitDescriptor);
	wgpuRenderPassEncoderSetPipeline(blit, Gpu.blitPipeline);
	wgpuRenderPassEncoderSetBindGroup(blit, 0, Gpu.blitBindGroup, 0, nullptr);
	wgpuRenderPassEncoderSetViewport(blit, x, y, width, std::max(1.0F, height * Gpu.viewportRowsFraction), 0.0F, 1.0F);
	wgpuRenderPassEncoderDraw(blit, 3, 1, 0, 0);
	wgpuRenderPassEncoderEnd(blit);
	wgpuRenderPassEncoderRelease(blit);
}

float FloorHeightAt(float x, float z)
{
	if (Level.generation == 0)
		return 0.0F;
	return FloorHeightOf(Level, x, z);
}

void WorldGpuRelease()
{
	ReleaseAll();
	Level = LevelGeometry {};
	InvalidateLevel();
}

void SetViewModelRenderer(ViewModelDrawFn draw)
{
	ViewModelDraw = draw;
	// The fpv HUD drops its 2D slash overlay while a 3D viewmodel is drawn.
	fpv::SetViewmodelAvailableQuery(draw != nullptr ? +[]() { return ViewModelDraw != nullptr && Gpu.ready; } : nullptr);
}

void SetSkinnedActorRenderer(SkinnedDrawFn draw, SkinnedWantsPlayerFn wantsPlayer)
{
	SkinnedDraw = draw;
	SkinnedWantsPlayer = wantsPlayer;
}

#else
float FloorHeightAt(float, float)
{
	return 0.0F;
}
#endif // DEVILUTIONX_WEBGPU

} // namespace render3d
} // namespace devilution
