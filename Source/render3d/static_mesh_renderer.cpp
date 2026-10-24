/**
 * @file render3d/static_mesh_renderer.cpp
 *
 * Draws the static glTF meshes placed in the level (render3d/placement_runtime.hpp) inside the first-person 3D world
 * pass, lit by the world's lights with its ray-traced shadows (kWorldCommonWgsl's worldShade, like the skinned actors).
 *
 * Group 0 is the world frame (uniforms + occluder grid), group 1 this renderer's per-draw uniforms (dynamic offset),
 * the sampler and the base colour texture of the primitive's material. Base colour = factor * texture (sRGB texture,
 * sampled linear), display-encoded before worldShade like the skinned renderer does. Alpha below 0.5 is cut out
 * (foliage cards); everything is double-sided.
 *
 * Replaces the visual role of Douglas Opan's diablo-3d imported town models (town_model_import.cpp drawn through his
 * town_gpu.cpp D3D11 / WebGL2 backends) with our WebGPU world pass; none of his renderer code is used here.
 */
#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#elif !defined(USE_SDL1)
#include <SDL.h>
#include <SDL_image.h>
#endif

#include "render3d/placement_runtime.hpp"
#include "render3d/world_renderer.hpp"
#include "render3d/world_shaders.hpp"
#include "utils/log.hpp"

namespace devilution::render3d {
namespace {

constexpr char ShaderSource[] = R"(
struct StaticDraw {
  model: mat4x4f,
  normalMatrix: mat4x4f, // inverse transpose of the model matrix (non-uniform scale)
  baseColor: vec4f,
};

@group(1) @binding(0) var<uniform> draw: StaticDraw;
@group(1) @binding(1) var baseSampler: sampler;
@group(1) @binding(2) var baseTexture: texture_2d<f32>;

struct VertexOutput {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) normal: vec3f,
  @location(2) uv: vec2f,
};

@vertex fn vertexMain(@location(0) position: vec3f, @location(1) normal: vec3f, @location(2) uv: vec2f) -> VertexOutput {
  var out: VertexOutput;
  let world = draw.model * vec4f(position, 1.0);
  out.world = world.xyz;
  out.clip = frame.viewProj * world;
  out.normal = (draw.normalMatrix * vec4f(normal, 0.0)).xyz;
  out.uv = uv;
  return out;
}

@fragment fn fragmentMain(input: VertexOutput, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  let texel = textureSample(baseTexture, baseSampler, input.uv);
  let albedo = draw.baseColor * texel;
  if (albedo.a < 0.5) {
    discard;
  }
  var n = normalize(input.normal);
  if (!front) {
    n = -n;
  }
  let display = pow(max(albedo.rgb, vec3f(0.0)), vec3f(1.0 / 2.2));
  return vec4f(worldShade(input.world, n, display, false), 1.0);
}
)";

constexpr uint64_t DrawStride = 256; // minUniformBufferOffsetAlignment
struct GpuDraw {
	std::array<float, 16> model;
	std::array<float, 16> normalMatrix;
	std::array<float, 4> baseColor;
};
static_assert(sizeof(GpuDraw) <= DrawStride);

/** Placements farther than this from the camera (tiles, nearest bounds point) are not drawn: beyond the light radius. */
constexpr float MaxDrawDistance = 48.0F;

struct GpuTexture {
	WGPUTexture texture = nullptr;
	WGPUTextureView view = nullptr;
	WGPUBindGroup bindGroup = nullptr;
};

struct GpuMesh {
	WGPUBuffer vertices = nullptr;
	WGPUBuffer indices = nullptr;
	uint64_t vertexBytes = 0;
	uint64_t indexBytes = 0;
	/** Per material: the bind group of its base colour texture (the white texture when untextured or undecodable). */
	std::vector<WGPUBindGroup> materialGroups;
	std::vector<GpuTexture> images;
	bool failed = false;
};

struct {
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	bool failed = false;
	WGPUShaderModule shader = nullptr;
	WGPUBindGroupLayout frameLayout = nullptr;
	WGPUBindGroupLayout drawLayout = nullptr;
	WGPUPipelineLayout pipelineLayout = nullptr;
	std::vector<std::pair<std::pair<WGPUTextureFormat, WGPUTextureFormat>, WGPURenderPipeline>> pipelines;
	WGPUSampler sampler = nullptr;
	GpuTexture white;
	WGPUBuffer drawBuffer = nullptr;
	uint32_t drawCapacity = 0;
	WGPUBindGroup frameGroup = nullptr;
	WGPUBuffer frameBuffer = nullptr;
	WGPUBuffer occluderBuffer = nullptr;
	std::unordered_map<uint32_t, GpuMesh> meshes;
	uint32_t prunedGeneration = 0;
} G;

