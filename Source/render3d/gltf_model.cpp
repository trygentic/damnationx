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

namespace {

/**
 * TRS of a node given as a matrix (other exporters than Blender's write these for static meshes). Assumes no shear;
 * a negative determinant is folded into the x scale.
 */
void DecomposeMatrix(const cgltf_float *m, GltfNode &node)
{
	node.translation = { m[12], m[13], m[14] };
	Vec3 axes[3] { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[8], m[9], m[10] } };
	float scale[3];
	for (int i = 0; i < 3; ++i) {
		scale[i] = std::sqrt(skm::Dot(axes[i], axes[i]));
		if (scale[i] > 1e-12F)
			axes[i] = axes[i] * (1.0F / scale[i]);
	}
	if (skm::Dot(skm::Cross(axes[0], axes[1]), axes[2]) < 0.0F) {
		scale[0] = -scale[0];
		axes[0] = axes[0] * -1.0F;
	}
	node.scale = { scale[0], scale[1], scale[2] };
	Mat4 rotation;
	for (int column = 0; column < 3; ++column) {
		rotation(0, column) = axes[column].x;
		rotation(1, column) = axes[column].y;
		rotation(2, column) = axes[column].z;
	}
	node.rotation = skm::QuatFromRotation(rotation);
}

// ponytail: a minimal JSON reader for the one extras block the loader reads (cgltf keeps its jsmn private); switch
// to a JSON library if a second consumer appears. \u escapes become '?', fine for the ASCII keys and names used.
struct Json {
	enum class Type : uint8_t { Null, Bool, Number, String, Array, Object } type = Type::Null;
	double number = 0.0;
	std::string string;
	std::vector<std::string> keys; // object keys, parallel to items
	std::vector<Json> items;       // array elements or object values

	[[nodiscard]] const Json *find(std::string_view key) const
	{
		for (size_t i = 0; i < keys.size(); ++i) {
			if (keys[i] == key)
				return &items[i];
		}
		return nullptr;
	}
};

class JsonReader {
public:
	explicit JsonReader(std::string_view text)
	    : text_(text)
	{
	}

	bool Read(Json &out)
	{
		if (!Value(out, 0))
			return false;
		SkipSpace();
		return pos_ == text_.size();
	}

private:
	std::string_view text_;
	size_t pos_ = 0;

	[[nodiscard]] bool AtDigit() const { return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9'; }

	void SkipSpace()
	{
		while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
			++pos_;
	}

	bool Eat(char c)
	{
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] != c)
			return false;
		++pos_;
		return true;
	}

	bool String(std::string &out)
	{
		if (!Eat('"'))
			return false;
		while (pos_ < text_.size()) {
			const char c = text_[pos_++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos_ >= text_.size())
				return false;
			const char escaped = text_[pos_++];
			switch (escaped) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u':
				if (pos_ + 4 > text_.size())
					return false;
				pos_ += 4;
				out += '?';
				break;
			default: out += escaped; break;
			}
		}
		return false;
	}

	// Locale independent (strtod would read "0,5" under a comma locale).
	bool Number(double &out)
	{
		double sign = 1.0;
		if (text_[pos_] == '-') {
			sign = -1.0;
			++pos_;
		}
		double mantissa = 0.0;
		int exponent = 0;
		bool digits = false;
		for (; AtDigit(); ++pos_, digits = true)
			mantissa = mantissa * 10.0 + (text_[pos_] - '0');
		if (pos_ < text_.size() && text_[pos_] == '.') {
			++pos_;
			for (; AtDigit(); ++pos_, digits = true, --exponent)
				mantissa = mantissa * 10.0 + (text_[pos_] - '0');
		}
		if (!digits)
			return false;
		if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
			++pos_;
			int exponentSign = 1;
			if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
				exponentSign = text_[pos_++] == '-' ? -1 : 1;
			if (!AtDigit())
				return false;
			int value = 0;
			for (; AtDigit(); ++pos_)
				value = std::min(value * 10 + (text_[pos_] - '0'), 9999);
			exponent += exponentSign * value;
		}
		out = sign * mantissa * std::pow(10.0, exponent);
		return true;
	}

	bool Literal(std::string_view word)
	{
		if (text_.substr(pos_, word.size()) != word)
			return false;
		pos_ += word.size();
		return true;
	}

	bool Value(Json &out, int depth)
	{
		SkipSpace();
		if (pos_ >= text_.size() || depth > 64)
			return false;
		const char c = text_[pos_];
		if (c == '{' || c == '[') {
			const bool object = c == '{';
			const char close = object ? '}' : ']';
			++pos_;
			out.type = object ? Json::Type::Object : Json::Type::Array;
			if (Eat(close))
				return true;
			do {
				if (object) {
					out.keys.emplace_back();
					if (!String(out.keys.back()) || !Eat(':'))
						return false;
				}
				out.items.emplace_back();
				if (!Value(out.items.back(), depth + 1))
					return false;
			} while (Eat(','));
			return Eat(close);
		}
		if (c == '"') {
			out.type = Json::Type::String;
			return String(out.string);
		}
		if (Literal("true")) {
			out.type = Json::Type::Bool;
			out.number = 1.0;
			return true;
		}
		if (Literal("false")) {
			out.type = Json::Type::Bool;
			return true;
		}
		if (Literal("null"))
			return true;
		out.type = Json::Type::Number;
		return Number(out.number);
	}
};

