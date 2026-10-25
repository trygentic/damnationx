#include "engine/path.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "engine/direction.hpp"
#include "utils/algorithm/container.hpp"

namespace devilution {

extern int TestPathGetHeuristicCost(Point startPosition, Point destinationPosition);

namespace {

using ::testing::ElementsAreArray;

TEST(PathTest, Heuristics)
{
	constexpr Point source { 25, 32 };
	Point destination = source;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), 0) << "Wrong cost for travelling to the same tile";

	destination = source + Direction::NorthEast;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathAxisAlignedStepCost) << "Wrong cost for travelling to horizontal/vertical adjacent tile";
	destination = source + Direction::SouthEast;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathAxisAlignedStepCost) << "Wrong cost for travelling to horizontal/vertical adjacent tile";
	destination = source + Direction::SouthWest;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathAxisAlignedStepCost) << "Wrong cost for travelling to horizontal/vertical adjacent tile";
	destination = source + Direction::NorthWest;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathAxisAlignedStepCost) << "Wrong cost for travelling to horizontal/vertical adjacent tile";

	destination = source + Direction::North;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathDiagonalStepCost) << "Wrong cost for travelling to diagonally adjacent tile";
	destination = source + Direction::East;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathDiagonalStepCost) << "Wrong cost for travelling to diagonally adjacent tile";
	destination = source + Direction::South;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathDiagonalStepCost) << "Wrong cost for travelling to diagonally adjacent tile";
	destination = source + Direction::West;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathDiagonalStepCost) << "Wrong cost for travelling to diagonally adjacent tile";
	destination = source + Direction::SouthWest + Direction::SouthEast; // Effectively the same as Direction::South
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathDiagonalStepCost) << "Wrong cost for travelling to diagonally adjacent tile";

	destination = source + Direction::NorthEast + Direction::North;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), PathAxisAlignedStepCost + PathDiagonalStepCost) << "Wrong cost for travelling to a { 2, 1 } offset";
	destination = source + Direction::SouthEast + Direction::SouthEast;
	EXPECT_EQ(TestPathGetHeuristicCost(source, destination), 2 * PathAxisAlignedStepCost) << "Wrong cost for travelling to a { 2, 0 } offset";
}

// These symbols are in terms of coordinates (not in terms of on-screen direction).
// -1, -1 is top-left.
enum class Dir {
	None,
	Up,
	Left,
	Right,
	Down,
	UpLeft,
	UpRight,
	DownRight,
	DownLeft
};
std::array<std::string_view, 9> DirSymbols = { "∅", "↑", "←", "→", "↓", "↖", "↗", "↘", "↙" };

std::ostream &operator<<(std::ostream &os, Dir dir)
{
	return os << DirSymbols[static_cast<size_t>(dir)];
}

std::vector<Dir> ToSyms(std::span<const std::string> strings)
{
	std::vector<Dir> result;
	result.reserve(strings.size());
	for (const std::string &str : strings)
		result.emplace_back(static_cast<Dir>(std::distance(DirSymbols.begin(), c_find(DirSymbols, str))));
	return result;
}

std::vector<Dir> ToSyms(std::span<const int8_t> indices)
{
	std::vector<Dir> result;
	result.reserve(indices.size());
	for (const int8_t idx : indices)
		result.emplace_back(static_cast<Dir>(idx));
	return result;
}

void CheckPath(Point startPosition, Point destinationPosition, std::vector<std::string> expectedSteps)
{
	// Restrict tests to the longest possible path length in vanilla Diablo
	constexpr size_t MaxPathLength = 24;
	int8_t pathSteps[MaxPathLength];
	auto pathLength = FindPath(
	    /*canStep=*/[](Point, Point) { return true; },
	    /*posOk=*/[](Point) { return true; },
	    startPosition, destinationPosition, pathSteps, MaxPathLength);
	EXPECT_THAT(ToSyms(std::span<const int8_t>(pathSteps, pathLength)), ElementsAreArray(ToSyms(expectedSteps)))
	    << "Path steps differs from expectation for a path from "
	    << startPosition << " to " << destinationPosition;
}

TEST(PathTest, FindPathToSelf)
{
	CheckPath({ 8, 8 }, { 8, 8 }, {});
}

