#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "render3d/gltf_model.hpp"

namespace devilution::render3d {
namespace {

using skm::Mat4;
using skm::Quat;
using skm::Vec3;

bool HaveWarrior()
{
	return std::ifstream(WARRIOR_GLB_PATH, std::ios::binary).good();
}

#define REQUIRE_WARRIOR() \
	if (!HaveWarrior()) \
	GTEST_SKIP() << "warrior.glb is not in this checkout (it ships in the HD art pack)"

const GltfModel &Warrior()
{
	static GltfModel model;
	static bool loaded = false;
	if (!loaded) {
		std::ifstream file(WARRIOR_GLB_PATH, std::ios::binary);
		const std::vector<uint8_t> bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		std::string error;
		EXPECT_TRUE(LoadGltfModel(bytes, model, error)) << error;
		loaded = true;
	}
	return model;
}

Vec3 Origin(const Mat4 &m) { return { m(0, 3), m(1, 3), m(2, 3) }; }

TEST(GltfModel, SlerpEndpointsAndMidpoint)
{
	const Quat a {};
	const Quat b = skm::AxisAngle({ 0.0F, 1.0F, 0.0F }, 1.0F);
	const Quat mid = skm::Slerp(a, b, 0.5F);
	const Quat expected = skm::AxisAngle({ 0.0F, 1.0F, 0.0F }, 0.5F);
	EXPECT_NEAR(mid.y, expected.y, 1e-5F);
	EXPECT_NEAR(mid.w, expected.w, 1e-5F);
	// Shorter arc: -b is the same rotation.
	const Quat flipped = skm::Slerp(a, Quat { -b.x, -b.y, -b.z, -b.w }, 0.5F);
	EXPECT_NEAR(std::abs(flipped.y), expected.y, 1e-5F);
}

TEST(GltfModel, InverseUndoesTrsAndPerspective)
{
	const Mat4 trs = skm::ComposeTrs({ 1.0F, 2.0F, 3.0F }, skm::AxisAngle({ 0.3F, 1.0F, 0.2F }, 0.7F), { 0.36F, 0.36F, 0.36F });
	const Mat4 projection = skm::Perspective(1.2F, 16.0F / 9.0F, 0.05F, 100.0F) * trs;
	for (const Mat4 &m : { trs, projection }) {
		Mat4 inverse;
		ASSERT_TRUE(skm::Inverse(m, inverse));
		const Mat4 identity = m * inverse;
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				EXPECT_NEAR(identity(r, c), r == c ? 1.0F : 0.0F, 1e-4F);
	}
}

TEST(GltfModel, LoadsTheWarrior)
{
	REQUIRE_WARRIOR();
	const GltfModel &model = Warrior();
	EXPECT_EQ(model.joints.size(), 33U);
	EXPECT_EQ(model.clips.size(), 9U);
	EXPECT_EQ(model.indices.size() / 3, 25740U);
	EXPECT_EQ(model.partNames.front(), "Warrior_Textured_Rigged");
	EXPECT_GE(model.findPart("Sword_RIGHT"), 1);
	EXPECT_GE(model.findPart("Town_Scabbard_Frog"), 1);
	EXPECT_FALSE(model.imageBytes.empty());
	const int attack = model.findClip("attack");
	ASSERT_GE(attack, 0);
	EXPECT_NEAR(model.clips[attack].duration, 0.75F, 1e-4F);
	EXPECT_LE(model.paletteSize(), 64U);
}

TEST(GltfModel, EquipmentHasBakedPbrTextures)
{
	REQUIRE_WARRIOR();
	// The body samples its own base colour; the equipment shares one baked atlas set (base colour, normal,
	// metallic-roughness), distinct from the body's image.
	const GltfModel &model = Warrior();
	ASSERT_GE(model.bodyImage, 0);
	EXPECT_EQ(model.images[static_cast<size_t>(model.bodyImage)].bytes, model.imageBytes);
	int equipment = 0;
	for (const GltfMaterial &material : model.materials) {
		if (material.normalImage < 0)
			continue;
		++equipment;
		EXPECT_TRUE(material.textured) << material.name;
		EXPECT_NE(material.baseColorImage, model.bodyImage) << material.name;
		EXPECT_GE(material.metallicRoughnessImage, 0) << material.name;
		EXPECT_NE(material.normalImage, material.baseColorImage) << material.name;
	}
	EXPECT_GE(equipment, 6);
	bool grip = false;
	for (const GltfMaterial &material : model.materials)
		grip = grip || material.name.find("Grip") != std::string::npos;
	EXPECT_TRUE(grip) << "the viewmodel finds the sword grip by its material name";
}

TEST(GltfModel, RestPaletteIsIdentityForSkinJoints)
{
	REQUIRE_WARRIOR();
	// In the bind pose every joint matrix is global * inverse bind = identity (glTF skinning contract).
	const GltfModel &model = Warrior();
	std::vector<NodePose> pose;
	std::vector<Mat4> globals;
	std::vector<Mat4> palette;
	SampleClip(model, -1, 0.0F, false, pose);
	ComputeGlobalMatrices(model, pose, globals);
	ComputePalette(model, globals, palette);
	ASSERT_EQ(palette.size(), model.paletteSize());
	// The exported rest pose of the nodes equals the bind pose for the hips at least.
	const int hips = model.findNode("mixamorig:Hips");
	ASSERT_GE(hips, 0);
	for (size_t j = 0; j < model.joints.size(); ++j) {
		if (model.joints[j] != hips)
			continue;
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				EXPECT_NEAR(palette[j](r, c), r == c ? 1.0F : 0.0F, 2e-3F);
	}
}

TEST(GltfModel, LoopingClipWrapsModuloDuration)
{
	REQUIRE_WARRIOR();
	const GltfModel &model = Warrior();
	const int walk = model.findClip("walk");
	ASSERT_GE(walk, 0);
	const float duration = model.clips[walk].duration;
	const int hips = model.findNode("mixamorig:Hips");
	std::vector<NodePose> a;
	std::vector<NodePose> b;
	SampleClip(model, walk, 0.3F, true, a);
	SampleClip(model, walk, 0.3F + duration, true, b);
	EXPECT_NEAR(a[hips].translation.y, b[hips].translation.y, 1e-5F);
	EXPECT_NEAR(a[hips].rotation.w, b[hips].rotation.w, 1e-5F);
	// The duplicate end key: the end of the loop is the start.
	SampleClip(model, walk, 0.0F, true, a);
	SampleClip(model, walk, duration - 1e-4F, true, b);
	EXPECT_NEAR(a[hips].translation.y, b[hips].translation.y, 2e-3F);
}

TEST(GltfModel, NonLoopingClipClamps)
{
	REQUIRE_WARRIOR();
	const GltfModel &model = Warrior();
	const int death = model.findClip("death");
	ASSERT_GE(death, 0);
	const int hips = model.findNode("mixamorig:Hips");
	std::vector<NodePose> end;
	std::vector<NodePose> past;
	SampleClip(model, death, model.clips[death].duration, false, end);
	SampleClip(model, death, model.clips[death].duration + 5.0F, false, past);
	EXPECT_NEAR(end[hips].translation.x, past[hips].translation.x, 1e-6F);
	EXPECT_NEAR(end[hips].translation.z, past[hips].translation.z, 1e-6F);
	// The fall lowers the hips.
	std::vector<NodePose> start;
	SampleClip(model, death, 0.0F, false, start);
	EXPECT_LT(end[hips].translation.y, start[hips].translation.y - 0.3F);
}

TEST(GltfModel, SwordFollowsTheRightHand)
{
	REQUIRE_WARRIOR();
	const GltfModel &model = Warrior();
	const int attack = model.findClip("attack");
	const int hand = model.findNode("mixamorig:RightHand");
	const int sword = model.findNode("Sword_RIGHT");
	ASSERT_GE(hand, 0);
	ASSERT_GE(sword, 0);
	std::vector<NodePose> pose;
	std::vector<Mat4> globals;
	for (const float time : { 0.0F, 0.4F, 0.7F }) {
		SampleClip(model, attack, time, false, pose);
		ComputeGlobalMatrices(model, pose, globals);
		const Vec3 handPos = Origin(globals[hand]);
		const Vec3 swordPos = Origin(globals[sword]);
		const Vec3 delta = swordPos - handPos;
		// The sword's local offset from the hand is rigid: its length never changes.
		const Vec3 local = model.nodes[sword].translation;
		EXPECT_NEAR(std::sqrt(skm::Dot(delta, delta)), std::sqrt(skm::Dot(local, local)), 1e-3F) << time;
	}
}

// Nodes 0 and 4 are body nodes; 1 -> 2 -> 3 is a chain of two joints and a tip.
std::vector<GltfNode> SpringNodes()
{
	std::vector<GltfNode> nodes(5);
	for (int i = 1; i <= 3; ++i) {
		nodes[i].parent = i - 1;
		nodes[i].translation = { 0.0F, -0.1F, 0.0F };
	}
	return nodes;
}

const std::string SpringExtras = R"({"note": "a \"quoted\" é name", "flag": true, "off": false, "nothing": null,
 "DAMNATIONX_springBone": {
  "colliders": [{"node": 0, "shape": {"capsule": {"offset": [0, -1.5e-1, 0.03], "tail": [0.2, -0.15, 3E-2], "radius": 0.05}}}],
  "colliderGroups": [{"name": "pelvis", "colliders": [0]}],
  "springs": [{"parentNode": 0, "colliderGroups": [0, 0],
   "joints": [{"node": 1, "hitRadius": 0.02, "stiffness": 2.5, "gravityPower": 0.1, "gravityDir": [0, -2, 0], "dragForce": 0.4},
              {"node": 2, "hitRadius": 0.03, "stiffness": 1, "gravityPower": 0, "dragForce": 0.5}, {"node": 3}],
   "parentBlend": [{"node": 0, "weight": 0.25, "restOffset": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1]}},
                   {"node": 4, "weight": 0.75, "restOffset": {"translation": [0.1, 0, 0], "rotation": [0, 0, 0, 2]}}]}]}})";