bool ReadFloat(const Json *value, float &out)
{
	if (value == nullptr || value->type != Json::Type::Number)
		return false;
	out = static_cast<float>(value->number);
	return std::isfinite(out);
}

bool ReadFloats(const Json *value, float *out, size_t count)
{
	if (value == nullptr || value->type != Json::Type::Array || value->items.size() != count)
		return false;
	for (size_t i = 0; i < count; ++i) {
		if (!ReadFloat(&value->items[i], out[i]))
			return false;
	}
	return true;
}

bool ReadVec3(const Json *value, Vec3 &out)
{
	float v[3];
	if (!ReadFloats(value, v, 3))
		return false;
	out = { v[0], v[1], v[2] };
	return true;
}

bool ReadIndex(const Json *value, size_t count, int &out)
{
	if (value == nullptr || value->type != Json::Type::Number || value->number < 0.0 || value->number >= static_cast<double>(count)
	    || value->number != std::floor(value->number))
		return false;
	out = static_cast<int>(value->number);
	return true;
}

bool ReadArray(const Json *value, const std::vector<Json> *&out)
{
	if (value == nullptr || value->type != Json::Type::Array)
		return false;
	out = &value->items;
	return true;
}

bool UnitScale(const GltfNode &node)
{
	return std::abs(node.scale.x - 1.0F) < 1e-3F && std::abs(node.scale.y - 1.0F) < 1e-3F && std::abs(node.scale.z - 1.0F) < 1e-3F;
}

