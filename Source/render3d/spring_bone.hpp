#pragma once
/**
 * @file render3d/spring_bone.hpp
 *
 * Runtime spring bones for loose cloth (GltfModel::springs): a VRM-style verlet solver at a fixed 60 Hz, ported line
 * by line from the offline reference solver p39_spring_solver.py (Sorcerer phase 39a, round 2). Per joint, root to
 * tip: inertia with drag, a pull back to the animated rest direction, gravity, the bone length, the optional maxAngle
 * cone around the rest direction, then a push out of
 * each capsule of the chain's collider groups whose part is drawn, then above the floor (re-applying the length after
 * each push). A chain root's parent frame is its parent
 * node, or with parentBlend the weighted blend of the root rest frame carried by each listed body node.
 *
 * The solver runs in a world frame: model space turned by `worldYaw` about +Y, so turning the character swings the
 * cloth while gravity stays -Y. ponytail: world translation is ignored (clips walk in place); feed the movement in
 * as a velocity if the cloth looks too still while running.
 *
 * It runs after the clip is sampled and its globals computed, and overwrites the globals of the spring joints and
 * their descendants; ComputePalette then builds the palette as usual.
 */

#include <vector>

#include "render3d/gltf_model.hpp"
#include "render3d/skinned_animation.hpp"
#include "render3d/skinned_math.hpp"

namespace devilution::render3d {

/** Spring state of one animated model instance (one per player and model kind). */
struct SpringState {
	/** Tail of every spring joint (chains in file order, tips excluded), world frame. */
	std::vector<skm::Vec3> tails;
	std::vector<skm::Vec3> prevTails;
	float accumulator = 0.0F;
	/** False until the first update; clear it on a model reload or when the slot changes owner. */
	bool valid = false;
};

constexpr float SpringStepSeconds = 1.0F / 60.0F;
/** A larger frame time (pause, menu, level load) resets the cloth to rest instead of stepping. */
constexpr float SpringMaxFrameSeconds = 0.25F;
/** Fixed steps run on the current pose after a reset, so the cloth has settled when it is first drawn. */
constexpr int SpringRunInSteps = 30;

/** Puts every joint at its rest pose under `globals` with zero velocity. */
void ResetSprings(const GltfModel &model, const std::vector<skm::Mat4> &globals, SpringState &state, float worldYaw = 0.0F);

/** One fixed 60 Hz step of every tail against the body pose `globals` (the joints' own globals are not read). */
void StepSprings(const GltfModel &model, const std::vector<skm::Mat4> &globals, SpringState &state, float worldYaw = 0.0F, PartMask visibleParts = ~PartMask {});

/**
 * Overwrites the model space globals of the spring joints (aimed at their tails) and of every node below them (from
 * `pose`).
 */
void PoseSprings(const GltfModel &model, const std::vector<NodePose> &pose, std::vector<skm::Mat4> &globals, const SpringState &state, float worldYaw = 0.0F);

/**
 * A frame: resets (plus the run-in) when the state is not valid or `dt` is over SpringMaxFrameSeconds, otherwise runs
 * the fixed steps `dt` adds up to; then PoseSprings. Does nothing for a model without springs.
 */
void UpdateSprings(const GltfModel &model, const std::vector<NodePose> &pose, std::vector<skm::Mat4> &globals, SpringState &state, float dt, float worldYaw, PartMask visibleParts = ~PartMask {});

} // namespace devilution::render3d
