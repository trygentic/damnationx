#include "engine/freeform_movement.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/animationinfo.h"

namespace devilution {
namespace {

TEST(FreeformMovementTest, ScreenOffsetsMatchClassicSteps)
{
	EXPECT_EQ(SubTileToScreen(TileStep(Direction::South)), Displacement(0, 32));
	EXPECT_EQ(SubTileToScreen(TileStep(Direction::East)), Displacement(64, 0));
	EXPECT_EQ(SubTileToScreen(TileStep(Direction::SouthEast)), Displacement(32, 16));
	EXPECT_EQ(SubTileToScreen(TileStep(Direction::NorthWest)), Displacement(-32, -16));
	EXPECT_EQ(SubTileToScreen(TileStep(Direction::West)), Displacement(-64, 0));
}

TEST(FreeformMovementTest, StraightStepMatchesClassicStep)
{
	const FreeformStep step = PlanFreeformStep({}, Direction::East, TileStep(Direction::East) * 5, 5);
	EXPECT_EQ(step.start, SubTileDisplacement(0, 0));
	EXPECT_EQ(step.end, TileStep(Direction::East));
	EXPECT_EQ(step.at(0), SubTileDisplacement(0, 0));
	EXPECT_EQ(step.at(AnimationInfo::baseValueFraction), TileStep(Direction::East));
	EXPECT_EQ(step.carryAfter(Direction::East), SubTileDisplacement(0, 0));
}

TEST(FreeformMovementTest, FacingQuantizesToEightSectors)
{
	for (int d = 0; d < 8; d++) {
		const Direction dir = static_cast<Direction>(d);
		EXPECT_EQ(FacingFromVector(TileStep(dir), Direction::NoDirection), dir);
		// Hysteresis never overrides a far away facing.
		EXPECT_EQ(FacingFromVector(TileStep(dir), Opposite(dir)), dir);
	}
	EXPECT_EQ(FacingFromVector({}, Direction::West), Direction::West);
}

TEST(FreeformMovementTest, FacingKeepsNeighbourNearSectorEdge)
{
	// Exactly between SouthEast {1,0} and South {1,1}: keep whatever we had.
	const SubTileDisplacement between { 1000, 414 };
	EXPECT_EQ(FacingFromVector(between, Direction::South), Direction::South);
	EXPECT_EQ(FacingFromVector(between, Direction::SouthEast), Direction::SouthEast);
}

TEST(FreeformMovementTest, LinePointsAreConnected)
{
	const Point origin { 10, 10 };
	const Point goal { 20, 14 };
	const int steps = StepsBetween(origin, goal);
	EXPECT_EQ(steps, 10);
	Point previous = origin;
	for (int k = 1; k <= steps; k++) {
		const Point p = LinePoint(origin, goal, k);
		EXPECT_EQ(StepsBetween(previous, p), 1);
		previous = p;
	}
	EXPECT_EQ(previous, goal);
}

/** Walks from origin to goal with ChooseStepToward and PlanFreeformStep, like an actor would. */
std::vector<Point> Walk(Point origin, Point goal, std::vector<SubTileDisplacement> *drawn = nullptr)
{
	std::vector<Point> tiles { origin };
	Point tile = origin;
	SubTileDisplacement carry {};
	for (int guard = 0; guard < 100 && tile != goal; guard++) {
		const Direction dir = ChooseStepToward(tile, goal, carry, [](Direction) { return true; });
		EXPECT_NE(dir, Direction::NoDirection);
		const FreeformStep step = PlanFreeformStep(carry, dir, ToSubTile(goal - tile), StepsBetween(tile, goal));
		EXPECT_LE(std::abs(step.end.deltaX - TileStep(dir).deltaX), MaxFreeformDeviation);
		EXPECT_LE(std::abs(step.end.deltaY - TileStep(dir).deltaY), MaxFreeformDeviation);
		carry = step.carryAfter(dir);
		tile = tile + dir;
		tiles.push_back(tile);
		if (drawn != nullptr)
			drawn->push_back(ToSubTile(tile - origin) + carry);
	}
	return tiles;
}

TEST(FreeformMovementTest, WalkTracksStraightLine)
{
	const Point origin { 10, 10 };
	const Point goal { 20, 14 };
	std::vector<SubTileDisplacement> drawn;
	const std::vector<Point> tiles = Walk(origin, goal, &drawn);
	ASSERT_EQ(tiles.back(), goal);
	EXPECT_EQ(tiles.size(), 11U) << "Straight walks take the minimum number of steps";

	// Every tile stays within one tile of the true line and diagonals are spread out instead of bunched up.
	const SubTileDisplacement line = ToSubTile(goal - origin);
	for (size_t k = 0; k < tiles.size(); k++) {
		const SubTileDisplacement p = ToSubTile(tiles[k] - origin);
		const int64_t cross = static_cast<int64_t>(p.deltaX) * line.deltaY - static_cast<int64_t>(p.deltaY) * line.deltaX;
		const int64_t lengthSq = LengthSquared(line);
		// |cross| / |line| is the distance from the line in sub-tile units.
		EXPECT_LE(cross * cross, lengthSq * SubTileUnits * SubTileUnits) << "Tile " << k << " strays more than a tile";
	}

	// Drawn points lie on the line.
	for (const SubTileDisplacement &p : drawn) {
		const int64_t cross = static_cast<int64_t>(p.deltaX) * line.deltaY - static_cast<int64_t>(p.deltaY) * line.deltaX;
		EXPECT_LE(cross * cross, LengthSquared(line) * 4 * 4);
	}
}

TEST(FreeformMovementTest, WalkReachesGoalAtAnyAngle)
{
	const Point origin { 50, 50 };
	for (int dx = -12; dx <= 12; dx++) {
		for (int dy = -12; dy <= 12; dy++) {
			const Point goal = origin + Displacement { dx, dy };
			const std::vector<Point> tiles = Walk(origin, goal);
			EXPECT_EQ(tiles.back(), goal);
			EXPECT_EQ(static_cast<int>(tiles.size()) - 1, StepsBetween(origin, goal));
		}
	}
}

TEST(FreeformMovementTest, StraightAheadStepsSettleCarry)
{
	// Monsters plan walks without a goal (wandering, fleeing) towards a point two steps straight ahead:
	// a carried offset must shrink step by step and never leave the tile square.
	SubTileDisplacement carry { 100, -90 };
	for (int i = 0; i < 8; i++) {
		const Direction dir = i % 2 == 0 ? Direction::East : Direction::SouthEast;
		const FreeformStep step = PlanFreeformStep(carry, dir, TileStep(dir) * 2, 2);
		const SubTileDisplacement next = step.carryAfter(dir);
		EXPECT_LE(std::abs(next.deltaX), MaxFreeformDeviation);
		EXPECT_LE(std::abs(next.deltaY), MaxFreeformDeviation);
		EXPECT_LE(LengthSquared(next), LengthSquared(carry));
		carry = next;
	}
	EXPECT_LE(LengthSquared(carry), 4 * 4);
}

TEST(FreeformMovementTest, ChooseStepFallsBackToNeighbours)
{
	const Point from { 10, 10 };
	const Point goal { 20, 10 }; // straight SouthEast
	EXPECT_EQ(ChooseStepToward(from, goal, {}, [](Direction) { return true; }), Direction::SouthEast);
	const Direction dir = ChooseStepToward(from, goal, {}, [](Direction d) { return d != Direction::SouthEast; });
	EXPECT_TRUE(dir == Direction::South || dir == Direction::East);
	EXPECT_EQ(ChooseStepToward(from, goal, {}, [](Direction) { return false; }), Direction::NoDirection);
	EXPECT_EQ(ChooseStepToward(from, from, {}, [](Direction) { return true; }), Direction::NoDirection);
}

/** A small walkability grid for the path tests. '#' is blocked. */
struct TestGrid {
	std::vector<std::string> rows;

