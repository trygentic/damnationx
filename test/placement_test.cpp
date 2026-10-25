#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include "render3d/placement.hpp"

namespace devilution::render3d {
namespace {

constexpr char kTown[] = R"(
; Tristram, one building and a barrel rule
[Placement]
format = damnationx.placement
version = 1

[mesh.tavern]
file = hd/3d/town/tavern.glb
sha256 = 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef

[mesh.barrel]
file = hd/3d/props/barrel.glb
units = meters

[instance.tavern-main]
mesh = tavern
tile = 46,54
offset = 0,0,0
yaw = 90
scale = 2
hide = 46,54,53,63
hide = 53,56,56,61
occluder = 1.5
levels = 0

[rule.barrels]
mesh = barrel
object = OBJ_BARREL
hide = self
setlevel = 0

[rule.pillars]
mesh = barrel
piece = 7, 9
hide = self
)";

PlacementFile ParseOk(std::string_view text)
{
	PlacementParseResult result = ParsePlacementFile(text);
	EXPECT_TRUE(result.file.has_value()) << result.error;
	return result.file.value_or(PlacementFile {});
}

std::string ParseError(std::string_view text)
{
	const PlacementParseResult result = ParsePlacementFile(text);
	EXPECT_FALSE(result.file.has_value());
	return result.error;
}

TEST(PlacementTest, ParsesEveryKey)
{
	const PlacementFile file = ParseOk(kTown);
	ASSERT_EQ(file.meshes.size(), 2U);
	EXPECT_EQ(file.meshes[0].file, "hd/3d/town/tavern.glb");
	EXPECT_EQ(file.meshes[0].units, PlacementUnits::Tiles);
	EXPECT_EQ(file.meshes[0].sha256.size(), 64U);
	EXPECT_EQ(file.meshes[1].units, PlacementUnits::Meters);

	ASSERT_EQ(file.instances.size(), 1U);
	const PlacementInstance &tavern = file.instances[0];
	EXPECT_EQ(tavern.id, "tavern-main");
	EXPECT_EQ(tavern.mesh, 0);
	EXPECT_EQ(tavern.tileX, 46);
	EXPECT_EQ(tavern.tileY, 54);
	EXPECT_FLOAT_EQ(tavern.offset.x, 0.0F);
	EXPECT_FLOAT_EQ(tavern.yawDegrees, 90.0F);
	EXPECT_FLOAT_EQ(tavern.scale.x, 2.0F);
	EXPECT_FLOAT_EQ(tavern.scale.z, 2.0F);
	ASSERT_EQ(tavern.hide.size(), 2U);
	EXPECT_EQ(tavern.hide[1].x0, 53);
	EXPECT_EQ(tavern.hide[1].y1, 61);
	EXPECT_EQ(tavern.occluder, PlacementOccluder::Fixed);
	EXPECT_FLOAT_EQ(tavern.occluderTop, 1.5F);
	EXPECT_EQ(tavern.levelMin, 0);
	EXPECT_EQ(tavern.levelMax, 0);

	ASSERT_EQ(file.rules.size(), 2U);
	EXPECT_EQ(file.rules[0].objectNames, (std::vector<std::string> { "OBJ_BARREL" }));
	EXPECT_TRUE(file.rules[0].hideSelf);
	EXPECT_EQ(file.rules[0].setLevel, 0);
	EXPECT_EQ(file.rules[1].pieces, (std::vector<uint16_t> { 7, 9 }));
	// Defaults: tile centre, auto occluder, any level.
	EXPECT_FLOAT_EQ(file.rules[1].offset.x, 0.5F);
	EXPECT_FLOAT_EQ(file.rules[1].offset.z, 0.5F);
	EXPECT_EQ(file.rules[1].occluder, PlacementOccluder::Auto);
	EXPECT_EQ(file.rules[1].levelMin, -1);
}

TEST(PlacementTest, RejectsMistakes)
{
	const std::string header = "[Placement]\nformat = damnationx.placement\nversion = 1\n[mesh.a]\nfile = hd/3d/a.glb\n";
	EXPECT_NE(ParseError("[mesh.a]\nfile = a.glb\n").find("[Placement]"), std::string::npos);
	EXPECT_NE(ParseError("[Placement]\nformat = d3d.town-map\nversion = 1\n").find("format"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 1,2\ncolour = red\n").find("unknown key 'colour'"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = b\ntile = 1,2\n").find("unknown mesh 'b'"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 112,2\n").find("tile"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 1,2\ntile = 3,4\n").find("duplicate key"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 1,2\n[instance.x]\nmesh = a\ntile = 1,2\n").find("duplicate section"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 1,2\nhide = 5,5,4,4\n").find("hide"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 1,2\nscale = 0\n").find("scale"), std::string::npos);
	EXPECT_NE(ParseError(header + "[rule.r]\nmesh = a\n").find("exactly one of object, piece or arch"), std::string::npos);
	EXPECT_NE(ParseError(header + "[rule.r]\nmesh = a\nobject = OBJ_BARREL\npiece = 1\n").find("exactly one"), std::string::npos);
	EXPECT_NE(ParseError(header + "[rule.r]\nmesh = a\npiece = 1\nhide = 1,1,2,2\n").find("hide = self"), std::string::npos);
	EXPECT_NE(ParseError(header + "[rule.r]\nmesh = a\npiece = 1\nshow = closed\n").find("object rules only"), std::string::npos);
	EXPECT_NE(ParseError(header + "[rule.r]\nmesh = a\narch = some\n").find("arch must be any"), std::string::npos);
	EXPECT_NE(ParseError(header + "[instance.x]\nmesh = a\ntile = 1,2\nscale = 1,2\n").find("scale"), std::string::npos);
	EXPECT_NE(ParseError("[Placement]\nformat = damnationx.placement\nversion = 1\n[mesh.a]\nfile = ../a.glb\n").find("file"), std::string::npos);
	EXPECT_NE(ParseError("[Placement]\nformat = damnationx.placement\nversion = 1\n[mesh.a]\nfile = a.glb\nsha256 = ABC\n").find("sha256"), std::string::npos);
	EXPECT_NE(ParseError("[Placement]\nformat = damnationx.placement\nversion = 1\n[thing.a]\n").find("unknown section kind"), std::string::npos);
	// Line numbers point at the offending line.
	EXPECT_EQ(ParseError(header + "[instance.x]\nmesh = a\ntile = oops\n").rfind("line 8:", 0), 0U);
}

TEST(PlacementTest, ModelMatrixFollowsTheWorldContract)
{
	PlacementCommon common;
	common.yawDegrees = 90.0F;
	common.scale = { 2.0F, 2.0F, 2.0F };
	const skm::Mat4 model = PlacementModelMatrix(common, PlacementUnits::Tiles, 10, 20);
	// The origin lands on the tile centre.
	const skm::Vec3 origin = skm::TransformPoint(model, { 0.0F, 0.0F, 0.0F });
	EXPECT_NEAR(origin.x, 10.5F, 1e-5F);
	EXPECT_NEAR(origin.y, 0.0F, 1e-5F);
	EXPECT_NEAR(origin.z, 20.5F, 1e-5F);
	// Yaw 90: the mesh's +z (its front) turns to world +x, as RotationY / SkinnedPlayerModelMatrix do; scale 2.
	const skm::Vec3 front = skm::TransformPoint(model, { 0.0F, 0.0F, 1.0F });
	EXPECT_NEAR(front.x - origin.x, 2.0F, 1e-5F);
	EXPECT_NEAR(front.z - origin.z, 0.0F, 1e-5F);

	const skm::Mat4 meters = PlacementModelMatrix(PlacementCommon {}, PlacementUnits::Meters, 0, 0);
	EXPECT_NEAR(skm::TransformPoint(meters, { 0.0F, 1.0F, 0.0F }).y, kPlacementMetersToWorld, 1e-6F);
}

TEST(PlacementTest, ResolvesInstancesAndRules)
{
	PlacementFile file = ParseOk(kTown);
	file.rules[0].objectTypes = { 5 }; // the runtime maps OBJ_BARREL to its enumerator

	PlacementLevel town;
	town.currlevel = 0;
	town.piece = [](int x, int y) { return static_cast<uint16_t>(x == 3 && y == 4 ? 9 : 0); };
	town.objects = { { 17, 5, 30, 31 }, { 18, 6, 32, 33 } };
	const std::vector<ResolvedPlacement> placements = ResolvePlacements(file, town);
	ASSERT_EQ(placements.size(), 3U);
	EXPECT_FALSE(placements[0].fromRule);
	EXPECT_EQ(placements[0].tileX, 46);
	EXPECT_EQ(placements[1].objectIndex, 17);
	EXPECT_EQ(placements[1].tileX, 30);
	EXPECT_EQ(placements[2].tileX, 3);
	EXPECT_EQ(placements[2].tileY, 4);
	EXPECT_EQ(placements[2].objectIndex, -1);

	std::vector<PlacementRect> rects;
	AppendHiddenRects(file, placements[0], rects);
	EXPECT_EQ(rects.size(), 2U);
	rects.clear();
	AppendHiddenRects(file, placements[1], rects); // object rules hide the billboard, not tiles
	EXPECT_TRUE(rects.empty());
	AppendHiddenRects(file, placements[2], rects);
	ASSERT_EQ(rects.size(), 1U);
	EXPECT_TRUE(rects[0].contains(3, 4));
	EXPECT_FALSE(rects[0].contains(4, 4));

	// Level filters: the tavern only on level 0, the barrels only off set levels.
	PlacementLevel dungeon = town;
	dungeon.currlevel = 2;
	dungeon.setLevel = 3;
	const std::vector<ResolvedPlacement> deeper = ResolvePlacements(file, dungeon);
	ASSERT_EQ(deeper.size(), 1U);
	EXPECT_EQ(deeper[0].tileX, 3);
}

TEST(PlacementTest, EnvironmentOverrides)
{
	const std::string header = "[Placement]\nformat = damnationx.placement\nversion = 1\n";
	const PlacementFile none = ParseOk(header);
	EXPECT_FALSE(none.environment.ambient.has_value());
	EXPECT_FALSE(none.environment.moonDirection.has_value());

	const PlacementFile file = ParseOk(header + "[environment]\nambient = 0.5\nexposure = 1.2\nmoon_direction = 0,2,0\nmoon_strength = 0\n");
	EXPECT_FLOAT_EQ(file.environment.ambient.value_or(-1.0F), 0.5F);
	EXPECT_FLOAT_EQ(file.environment.exposure.value_or(-1.0F), 1.2F);
	EXPECT_FLOAT_EQ(file.environment.moonStrength.value_or(-1.0F), 0.0F);
	ASSERT_TRUE(file.environment.moonDirection.has_value());
	EXPECT_NEAR(file.environment.moonDirection->y, 1.0F, 1e-6F); // normalized

	EXPECT_NE(ParseError(header + "[environment]\nmoon_direction = 1,0,0\n").find("height > 0"), std::string::npos);
	EXPECT_NE(ParseError(header + "[environment]\nexposure = 0\n").find("exposure"), std::string::npos);
	EXPECT_NE(ParseError(header + "[environment]\nfog = 1\n").find("unknown key 'fog'"), std::string::npos);
}

TEST(PlacementTest, DungeonRuleKeys)
{
	const std::string text = R"([Placement]
format = damnationx.placement
version = 1
[mesh.chest]
file = hd/3d/chest.glb
[mesh.door]
file = hd/3d/door.glb
[mesh.arch]
file = hd/3d/arch.glb
[mesh.torch]
file = hd/3d/torch.glb
[rule.chests]
mesh = chest
object = OBJ_CHEST1, OBJ_TCHEST1
show = unopened
facing = open_side
hide = self
scale = 1, 0.5, 2
[rule.doors]
mesh = door
object = OBJ_L1LDOOR, OBJ_L1RDOOR
show = closed
facing = passage
hide = self
occluder = none
[rule.arches]
mesh = arch
arch = any
facing = passage
hide = self
[rule.torches]
mesh = torch
object = OBJ_TORCHL
)";
	PlacementFile file = ParseOk(text);
	ASSERT_EQ(file.rules.size(), 4U);
	EXPECT_EQ(file.rules[0].objectNames.size(), 2U);
	EXPECT_EQ(file.rules[0].show, PlacementShow::Unopened);
	EXPECT_EQ(file.rules[0].facing, PlacementFacing::OpenSide);
	EXPECT_FLOAT_EQ(file.rules[0].scale.y, 0.5F);
	EXPECT_EQ(file.rules[1].show, PlacementShow::Closed);
	EXPECT_TRUE(file.rules[2].arch);
	file.rules[0].objectTypes = { 1, 2 }; // chests
	file.rules[1].objectTypes = { 3, 4 }; // doors
	file.rules[3].objectTypes = { 5 };    // torch

	// A room: x 10..20, y 10..20 open, everything else solid. Arch at (15, 10) cut into a wall along y (SE).
	PlacementLevel level;
	level.solid = [](int x, int y) { return x < 10 || x > 20 || y < 10 || y > 20; };
	level.arch = [](int x, int y) -> uint8_t { return x == 15 && y == 10 ? 2 : 0; };
	level.objects = {
		{ 40, 1, 10, 15, false, false, false, false }, // chest against the west wall (-x solid): faces +x
		{ 41, 2, 12, 12, true, false, false, false },  // opened chest: not shown
		{ 42, 3, 18, 10, false, true, false, true },   // closed left door: passage along x
		{ 43, 4, 19, 10, false, true, true, false },   // open door: not shown
		{ 44, 5, 15, 9, false, false, false, false },  // torch in the north wall: pulled out to +y
	};
	const std::vector<ResolvedPlacement> placements = ResolvePlacements(file, level);
	ASSERT_EQ(placements.size(), 4U);

	const auto front = [](const ResolvedPlacement &p) {
		const skm::Vec3 o = skm::TransformPoint(p.model, { 0.0F, 0.0F, 0.0F });
		const skm::Vec3 f = skm::TransformPoint(p.model, { 0.0F, 0.0F, 1.0F });
		const skm::Vec3 d = f - o;
		const float length = std::sqrt(skm::Dot(d, d));
		return d * (1.0F / length);
	};
	EXPECT_EQ(placements[0].objectIndex, 40);
	EXPECT_NEAR(front(placements[0]).x, 1.0F, 1e-5F);
	// Non-uniform scale: +z is stretched 2x, y halved.
	EXPECT_NEAR(skm::TransformPoint(placements[0].model, { 0.0F, 1.0F, 0.0F }).y, 0.5F, 1e-5F);

	EXPECT_EQ(placements[1].objectIndex, 42);
	EXPECT_NEAR(front(placements[1]).x, 1.0F, 1e-5F);

	EXPECT_TRUE(placements[2].arch);
	EXPECT_EQ(placements[2].tileX, 15);
	EXPECT_NEAR(front(placements[2]).x, 1.0F, 1e-5F);

	EXPECT_EQ(placements[3].objectIndex, 44);
	const skm::Vec3 torch = skm::TransformPoint(placements[3].model, { 0.0F, 0.0F, 0.0F });
	EXPECT_NEAR(torch.x, 15.5F, 1e-5F);
	EXPECT_NEAR(torch.z, 9.5F + 0.56F, 1e-5F);
}

TEST(PlacementTest, TiersMergeById)
{
	const std::string header = "[Placement]\nformat = damnationx.placement\nversion = 1\n";
	PlacementFile free = ParseOk(header
	    + "[environment]\nambient = 0.5\nexposure = 1\n"
	      "[mesh.house]\nfile = hd/3d/free/house.glb\n[mesh.well]\nfile = hd/3d/free/well.glb\n"
	      "[instance.house]\nmesh = house\ntile = 1,1\n[instance.well]\nmesh = well\ntile = 5,5\n");
	const PlacementFile hd = ParseOk(header
	    + "[environment]\nambient = 0.7\n"
	      "[mesh.house]\nfile = hd/3d/hd/house.glb\n[mesh.cart]\nfile = hd/3d/hd/cart.glb\n"
	      "[instance.cart]\nmesh = cart\ntile = 9,9\n[instance.well]\nmesh = cart\ntile = 6,6\n");
	MergePlacementFiles(free, hd);
	ASSERT_EQ(free.meshes.size(), 3U);
	EXPECT_EQ(free.meshes[0].file, "hd/3d/hd/house.glb"); // replaced in place, the free instance now uses it
	ASSERT_EQ(free.instances.size(), 3U);
	EXPECT_EQ(free.meshes[static_cast<size_t>(free.instances[0].mesh)].file, "hd/3d/hd/house.glb");
	EXPECT_EQ(free.instances[1].tileX, 6); // [instance.well] overridden by the HD tier
	EXPECT_EQ(free.meshes[static_cast<size_t>(free.instances[1].mesh)].file, "hd/3d/hd/cart.glb");
	EXPECT_EQ(free.meshes[static_cast<size_t>(free.instances[2].mesh)].id, "cart");
	EXPECT_FLOAT_EQ(free.environment.ambient.value_or(0.0F), 0.7F);
	EXPECT_FLOAT_EQ(free.environment.exposure.value_or(0.0F), 1.0F);
}

TEST(PlacementTest, InstalledStockArchitectureUpgradesWithoutRescalingCustomMeshes)
{
	PlacementFile file = ParseOk(R"(
[Placement]
format = damnationx.placement
version = 1
[mesh.arch_stone]
file = hd/3d/free/models/arch_stone.glb
sha256 = 8a902f99745b65f5b99f8717c3a982eb2d5eb0a3c4f449f9fa24de5c0c0c0c30
[rule.arch]
mesh = arch_stone
arch = any
scale = 1.26,1,1.5
)");
	PlacementLevel level;
	level.arch = [](int x, int y) -> uint8_t { return x == 10 && y == 10 ? 1 : 0; };
	const auto opening = [&] {
		const auto placements = ResolvePlacements(file, level);
		EXPECT_EQ(placements.size(), 1U);
		return placements.front().model;
	};
	// Actual stock model intrados: x +/-0.2356 at its jamb, y0.8721 at apex.
	const auto upgraded = opening();
	EXPECT_GE(skm::TransformPoint(upgraded, { 0, 0.8721F, 0 }).y, 3.2F);
	EXPECT_GE(skm::TransformPoint(upgraded, { 0.2356F, 0, 0 }).x - skm::TransformPoint(upgraded, { -0.2356F, 0, 0 }).x, 1.6F);
	// A newly generated pack already has the dimensions; never scale it twice.
	file.rules[0].scale = { 3.4F, 3.67F, 1.5F };
	EXPECT_NEAR(skm::TransformPoint(opening(), { 0, 1, 0 }).y, 3.67F, 0.0001F);
	// Reusing the label for different art does not opt into this migration.
	file.meshes[0].sha256 = std::string(64, 'a');
	file.rules[0].scale = { 1.26F, 1.0F, 1.5F };
	EXPECT_NEAR(skm::TransformPoint(opening(), { 0, 1, 0 }).y, 1.0F, 0.0001F);
}

TEST(PlacementTest, FileNamesPerLevelType)
{
	EXPECT_EQ(PlacementFileNameForLevelType(0), "town.ini");
	EXPECT_EQ(PlacementFileNameForLevelType(1), "cathedral.ini");
	EXPECT_EQ(PlacementFileNameForLevelType(6), "crypt.ini");
	EXPECT_TRUE(PlacementFileNameForLevelType(-1).empty());
	EXPECT_TRUE(PlacementFileNameForLevelType(7).empty());
}

} // namespace
} // namespace devilution::render3d