TEST(PathTest, FindPathTwoStepsUp)
{
	CheckPath({ 8, 8 }, { 8, 6 }, { "↑", "↑" });
}

TEST(PathTest, FindPathTwoStepsLeft)
{
	CheckPath({ 8, 8 }, { 6, 8 }, { "←", "←" });
}

TEST(PathTest, FindPathTwoStepsRight)
{
	CheckPath({ 8, 8 }, { 10, 8 }, { "→", "→" });
}

TEST(PathTest, FindPathTwoStepsDown)
{
	CheckPath({ 8, 8 }, { 8, 10 }, { "↓", "↓" });
}

TEST(PathTest, FindPathDiagonalsFirst3Left2Up)
{
	// Pathing biases along diagonals and the diagonal steps will always be first
	CheckPath({ 8, 8 }, { 5, 6 }, { "↖", "↖", "←" });
}

TEST(PathTest, FindPathDiagonalsFirst4Left4Up)
{
	CheckPath({ 8, 8 }, { 4, 4 }, { "↖", "↖", "↖", "↖" });
}

TEST(PathTest, FindPathDiagonalsFirst2Right4Down)
{
	CheckPath({ 8, 8 }, { 10, 12 }, { "↘", "↘", "↓", "↓" });
}

TEST(PathTest, FindPathDiagonalsFirst4Right12Down)
{
	CheckPath({ 8, 8 }, { 12, 20 }, { "↘", "↘", "↘", "↘", "↓", "↓", "↓", "↓", "↓", "↓", "↓", "↓" });
}

TEST(PathTest, LongPaths)
{
	// Starting from the middle of the world and trying to path to a border exceeds the maximum path size
	CheckPath({ 56, 56 }, { 0, 0 }, {});

	// Longest possible path used to be 24 steps meaning tiles 24 units away are reachable
	const Point startingPosition { 56, 56 };
	CheckPath(startingPosition, startingPosition + Displacement { 24, 24 }, std::vector<std::string>(24, "↘"));

	// But trying to navigate 25 units fails
	CheckPath(startingPosition, startingPosition + Displacement { 25, 25 }, {});
}

TEST(PathTest, ApproachBlockedAndDisconnectedTargets)
{
	const auto canStep = [](Point, Point) { return true; };
	// An impenetrable building spans x=5..9, including its non-walkable roof target.
	const auto posOk = [](Point tile) { return tile.x >= 0 && tile.x <= 12 && tile.y >= 0 && tile.y <= 12 && tile.x < 5; };
	int8_t path[MaxPathLengthPlayer];
	Point reached;
	// Interaction paths retain the legacy final blocked step; plain walking never takes it.
	EXPECT_EQ(FindPath(canStep, posOk, { 4, 6 }, { 5, 6 }, path, MaxPathLengthPlayer), 1);
	EXPECT_EQ(FindPath(canStep, posOk, { 4, 6 }, { 5, 6 }, path, MaxPathLengthPlayer, &reached), 0);
	EXPECT_EQ(reached, (Point { 4, 6 }));
	EXPECT_EQ(FindPath(canStep, posOk, { 1, 6 }, { 8, 6 }, path, MaxPathLengthPlayer), 0);
	EXPECT_GT(FindPath(canStep, posOk, { 1, 6 }, { 8, 6 }, path, MaxPathLengthPlayer, &reached), 0);
	EXPECT_EQ(reached, (Point { 4, 6 }));
	// Held walking at the boundary stays there instead of trying to enter the wall.
	EXPECT_EQ(FindPath(canStep, posOk, reached, { 8, 6 }, path, MaxPathLengthPlayer, &reached), 0);
	EXPECT_EQ(reached, (Point { 4, 6 }));
	// A walkable but disconnected island must not win over the reachable boundary.
	const auto disconnected = [&posOk](Point tile) { return posOk(tile) || tile == Point { 8, 6 }; };
	EXPECT_GT(FindPath(canStep, disconnected, { 1, 6 }, { 8, 6 }, path, MaxPathLengthPlayer, &reached), 0);
	EXPECT_EQ(reached, (Point { 4, 6 }));
	// Moving the held pointer to accessible ground resumes ordinary movement.
	EXPECT_GT(FindPath(canStep, posOk, reached, { 2, 3 }, path, MaxPathLengthPlayer, &reached), 0);
	EXPECT_EQ(reached, (Point { 2, 3 }));
}