	[[nodiscard]] bool walkable(Point p) const
	{
		if (p.y < 0 || p.y >= static_cast<int>(rows.size()) || p.x < 0 || p.x >= static_cast<int>(rows[p.y].size()))
			return false;
		return rows[p.y][p.x] != '#';
	}

	/** Same shape of rule as the game: the target must be walkable and a diagonal step may not cut a blocked corner. */
	[[nodiscard]] bool canStep(Point from, Point to) const
	{
		if (!walkable(to))
			return false;
		if (from.x != to.x && from.y != to.y)
			return walkable({ from.x, to.y }) && walkable({ to.x, from.y });
		return true;
	}

	/** Breadth first shortest path that prefers diagonals first, like the classic "diagonal, then straight" paths. */
	[[nodiscard]] std::vector<Point> shortestPath(Point from, Point to) const
	{
		constexpr Displacement Steps[8] = { { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		std::map<std::pair<int, int>, Point> previous;
		std::vector<Point> frontier { from };
		previous[{ from.x, from.y }] = from;
		for (size_t i = 0; i < frontier.size(); i++) {
			const Point current = frontier[i];
			if (current == to)
				break;
			for (const Displacement step : Steps) {
				const Point next = current + step;
				if (previous.count({ next.x, next.y }) != 0 || !canStep(current, next))
					continue;
				previous[{ next.x, next.y }] = current;
				frontier.push_back(next);
			}
		}
		std::vector<Point> path;
		if (previous.count({ to.x, to.y }) == 0)
			return path;
		for (Point p = to; p != from; p = previous[{ p.x, p.y }])
			path.push_back(p);
		path.push_back(from);
		std::reverse(path.begin(), path.end());
		return path;
	}
};

/** Checks that every step of a tile path is a single allowed step. */
void ExpectValidPath(const TestGrid &grid, const std::vector<Point> &path)
{
	for (size_t i = 1; i < path.size(); i++) {
		EXPECT_EQ(StepsBetween(path[i - 1], path[i]), 1) << "Step " << i << " is not a single step";
		EXPECT_TRUE(grid.canStep(path[i - 1], path[i])) << "Step " << i << " is not allowed";
	}
}

std::vector<Point> Straighten(const TestGrid &grid, std::vector<Point> path)
{
	const size_t count = StraightenPath(path.data(), path.size(), [&grid](Point from, Point to) { return grid.canStep(from, to); });
	path.resize(count);
	return path;
}

TEST(FreeformMovementTest, StraightenPathFollowsLineInTheOpen)
{
	const TestGrid grid { std::vector<std::string>(20, std::string(20, '.')) };
	const Point origin { 1, 1 };
	const Point goal { 15, 6 };

	// Classic path: all diagonals first, then straight.
	std::vector<Point> path { origin };
	for (Point p = origin; p != goal;) {
		p += Displacement { p.x != goal.x ? 1 : 0, p.y != goal.y ? 1 : 0 };
		path.push_back(p);
	}

	const std::vector<Point> straight = Straighten(grid, path);
	ASSERT_EQ(straight.size(), path.size());
	for (int k = 0; k < static_cast<int>(straight.size()); k++)
		EXPECT_EQ(straight[k], LinePoint(origin, goal, k));
}

TEST(FreeformMovementTest, StraightenPathShortensDetours)
{
	const TestGrid grid { std::vector<std::string>(10, std::string(10, '.')) };
	// A needless detour: right, down, left again.
	const std::vector<Point> path { { 1, 1 }, { 2, 1 }, { 3, 2 }, { 2, 3 }, { 1, 4 } };
	const std::vector<Point> straight = Straighten(grid, path);
	EXPECT_EQ(straight.size(), 4U);
	EXPECT_EQ(straight.front(), path.front());
	EXPECT_EQ(straight.back(), path.back());
	ExpectValidPath(grid, straight);
}

TEST(FreeformMovementTest, StraightenPathKeepsShortPathsAndInvalidLastStep)
{
	const TestGrid grid { std::vector<std::string>(10, std::string(10, '.')) };
	const std::vector<Point> single { { 1, 1 }, { 2, 2 } };
	EXPECT_EQ(Straighten(grid, single), single);

	// The last tile is not walkable (e.g. a monster being walked to): the original step onto it is kept.
	const std::vector<Point> path { { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 3 } };
	std::vector<Point> straight = path;
	straight.resize(StraightenPath(straight.data(), straight.size(), [](Point, Point to) { return to != Point { 4, 3 }; }));
	EXPECT_EQ(straight, path);
}

TEST(FreeformMovementTest, StraightenPathAvoidsObstacles)
{
	const TestGrid grid { {
	    "....................",
	    "....................",
	    "......#.............",
	    "......#.....###.....",
	    "......#.......#.....",
	    "..............#.....",
	    "....#####.....#.....",
	    "..............#.....",
	    "....................",
	    "....................",
	} };
	for (int fromY = 0; fromY < 10; fromY += 3) {
		for (int toY = 0; toY < 10; toY += 2) {
			const Point from { 0, fromY };
			const Point to { 19, toY };
			const std::vector<Point> path = grid.shortestPath(from, to);
			ASSERT_FALSE(path.empty());
			const std::vector<Point> straight = Straighten(grid, path);
			EXPECT_LE(straight.size(), path.size());
			EXPECT_EQ(straight.front(), from);
			EXPECT_EQ(straight.back(), to);
			ExpectValidPath(grid, straight);
		}
	}
}

TEST(FreeformMovementTest, FurthestStraightGoal)
{
	std::vector<Point> line;
	for (int k = 0; k <= 10; k++)
		line.push_back(LinePoint({ 0, 0 }, { 10, 4 }, k));
	EXPECT_EQ(FurthestStraightGoal(line.data(), line.size()), 10U);

	// Diagonal first, then straight: only the start of the diagonal run lies on one line.
	const std::vector<Point> bent { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 2 }, { 4, 2 }, { 5, 2 } };
	const size_t goal = FurthestStraightGoal(bent.data(), bent.size());
	EXPECT_GE(goal, 2U);
	EXPECT_LT(goal, 5U);

	// A detour is never straight.
	const std::vector<Point> detour { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	EXPECT_EQ(FurthestStraightGoal(detour.data(), detour.size()), 1U);

	EXPECT_EQ(FurthestStraightGoal(line.data(), 1), 0U);
	EXPECT_EQ(FurthestStraightGoal(line.data(), 2), 1U);
}

TEST(FreeformMovementTest, StraightGoalDrawsInsideEveryTile)
{
	// Walking a straight line towards the goal FurthestStraightGoal picks never needs clamping.
	for (int dx = -9; dx <= 9; dx++) {
		for (int dy = -9; dy <= 9; dy++) {
			const Point goal { dx, dy };
			const int steps = StepsBetween({ 0, 0 }, goal);
			if (steps == 0)
				continue;
			std::vector<Point> tiles;
			for (int k = 0; k <= steps; k++)
				tiles.push_back(LinePoint({ 0, 0 }, goal, k));
			ASSERT_EQ(FurthestStraightGoal(tiles.data(), tiles.size()), static_cast<size_t>(steps));

			SubTileDisplacement carry {};
			for (int k = 0; k < steps; k++) {
				const Direction dir = GetDirection(tiles[k], tiles[k + 1]);
				const SubTileDisplacement toGoal = ToSubTile(goal - tiles[k]);
				const SubTileDisplacement ideal = IdealNextPoint(carry, toGoal, steps - k);
				const FreeformStep step = PlanFreeformStep(carry, dir, toGoal, steps - k);
				EXPECT_EQ(step.end, ideal) << "Clamped on the way to " << dx << "," << dy << " at step " << k;
				carry = step.carryAfter(dir);
			}
			EXPECT_EQ(carry, SubTileDisplacement(0, 0));
		}
	}
}

} // namespace
} // namespace devilution