TEST(GltfModel, ReadsSpringBones)
{
	SpringBones springs;
	std::string error;
	ASSERT_TRUE(ParseSpringBones(SpringExtras, SpringNodes(), springs, error)) << error;
	ASSERT_EQ(springs.capsules.size(), 1U);
	EXPECT_FLOAT_EQ(springs.capsules[0].offset.y, -0.15F);
	EXPECT_FLOAT_EQ(springs.capsules[0].tail.z, 0.03F);
	EXPECT_FLOAT_EQ(springs.capsules[0].radius, 0.05F);
	ASSERT_EQ(springs.chains.size(), 1U);
	const SpringChain &chain = springs.chains[0];
	EXPECT_EQ(chain.parentNode, 0);
	EXPECT_EQ(chain.capsules, (std::vector<int> { 0, 0 }));
	ASSERT_EQ(chain.joints.size(), 3U);
	EXPECT_EQ(chain.joints[2].node, 3);
	EXPECT_FLOAT_EQ(chain.joints[0].stiffness, 2.5F);
	EXPECT_FLOAT_EQ(chain.joints[0].gravityDir.y, -1.0F); // normalised
	EXPECT_FLOAT_EQ(chain.joints[1].hitRadius, 0.03F);
	EXPECT_FLOAT_EQ(chain.joints[1].gravityDir.y, -1.0F); // default
	ASSERT_EQ(chain.parentBlend.size(), 2U);
	EXPECT_EQ(chain.parentBlend[0].node, 4); // highest weight first
	EXPECT_FLOAT_EQ(chain.parentBlend[0].translation.x, 0.1F);
	EXPECT_FLOAT_EQ(chain.parentBlend[0].rotation.w, 1.0F); // normalised

	ASSERT_TRUE(ParseSpringBones(R"({"other": [1, 2]})", SpringNodes(), springs, error));
	EXPECT_TRUE(springs.chains.empty());
}