bool ReadSpringBones(const Json &root, const std::vector<GltfNode> &nodes, SpringBones &springs, std::string &error)
{
	const Json *block = root.find("DAMNATIONX_springBone");
	if (block == nullptr)
		return true;
	const std::vector<Json> *colliders;
	const std::vector<Json> *groups;
	const std::vector<Json> *chains;
	if (!ReadArray(block->find("colliders"), colliders) || !ReadArray(block->find("colliderGroups"), groups) || !ReadArray(block->find("springs"), chains)) {
		error = "colliders, colliderGroups and springs must be arrays";
		return false;
	}
	if (const Json *floor = block->find("floor"); floor != nullptr && !ReadFloat(floor, springs.floor)) {
		error = "floor must be a number";
		return false;
	}
	for (const Json &collider : *colliders) {
		const Json *shape = collider.find("shape");
		const Json *capsule = shape != nullptr ? shape->find("capsule") : nullptr;
		SpringCapsule &out = springs.capsules.emplace_back();
		if (capsule == nullptr || !ReadIndex(collider.find("node"), nodes.size(), out.node) || !ReadVec3(capsule->find("offset"), out.offset)
		    || !ReadVec3(capsule->find("tail"), out.tail) || !ReadFloat(capsule->find("radius"), out.radius) || out.radius < 0.0F) {
			error = "collider " + std::to_string(springs.capsules.size() - 1) + " is not a capsule on a node";
			return false;
		}
	}
	std::vector<std::vector<int>> groupCapsules;
	for (const Json &group : *groups) {
		const std::vector<Json> *members;
		if (!ReadArray(group.find("colliders"), members)) {
			error = "a collider group has no colliders array";
			return false;
		}
		std::vector<int> &out = groupCapsules.emplace_back();
		for (const Json &member : *members) {
			if (!ReadIndex(&member, springs.capsules.size(), out.emplace_back())) {
				error = "a collider group names a missing collider";
				return false;
			}
		}
	}
	for (const Json &chain : *chains) {
		SpringChain &out = springs.chains.emplace_back();
		const std::string name = "spring " + std::to_string(springs.chains.size() - 1);
		const std::vector<Json> *joints;
		if (!ReadIndex(chain.find("parentNode"), nodes.size(), out.parentNode) || !ReadArray(chain.find("joints"), joints) || joints->size() < 2) {
			error = name + " needs a parentNode and at least two joints";
			return false;
		}
		for (const Json &joint : *joints) {
			SpringJoint &j = out.joints.emplace_back();
			const bool tip = out.joints.size() == joints->size();
			if (!ReadIndex(joint.find("node"), nodes.size(), j.node)) {
				error = name + " has a joint without a valid node";
				return false;
			}
			const int expectedParent = out.joints.size() == 1 ? out.parentNode : out.joints[out.joints.size() - 2].node;
			if (nodes[j.node].parent != expectedParent) {
				error = name + ": node " + std::to_string(j.node) + " is not the child of the joint before it";
				return false;
			}
			if (tip)
				continue;
			if (!ReadFloat(joint.find("hitRadius"), j.hitRadius) || !ReadFloat(joint.find("stiffness"), j.stiffness)
			    || !ReadFloat(joint.find("gravityPower"), j.gravityPower) || !ReadFloat(joint.find("dragForce"), j.dragForce)) {
				error = name + " has a joint without hitRadius, stiffness, gravityPower and dragForce";
				return false;
			}
			if (joint.find("gravityDir") != nullptr && !ReadVec3(joint.find("gravityDir"), j.gravityDir)) {
				error = name + " has a bad gravityDir";
				return false;
			}
			j.gravityDir = skm::Normalize(j.gravityDir);
			if (const Json *maxAngle = joint.find("maxAngle"); maxAngle != nullptr) {
				float degrees;
				if (!ReadFloat(maxAngle, degrees) || degrees < 0.0F || degrees > 180.0F) {
					error = name + " has a maxAngle outside 0..180";
					return false;
				}
				j.cosMaxAngle = std::cos(degrees * 3.14159265F / 180.0F);
			}
			if (!UnitScale(nodes[j.node])) {
				error = name + ": spring joints must have unit scale";
				return false;
			}
		}
		for (size_t k = 1; k < out.joints.size(); ++k) {
			const Vec3 bone = nodes[out.joints[k].node].translation;
			if (skm::Dot(bone, bone) < 1e-12F) {
				error = name + " has a zero length bone";
				return false;
			}
		}
		const std::vector<Json> *chainGroups;
		if (!ReadArray(chain.find("colliderGroups"), chainGroups)) {
			error = name + " has no colliderGroups array";
			return false;
		}
		for (const Json &group : *chainGroups) {
			int index;
			if (!ReadIndex(&group, groupCapsules.size(), index)) {
				error = name + " names a missing collider group";
				return false;
			}
			out.capsules.insert(out.capsules.end(), groupCapsules[index].begin(), groupCapsules[index].end());
		}
		const Json *blend = chain.find("parentBlend");
		if (blend == nullptr || blend->type == Json::Type::Null)
			continue;
		const std::vector<Json> *entries;
		if (!ReadArray(blend, entries) || entries->empty()) {
			error = name + ": parentBlend must be a non-empty array";
			return false;
		}
		float weightSum = 0.0F;
		for (const Json &entry : *entries) {
			SpringParentBlend &e = out.parentBlend.emplace_back();
			const Json *offset = entry.find("restOffset");
			float rotation[4];
			if (!ReadIndex(entry.find("node"), nodes.size(), e.node) || !ReadFloat(entry.find("weight"), e.weight) || e.weight < 0.0F
			    || offset == nullptr || !ReadVec3(offset->find("translation"), e.translation) || !ReadFloats(offset->find("rotation"), rotation, 4)) {
				error = name + " has a bad parentBlend entry";
				return false;
			}
			e.rotation = skm::Normalize(Quat { rotation[0], rotation[1], rotation[2], rotation[3] });
			weightSum += e.weight;
		}
		if (std::abs(weightSum - 1.0F) > 1e-3F) {
			error = name + ": parentBlend weights must sum to 1";
			return false;
		}
		std::stable_sort(out.parentBlend.begin(), out.parentBlend.end(), [](const SpringParentBlend &a, const SpringParentBlend &b) { return a.weight > b.weight; });
	}
	return true;
}

} // namespace

