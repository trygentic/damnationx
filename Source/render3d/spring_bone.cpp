#include "render3d/spring_bone.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace devilution::render3d {
namespace {

using skm::Mat4;
using skm::Quat;
using skm::Vec3;

struct WorldCapsule {
	Vec3 a;
	Vec3 b;
	float radius;
	int part;
};

Vec3 Origin(const Mat4 &m) { return { m(0, 3), m(1, 3), m(2, 3) }; }

Mat4 RestLocal(const GltfNode &node) { return skm::ComposeTrs(node.translation, node.rotation, { 1.0F, 1.0F, 1.0F }); }

/** Rotation taking direction `a` onto direction `b` (Rodrigues; antiparallel handled). */
Mat4 FromTo(Vec3 a, Vec3 b)
{
	a = skm::Normalize(a);
	b = skm::Normalize(b);
	const Vec3 v = skm::Cross(a, b);
	const float c = skm::Dot(a, b);
	Mat4 r;
	if (c < -0.999999F) { // 180 degrees: about any axis perpendicular to a
		const Vec3 axis = skm::Normalize(std::abs(a.x) < 0.9F ? skm::Cross(a, { 1.0F, 0.0F, 0.0F }) : skm::Cross(a, { 0.0F, 1.0F, 0.0F }));
		const float u[3] { axis.x, axis.y, axis.z };
		for (int row = 0; row < 3; ++row) {
			for (int column = 0; column < 3; ++column)
				r(row, column) = 2.0F * u[row] * u[column] - (row == column ? 1.0F : 0.0F);
		}
		return r;
	}
	// I + [v]x + [v]x^2 / (1 + c), with [v]x^2 = v v^T - |v|^2 I.
	const float k = 1.0F / (1.0F + c);
	const float u[3] { v.x, v.y, v.z };
	const float v2 = skm::Dot(v, v);
	r(0, 1) = -v.z;
	r(0, 2) = v.y;
	r(1, 0) = v.z;
	r(1, 2) = -v.x;
	r(2, 0) = -v.y;
	r(2, 1) = v.x;
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column)
			r(row, column) += k * (u[row] * u[column] - (row == column ? v2 : 0.0F));
	}
	return r;
}

Vec3 ClosestOnSegment(Vec3 p, Vec3 a, Vec3 b)
{
	const Vec3 ab = b - a;
	const float denom = skm::Dot(ab, ab);
	const float t = denom < 1e-12F ? 0.0F : std::clamp(skm::Dot(p - a, ab) / denom, 0.0F, 1.0F);
	return a + ab * t;
}

/** World frame the chain root hangs from: its parent node, or the parentBlend virtual parent. */
Mat4 ParentWorld(const GltfModel &model, const SpringChain &chain, const std::vector<Mat4> &globals, const Mat4 &toWorld)
{
	if (chain.parentBlend.empty())
		return toWorld * globals[chain.parentNode];
	// Blend of the root rest frame carried by each entry: translation linear, rotation nlerp sign-aligned to the first.
	Quat reference;
	Quat sum { 0.0F, 0.0F, 0.0F, 0.0F };
	Vec3 translation;
	float weights = 0.0F;
	for (size_t i = 0; i < chain.parentBlend.size(); ++i) {
		const SpringParentBlend &entry = chain.parentBlend[i];
		const Mat4 carried = toWorld * globals[entry.node] * skm::ComposeTrs(entry.translation, entry.rotation, { 1.0F, 1.0F, 1.0F });
		Quat q = skm::QuatFromRotation(carried);
		if (i == 0)
			reference = q;
		else if (q.x * reference.x + q.y * reference.y + q.z * reference.z + q.w * reference.w < 0.0F)
			q = { -q.x, -q.y, -q.z, -q.w };
		sum = { sum.x + entry.weight * q.x, sum.y + entry.weight * q.y, sum.z + entry.weight * q.z, sum.w + entry.weight * q.w };
		translation = translation + Origin(carried) * entry.weight;
		weights += entry.weight;
	}
	const Mat4 blended = skm::ComposeTrs(translation * (1.0F / weights), skm::Normalize(sum), { 1.0F, 1.0F, 1.0F });
	Mat4 inverseRoot;
	skm::Inverse(RestLocal(model.nodes[chain.joints[0].node]), inverseRoot);
	return blended * inverseRoot;
}