WGPUStringView View(const char *text)
{
	return { text, WGPU_STRLEN };
}

void ReleaseBuffer(WGPUBuffer &buffer)
{
	if (buffer != nullptr) {
		wgpuBufferDestroy(buffer);
		wgpuBufferRelease(buffer);
	}
	buffer = nullptr;
}

void ReleaseTexture(GpuTexture &texture)
{
	if (texture.bindGroup != nullptr)
		wgpuBindGroupRelease(texture.bindGroup);
	if (texture.view != nullptr)
		wgpuTextureViewRelease(texture.view);
	if (texture.texture != nullptr) {
		wgpuTextureDestroy(texture.texture);
		wgpuTextureRelease(texture.texture);
	}
	texture = {};
}

void ReleaseMesh(GpuMesh &mesh)
{
	ReleaseBuffer(mesh.vertices);
	ReleaseBuffer(mesh.indices);
	for (GpuTexture &image : mesh.images)
		ReleaseTexture(image);
	mesh = {};
}

void ReleaseEverything()
{
	for (auto &[id, mesh] : G.meshes)
		ReleaseMesh(mesh);
	G.meshes.clear();
	for (auto &[key, pipeline] : G.pipelines) {
		if (pipeline != nullptr)
			wgpuRenderPipelineRelease(pipeline);
	}
	G.pipelines.clear();
	ReleaseTexture(G.white);
	ReleaseBuffer(G.drawBuffer);
	G.drawCapacity = 0;
	if (G.frameGroup != nullptr)
		wgpuBindGroupRelease(G.frameGroup);
	if (G.sampler != nullptr)
		wgpuSamplerRelease(G.sampler);
	if (G.pipelineLayout != nullptr)
		wgpuPipelineLayoutRelease(G.pipelineLayout);
	if (G.drawLayout != nullptr)
		wgpuBindGroupLayoutRelease(G.drawLayout);
	if (G.frameLayout != nullptr)
		wgpuBindGroupLayoutRelease(G.frameLayout);
	if (G.shader != nullptr)
		wgpuShaderModuleRelease(G.shader);
	G.frameGroup = nullptr;
	G.sampler = nullptr;
	G.pipelineLayout = nullptr;
	G.drawLayout = nullptr;
	G.frameLayout = nullptr;
	G.shader = nullptr;
	G.frameBuffer = nullptr;
	G.occluderBuffer = nullptr;
	G.device = nullptr;
	G.queue = nullptr;
	G.failed = false;
}

WGPUBuffer CreateBuffer(const char *label, WGPUBufferUsage usage, uint64_t size, const void *data)
{
	WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
	descriptor.label = View(label);
	descriptor.usage = usage | WGPUBufferUsage_CopyDst;
	descriptor.size = std::max<uint64_t>((size + 3) & ~uint64_t { 3 }, 4);
	WGPUBuffer buffer = wgpuDeviceCreateBuffer(G.device, &descriptor);
	if (buffer != nullptr && data != nullptr && size >= 4)
		wgpuQueueWriteBuffer(G.queue, buffer, 0, data, size & ~uint64_t { 3 });
	return buffer;
}

struct ImageLevel {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
};

/** PNG / JPEG bytes to RGBA8 with box-filtered mips. False when SDL_image cannot decode them. */
bool DecodeImage(const std::vector<uint8_t> &bytes, std::vector<ImageLevel> &chain)
{
#ifdef USE_SDL1
	(void)bytes;
	(void)chain;
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
	SDL_Surface *decoded = io != nullptr ? IMG_Load_RW(io, 1) : nullptr;
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
		std::memcpy(level.pixels.data() + static_cast<size_t>(y) * level.width * 4, static_cast<const uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch,
		    static_cast<size_t>(level.width) * 4);
#ifdef USE_SDL3
	SDL_DestroySurface(rgba);
#else
	SDL_FreeSurface(rgba);
#endif
	chain.clear();
	chain.push_back(std::move(level));
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
					for (int dy = 0; dy < 2; ++dy)
						for (int dx = 0; dx < 2; ++dx)
							sum += source.pixels[(static_cast<size_t>(std::min(y * 2 + dy, source.height - 1)) * source.width + std::min(x * 2 + dx, source.width - 1)) * 4 + c];
					next.pixels[(static_cast<size_t>(y) * next.width + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
				}
			}
		}
		chain.push_back(std::move(next));
	}
	return true;