bool ParseSpringBones(std::string_view extrasJson, const std::vector<GltfNode> &nodes, SpringBones &springs, std::string &error)
{
	springs = {};
	Json root;
	if (!JsonReader(extrasJson).Read(root)) {
		error = "the extras are not valid JSON";
		return false;
	}
	if (root.type != Json::Type::Object)
		return true;
	if (ReadSpringBones(root, nodes, springs, error))
		return true;
	springs = {};
	error = "DAMNATIONX_springBone: " + error;
	return false;
}

bool LoadGltfModel(std::span<const uint8_t> glb, GltfModel &model, std::string &error, const GltfLoadOptions &options)
{
	model = {};
	cgltf_options parseOptions {};
	CgltfData parsed;
	if (cgltf_parse(&parseOptions, glb.data(), glb.size(), &parsed.data) != cgltf_result_success) {
		error = "not a valid glTF / GLB file";
		return false;
	}
	if (cgltf_load_buffers(&parseOptions, parsed.data, nullptr) != cgltf_result_success) {
		error = "the GLB has no embedded binary buffer";
		return false;
	}
	const cgltf_data &data = *parsed.data;
	if (data.skins_count != 1 && !(options.allowStatic && data.skins_count == 0)) {
		error = options.allowStatic ? "expected no skin or exactly one skin" : "expected exactly one skin";
		return false;
	}

	// Nodes and a parents-first order.
	model.nodes.resize(data.nodes_count);
	for (cgltf_size i = 0; i < data.nodes_count; ++i) {
		const cgltf_node &source = data.nodes[i];
		GltfNode &node = model.nodes[i];
		node.name = source.name != nullptr ? source.name : "";
		node.parent = NodeIndex(data, source.parent);
		if (source.has_matrix)
			DecomposeMatrix(source.matrix, node);
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

	// Skin (static models have none: every mesh node is rigid).
	for (cgltf_size j = 0; data.skins_count != 0 && j < data.skins[0].joints_count; ++j) {
		const cgltf_skin &skin = data.skins[0];
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
	if (meshNodes.size() > (options.allowStatic ? 255U : 128U)) { // skinned: the shader's 7-bit part index and 128-bit visibility mask (MaxSkinnedParts)
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
	// Bad spring data leaves springError set; the model still loads and its cloth follows the skeleton rigidly.
	if (data.extras.data != nullptr)
		ParseSpringBones(data.extras.data, model.nodes, model.springs, model.springError);
	for (SpringCapsule &capsule : model.springs.capsules)
		capsule.part = model.findPart(model.nodes[capsule.node].name);
	return LoadClips(data, model);
}

void SampleClip(const GltfModel &model, int clip, float time, bool loop, std::vector<NodePose> &pose)
{
	pose.resize(model.nodes.size());
	for (size_t i = 0; i < model.nodes.size(); ++i)
		pose[i] = { model.nodes[i].translation, model.nodes[i].rotation, model.nodes[i].scale };
	SampleClipChannels(model, clip, time, loop, pose);
}

void SampleClipChannels(const GltfModel &model, int clip, float time, bool loop, std::vector<NodePose> &pose)
{
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
