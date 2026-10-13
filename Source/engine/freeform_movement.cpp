#include "engine/freeform_movement.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>

#include "engine/animationinfo.h"

namespace devilution {

namespace {

/** Divides and rounds half away from zero, so results are symmetric for both signs. */
constexpr int32_t RoundedDivide(int64_t numerator, int64_t denominator)
{
	if (denominator < 0) {
		numerator = -numerator;
		denominator = -denominator;
	}
	if (numerator >= 0)
		return static_cast<int32_t>((numerator + denominator / 2) / denominator);
	return static_cast<int32_t>(-((-numerator + denominator / 2) / denominator));
}

/** Sector index (0..7, Direction order) of a world-space vector, or -1 for a zero vector. */
int SectorOf(SubTileDisplacement v, int64_t &alongBest, int64_t &alongNext)
{
	if (v.deltaX == 0 && v.deltaY == 0)
		return -1;

	// Project onto all 8 unit-ish direction vectors; diagonals in world space
	// ({1,1} etc.) are sqrt(2) long so scale the axis-aligned ones to match.
	// 181/128 ~= sqrt(2).
	int best = 0;
	int64_t bestValue = INT64_MIN;
	std::array<int64_t, 8> values {};
	for (int d = 0; d < 8; d++) {
		const SubTileDisplacement step = TileStep(static_cast<Direction>(d));
		int64_t dot = static_cast<int64_t>(step.deltaX) * v.deltaX + static_cast<int64_t>(step.deltaY) * v.deltaY;
		const bool axisAligned = step.deltaX == 0 || step.deltaY == 0;
		if (axisAligned)
			dot = dot * 181 / 128;
		values[d] = dot;
		if (dot > bestValue) {
			bestValue = dot;
			best = d;
		}
	}
	alongBest = bestValue;
	alongNext = std::max(values[(best + 1) % 8], values[(best + 7) % 8]);
	return best;
}

} // namespace

SubTileDisplacement FreeformStep::at(uint8_t progress) const
{
	const int64_t fraction = AnimationInfo::baseValueFraction;
	const int64_t p = std::min<int64_t>(progress, fraction);
	return {
		start.deltaX + RoundedDivide((static_cast<int64_t>(end.deltaX) - start.deltaX) * p, fraction),
		start.deltaY + RoundedDivide((static_cast<int64_t>(end.deltaY) - start.deltaY) * p, fraction),
	};
}

SubTileDisplacement IdealNextPoint(SubTileDisplacement carry, SubTileDisplacement goal, int stepsToGoal)
{
	const int steps = std::max(stepsToGoal, 1);
	return {
		carry.deltaX + RoundedDivide(static_cast<int64_t>(goal.deltaX) - carry.deltaX, steps),
		carry.deltaY + RoundedDivide(static_cast<int64_t>(goal.deltaY) - carry.deltaY, steps),
	};
}

FreeformStep PlanFreeformStep(SubTileDisplacement carry, Direction stepDirection, SubTileDisplacement goal, int stepsToGoal)
{
	SubTileDisplacement ideal = IdealNextPoint(carry, goal, stepsToGoal);

	const SubTileDisplacement target = TileStep(stepDirection);
	ideal.deltaX = std::clamp(ideal.deltaX, target.deltaX - MaxFreeformDeviation, target.deltaX + MaxFreeformDeviation);
	ideal.deltaY = std::clamp(ideal.deltaY, target.deltaY - MaxFreeformDeviation, target.deltaY + MaxFreeformDeviation);

	// Keep the carried start inside the square of the tile being left so a stale carry can never drag the sprite away.
	SubTileDisplacement start = carry;
	start.deltaX = std::clamp(start.deltaX, -MaxFreeformDeviation, MaxFreeformDeviation);
	start.deltaY = std::clamp(start.deltaY, -MaxFreeformDeviation, MaxFreeformDeviation);

	return FreeformStep { start, ideal, true };
}

Displacement SubTileToScreen(SubTileDisplacement offset)
{
	// A world step {1, 0} (SouthEast) is {32, 16} pixels on screen.
	return {
		RoundedDivide((static_cast<int64_t>(offset.deltaX) - offset.deltaY) * 32, SubTileUnits),
		RoundedDivide((static_cast<int64_t>(offset.deltaX) + offset.deltaY) * 16, SubTileUnits),
	};
}

Displacement SubTileToLight(SubTileDisplacement offset)
{
	// Light offsets use 8 units per tile along each world axis.
	return {
		RoundedDivide(static_cast<int64_t>(offset.deltaX) * 8, SubTileUnits),
		RoundedDivide(static_cast<int64_t>(offset.deltaY) * 8, SubTileUnits),
	};
}

Direction FacingFromVector(SubTileDisplacement vector, Direction current)
{
	int64_t alongBest = 0;
	int64_t alongNext = 0;
	const int sector = SectorOf(vector, alongBest, alongNext);
	if (sector < 0)
		return current;

	const Direction best = static_cast<Direction>(sector);
	if (current == best || current == Direction::NoDirection)
		return best;

	// Keep the current facing if it is a neighbour of the best sector and
	// almost as good: within ~10% of the best projection.
	if (current == Left(best) || current == Right(best)) {
		const SubTileDisplacement step = TileStep(current);
		int64_t dot = static_cast<int64_t>(step.deltaX) * vector.deltaX + static_cast<int64_t>(step.deltaY) * vector.deltaY;
		if (step.deltaX == 0 || step.deltaY == 0)
			dot = dot * 181 / 128;
		if (dot * 10 >= alongBest * 9)
			return current;
	}
	(void)alongNext;
	return best;
}

Point LinePoint(Point origin, Point goal, int k)
{
	const int steps = StepsBetween(origin, goal);
	if (steps == 0)
		return origin;
	k = std::clamp(k, 0, steps);
	return {
		origin.x + RoundedDivide(static_cast<int64_t>(goal.x - origin.x) * k, steps),
		origin.y + RoundedDivide(static_cast<int64_t>(goal.y - origin.y) * k, steps),
	};
}

size_t FurthestStraightGoal(const Point *tiles, size_t count)
{
	if (count < 2)
		return 0;

	const Point origin = tiles[0];
	size_t best = 1;
	for (size_t j = 2; j < count; j++) {
		const Displacement delta = tiles[j] - origin;
		const int steps = static_cast<int>(j);
		// Once the path takes a detour every longer stretch does too.
		if (StepsBetween(origin, tiles[j]) != steps)
			break;

		// Tile k must lie within half a tile of the point k/j along the line:
		// |k * delta - j * (tile - origin)| <= j / 2, doubled to stay in integers.
		bool straight = true;
		for (int k = 1; k < steps && straight; k++) {
			const Displacement along = tiles[k] - origin;
			const int errorX = std::abs(2 * k * delta.deltaX - 2 * steps * along.deltaX);
			const int errorY = std::abs(2 * k * delta.deltaY - 2 * steps * along.deltaY);
			straight = errorX <= steps && errorY <= steps;
		}
		if (straight)
			best = j;
	}
	return best;
}

} // namespace devilution