TEST(GltfModel, RejectsBadSpringBones)
{
	const auto replaced = [](std::string_view from, std::string_view to) {
		std::string text = SpringExtras;
		const size_t at = text.find(from);
		EXPECT_NE(at, std::string::npos) << from;
		return text.replace(at, from.size(), to);
	};
	const std::string cases[] {
		"{\"DAMNATIONX_springBone\": ",
		replaced("{\"node\": 2, \"hitRadius\"", "{\"node\": 3, \"hitRadius\""), // skips a level of the chain
		replaced("\"weight\": 0.75", "\"weight\": 0.5"),
		replaced("\"colliderGroups\": [0, 0]", "\"colliderGroups\": [0, 1]"),
		replaced("\"radius\": 0.05", "\"radius\": -1"),
		replaced("\"stiffness\": 2.5, ", ""),
		replaced("{\"node\": 3}", "{\"node\": 5}"),
		replaced("\"node\": 1,", "\"node\": 1.5,"),
	};
	for (const std::string &text : cases) {
		SpringBones springs;
		std::string error;
		EXPECT_FALSE(ParseSpringBones(text, SpringNodes(), springs, error)) << text;
		EXPECT_FALSE(error.empty());
		EXPECT_TRUE(springs.chains.empty() && springs.capsules.empty());
	}
}

} // namespace
} // namespace devilution::render3d