#endif
}

WGPUBindGroup CreateDrawGroup(WGPUTextureView view)
{
	std::array<WGPUBindGroupEntry, 3> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	entries[0].binding = 0;
	entries[0].buffer = G.drawBuffer;
	entries[0].size = sizeof(GpuDraw);
	entries[1].binding = 1;
	entries[1].sampler = G.sampler;
	entries[2].binding = 2;
	entries[2].textureView = view;
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX static mesh draw");
	descriptor.layout = G.drawLayout;
	descriptor.entryCount = entries.size();
	descriptor.entries = entries.data();
	return wgpuDeviceCreateBindGroup(G.device, &descriptor);
}

bool UploadTexture(const std::vector<ImageLevel> &levels, GpuTexture &out)
{
	WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX static mesh base colour");
	descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	descriptor.dimension = WGPUTextureDimension_2D;
	descriptor.size = { static_cast<uint32_t>(levels[0].width), static_cast<uint32_t>(levels[0].height), 1 };
	descriptor.format = WGPUTextureFormat_RGBA8UnormSrgb;
	descriptor.mipLevelCount = static_cast<uint32_t>(levels.size());
	out.texture = wgpuDeviceCreateTexture(G.device, &descriptor);
	if (out.texture == nullptr)
		return false;
	for (size_t level = 0; level < levels.size(); ++level) {
		const ImageLevel &source = levels[level];
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = out.texture;
		destination.mipLevel = static_cast<uint32_t>(level);
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = static_cast<uint32_t>(source.width * 4);
		layout.rowsPerImage = static_cast<uint32_t>(source.height);
		const WGPUExtent3D extent = { static_cast<uint32_t>(source.width), static_cast<uint32_t>(source.height), 1 };
		wgpuQueueWriteTexture(G.queue, &destination, source.pixels.data(), source.pixels.size(), &layout, &extent);
	}
	out.view = wgpuTextureCreateView(out.texture, nullptr);
	if (out.view == nullptr)
		return false;
	out.bindGroup = CreateDrawGroup(out.view);
	return out.bindGroup != nullptr;
}

bool EnsureDrawBuffer(uint32_t draws)
{
	if (draws <= G.drawCapacity && G.drawBuffer != nullptr)
		return true;
	// Every bind group references the draw buffer: growing it rebuilds them all.
	for (auto &[id, mesh] : G.meshes)
		ReleaseMesh(mesh);
	G.meshes.clear();
	ReleaseTexture(G.white);
	ReleaseBuffer(G.drawBuffer);
	G.drawCapacity = std::max<uint32_t>(256, draws * 2);
	G.drawBuffer = CreateBuffer("DamnationX static mesh draws", WGPUBufferUsage_Uniform, DrawStride * G.drawCapacity, nullptr);
	if (G.drawBuffer == nullptr)
		return false;
	const std::vector<ImageLevel> white { { 1, 1, { 255, 255, 255, 255 } } };
	return UploadTexture(white, G.white);
}

