#include "render3d/gltf_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <cgltf.h>

namespace devilution::render3d {
namespace {

using skm::Mat4;
using skm::Quat;
using skm::Vec3;

struct CgltfData {
	cgltf_data *data = nullptr;
	~CgltfData()
	{
		if (data != nullptr)
			cgltf_free(data);
	}
};

int NodeIndex(const cgltf_data &data, const cgltf_node *node)
{
	return node == nullptr ? -1 : static_cast<int>(node - data.nodes);
}

const uint8_t *BufferViewBytes(const cgltf_buffer_view *view)
{
	if (view == nullptr || view->buffer == nullptr || view->buffer->data == nullptr)
		return nullptr;
	return static_cast<const uint8_t *>(view->buffer->data) + view->offset;
}

int MaterialIndex(const cgltf_data &data, const cgltf_material *material)
{
	return material == nullptr ? -1 : static_cast<int>(material - data.materials);
}

/** Index of `image` in model.images (added on first use), -1 when it has no embedded bytes. */
int ImageIndex(const cgltf_texture_view &view, std::vector<const cgltf_image *> &seen, GltfModel &model)
{
	if (view.texture == nullptr || view.texture->image == nullptr)
		return -1;
	const cgltf_image *image = view.texture->image;
	for (size_t i = 0; i < seen.size(); ++i) {
		if (seen[i] == image)
			return static_cast<int>(i);
	}
	const uint8_t *bytes = BufferViewBytes(image->buffer_view);
	if (bytes == nullptr)
		return -1;
	GltfImage loaded;
	loaded.bytes.assign(bytes, bytes + image->buffer_view->size);
	loaded.mimeType = image->mime_type != nullptr ? image->mime_type : "";
	seen.push_back(image);
	model.images.push_back(std::move(loaded));
	return static_cast<int>(model.images.size()) - 1;
}

bool LoadMaterials(const cgltf_data &data, GltfModel &model)
{
	std::vector<const cgltf_image *> seen;
	for (cgltf_size i = 0; i < data.materials_count; ++i) {
		const cgltf_material &source = data.materials[i];
		GltfMaterial material;
		material.name = source.name != nullptr ? source.name : "";
		if (source.has_pbr_metallic_roughness) {
			const cgltf_pbr_metallic_roughness &pbr = source.pbr_metallic_roughness;
			std::copy_n(pbr.base_color_factor, 4, material.baseColor);
			material.metallic = pbr.metallic_factor;
			material.roughness = pbr.roughness_factor;
			material.baseColorImage = ImageIndex(pbr.base_color_texture, seen, model);
			material.metallicRoughnessImage = ImageIndex(pbr.metallic_roughness_texture, seen, model);
			material.textured = material.baseColorImage >= 0;
		}
		material.normalImage = ImageIndex(source.normal_texture, seen, model);
		if (material.normalImage >= 0)
			material.normalScale = source.normal_texture.scale;
		if (source.has_specular)
			material.specular = source.specular.specular_factor;
		model.materials.push_back(std::move(material));
	}
	if (model.materials.empty())
		model.materials.push_back({});
	return true;
}

const cgltf_accessor *FindAttribute(const cgltf_primitive &primitive, cgltf_attribute_type type, cgltf_int index = 0)
{
	for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
		if (primitive.attributes[i].type == type && primitive.attributes[i].index == index)
			return primitive.attributes[i].data;
	}
	return nullptr;
}

bool AppendPrimitive(const cgltf_data &data, const cgltf_primitive &primitive, int part, bool skinned, int rigidPaletteEntry,
    GltfModel &model, std::string &error)
{
	if (primitive.type != cgltf_primitive_type_triangles) {
		error = "only triangle primitives are supported";
		return false;
	}
	const cgltf_accessor *positions = FindAttribute(primitive, cgltf_attribute_type_position);
	const cgltf_accessor *normals = FindAttribute(primitive, cgltf_attribute_type_normal);
	const cgltf_accessor *uvs = FindAttribute(primitive, cgltf_attribute_type_texcoord);
	const cgltf_accessor *joints = FindAttribute(primitive, cgltf_attribute_type_joints);
	const cgltf_accessor *weights = FindAttribute(primitive, cgltf_attribute_type_weights);
	if (positions == nullptr || normals == nullptr) {
		error = "a primitive lacks POSITION or NORMAL";
		return false;
	}
	if (skinned && (joints == nullptr || weights == nullptr)) {
		error = "a skinned primitive lacks JOINTS_0 / WEIGHTS_0";
		return false;
	}
	const int material = std::max(MaterialIndex(data, primitive.material), 0);
	const auto base = static_cast<uint32_t>(model.vertices.size());
	for (cgltf_size v = 0; v < positions->count; ++v) {
		SkinnedVertex vertex {};
		cgltf_accessor_read_float(positions, v, vertex.position, 3);
		cgltf_accessor_read_float(normals, v, vertex.normal, 3);
		if (uvs != nullptr)
			cgltf_accessor_read_float(uvs, v, vertex.uv, 2);
		if (skinned) {
			cgltf_uint jointIndices[4] {};
			cgltf_accessor_read_uint(joints, v, jointIndices, 4);
			cgltf_accessor_read_float(weights, v, vertex.weights, 4);
			float sum = 0.0F;
			for (int k = 0; k < 4; ++k) {
				vertex.joints[k] = static_cast<uint8_t>(std::min<cgltf_uint>(jointIndices[k], 255));
				sum += vertex.weights[k];
			}
			if (sum > 0.0F) {
				for (float &weight : vertex.weights)
					weight /= sum;
			} else {
				vertex.weights[0] = 1.0F;
			}
		} else {
			vertex.joints[0] = static_cast<uint8_t>(rigidPaletteEntry);
			vertex.weights[0] = 1.0F;
		}
		vertex.packed = static_cast<uint32_t>(material) | (static_cast<uint32_t>(part) << 8);
		model.vertices.push_back(vertex);
	}
	GltfPrimitive range;
	range.firstIndex = static_cast<uint32_t>(model.indices.size());
	range.material = material;
	range.part = part;
	if (primitive.indices != nullptr) {
		for (cgltf_size i = 0; i < primitive.indices->count; ++i)
			model.indices.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i)));
	} else {
		for (cgltf_size i = 0; i < positions->count; ++i)
			model.indices.push_back(base + static_cast<uint32_t>(i));
	}
	range.indexCount = static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
	model.primitives.push_back(range);
	return true;
}

