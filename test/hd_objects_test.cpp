#include "engine/render/hd_objects_core.hpp"

#include <gtest/gtest.h>

namespace devilution::hd_sprites {
namespace {

constexpr char SampleIndex[] = R"(# DamnationX HD objects
static chest1 1 object/chest1/closed_a
static chest1 3 object/chest1/open_a
trans chest1 1 3 object/chest1/opening_a
anim l1braz 1 26 object/l1braz/loop
static barrel 1 object/barrel/intact
anim barrel 1 9 object/barrel/break
static barrel 9 object/barrel/broken
anim sarc 1 5 object/sarc/opening
clip object/chest1/closed_a objects/chest1_closed_a.png 1 1 171 164 95 163 0
clip object/chest1/open_a objects/chest1_open_a.png 1 1 171 200 95 199 0
clip object/chest1/opening_a objects/chest1_opening_a.png 18 6 171 200 95 199 0
clip object/l1braz/loop objects/l1braz_loop.png 78 13 90 465 43 464 1
clip object/barrel/intact objects/barrel_intact.png 1 1 100 150 50 149 0
clip object/barrel/break objects/barrel_break.png 27 9 140 160 70 159 0
clip object/barrel/broken objects/barrel_broken.png 1 1 140 160 70 159 0
clip object/sarc/opening objects/sarc_opening.png 45 9 300 200 150 199 0
)";

TEST(HdObjectsIndex, ParsesBindingsAndClips)
{
	const ObjectIndex index = ParseObjectIndex(SampleIndex);
	EXPECT_TRUE(index.errors.empty());
	ASSERT_EQ(index.clips.size(), 8U);
	ASSERT_EQ(index.bindings.size(), 8U);
	const std::vector<int> *chest = index.find("chest1");
	ASSERT_NE(chest, nullptr);
	ASSERT_EQ(chest->size(), 3U);
	const ObjectBinding &opening = index.bindings[(*chest)[2]];
	EXPECT_EQ(opening.kind, ObjectBinding::Kind::Transition);
	EXPECT_EQ(opening.first, 1);
	EXPECT_EQ(opening.last, 3);
	EXPECT_EQ(index.clips[opening.clip].key, "object/chest1/opening_a");
	EXPECT_EQ(index.clips[opening.clip].frames, 18);
	EXPECT_EQ(index.find("nothing"), nullptr);
}

TEST(HdObjectsIndex, RejectsMalformedLines)
{
	const ObjectIndex index = ParseObjectIndex(
	    "clip object/a/x a.png 1 1 10 10 5 9 0\n"
	    "static a 1 object/a/x\n"
	    "static a 0 object/a/x\n"         // frames are 1-based
	    "static a 2\n"                    // missing key
	    "anim a 5 2 object/a/x\n"         // empty range
	    "trans a 2 2 object/a/x\n"        // no change
	    "anim a 1 2 object/a/missing\n"   // unknown clip
	    "clip object/a/x b.png 1 1 1 1 0 0 0\n" // duplicate clip
	    "frobnicate a\n");
	EXPECT_EQ(index.bindings.size(), 1U);
	EXPECT_EQ(index.clips.size(), 1U);
	EXPECT_EQ(index.errors.size(), 7U);
	ASSERT_FALSE(index.errors.empty());
	EXPECT_EQ(index.errors[0].rfind("line 3:", 0), 0U) << index.errors[0];
}

TEST(HdObjectsSelect, StaticFramesAndMissingStates)
{
	const ObjectIndex index = ParseObjectIndex(SampleIndex);
	ObjectSelection closed = SelectObjectFrame(index, "chest1", { .frame = 1 });
	ASSERT_GE(closed.binding, 0);
	EXPECT_EQ(index.clips[index.bindings[closed.binding].clip].key, "object/chest1/closed_a");
	EXPECT_EQ(closed.frame, 0);

	const ObjectSelection open = SelectObjectFrame(index, "chest1", { .frame = 3 });
	ASSERT_GE(open.binding, 0);
	EXPECT_EQ(index.clips[index.bindings[open.binding].clip].key, "object/chest1/open_a");

	// Frame 2 ("full", never used by vanilla) and unknown objects stay classic; transitions are never picked at rest.
	EXPECT_EQ(SelectObjectFrame(index, "chest1", { .frame = 2 }).binding, -1);
	EXPECT_EQ(SelectObjectFrame(index, "lever", { .frame = 1 }).binding, -1);
}

TEST(HdObjectsSelect, LoopKeepsPhaseAndAdvancesWithTicks)
{
	const ObjectIndex index = ParseObjectIndex(SampleIndex);
	// l1braz: 26 CEL frames x 1 tick = 78 HD frames, 3 per CEL frame.
	EXPECT_EQ(SelectObjectFrame(index, "l1braz", { .frame = 1, .animating = true }).frame, 0);
	EXPECT_EQ(SelectObjectFrame(index, "l1braz", { .frame = 14, .animating = true }).frame, 39);
	EXPECT_EQ(SelectObjectFrame(index, "l1braz", { .frame = 14, .animating = true, .subtick = 0.5F }).frame, 40);
	EXPECT_EQ(SelectObjectFrame(index, "l1braz", { .frame = 26, .animating = true, .subtick = 0.99F }).frame, 77);
	// Frozen (flame trap lever, _oAnimFlag cleared): no sub-tick motion.
	EXPECT_EQ(SelectObjectFrame(index, "l1braz", { .frame = 14, .animating = false, .subtick = 0.9F }).frame, 39);
}

TEST(HdObjectsSelect, OneShotPlaysWhileAnimatingAndHoldsLastFrame)
{
	const ObjectIndex index = ParseObjectIndex(SampleIndex);
	const auto key = [&](const ObjectSelection &selection) { return index.clips[index.bindings[selection.binding].clip].key; };

	// Intact barrel at rest: its static frame. Breaking (animating from frame 1): the break clip.
	EXPECT_EQ(key(SelectObjectFrame(index, "barrel", { .frame = 1 })), "object/barrel/intact");
	const ObjectSelection start = SelectObjectFrame(index, "barrel", { .frame = 1, .animating = true });
	EXPECT_EQ(key(start), "object/barrel/break");
	EXPECT_EQ(start.frame, 0);
	// 9 CEL frames: the 27 HD frames are spread over the 8 steps to the last CEL frame.
	const ObjectSelection middle = SelectObjectFrame(index, "barrel", { .frame = 5, .animating = true, .subtick = 0.5F });
	EXPECT_EQ(middle.frame, 15); // floor(27 * 4.5 / 8)
	const ObjectSelection arrived = SelectObjectFrame(index, "barrel", { .frame = 9, .animating = true });
	EXPECT_EQ(key(arrived), "object/barrel/break");
	EXPECT_EQ(arrived.frame, 26);
	// Parked on the last frame (delay 1000): the broken static.
	EXPECT_EQ(key(SelectObjectFrame(index, "barrel", { .frame = 9 })), "object/barrel/broken");

	// Sarcophagus without static states: frame 1 at rest is the clip's first frame, the parked last frame its last.
	EXPECT_EQ(SelectObjectFrame(index, "sarc", { .frame = 1 }).frame, 0);
	EXPECT_EQ(SelectObjectFrame(index, "sarc", { .frame = 3, .animating = true, .tick = 1, .ticksPerFrame = 3 }).frame, 26); // floor(45 * 2.333 / 4)
	EXPECT_EQ(SelectObjectFrame(index, "sarc", { .frame = 5 }).frame, 44);
}

TEST(HdObjectsTransition, FindAndTiming)
{
	const ObjectIndex index = ParseObjectIndex(SampleIndex);
	const int opening = FindObjectTransition(index, "chest1", 1, 3);
	ASSERT_GE(opening, 0);
	EXPECT_EQ(index.clips[index.bindings[opening].clip].key, "object/chest1/opening_a");
	EXPECT_EQ(FindObjectTransition(index, "chest1", 3, 1), -1);
	EXPECT_EQ(FindObjectTransition(index, "barrel", 1, 9), -1);

	EXPECT_EQ(TransitionFrame(18, 0.0F), 0);
	EXPECT_EQ(TransitionFrame(18, 1.0F), 3);
	EXPECT_EQ(TransitionFrame(18, 5.99F), 17);
	EXPECT_EQ(TransitionFrame(18, 6.0F), -1);
	EXPECT_EQ(TransitionFrame(18, -1.0F), -1);
}

TEST(HdObjectsSelect, OneShotFrameEdgeCases)
{
	EXPECT_EQ(SelectOneShotFrame(0, 3.0F, 5), 0);
	EXPECT_EQ(SelectOneShotFrame(10, -2.0F, 5), 0);
	EXPECT_EQ(SelectOneShotFrame(10, 100.0F, 5), 9);
	// A single CEL frame: progress through it.
	EXPECT_EQ(SelectOneShotFrame(15, 0.5F, 1), 7);
}

} // namespace
} // namespace devilution::hd_sprites
