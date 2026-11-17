#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "render3d/gltf_model.hpp"
#include "render3d/spring_bone.hpp"

namespace devilution::render3d {
namespace {

using skm::Mat4;
using skm::Quat;
using skm::Vec3;

// ChainExtras / BlendExtras, BlendRootT / BlendRootQ, {Chain,Blend}Tails and {Chain,Blend}Worlds from the reference.
#include "fixtures/spring_bone/expected.inc"

constexpr float Tolerance = 1e-4F; // float32 here against the float64 reference, after 120 steps
constexpr int WorldSamples = FixtureSteps / 10;

float At(Vec3 v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

/** `bodyNodes` free body nodes, then the root joint under node 0, two more joints and the tip, 0.1 m apart. */
GltfModel Toy(int bodyNodes, Vec3 rootT, Quat rootQ, const char *extras)
{
	GltfModel model;
	model.nodes.resize(static_cast<size_t>(bodyNodes));
	GltfNode &root = model.nodes.emplace_back();
	root.parent = 0;
	root.translation = rootT;
	root.rotation = rootQ;
	for (int k = 0; k < 3; ++k) {
		GltfNode &node = model.nodes.emplace_back();
		node.parent = static_cast<int>(model.nodes.size()) - 2;
		node.translation = { 0.0F, -0.1F, 0.0F };
	}
	for (size_t i = 0; i < model.nodes.size(); ++i)
		model.nodeOrder.push_back(static_cast<int>(i));
	std::string error;
	EXPECT_TRUE(ParseSpringBones(extras, model.nodes, model.springs, error)) << error;
	return model;
}

GltfModel ChainModel() { return Toy(1, {}, {}, ChainExtras); }
GltfModel BlendModel() { return Toy(2, { BlendRootT[0], BlendRootT[1], BlendRootT[2] }, { BlendRootQ[0], BlendRootQ[1], BlendRootQ[2], BlendRootQ[3] }, BlendExtras); }

Mat4 Rigid(Vec3 axis, float radians, Vec3 origin) { return skm::ComposeTrs(origin, skm::AxisAngle(axis, radians), { 1.0F, 1.0F, 1.0F }); }

/** The reference's body motion (gen.py parent_pose / node1_pose). */
void BodyPose(float t, std::vector<Mat4> &globals)
{
	globals[0] = Rigid({ 0.0F, 0.0F, 1.0F }, 0.8F * std::sin(1.7F * t), { 0.3F * std::sin(2.0F * t), 0.1F * std::cos(3.0F * t), 0.2F * std::sin(t) });
	const Mat4 rest1 = Rigid({ 0.3F, 0.2F, 1.0F }, 0.4F, {});
	globals[1] = Rigid({ 0.1F, 1.0F, 0.3F }, 0.9F * std::sin(2.3F * t), {}) * rest1;
	globals[1](0, 3) = 0.08F + 0.1F * std::sin(3.0F * t);
	globals[1](1, 3) = -0.05F + 0.05F * std::cos(t);
	globals[1](2, 3) = 0.02F;
}

void ExpectMatchesReference(const GltfModel &model, const float (&tails)[FixtureSteps][3][3], const float (&worlds)[WorldSamples][3][12])
{
	std::vector<NodePose> pose;
	SampleClip(model, -1, 0.0F, false, pose);
	std::vector<Mat4> globals(model.nodes.size());
	SpringState state;
	BodyPose(0.0F, globals);
	ResetSprings(model, globals, state);
	const SpringChain &chain = model.springs.chains[0];
	float worstTail = 0.0F;
	float worstWorld = 0.0F;
	for (int k = 0; k < FixtureSteps; ++k) {
		BodyPose(static_cast<float>(k) * SpringStepSeconds, globals);
		StepSprings(model, globals, state);
		for (int j = 0; j < 3; ++j) {
			for (int axis = 0; axis < 3; ++axis)
				worstTail = std::max(worstTail, std::abs(At(state.tails[j], axis) - tails[k][j][axis]));
		}
		if (k % 10 != 9)
			continue;
		PoseSprings(model, pose, globals, state);
		for (int j = 0; j < 3; ++j) {
			const Mat4 &world = globals[chain.joints[j].node];
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 4; ++column)
					worstWorld = std::max(worstWorld, std::abs(world(row, column) - worlds[k / 10][j][row * 4 + column]));
			}
		}
		// The tip (not a spring joint) follows the last joint, so it sits on that joint's tail.
		const Mat4 &tip = globals[chain.joints[3].node];
		for (int axis = 0; axis < 3; ++axis)
			EXPECT_NEAR(tip(axis, 3), At(state.tails[2], axis), 1e-5F);
	}
	EXPECT_LT(worstTail, Tolerance);
	EXPECT_LT(worstWorld, Tolerance);
}

TEST(SpringBone, ChainWithCapsuleMatchesReference)
{
	ExpectMatchesReference(ChainModel(), ChainTails, ChainWorlds);
}

TEST(SpringBone, ParentBlendMatchesReference)
{
	const GltfModel model = BlendModel();
	ASSERT_EQ(model.springs.chains[0].parentBlend.size(), 2U);
	EXPECT_EQ(model.springs.chains[0].parentBlend[0].node, 1); // highest weight first
	ExpectMatchesReference(model, BlendTails, BlendWorlds);
}

TEST(SpringBone, UpdateResetsOnLongFramesAndStepsAtSixtyHertz)
{
	const GltfModel model = ChainModel();
	std::vector<NodePose> pose;
	SampleClip(model, -1, 0.0F, false, pose);
	std::vector<Mat4> globals(model.nodes.size());
	BodyPose(0.3F, globals);

	SpringState updated;
	updated.valid = true; // stale state from another model: the size mismatch forces a reset
	UpdateSprings(model, pose, globals, updated, 1.0F / 30.0F, 0.0F);
	SpringState manual;
	ResetSprings(model, globals, manual);
	for (int i = 0; i < SpringRunInSteps; ++i)
		StepSprings(model, globals, manual);
	for (size_t j = 0; j < manual.tails.size(); ++j)
		EXPECT_NEAR(updated.tails[j].y, manual.tails[j].y, 1e-6F);

	BodyPose(0.35F, globals);
	UpdateSprings(model, pose, globals, updated, 2.5F * SpringStepSeconds, 0.0F);
	for (int i = 0; i < 2; ++i)
		StepSprings(model, globals, manual);
	for (size_t j = 0; j < manual.tails.size(); ++j)
		EXPECT_NEAR(updated.tails[j].x, manual.tails[j].x, 1e-6F);
	EXPECT_NEAR(updated.accumulator, 0.5F * SpringStepSeconds, 1e-5F);

	UpdateSprings(model, pose, globals, updated, 0.5F, 0.0F); // a pause: back to rest plus the run-in
	EXPECT_EQ(updated.accumulator, 0.0F);
}

TEST(SpringBone, ConstantYawGivesTheSameModelSpacePose)
{
	const GltfModel model = BlendModel();
	std::vector<NodePose> pose;
	SampleClip(model, -1, 0.0F, false, pose);
	std::vector<Mat4> plain(model.nodes.size());
	std::vector<Mat4> turned(model.nodes.size());
	SpringState plainState;
	SpringState turnedState;
	for (int k = 0; k < 60; ++k) {
		BodyPose(static_cast<float>(k) * SpringStepSeconds, plain);
		BodyPose(static_cast<float>(k) * SpringStepSeconds, turned);
		UpdateSprings(model, pose, plain, plainState, SpringStepSeconds, 0.0F);
		UpdateSprings(model, pose, turned, turnedState, SpringStepSeconds, 2.0F);
	}
	for (const SpringJoint &joint : model.springs.chains[0].joints) {
		for (size_t i = 0; i < 16; ++i)
			EXPECT_NEAR(turned[joint.node].m[i], plain[joint.node].m[i], 1e-4F);
	}
}

/** Tails after `steps` fixed steps of the reference body motion. */
std::vector<Vec3> RunTails(const GltfModel &model, int steps, PartMask visibleParts)
{
	std::vector<Mat4> globals(model.nodes.size());
	SpringState state;
	BodyPose(0.0F, globals);
	ResetSprings(model, globals, state);
	for (int k = 0; k < steps; ++k) {
		BodyPose(static_cast<float>(k) * SpringStepSeconds, globals);
		StepSprings(model, globals, state, 0.0F, visibleParts);
	}
	return state.tails;
}

TEST(SpringBone, ColliderOnAPartOnlyCollidesWhileItIsDrawn)
{
	GltfModel model = ChainModel();
	model.springs.capsules[0].part = 3; // as if its node were the mesh of a held staff
	GltfModel without = ChainModel();
	without.springs.chains[0].capsules.clear();
	const std::vector<Vec3> hidden = RunTails(model, FixtureSteps, PartMask { 1 });
	const std::vector<Vec3> bare = RunTails(without, FixtureSteps, PartMask { 1 });
	for (size_t j = 0; j < hidden.size(); ++j)
		EXPECT_EQ(hidden[j].y, bare[j].y);
	EXPECT_EQ(RunTails(model, FixtureSteps, PartMask { 1 } << 3)[2].y, RunTails(ChainModel(), FixtureSteps, ~PartMask {})[2].y);
}

/** Lowest tail over every step of the reference body motion. */
float LowestTail(const GltfModel &model)
{
	float lowest = 1.0F;
	for (int steps = 1; steps <= FixtureSteps; ++steps) {
		for (const Vec3 &tail : RunTails(model, steps, ~PartMask {}))
			lowest = std::min(lowest, tail.y);
	}
	return lowest;
}

TEST(SpringBone, TailsStayAboveTheFloor)
{
	GltfModel model = ChainModel();
	EXPECT_LT(LowestTail(model), -0.35F); // the bobbing root dips the hanging tip below the floor
	model.springs.floor = -0.34F;          // below every joint head, as the ground is below the hips
	EXPECT_GE(LowestTail(model), -0.34F - 1e-4F);
}

/** Widest angle (radians) the chain's root bone swings from its animated rest direction over the reference motion. */
float WidestRootSwing(const GltfModel &model)
{
	const SpringChain &chain = model.springs.chains[0];
	const GltfNode &root = model.nodes[chain.joints[0].node];
	const Vec3 tailLocal = model.nodes[chain.joints[1].node].translation;
	std::vector<Mat4> globals(model.nodes.size());
	SpringState state;
	BodyPose(0.0F, globals);
	ResetSprings(model, globals, state);
	float widest = 0.0F;
	for (int k = 0; k < FixtureSteps; ++k) {
		BodyPose(static_cast<float>(k) * SpringStepSeconds, globals);
		StepSprings(model, globals, state);
		const Mat4 rest = globals[chain.parentNode] * skm::ComposeTrs(root.translation, root.rotation, { 1.0F, 1.0F, 1.0F });
		const Vec3 head { rest(0, 3), rest(1, 3), rest(2, 3) };
		const Vec3 restDir = skm::Normalize(skm::TransformDirection(rest, tailLocal));
		widest = std::max(widest, std::acos(std::clamp(skm::Dot(skm::Normalize(state.tails[0] - head), restDir), -1.0F, 1.0F)));
	}
	return widest;
}

TEST(SpringBone, MaxAngleKeepsTheBoneInItsCone)
{
	GltfModel model = ChainModel();
	model.springs.chains[0].capsules.clear(); // a collider may push past the cone on purpose
	constexpr float Limit = 0.1F;
	EXPECT_GT(WidestRootSwing(model), 2.0F * Limit);
	for (SpringJoint &joint : model.springs.chains[0].joints)
		joint.cosMaxAngle = std::cos(Limit);
	EXPECT_LE(WidestRootSwing(model), Limit + 1e-3F);
}

// The real asset is not in Git (Sorcerer phase 39a round 2, on the art SSD): point DAMNATIONX_SORCERER_GLB at it.
TEST(SpringBone, SorcererClothStaysAttachedThroughTheWalk)
{
	const char *path = std::getenv("DAMNATIONX_SORCERER_GLB");
	if (path == nullptr)
		GTEST_SKIP() << "set DAMNATIONX_SORCERER_GLB to a Sorcerer GLB with spring bones";
	std::ifstream file(path, std::ios::binary);
	const std::vector<uint8_t> bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	GltfModel model;
	std::string error;
	ASSERT_TRUE(LoadGltfModel(bytes, model, error)) << error;
	ASSERT_TRUE(model.springError.empty()) << model.springError;
	EXPECT_EQ(model.springs.chains.size(), 20U);
	EXPECT_GE(model.springs.capsules.size(), 19U);
	int walk = model.findClip("walk");
	if (walk < 0)
		walk = model.findClip("walk_rt");
	ASSERT_GE(walk, 0);
	std::vector<NodePose> pose;
	std::vector<Mat4> globals;
	SpringState state;
	for (int frame = 0; frame < 120; ++frame) {
		SampleClip(model, walk, static_cast<float>(frame) / 60.0F, true, pose);
		ComputeGlobalMatrices(model, pose, globals);
		UpdateSprings(model, pose, globals, state, 1.0F / 60.0F, 0.3F * static_cast<float>(frame) / 60.0F);
		for (const SpringChain &chain : model.springs.chains) {
			for (size_t k = 0; k + 1 < chain.joints.size(); ++k) {
				const Mat4 &joint = globals[chain.joints[k].node];
				const Mat4 &child = globals[chain.joints[k + 1].node];
				const Vec3 bone { child(0, 3) - joint(0, 3), child(1, 3) - joint(1, 3), child(2, 3) - joint(2, 3) };
				const Vec3 rest = model.nodes[chain.joints[k + 1].node].translation;
				ASSERT_TRUE(std::isfinite(bone.x) && std::isfinite(bone.y) && std::isfinite(bone.z));
				EXPECT_NEAR(std::sqrt(skm::Dot(bone, bone)), std::sqrt(skm::Dot(rest, rest)), 1e-4F);
			}
		}
	}
}

} // namespace
} // namespace devilution::render3d