bool LoadClips(const cgltf_data &data, GltfModel &model)
{
	for (cgltf_size a = 0; a < data.animations_count; ++a) {
		const cgltf_animation &animation = data.animations[a];
		GltfClip clip;
		clip.name = animation.name != nullptr ? animation.name : "";
		for (cgltf_size c = 0; c < animation.channels_count; ++c) {
			const cgltf_animation_channel &source = animation.channels[c];
			if (source.target_node == nullptr || source.sampler == nullptr)
				continue;
			GltfChannel channel;
			channel.node = NodeIndex(data, source.target_node);
			int components = 3;
			switch (source.target_path) {
			case cgltf_animation_path_type_translation:
				channel.path = ChannelPath::Translation;
				break;
			case cgltf_animation_path_type_rotation:
				channel.path = ChannelPath::Rotation;
				components = 4;
				break;
			case cgltf_animation_path_type_scale:
				channel.path = ChannelPath::Scale;
				break;
			default:
				continue; // morph weights: none in the warrior
			}
			const cgltf_animation_sampler &sampler = *source.sampler;
			channel.step = sampler.interpolation == cgltf_interpolation_type_step;
			const bool cubic = sampler.interpolation == cgltf_interpolation_type_cubic_spline;
			const cgltf_size keys = sampler.input->count;
			channel.times.resize(keys);
			channel.values.resize(keys * components);
			for (cgltf_size k = 0; k < keys; ++k) {
				cgltf_accessor_read_float(sampler.input, k, &channel.times[k], 1);
				// Cubic splines store (in-tangent, value, out-tangent) per key: keep the value, sample linearly.
				cgltf_accessor_read_float(sampler.output, cubic ? k * 3 + 1 : k, &channel.values[k * components], static_cast<cgltf_size>(components));
			}
			if (!channel.times.empty())
				clip.duration = std::max(clip.duration, channel.times.back());
			clip.channels.push_back(std::move(channel));
		}
		model.clips.push_back(std::move(clip));
	}
	return true;
}

/** Index of the key interval holding `time`: times[i] <= time < times[i + 1] (clamped to the ends). */
size_t FindKey(const std::vector<float> &times, float time, float &fraction)
{
	fraction = 0.0F;
	if (times.size() < 2 || time <= times.front())
		return 0;
	if (time >= times.back()) {
		fraction = 1.0F;
		return times.size() - 2;
	}
	const auto it = std::upper_bound(times.begin(), times.end(), time);
	const size_t i = static_cast<size_t>(it - times.begin()) - 1;
	const float span = times[i + 1] - times[i];
	fraction = span > 0.0F ? (time - times[i]) / span : 0.0F;
	return i;
}

} // namespace