/**
 * Walks every chain root to tip against the body pose. For joint `index` (flattened, tips excluded), `aim(index, joint,
 * head, restDir, length, capsules)` returns the tail the joint points at; the joint's world matrix (rest rotation
 * turned onto the tail, origin at the head) is the next joint's parent and goes to `jointWorld` when given.
 */
template <typename Aim>
void WalkChains(const GltfModel &model, const std::vector<Mat4> &globals, float worldYaw, Aim aim, std::vector<Mat4> *jointWorld = nullptr)
{
	const Mat4 toWorld = skm::RotationY(worldYaw);
	std::vector<WorldCapsule> capsules;
	size_t index = 0;
	for (const SpringChain &chain : model.springs.chains) {
		capsules.clear();
		for (const int c : chain.capsules) {
			const SpringCapsule &capsule = model.springs.capsules[c];
			const Mat4 world = toWorld * globals[capsule.node];
			capsules.push_back({ skm::TransformPoint(world, capsule.offset), skm::TransformPoint(world, capsule.tail), capsule.radius, capsule.part });
		}
		Mat4 parent = ParentWorld(model, chain, globals, toWorld);
		for (size_t k = 0; k + 1 < chain.joints.size(); ++k, ++index) {
			const SpringJoint &joint = chain.joints[k];
			const Vec3 tailLocal = model.nodes[chain.joints[k + 1].node].translation;
			Mat4 rest = parent * RestLocal(model.nodes[joint.node]);
			const Vec3 head = Origin(rest);
			const Vec3 restDir = skm::Normalize(skm::TransformDirection(rest, tailLocal));
			const Vec3 tail = aim(index, joint, head, restDir, std::sqrt(skm::Dot(tailLocal, tailLocal)), std::span<const WorldCapsule>(capsules));
			rest(0, 3) = rest(1, 3) = rest(2, 3) = 0.0F;
			Mat4 world = FromTo(restDir, tail - head) * rest;
			world(0, 3) = head.x;
			world(1, 3) = head.y;
			world(2, 3) = head.z;
			if (jointWorld != nullptr)
				jointWorld->push_back(world);
			parent = world;
		}
	}
}

size_t JointCount(const GltfModel &model)
{
	size_t count = 0;
	for (const SpringChain &chain : model.springs.chains)
		count += chain.joints.size() - 1;
	return count;
}

} // namespace

void ResetSprings(const GltfModel &model, const std::vector<Mat4> &globals, SpringState &state, float worldYaw)
{
	state.tails.clear();
	WalkChains(model, globals, worldYaw, [&](size_t, const SpringJoint &, Vec3 head, Vec3 restDir, float length, std::span<const WorldCapsule>) {
		state.tails.push_back(head + restDir * length);
		return state.tails.back();
	});
	state.prevTails = state.tails;
	state.accumulator = 0.0F;
	state.valid = true;
}

