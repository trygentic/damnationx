#include "render3d/static_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace devilution::render3d {

bool LoadStaticMesh(std::span<const uint8_t> glb, StaticMesh &mesh, std::string &error)
{
	mesh = {};
	GltfModel model;
	GltfLoadOptions options;
	options.allowStatic = true;
	if (!LoadGltfModel(glb, model, error, options))
		return false;
	const size_t triangles = model.indices.size() / 3;
	if (triangles == 0 || triangles > 500000) {
		error = "triangle count outside 1..500000";
		return false;
	}

	std::vector<NodePose> pose;
	SampleClip(model, -1, 0.0F, false, pose);
	std::vector<skm::Mat4> globals;
	ComputeGlobalMatrices(model, pose, globals);
	std::vector<skm::Mat4> palette;
	ComputePalette(model, globals, palette);

	constexpr float kInf = std::numeric_limits<float>::infinity();
	mesh.boundsMin = { kInf, kInf, kInf };
	mesh.boundsMax = { -kInf, -kInf, -kInf };
	mesh.vertices.reserve(model.vertices.size());
	for (const SkinnedVertex &source : model.vertices) {
		skm::Vec3 position {};
		skm::Vec3 normal {};
		for (int w = 0; w < 4; ++w) {
			const float weight = source.weights[w];
			if (weight == 0.0F || source.joints[w] >= palette.size())
				continue;
			const skm::Mat4 &matrix = palette[source.joints[w]];
			position = position + skm::TransformPoint(matrix, { source.position[0], source.position[1], source.position[2] }) * weight;
			normal = normal + skm::TransformDirection(matrix, { source.normal[0], source.normal[1], source.normal[2] }) * weight;
		}
		if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
			error = "non-finite vertex position";
			return false;
		}
		const float length = std::sqrt(skm::Dot(normal, normal));
		normal = length > 1e-12F ? normal * (1.0F / length) : skm::Vec3 { 0.0F, 1.0F, 0.0F };
		StaticVertex vertex {};
		vertex.position[0] = position.x;
		vertex.position[1] = position.y;
		vertex.position[2] = position.z;
		vertex.normal[0] = normal.x;
		vertex.normal[1] = normal.y;
		vertex.normal[2] = normal.z;
		vertex.uv[0] = source.uv[0];
		vertex.uv[1] = source.uv[1];
		vertex.material = source.packed & 0xFFU;
		mesh.vertices.push_back(vertex);
		mesh.boundsMin = { std::min(mesh.boundsMin.x, position.x), std::min(mesh.boundsMin.y, position.y), std::min(mesh.boundsMin.z, position.z) };
		mesh.boundsMax = { std::max(mesh.boundsMax.x, position.x), std::max(mesh.boundsMax.y, position.y), std::max(mesh.boundsMax.z, position.z) };
	}
	mesh.indices = std::move(model.indices);
	mesh.primitives = std::move(model.primitives);
	mesh.materials = std::move(model.materials);
	mesh.images = std::move(model.images);
	if (mesh.materials.empty())
		mesh.materials.emplace_back();
	for (GltfPrimitive &primitive : mesh.primitives) {
		if (primitive.material < 0 || static_cast<size_t>(primitive.material) >= mesh.materials.size())
			primitive.material = 0;
	}
	return true;
}

void TransformBounds(const StaticMesh &mesh, const skm::Mat4 &model, skm::Vec3 &outMin, skm::Vec3 &outMax)
{
	constexpr float kInf = std::numeric_limits<float>::infinity();
	outMin = { kInf, kInf, kInf };
	outMax = { -kInf, -kInf, -kInf };
	for (int corner = 0; corner < 8; ++corner) {
		const skm::Vec3 local {
			(corner & 1) != 0 ? mesh.boundsMax.x : mesh.boundsMin.x,
			(corner & 2) != 0 ? mesh.boundsMax.y : mesh.boundsMin.y,
			(corner & 4) != 0 ? mesh.boundsMax.z : mesh.boundsMin.z,
		};
		const skm::Vec3 p = skm::TransformPoint(model, local);
		outMin = { std::min(outMin.x, p.x), std::min(outMin.y, p.y), std::min(outMin.z, p.z) };
		outMax = { std::max(outMax.x, p.x), std::max(outMax.y, p.y), std::max(outMax.z, p.z) };
	}
}

} // namespace devilution::render3d