TEST(PathTest, ApproachHonorsStepConstraintsAndLengthLimit)
{
	int8_t path[8];
	Point reached;
	const auto posOk = [](Point tile) { return tile.x >= 0 && tile.x < 20 && tile.y >= 0 && tile.y < 20; };
	const auto canStep = [](Point from, Point to) { return from.x == to.x; };
	EXPECT_GT(FindPath(canStep, posOk, { 2, 2 }, { 10, 15 }, path, 8, &reached), 0);
	EXPECT_EQ(reached, (Point { 2, 10 }));
	EXPECT_EQ(FindPath([](Point, Point) { return false; }, posOk, { 2, 2 }, { 10, 15 }, path, 8, &reached), 0);
	EXPECT_EQ(reached, (Point { 2, 2 }));
}

TEST(PathTest, DiagonalBlockedApproachDoesNotReverseWhileReplanning)
{
	const auto posOk = [](Point p) { return p.x >= 0 && p.y >= 0 && p.x + p.y < 16; };
	const auto canStep = [](Point, Point) { return true; };
	constexpr Displacement offsets[] = { { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	const Point desired { 12, 12 };
	Point current { 2, 2 };
	std::vector<Point> visited { current };
	for (int tick = 0; tick < 30; ++tick) {
		int8_t path[MaxPathLengthPlayer];
		Point reached;
		const int length = FindPath(canStep, posOk, current, desired, path, MaxPathLengthPlayer, &reached);
		if (length == 0)
			break;
		const Point next = current + offsets[path[0] - 1];
		EXPECT_TRUE(posOk(next));
		EXPECT_LE(next.WalkingDistance(desired), current.WalkingDistance(desired));
		EXPECT_EQ(std::find(visited.begin(), visited.end(), next), visited.end()) << "a held approach must not cycle";
		visited.push_back(next);
		current = next;
	}
	EXPECT_EQ(current.x + current.y, 15);
}

TEST(PathTest, UShapedWallApproachCanTemporarilyMoveAwayWithoutCycling)
{
	const auto posOk = [](Point p) {
		return p.x >= 0 && p.y >= 0 && p.x <= 20 && p.y <= 20
		    && !((p.x == 6 || p.x == 14) && p.y >= 6 && p.y <= 16)
		    && !(p.y == 6 && p.x >= 6 && p.x <= 14);
	};
	const auto canStep = [&posOk](Point from, Point to) {
		return from.x == to.x || from.y == to.y || (posOk(Point { from.x, to.y }) && posOk(Point { to.x, from.y }));
	};
	constexpr Displacement offsets[] = { { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	const Point desired { 10, 7 };
	Point current { 10, 3 };
	std::vector<Point> visited { current };
	int previousRemaining = 100;
	bool movedAway = false;
	for (int tick = 0; tick < 80 && current != desired; ++tick) {
		int8_t path[MaxPathLengthPlayer];
		Point reached;
		const int length = FindPath(canStep, posOk, current, desired, path, MaxPathLengthPlayer, &reached);
		ASSERT_GT(length, 0);
		EXPECT_EQ(reached, desired);
		EXPECT_LT(length, previousRemaining);
		previousRemaining = length;
		const Point next = current + offsets[path[0] - 1];
		movedAway |= next.WalkingDistance(desired) > current.WalkingDistance(desired);
		EXPECT_TRUE(posOk(next));
		EXPECT_EQ(std::find(visited.begin(), visited.end(), next), visited.end());
		visited.push_back(next);
		current = next;
	}
	EXPECT_TRUE(movedAway) << "the fixture must exercise a detour around the building";
	EXPECT_EQ(current, desired);
}

TEST(PathTest, FindClosest)
{
	{
		std::array<std::array<int, 101>, 101> searchedTiles {};

		const std::optional<Point> nearPosition = FindClosestValidPosition(
		    [&searchedTiles](Point testPosition) {
			    searchedTiles[testPosition.x][testPosition.y]++;
			    return false;
		    },
		    { 50, 50 }, 0, 50);

		EXPECT_FALSE(nearPosition) << "Searching with no valid tiles should return an empty optional";

		for (size_t x = 0; x < searchedTiles.size(); x++) {
			for (size_t y = 0; y < searchedTiles[x].size(); y++) {
				if ((x == 0 || x == 100) && (y == 0 || y == 100)) {
					EXPECT_EQ(searchedTiles[x][y], 0) << "Extreme corners should be skipped due to the inset/rounded search space";
				} else {
					EXPECT_EQ(searchedTiles[x][y], 1) << "Position " << x << " " << y << " should have been searched exactly once";
				}
			}
		}
	}
	{
		std::array<std::array<int, 5>, 5> searchedTiles {};

		const std::optional<Point> nearPosition = FindClosestValidPosition(
		    [&searchedTiles](Point testPosition) {
			    searchedTiles[testPosition.x][testPosition.y]++;
			    return false;
		    },
		    { 2, 2 }, 1, 2);

		EXPECT_FALSE(nearPosition) << "Still shouldn't find a valid position with no valid tiles";

		for (size_t x = 0; x < searchedTiles.size(); x++) {
			for (size_t y = 0; y < searchedTiles[x].size(); y++) {
				if (x == 2 && y == 2) {
					EXPECT_EQ(searchedTiles[x][y], 0) << "The starting tile should be skipped with a min radius of 1";
				} else if ((x == 0 || x == 4) && (y == 0 || y == 4)) {
					EXPECT_EQ(searchedTiles[x][y], 0) << "Corners should be skipped";
				} else {
					EXPECT_EQ(searchedTiles[x][y], 1) << "All tiles in range should be searched exactly once";
				}
			}
		}
	}
	{
		std::array<std::array<int, 3>, 3> searchedTiles {};

		const std::optional<Point> nearPosition = FindClosestValidPosition(
		    [&searchedTiles](Point testPosition) {
			    searchedTiles[testPosition.x][testPosition.y]++;
			    return false;
		    },
		    { 1, 1 }, 0, 0);

		EXPECT_FALSE(nearPosition) << "Searching with no valid tiles should return an empty optional";

		for (size_t x = 0; x < searchedTiles.size(); x++) {
			for (size_t y = 0; y < searchedTiles[x].size(); y++) {
				if (x == 1 && y == 1) {
					EXPECT_EQ(searchedTiles[x][y], 1) << "Only the starting tile should be searched with max radius 0";
				} else {
					EXPECT_EQ(searchedTiles[x][y], 0) << "Position " << x << " " << y << " should not have been searched";
				}
			}
		}
	}

	{
		std::array<std::array<int, 7>, 7> searchedTiles {};

		const std::optional<Point> nearPosition = FindClosestValidPosition(
		    [&searchedTiles](Point testPosition) {
			    searchedTiles[testPosition.x][testPosition.y]++;
			    return false;
		    },
		    { 3, 3 }, 3, 3);

		EXPECT_FALSE(nearPosition) << "Searching with no valid tiles should return an empty optional";

		for (size_t x = 0; x < searchedTiles.size(); x++) {
			for (size_t y = 0; y < searchedTiles[x].size(); y++) {
				if (((x == 1 || x == 5) && (y == 1 || y == 5))  // inset corners
				    || ((x == 0 || x == 6) && y != 0 && y != 6) // left/right sides
				    || (x != 0 && x != 6 && (y == 0 || y == 6)) // top/bottom sides
				) {
					EXPECT_EQ(searchedTiles[x][y], 1) << "Searching with a fixed radius should make a square with inset corners";
				} else {
					EXPECT_EQ(searchedTiles[x][y], 0) << "Position " << x << " " << y << " should not have been searched";
				}
			}
		}
	}
	{
		std::optional<Point> nearPosition = FindClosestValidPosition(
		    [](Point testPosition) {
			    return true;
		    },
		    { 50, 50 }, 21, 50);

		EXPECT_EQ(*nearPosition, (Point { 50, 50 } + Displacement { 0, 21 })) << "First candidate position with a minimum radius should be at {0, +y}";
	}
}

} // namespace
} // namespace devilution