int GltfModel::findClip(std::string_view name) const
{
	for (size_t i = 0; i < clips.size(); ++i) {
		if (clips[i].name == name)
			return static_cast<int>(i);
	}
	return -1;
}

int GltfModel::findNode(std::string_view name) const
{
	for (size_t i = 0; i < nodes.size(); ++i) {
		if (nodes[i].name == name)
			return static_cast<int>(i);
	}
	return -1;
}

int GltfModel::findPart(std::string_view name) const
{
	for (size_t i = 0; i < partNames.size(); ++i) {
		if (partNames[i] == name)
			return static_cast<int>(i);
	}
	return -1;
}

bool LoadGltfModel(std::span<const uint8_t> glb, GltfModel &model, std::string &error)
{
	model = {};
	cgltf_options options {};
	CgltfData parsed;
	if (cgltf_parse(&options, glb.data(), glb.size(), &parsed.data) != cgltf_result_success) {
		error = "not a valid glTF / GLB file";
		return false;
	}
	if (cgltf_load_buffers(&options, parsed.data, nullptr) != cgltf_result_success) {
		error = "the GLB has no embedded binary buffer";
		return false;
	}
	const cgltf_data &data = *parsed.data;
	if (data.skins_count != 1) {
		error = "expected exactly one skin";
		return false;
	}

	// Nodes and a parents-first order.
	model.nodes.resize(data.nodes_count);
	for (cgltf_size i = 0; i < data.nodes_count; ++i) {
		const cgltf_node &source = data.nodes[i];
		GltfNode &node = model.nodes[i];
		node.name = source.name != nullptr ? source.name : "";
		node.parent = NodeIndex(data, source.parent);
		if (source.has_matrix) {
			// Not produced by the Blender exporter for this asset; keep the translation only.
			node.translation = { source.matrix[12], source.matrix[13], source.matrix[14] };
		}
		if (source.has_translation)
			node.translation = { source.translation[0], source.translation[1], source.translation[2] };
		if (source.has_rotation)
			node.rotation = skm::Normalize(Quat { source.rotation[0], source.rotation[1], source.rotation[2], source.rotation[3] });
		if (source.has_scale)
			node.scale = { source.scale[0], source.scale[1], source.scale[2] };
	}
	std::vector<bool> placed(model.nodes.size(), false);
	const std::function<void(int)> place = [&](int index) {
		if (placed[index])
			return;
		if (model.nodes[index].parent >= 0)
			place(model.nodes[index].parent);
		placed[index] = true;
		model.nodeOrder.push_back(index);
	};
	for (size_t i = 0; i < model.nodes.size(); ++i)
		place(static_cast<int>(i));

	// Skin.
	const cgltf_skin &skin = data.skins[0];
	for (cgltf_size j = 0; j < skin.joints_count; ++j) {
		model.joints.push_back(NodeIndex(data, skin.joints[j]));
		Mat4 inverseBind;
		if (skin.inverse_bind_matrices != nullptr)
			cgltf_accessor_read_float(skin.inverse_bind_matrices, j, inverseBind.m.data(), 16);
		model.inverseBind.push_back(inverseBind);
	}
	if (model.joints.size() > 200) {
		error = "too many joints";
		return false;
	}

	if (!LoadMaterials(data, model))
		return false;

	// Mesh nodes become parts; the skinned body (the skinned mesh with the most vertices) first so it is part 0.
	std::vector<int> meshNodes;
	int bodyNode = -1;
	cgltf_size bodyVertices = 0;
	for (cgltf_size i = 0; i < data.nodes_count; ++i) {
		const cgltf_mesh *mesh = data.nodes[i].mesh;
		if (mesh == nullptr)
			continue;
		meshNodes.push_back(static_cast<int>(i));
		if (data.nodes[i].skin == nullptr)
			continue;
		cgltf_size vertices = 0;
		for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
			if (const cgltf_accessor *positions = FindAttribute(mesh->primitives[p], cgltf_attribute_type_position); positions != nullptr)
				vertices += positions->count;
		}
		if (vertices > bodyVertices) {
			bodyVertices = vertices;
			bodyNode = static_cast<int>(i);
		}
	}
	std::stable_sort(meshNodes.begin(), meshNodes.end(), [&](int a, int b) { return a == bodyNode && b != bodyNode; });
	if (bodyNode >= 0) {
		const cgltf_mesh *mesh = data.nodes[bodyNode].mesh;
		for (cgltf_size p = 0; p < mesh->primitives_count && model.bodyImage < 0; ++p) {
			const int material = MaterialIndex(data, mesh->primitives[p].material);
			if (material >= 0)
				model.bodyImage = model.materials[static_cast<size_t>(material)].baseColorImage;
		}
	}
	if (model.bodyImage >= 0) {
		model.imageBytes = model.images[static_cast<size_t>(model.bodyImage)].bytes;
		model.imageMimeType = model.images[static_cast<size_t>(model.bodyImage)].mimeType;
	}
	if (meshNodes.size() > 24) {
		error = "too many mesh nodes";
		return false;
	}
	for (const int nodeIndex : meshNodes) {
		const cgltf_node &node = data.nodes[nodeIndex];
		const int part = static_cast<int>(model.partNames.size());
		model.partNames.push_back(model.nodes[nodeIndex].name);
		const bool skinned = node.skin != nullptr;
		if (skinned && node.skin != &data.skins[0]) {
			error = "a mesh uses another skin";
			return false;
		}
		int rigidEntry = -1;
		if (!skinned) {
			rigidEntry = static_cast<int>(model.joints.size() + model.rigidNodes.size());
			if (rigidEntry > 255) {
				error = "too many rigid mesh nodes";
				return false;
			}
			model.rigidNodes.push_back(nodeIndex);
		}
		for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
			if (!AppendPrimitive(data, node.mesh->primitives[p], part, skinned, rigidEntry, model, error))
				return false;
		}
	}
	if (model.vertices.empty() || model.indices.empty()) {
		error = "no geometry";
		return false;
	}
	return LoadClips(data, model);
}

