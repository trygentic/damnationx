#pragma once
/**
 * @file render3d/gltf_model.hpp
 *
 * CPU side of the skinned 3D warrior (assets/3d/warrior/warrior.glb, see warrior_manifest.json): a binary glTF 2.0
 * file parsed with cgltf into one interleaved vertex / index buffer, the node hierarchy, the skin and the animation
 * clips, plus a pose sampler that turns (clip, time) into the joint palette the GPU skins with.
 *
 * Joint palette: entries [0, joints.size()) are skin joints (global joint matrix * inverse bind matrix); after them
 * one entry per rigid mesh node (equipment parented to a bone, e.g. Sword_RIGHT under mixamorig:RightHand), holding
 * the node's global matrix. Rigid vertices are stored in their node's local space with weight 1 on that entry, so
 * every primitive goes through the same skinning shader.
 *
 * Parts: every mesh node is one part (bit in a visibility mask); `partNames[i]` is the node name. The body is part 0.
 */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render3d/skinned_math.hpp"

namespace devilution::render3d {

/** Interleaved GPU vertex (56 bytes); `packed` = material index | part index << 8. */
struct SkinnedVertex {
	float position[3];
	float normal[3];
	float uv[2];
	uint8_t joints[4];
	float weights[4];
	uint32_t packed;
};
static_assert(sizeof(SkinnedVertex) == 56);

struct GltfMaterial {
	std::string name;
	float baseColor[4] { 1.0F, 1.0F, 1.0F, 1.0F }; // linear
	float metallic = 0.0F;
	float roughness = 1.0F;
	/** KHR_materials_specular specularFactor (1 = the glTF default dielectric F0 0.04). */
	float specular = 1.0F;
	bool textured = false; // has a base colour texture (baseColorImage)
	/** Indices into GltfModel::images, -1 when the material has no such texture. */
	int baseColorImage = -1;
	int normalImage = -1;            // tangent space, +Y up (glTF convention)
	int metallicRoughnessImage = -1; // G = roughness, B = metallic (times the factors)
	float normalScale = 1.0F;
};

/** An image used by a material, as stored in the file (PNG / JPEG bytes); decoded by the renderer. */
struct GltfImage {
	std::vector<uint8_t> bytes;
	std::string mimeType;
};

struct GltfPrimitive {
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	int material = 0;
	int part = 0;
};

struct GltfNode {
	std::string name;
	int parent = -1;
	skm::Vec3 translation;
	skm::Quat rotation;
	skm::Vec3 scale { 1.0F, 1.0F, 1.0F };
};

enum class ChannelPath : uint8_t { Translation, Rotation, Scale };

struct GltfChannel {
	int node = -1;
	ChannelPath path = ChannelPath::Translation;
	bool step = false;         // STEP interpolation (else LINEAR)
	std::vector<float> times;  // ascending
	std::vector<float> values; // 3 (T, S) or 4 (R) floats per key
};

struct GltfClip {
	std::string name;
	float duration = 0.0F;
	std::vector<GltfChannel> channels;
};

struct GltfModel {
	std::vector<SkinnedVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<GltfPrimitive> primitives;
	std::vector<GltfMaterial> materials;
	std::vector<GltfNode> nodes;
	/** Node indices with every parent before its children. */
	std::vector<int> nodeOrder;
	/** Skin joints (node indices) and their inverse bind matrices. */
	std::vector<int> joints;
	std::vector<skm::Mat4> inverseBind;
	/** Rigid mesh nodes, palette entry joints.size() + i. */
	std::vector<int> rigidNodes;
	std::vector<std::string> partNames;
	std::vector<GltfClip> clips;
	/** Every image a material samples (base colour, normal, metallic-roughness), GltfMaterial indices refer to these. */
	std::vector<GltfImage> images;
	/** Index in `images` of the body's base colour image (part 0), -1 when untextured. */
	int bodyImage = -1;
	/** A copy of the body's base colour image bytes (images[bodyImage]); the renderer may release it after decoding. */
	std::vector<uint8_t> imageBytes;
	std::string imageMimeType;

	[[nodiscard]] size_t paletteSize() const { return joints.size() + rigidNodes.size(); }
	[[nodiscard]] int findClip(std::string_view name) const;
	[[nodiscard]] int findNode(std::string_view name) const;
	[[nodiscard]] int findPart(std::string_view name) const;
};

/** Parses a .glb held in memory. False (with `error` set) when the file is not a usable skinned model. */
bool LoadGltfModel(std::span<const uint8_t> glb, GltfModel &model, std::string &error);

/** Local TRS of every node, the pose being built. */
struct NodePose {
	skm::Vec3 translation;
	skm::Quat rotation;
	skm::Vec3 scale { 1.0F, 1.0F, 1.0F };
};

/**
 * Samples `clip` at `time` seconds into `pose` (resized to the node count; nodes without channels keep their rest
 * TRS). Looping clips wrap `time` modulo the duration (their last key duplicates the first); others clamp it.
 * clip < 0 gives the rest pose.
 */
void SampleClip(const GltfModel &model, int clip, float time, bool loop, std::vector<NodePose> &pose);

/** Global (model space) matrix of every node for `pose`. */
void ComputeGlobalMatrices(const GltfModel &model, const std::vector<NodePose> &pose, std::vector<skm::Mat4> &globals);

/** The joint palette (see file comment) from the global node matrices. */
void ComputePalette(const GltfModel &model, const std::vector<skm::Mat4> &globals, std::vector<skm::Mat4> &palette);

} // namespace devilution::render3d