bool EnsureDevice(const SkinnedDrawContext &context)
{
	if (context.device != G.device) {
		ReleaseEverything();
		G.device = context.device;
		G.queue = context.queue;
	}
	if (G.failed)
		return false;
	if (G.shader != nullptr)
		return true;

	const std::string source = std::string(kWorldCommonWgsl) + ShaderSource;
	WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	wgsl.code = { source.c_str(), WGPU_STRLEN };
	WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	moduleDescriptor.nextInChain = &wgsl.chain;
	moduleDescriptor.label = View("DamnationX static mesh shader");
	G.shader = wgpuDeviceCreateShaderModule(G.device, &moduleDescriptor);

	std::array<WGPUBindGroupLayoutEntry, 2> frameEntries {};
	frameEntries.fill(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
	frameEntries[0].binding = 0;
	frameEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	frameEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
	frameEntries[1].binding = 1;
	frameEntries[1].visibility = WGPUShaderStage_Fragment;
	frameEntries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
	WGPUBindGroupLayoutDescriptor frameLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	frameLayoutDescriptor.label = View("DamnationX static mesh frame layout");
	frameLayoutDescriptor.entryCount = frameEntries.size();
	frameLayoutDescriptor.entries = frameEntries.data();
	G.frameLayout = wgpuDeviceCreateBindGroupLayout(G.device, &frameLayoutDescriptor);

	std::array<WGPUBindGroupLayoutEntry, 3> drawEntries {};
	drawEntries.fill(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
	drawEntries[0].binding = 0;
	drawEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
	drawEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
	drawEntries[0].buffer.hasDynamicOffset = true;
	drawEntries[0].buffer.minBindingSize = sizeof(GpuDraw);
	drawEntries[1].binding = 1;
	drawEntries[1].visibility = WGPUShaderStage_Fragment;
	drawEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;
	drawEntries[2].binding = 2;
	drawEntries[2].visibility = WGPUShaderStage_Fragment;
	drawEntries[2].texture.sampleType = WGPUTextureSampleType_Float;
	drawEntries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
	WGPUBindGroupLayoutDescriptor drawLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	drawLayoutDescriptor.label = View("DamnationX static mesh draw layout");
	drawLayoutDescriptor.entryCount = drawEntries.size();
	drawLayoutDescriptor.entries = drawEntries.data();
	G.drawLayout = wgpuDeviceCreateBindGroupLayout(G.device, &drawLayoutDescriptor);

	const std::array<WGPUBindGroupLayout, 2> layouts { G.frameLayout, G.drawLayout };
	WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pipelineLayoutDescriptor.bindGroupLayoutCount = layouts.size();
	pipelineLayoutDescriptor.bindGroupLayouts = layouts.data();
	G.pipelineLayout = wgpuDeviceCreatePipelineLayout(G.device, &pipelineLayoutDescriptor);

	WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
	samplerDescriptor.addressModeU = WGPUAddressMode_Repeat;
	samplerDescriptor.addressModeV = WGPUAddressMode_Repeat;
	samplerDescriptor.magFilter = WGPUFilterMode_Linear;
	samplerDescriptor.minFilter = WGPUFilterMode_Linear;
	samplerDescriptor.mipmapFilter = WGPUMipmapFilterMode_Linear;
	samplerDescriptor.maxAnisotropy = 4;
	G.sampler = wgpuDeviceCreateSampler(G.device, &samplerDescriptor);

	if (G.shader == nullptr || G.frameLayout == nullptr || G.drawLayout == nullptr || G.pipelineLayout == nullptr || G.sampler == nullptr) {
		LogError("3D placements: could not create the static mesh GPU resources; placed meshes are not drawn");
		G.failed = true;
		return false;
	}
	return true;
}

WGPURenderPipeline GetPipeline(WGPUTextureFormat color, WGPUTextureFormat depthFormat)
{
	for (const auto &[key, pipeline] : G.pipelines) {
		if (key.first == color && key.second == depthFormat)
			return pipeline;
	}
	std::array<WGPUVertexAttribute, 3> attributes {};
	attributes.fill(WGPU_VERTEX_ATTRIBUTE_INIT);
	attributes[0] = { nullptr, WGPUVertexFormat_Float32x3, offsetof(StaticVertex, position), 0 };
	attributes[1] = { nullptr, WGPUVertexFormat_Float32x3, offsetof(StaticVertex, normal), 1 };
	attributes[2] = { nullptr, WGPUVertexFormat_Float32x2, offsetof(StaticVertex, uv), 2 };
	WGPUVertexBufferLayout vertexLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vertexLayout.stepMode = WGPUVertexStepMode_Vertex;
	vertexLayout.arrayStride = sizeof(StaticVertex);
	vertexLayout.attributeCount = attributes.size();
	vertexLayout.attributes = attributes.data();

	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format = color;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = G.shader;
	fragment.entryPoint = View("fragmentMain");
	fragment.targetCount = 1;
	fragment.targets = &target;

	WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
	depth.format = depthFormat;
	depth.depthWriteEnabled = WGPUOptionalBool_True;
	depth.depthCompare = WGPUCompareFunction_Less;

	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX static mesh pipeline");
	descriptor.layout = G.pipelineLayout;
	descriptor.vertex.module = G.shader;
	descriptor.vertex.entryPoint = View("vertexMain");
	descriptor.vertex.bufferCount = 1;
	descriptor.vertex.buffers = &vertexLayout;
	descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	descriptor.primitive.cullMode = WGPUCullMode_None;
	descriptor.depthStencil = &depth;
	descriptor.multisample.count = 1;
	descriptor.fragment = &fragment;
	WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(G.device, &descriptor);
	if (pipeline == nullptr) {
		LogError("3D placements: static mesh pipeline creation failed; placed meshes are not drawn");
		G.failed = true;
	}
	G.pipelines.push_back({ { color, depthFormat }, pipeline });
	return pipeline;
}

WGPUBindGroup FrameGroup(const SkinnedDrawContext &context)
{
	if (G.frameGroup != nullptr && G.frameBuffer == context.frameUniforms && G.occluderBuffer == context.occluderGrid)
		return G.frameGroup;
	if (G.frameGroup != nullptr)
		wgpuBindGroupRelease(G.frameGroup);
	std::array<WGPUBindGroupEntry, 2> entries {};
	entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
	entries[0].binding = 0;
	entries[0].buffer = context.frameUniforms;
	entries[0].size = context.frameUniformsSize;
	entries[1].binding = 1;
	entries[1].buffer = context.occluderGrid;
	entries[1].size = context.occluderGridSize;
	WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	descriptor.label = View("DamnationX static mesh frame");
	descriptor.layout = G.frameLayout;
	descriptor.entryCount = entries.size();
	descriptor.entries = entries.data();
	G.frameGroup = wgpuDeviceCreateBindGroup(G.device, &descriptor);
	G.frameBuffer = context.frameUniforms;
	G.occluderBuffer = context.occluderGrid;
	return G.frameGroup;
}

/** The GPU copy of `mesh` (created on first use; `failed` when it cannot be drawn). */
GpuMesh &GetMesh(uint32_t id, const StaticMesh &mesh)
{
	GpuMesh &gpu = G.meshes[id];
	if (gpu.vertices != nullptr || gpu.failed)
		return gpu;
	gpu.vertexBytes = mesh.vertices.size() * sizeof(StaticVertex);
	gpu.indexBytes = mesh.indices.size() * sizeof(uint32_t);
	gpu.vertices = CreateBuffer("DamnationX static mesh vertices", WGPUBufferUsage_Vertex, gpu.vertexBytes, mesh.vertices.data());
	gpu.indices = CreateBuffer("DamnationX static mesh indices", WGPUBufferUsage_Index, gpu.indexBytes, mesh.indices.data());
	if (gpu.vertices == nullptr || gpu.indices == nullptr) {
		gpu.failed = true;
		return gpu;
	}
	gpu.images.resize(mesh.images.size());
	std::vector<bool> decoded(mesh.images.size(), false);
	gpu.materialGroups.assign(mesh.materials.size(), G.white.bindGroup);
	for (size_t m = 0; m < mesh.materials.size(); ++m) {
		const int image = mesh.materials[m].baseColorImage;
		if (image < 0 || static_cast<size_t>(image) >= mesh.images.size())
			continue;
		if (!decoded[static_cast<size_t>(image)]) {
			decoded[static_cast<size_t>(image)] = true;
			std::vector<ImageLevel> levels;
			if (!DecodeImage(mesh.images[static_cast<size_t>(image)].bytes, levels) || !UploadTexture(levels, gpu.images[static_cast<size_t>(image)]))
				LogError("3D placements: could not decode or upload image {} ({}); drawn untextured", image, mesh.images[static_cast<size_t>(image)].mimeType);
		}
		if (gpu.images[static_cast<size_t>(image)].bindGroup != nullptr)
			gpu.materialGroups[m] = gpu.images[static_cast<size_t>(image)].bindGroup;
	}
	return gpu;
}

/** Drops the GPU copies of meshes no placement of the current level uses. */
void PruneMeshes(const LevelPlacements &placements)
{
	if (G.prunedGeneration == placements.generation)
		return;
	G.prunedGeneration = placements.generation;
	for (auto it = G.meshes.begin(); it != G.meshes.end();) {
		const bool used = std::any_of(placements.items.begin(), placements.items.end(), [&](const LoadedPlacement &item) { return item.meshId == it->first; });
		if (used) {
			++it;
		} else {
			ReleaseMesh(it->second);
			it = G.meshes.erase(it);
		}
	}
}

/** Inverse transpose of `model` (translation dropped): keeps normals perpendicular under non-uniform scale. */
std::array<float, 16> NormalMatrix(const skm::Mat4 &model)
{
	skm::Mat4 linear = model;
	linear(0, 3) = 0.0F;
	linear(1, 3) = 0.0F;
	linear(2, 3) = 0.0F;
	skm::Mat4 inverse;
	if (!skm::Inverse(linear, inverse))
		return model.m;
	skm::Mat4 out;
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out(r, c) = inverse(c, r);
	return out.m;
}

float DistanceToBounds(const Vec3 &p, const skm::Vec3 &lo, const skm::Vec3 &hi)
{
	const float dx = std::max({ lo.x - p.x, 0.0F, p.x - hi.x });
	const float dy = std::max({ lo.y - p.y, 0.0F, p.y - hi.y });
	const float dz = std::max({ lo.z - p.z, 0.0F, p.z - hi.z });
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

void StaticMeshEncode(const SkinnedDrawContext &context)
{
	const LevelPlacements &placements = CurrentPlacements();
	if (placements.items.empty() || context.pass == nullptr || context.device == nullptr || !EnsureDevice(context))
		return;
	PruneMeshes(placements);

	// Visible placements; one uniform slot per (placement, primitive): the model matrix and that material's factor.
	std::vector<const LoadedPlacement *> visible;
	uint32_t slots = 0;
	for (const LoadedPlacement &item : placements.items) {
		if (DistanceToBounds(context.cameraPosition, item.worldMin, item.worldMax) <= MaxDrawDistance) {
			visible.push_back(&item);
			slots += static_cast<uint32_t>(item.mesh->primitives.size());
		}
	}
	if (slots == 0 || !EnsureDrawBuffer(slots))
		return;
	WGPURenderPipeline pipeline = GetPipeline(context.colorFormat, context.depthFormat);
	WGPUBindGroup frameGroup = FrameGroup(context);
	if (pipeline == nullptr || frameGroup == nullptr)
		return;

	std::vector<uint8_t> uniforms(static_cast<size_t>(slots) * DrawStride, 0);
	uint32_t slot = 0;
	wgpuRenderPassEncoderSetPipeline(context.pass, pipeline);
	wgpuRenderPassEncoderSetBindGroup(context.pass, 0, frameGroup, 0, nullptr);
	for (const LoadedPlacement *item : visible) {
		GpuMesh &gpu = GetMesh(item->meshId, *item->mesh);
		if (gpu.failed)
			continue;
		const std::array<float, 16> normalMatrix = NormalMatrix(item->placement.model);
		wgpuRenderPassEncoderSetVertexBuffer(context.pass, 0, gpu.vertices, 0, gpu.vertexBytes);
		wgpuRenderPassEncoderSetIndexBuffer(context.pass, gpu.indices, WGPUIndexFormat_Uint32, 0, gpu.indexBytes);
		for (const GltfPrimitive &primitive : item->mesh->primitives) {
			const GltfMaterial &material = item->mesh->materials[static_cast<size_t>(primitive.material)];
			GpuDraw draw {};
			draw.model = item->placement.model.m;
			draw.normalMatrix = normalMatrix;
			std::copy_n(material.baseColor, 4, draw.baseColor.begin());
			std::memcpy(uniforms.data() + static_cast<size_t>(slot) * DrawStride, &draw, sizeof(draw));
			const uint32_t offset = slot * static_cast<uint32_t>(DrawStride);
			++slot;
			wgpuRenderPassEncoderSetBindGroup(context.pass, 1, gpu.materialGroups[static_cast<size_t>(primitive.material)], 1, &offset);
			wgpuRenderPassEncoderDrawIndexed(context.pass, primitive.indexCount, 1, primitive.firstIndex, 0, 0);
		}
	}
	// Written before the presenter submits this pass's command buffer, so the draws above see it.
	if (slot != 0)
		wgpuQueueWriteBuffer(context.queue, G.drawBuffer, 0, uniforms.data(), static_cast<size_t>(slot) * DrawStride);
}

void StaticMeshRelease()
{
	ReleaseEverything();
}

} // namespace devilution::render3d

#endif // DEVILUTIONX_WEBGPU
