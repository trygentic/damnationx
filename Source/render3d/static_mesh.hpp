#pragma once
/**
 * @file render3d/static_mesh.hpp
 *
 * CPU side of a static (unskinned) glTF mesh placed in the 3D world (render3d/placement.hpp): the model's rest pose
 * baked into one vertex / index buffer in model space, its materials and images, and its bounds.
 */

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "render3d/gltf_model.hpp"
#include "render3d/skinned_math.hpp"

namespace devilution::render3d {

/** 36 bytes; matches StaticVertexIn in static_mesh_renderer.cpp. */
struct StaticVertex {
	float position[3];
	float normal[3];
	float uv[2];
	uint32_t material;
};
static_assert(sizeof(StaticVertex) == 36);

struct StaticMesh {
	std::vector<StaticVertex> vertices;
	std::vector<uint32_t> indices;
	/** Index ranges with one material each (GltfPrimitive::part is unused). */
	std::vector<GltfPrimitive> primitives;
	std::vector<GltfMaterial> materials;
	std::vector<GltfImage> images;
	skm::Vec3 boundsMin;
	skm::Vec3 boundsMax;
};

/**
 * Parses a .glb (skinned or not) and bakes its rest pose into `mesh`. False (with `error` set) when the file is not
 * usable. Limits: 1..500000 triangles.
 */
bool LoadStaticMesh(std::span<const uint8_t> glb, StaticMesh &mesh, std::string &error);

/** World-space axis-aligned bounds of `mesh` under `model`. */
void TransformBounds(const StaticMesh &mesh, const skm::Mat4 &model, skm::Vec3 &outMin, skm::Vec3 &outMax);

} // namespace devilution::render3d