void StepSprings(const GltfModel &model, const std::vector<Mat4> &globals, SpringState &state, float worldYaw, PartMask visibleParts)
{
	constexpr float dt = SpringStepSeconds;
	WalkChains(model, globals, worldYaw, [&](size_t index, const SpringJoint &joint, Vec3 head, Vec3 restDir, float length, std::span<const WorldCapsule> capsules) {
		const Vec3 tail = state.tails[index];
		Vec3 next = tail + (tail - state.prevTails[index]) * (1.0F - joint.dragForce) + restDir * (dt * joint.stiffness)
		    + joint.gravityDir * (dt * joint.gravityPower);
		next = head + skm::Normalize(next - head) * length;
		// Back onto the cone of maxAngle around the rest direction, before the colliders so they still win.
		if (const Vec3 dir = (next - head) * (1.0F / length); skm::Dot(dir, restDir) < joint.cosMaxAngle) {
			const Vec3 side = dir - restDir * skm::Dot(dir, restDir);
			const float sideLength = std::sqrt(skm::Dot(side, side));
			if (sideLength > 1e-9F) {
				const float sinMaxAngle = std::sqrt(std::max(0.0F, 1.0F - joint.cosMaxAngle * joint.cosMaxAngle));
				next = head + (restDir * joint.cosMaxAngle + side * (sinMaxAngle / sideLength)) * length;
			}
		}
		for (const WorldCapsule &capsule : capsules) {
			if (capsule.part >= 0 && (visibleParts & (PartMask { 1 } << capsule.part)) == PartMask {})
				continue;
			const Vec3 closest = ClosestOnSegment(next, capsule.a, capsule.b);
			const Vec3 away = next - closest;
			const float distance = std::sqrt(skm::Dot(away, away));
			const float minDistance = capsule.radius + joint.hitRadius;
			if (distance >= minDistance)
				continue;
			const Vec3 normal = distance > 1e-9F ? away * (1.0F / distance) : skm::Normalize(next - head);
			next = closest + normal * minDistance;
			next = head + skm::Normalize(next - head) * length; // VRM re-applies the length after the push
		}
		// Onto the floor at bone length from the head, same heading (the world frame keeps model space height). A head
		// within reach of the floor always gets there; a straight drop keeps x, and a head below it leaves the tail.
		// ponytail: the tail itself, not tail + hitRadius: the hem's hit radii (~0.1 m) would lift it off the ground in
		// every clip; add a separate floor margin if cloth visibly sinks into the ground.
		if (const float floor = model.springs.floor, drop = floor - head.y; next.y < floor && std::abs(drop) < length) {
			const Vec3 heading { next.x - head.x, 0.0F, next.z - head.z };
			const float spread = std::sqrt(skm::Dot(heading, heading));
			const Vec3 out = spread > 1e-9F ? heading * (1.0F / spread) : Vec3 { 1.0F, 0.0F, 0.0F };
			next = head + out * std::sqrt(length * length - drop * drop) + Vec3 { 0.0F, drop, 0.0F };
		}
		state.prevTails[index] = tail;
		state.tails[index] = next;
		return next;
	});
}

void PoseSprings(const GltfModel &model, const std::vector<NodePose> &pose, std::vector<Mat4> &globals, const SpringState &state, float worldYaw)
{
	if (state.tails.size() != JointCount(model))
		return;
	std::vector<Mat4> jointWorld;
	jointWorld.reserve(state.tails.size());
	WalkChains(model, globals, worldYaw, [&](size_t index, const SpringJoint &, Vec3, Vec3, float, std::span<const WorldCapsule>) { return state.tails[index]; }, &jointWorld);

	const Mat4 toModel = skm::RotationY(-worldYaw);
	std::vector<uint8_t> moved(model.nodes.size(), 0);
	size_t index = 0;
	for (const SpringChain &chain : model.springs.chains) {
		for (size_t k = 0; k + 1 < chain.joints.size(); ++k, ++index) {
			globals[chain.joints[k].node] = toModel * jointWorld[index];
			moved[chain.joints[k].node] = 1;
		}
	}
	for (const int node : model.nodeOrder) {
		const int parent = model.nodes[node].parent;
		if (moved[node] != 0 || parent < 0 || moved[parent] == 0)
			continue;
		globals[node] = globals[parent] * skm::ComposeTrs(pose[node].translation, pose[node].rotation, pose[node].scale);
		moved[node] = 1;
	}
}

void UpdateSprings(const GltfModel &model, const std::vector<NodePose> &pose, std::vector<Mat4> &globals, SpringState &state, float dt, float worldYaw, PartMask visibleParts)
{
	if (model.springs.chains.empty())
		return;
	if (!state.valid || dt > SpringMaxFrameSeconds || state.tails.size() != JointCount(model)) {
		ResetSprings(model, globals, state, worldYaw);
		for (int i = 0; i < SpringRunInSteps; ++i)
			StepSprings(model, globals, state, worldYaw, visibleParts);
	} else {
		state.accumulator += std::max(dt, 0.0F);
		for (; state.accumulator >= SpringStepSeconds; state.accumulator -= SpringStepSeconds)
			StepSprings(model, globals, state, worldYaw, visibleParts);
	}
	PoseSprings(model, pose, globals, state, worldYaw);
}

} // namespace devilution::render3d