void SampleClip(const GltfModel &model, int clip, float time, bool loop, std::vector<NodePose> &pose)
{
	pose.resize(model.nodes.size());
	for (size_t i = 0; i < model.nodes.size(); ++i)
		pose[i] = { model.nodes[i].translation, model.nodes[i].rotation, model.nodes[i].scale };
	if (clip < 0 || static_cast<size_t>(clip) >= model.clips.size())
		return;
	const GltfClip &source = model.clips[clip];
	if (source.duration > 0.0F) {
		if (loop) {
			time = std::fmod(time, source.duration);
			if (time < 0.0F)
				time += source.duration;
		} else {
			time = std::clamp(time, 0.0F, source.duration);
		}
	}
	for (const GltfChannel &channel : source.channels) {
		if (channel.node < 0 || static_cast<size_t>(channel.node) >= pose.size() || channel.times.empty())
			continue;
		float fraction = 0.0F;
		const size_t key = FindKey(channel.times, time, fraction);
		const size_t next = std::min(key + 1, channel.times.size() - 1);
		if (channel.step)
			fraction = fraction >= 1.0F ? 1.0F : 0.0F;
		NodePose &target = pose[channel.node];
		if (channel.path == ChannelPath::Rotation) {
			const float *a = &channel.values[key * 4];
			const float *b = &channel.values[next * 4];
			target.rotation = skm::Slerp(Quat { a[0], a[1], a[2], a[3] }, Quat { b[0], b[1], b[2], b[3] }, fraction);
		} else {
			const float *a = &channel.values[key * 3];
			const float *b = &channel.values[next * 3];
			const Vec3 value = skm::Lerp(Vec3 { a[0], a[1], a[2] }, Vec3 { b[0], b[1], b[2] }, fraction);
			if (channel.path == ChannelPath::Translation)
				target.translation = value;
			else
				target.scale = value;
		}
	}
}

void ComputeGlobalMatrices(const GltfModel &model, const std::vector<NodePose> &pose, std::vector<skm::Mat4> &globals)
{
	globals.resize(model.nodes.size());
	for (const int index : model.nodeOrder) {
		const NodePose &local = pose[index];
		const Mat4 matrix = skm::ComposeTrs(local.translation, local.rotation, local.scale);
		const int parent = model.nodes[index].parent;
		globals[index] = parent >= 0 ? globals[parent] * matrix : matrix;
	}
}

void ComputePalette(const GltfModel &model, const std::vector<skm::Mat4> &globals, std::vector<skm::Mat4> &palette)
{
	palette.resize(model.paletteSize());
	for (size_t j = 0; j < model.joints.size(); ++j)
		palette[j] = globals[model.joints[j]] * model.inverseBind[j];
	for (size_t r = 0; r < model.rigidNodes.size(); ++r)
		palette[model.joints.size() + r] = globals[model.rigidNodes[r]];
}

} // namespace devilution::render3d
