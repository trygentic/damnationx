#include "fpv/fpv_math.hpp"

#include <cmath>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace devilution::fpv {
namespace {

constexpr float Eps = 1e-4F;

/** World step (x, z) of a classic facing, see TileStep in engine/freeform_movement.hpp. */
std::pair<int, int> Step(Direction dir)
{
	switch (dir) {
	case Direction::South: return { 1, 1 };
	case Direction::SouthWest: return { 0, 1 };
	case Direction::West: return { -1, 1 };
	case Direction::NorthWest: return { -1, 0 };
	case Direction::North: return { -1, -1 };
	case Direction::NorthEast: return { 0, -1 };
	case Direction::East: return { 1, -1 };
	case Direction::SouthEast: return { 1, 0 };
	default: return { 0, 0 };
	}
}

TEST(FpvMathTest, YawZeroLooksTowardPlusZ)
{
	EXPECT_NEAR(DirectionToYaw(Direction::SouthWest), 0.0F, Eps);
	const CameraBasis basis = MakeBasis(0.0F, 0.0F);
	EXPECT_NEAR(basis.forward.x, 0.0F, Eps);
	EXPECT_NEAR(basis.forward.z, 1.0F, Eps);
	EXPECT_NEAR(basis.up.y, 1.0F, Eps);
	// Facing +z with y up, right is -x (yaw grows counter-clockwise seen from above).
	EXPECT_NEAR(basis.right.x, -1.0F, Eps);
}

TEST(FpvMathTest, DirectionYawMatchesTileStep)
{
	for (int d = 0; d < 8; ++d) {
		const Direction dir = static_cast<Direction>(d);
		const auto [sx, sz] = Step(dir);
		const float yaw = DirectionToYaw(dir);
		const float len = std::sqrt(static_cast<float>(sx * sx + sz * sz));
		EXPECT_NEAR(std::sin(yaw), static_cast<float>(sx) / len, Eps) << d;
		EXPECT_NEAR(std::cos(yaw), static_cast<float>(sz) / len, Eps) << d;
	}
}

TEST(FpvMathTest, YawToDirectionRoundTripsAndRounds)
{
	for (int d = 0; d < 8; ++d) {
		const Direction dir = static_cast<Direction>(d);
		const float yaw = DirectionToYaw(dir);
		EXPECT_EQ(YawToDirection(yaw), dir);
		EXPECT_EQ(YawToDirection(yaw + 0.3F), dir);
		EXPECT_EQ(YawToDirection(yaw - 0.3F), dir);
		EXPECT_EQ(YawToDirection(yaw + 4.0F * kPi), dir);
	}
	// Turning right (yaw decreasing) is Right() of the facing.
	EXPECT_EQ(YawToDirection(DirectionToYaw(Direction::South) - kPi / 4.0F), Right(Direction::South));
}

TEST(FpvMathTest, WrapAngle)
{
	EXPECT_NEAR(WrapAngle(3.0F * kPi), kPi, Eps);
	EXPECT_NEAR(WrapAngle(-kPi / 2.0F - 2.0F * kPi), -kPi / 2.0F, Eps);
	EXPECT_NEAR(WrapAngle(0.25F), 0.25F, Eps);
}

TEST(FpvMathTest, HorizontalFovToVertical)
{
	EXPECT_NEAR(HorizontalToVerticalFov(90.0F, 1.0F), 90.0F, 1e-3F);
	// 95 degrees across 16:9 is about 63 degrees vertically.
	EXPECT_NEAR(HorizontalToVerticalFov(95.0F, 16.0F / 9.0F), 63.09F, 0.05F);
}

TEST(FpvMathTest, CentreRayIsForwardAndProjectsBack)
{
	CameraState camera;
	camera.x = 10.5F;
	camera.z = 20.5F;
	camera.yaw = 0.7F;
	camera.pitch = -0.2F;
	camera.fovYDeg = 60.0F;
	const Ray centre = ScreenRay(camera, 320.0F, 240.0F, 640, 480);
	const CameraBasis basis = MakeBasis(camera.yaw, camera.pitch);
	EXPECT_NEAR(centre.dir.dot(basis.forward), 1.0F, Eps);

	const Ray corner = ScreenRay(camera, 100.0F, 50.0F, 640, 480);
	const std::optional<ScreenPoint> projected = ProjectToScreen(camera, corner.at(5.0F), 640, 480);
	ASSERT_TRUE(projected.has_value());
	EXPECT_NEAR(projected->x, 100.0F, 0.01F);
	EXPECT_NEAR(projected->y, 50.0F, 0.01F);
	EXPECT_FALSE(ProjectToScreen(camera, centre.at(-1.0F), 640, 480).has_value());
}

TEST(FpvMathTest, TraceGridHitsFirstSolidTile)
{
	// Looking along +x from inside tile (2, 5): tiles x = 6.. are solid.
	const Ray ray { { 2.5F, 0.6F, 5.5F }, Vec3f { 1.0F, 0.0F, 0.0F } };
	std::vector<std::pair<int, int>> visited;
	const GridHit hit = TraceGrid(
	    ray, 20.0F, [](int x, int) { return x >= 6; },
	    [&](int x, int y, float, float) { visited.emplace_back(x, y); });
	ASSERT_TRUE(hit.hit);
	EXPECT_EQ(hit.tileX, 6);
	EXPECT_EQ(hit.tileY, 5);
	EXPECT_NEAR(hit.distance, 3.5F, Eps);
	EXPECT_EQ(hit.lastOpenX, 5);
	EXPECT_EQ(visited.size(), 4U);
}

TEST(FpvMathTest, TraceGridDiagonalAndRange)
{
	const Vec3f dir = Vec3f { 1.0F, 0.0F, 1.0F }.normalized();
	const Ray ray { { 0.5F, 0.5F, 0.25F }, dir };
	const GridHit hit = TraceGrid(
	    ray, 50.0F, [](int x, int y) { return x == 3 && y == 3; }, [](int, int, float, float) {});
	ASSERT_TRUE(hit.hit);
	EXPECT_EQ(hit.tileX, 3);
	EXPECT_EQ(hit.tileY, 3);

	const GridHit miss = TraceGrid(
	    ray, 2.0F, [](int, int) { return false; }, [](int, int, float, float) {});
	EXPECT_FALSE(miss.hit);
}

TEST(FpvMathTest, TraceGridDistanceIsAlong3dRay)
{
	// Looking 45 degrees down: the wall 2 tiles ahead is reached after 2 * sqrt(2) along the ray.
	const Ray ray { { 0.5F, 3.0F, 0.0F }, Vec3f { 0.0F, -1.0F, 1.0F }.normalized() };
	const GridHit hit = TraceGrid(
	    ray, 10.0F, [](int, int y) { return y >= 2; }, [](int, int, float, float) {});
	ASSERT_TRUE(hit.hit);
	EXPECT_NEAR(hit.distance, 2.0F * std::sqrt(2.0F), 1e-3F);
}

TEST(FpvMathTest, RayCylinder)
{
	const Ray ray { { 0.0F, 0.5F, 0.0F }, Vec3f { 0.0F, 0.0F, 1.0F } };
	const std::optional<float> t = RayCylinder(ray, 0.0F, 5.0F, 0.5F, 0.0F, 1.0F);
	ASSERT_TRUE(t.has_value());
	EXPECT_NEAR(*t, 4.5F, Eps);
	// Passing above the cylinder.
	EXPECT_FALSE(RayCylinder({ { 0.0F, 1.5F, 0.0F }, Vec3f { 0.0F, 0.0F, 1.0F } }, 0.0F, 5.0F, 0.5F, 0.0F, 1.0F).has_value());
	// Missing to the side.
	EXPECT_FALSE(RayCylinder(ray, 2.0F, 5.0F, 0.5F, 0.0F, 1.0F).has_value());
	// Behind the origin.
	EXPECT_FALSE(RayCylinder(ray, 0.0F, -5.0F, 0.5F, 0.0F, 1.0F).has_value());
	// Looking straight down onto the top cap.
	const std::optional<float> cap = RayCylinder({ { 0.1F, 3.0F, 5.0F }, Vec3f { 0.0F, -1.0F, 0.0F } }, 0.0F, 5.0F, 0.5F, 0.0F, 1.0F);
	ASSERT_TRUE(cap.has_value());
	EXPECT_NEAR(*cap, 2.0F, Eps);
	// Origin inside.
	const std::optional<float> inside = RayCylinder({ { 0.0F, 0.5F, 5.0F }, Vec3f { 1.0F, 0.0F, 0.0F } }, 0.0F, 5.0F, 0.5F, 0.0F, 1.0F);
	ASSERT_TRUE(inside.has_value());
	EXPECT_NEAR(*inside, 0.0F, Eps);
}

TEST(FpvMathTest, RayBoxAndFloor)
{
	const Ray down { { 0.5F, 1.0F, 0.0F }, Vec3f { 0.0F, -1.0F, 1.0F }.normalized() };
	const std::optional<float> floor = RayFloor(down);
	ASSERT_TRUE(floor.has_value());
	EXPECT_NEAR(down.at(*floor).y, 0.0F, Eps);
	EXPECT_NEAR(down.at(*floor).z, 1.0F, Eps);
	EXPECT_FALSE(RayFloor({ { 0.0F, 1.0F, 0.0F }, Vec3f { 0.0F, 1.0F, 0.0F } }).has_value());

	const std::optional<float> box = RayBox({ { 0.5F, 0.2F, 0.0F }, Vec3f { 0.0F, 0.0F, 1.0F } }, { 0.0F, 0.0F, 3.0F }, { 1.0F, 0.5F, 4.0F });
	ASSERT_TRUE(box.has_value());
	EXPECT_NEAR(*box, 3.0F, Eps);
	EXPECT_FALSE(RayBox({ { 0.5F, 0.8F, 0.0F }, Vec3f { 0.0F, 0.0F, 1.0F } }, { 0.0F, 0.0F, 3.0F }, { 1.0F, 0.5F, 4.0F }).has_value());
}

} // namespace
} // namespace devilution::fpv
