#pragma once

/**
 * @file freeform_movement.hpp
 *
 * Free-form (Diablo II style) movement on top of the classic tile grid.
 *
 * Gameplay stays tile based: an actor still occupies one tile, steps to a
 * neighbouring tile per walk animation and combat, collision, AI and network
 * sync keep using tiles. What changes is how the steps are chosen and drawn:
 *
 * - Steps follow a straight line towards a goal (any angle) instead of
 *   "diagonal first, then straight" 8-direction runs.
 * - The drawn position travels along that line in sub-tile precision instead
 *   of hopping between tile centres, so the actor glides at any angle and
 *   curves smoothly when the goal moves.
 * - The facing is taken from the drawn movement vector.
 *
 * All maths is integer and deterministic, so every client computes the same
 * result. The drawn position never leaves the square of the logical tile it is
 * walking to, so what you see is always within half a tile of what combat uses.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "engine/direction.hpp"
#include "engine/displacement.hpp"
#include "engine/point.hpp"

namespace devilution {

/** Fixed-point units per world tile used by free-form movement. */
constexpr int32_t SubTileUnits = 256;
/** How far (per world axis) the drawn position may stray from the centre of the tile being walked to. */
constexpr int32_t MaxFreeformDeviation = SubTileUnits / 2;

/** A world-space offset from a tile centre in 1/SubTileUnits of a tile. */
using SubTileDisplacement = DisplacementOf<int32_t>;

/** Returns the world-space vector of a single step in the given direction, in sub-tile units. */
constexpr SubTileDisplacement TileStep(Direction dir)
{
	constexpr std::array<SubTileDisplacement, 9> Steps { {
	    { SubTileUnits, SubTileUnits },   // South
	    { 0, SubTileUnits },              // SouthWest
	    { -SubTileUnits, SubTileUnits },  // West
	    { -SubTileUnits, 0 },             // NorthWest
	    { -SubTileUnits, -SubTileUnits }, // North
	    { 0, -SubTileUnits },             // NorthEast
	    { SubTileUnits, -SubTileUnits },  // East
	    { SubTileUnits, 0 },              // SouthEast
	    { 0, 0 },                         // NoDirection
	} };
	return Steps[static_cast<size_t>(dir)];
}

/** Converts a world tile offset to sub-tile units. */
constexpr SubTileDisplacement ToSubTile(Displacement tiles)
{
	return { tiles.deltaX * SubTileUnits, tiles.deltaY * SubTileUnits };
}

/**
 * @brief The drawn path of one logical walk step.
 *
 * Both ends are relative to the centre of the tile the actor is leaving
 * (ActorPosition::tile while the step is in progress).
 */
struct FreeformStep {
	/** Drawn position when the step starts. */
	SubTileDisplacement start;
	/** Drawn position when the step ends (near TileStep(stepDirection)). */
	SubTileDisplacement end;
	/** False means "use the classic fixed 8-direction offset". */
	bool active = false;

	/**
	 * @brief Drawn position part way through the step.
	 * @param progress Animation progress in AnimationInfo::baseValueFraction units (0..baseValueFraction).
	 */
	[[nodiscard]] SubTileDisplacement at(uint8_t progress) const;

	/** Drawn offset at the end of the step, relative to the centre of the tile being walked to. */
	[[nodiscard]] SubTileDisplacement carryAfter(Direction stepDirection) const
	{
		return end - TileStep(stepDirection);
	}
};

/** Number of 8-connected steps between two tiles. */
constexpr int StepsBetween(Point from, Point to)
{
	// Not std::abs: it is not constexpr before C++23 (MSVC rejects it here).
	const int dx = to.x > from.x ? to.x - from.x : from.x - to.x;
	const int dy = to.y > from.y ? to.y - from.y : from.y - to.y;
	return dx > dy ? dx : dy;
}

/**
 * @brief Plans the drawn path of one logical step.
 *
 * The drawn position moves an equal share of the remaining way towards the
 * goal each step, then is clamped into the square of the tile being walked
 * to. Walking straight at a goal therefore draws a straight line at any angle,
 * and changing the goal bends the line smoothly.
 *
 * @param carry Drawn offset at the start of the step, relative to the tile being left.
 * @param stepDirection The logical step being taken.
 * @param goal The point being walked towards, relative to the tile being left.
 * @param stepsToGoal Logical steps left to reach the goal (clamped to at least 1).
 */
FreeformStep PlanFreeformStep(SubTileDisplacement carry, Direction stepDirection, SubTileDisplacement goal, int stepsToGoal);

/**
 * @brief Converts a world sub-tile offset to screen pixels.
 *
 * Matches the classic walking offsets, e.g. a full South step is {0, 32} and a full East step is {64, 0}.
 */
Displacement SubTileToScreen(SubTileDisplacement offset);

/**
 * @brief Converts a world sub-tile offset to light offset units (1/8 tile per axis, see Displacement::screenToLight).
 */
Displacement SubTileToLight(SubTileDisplacement offset);

/**
 * @brief Picks the 8-direction sprite facing closest to a world-space movement vector.
 *
 * Uses eight equal 45 degree sectors in world space. When the vector is near
 * a sector edge the current facing is kept (hysteresis) so facings do not
 * flicker on lines that run between two directions.
 *
 * @param vector Movement vector in world space (any scale).
 * @param current Facing to keep when the vector is zero or close to the current sector.
 */
Direction FacingFromVector(SubTileDisplacement vector, Direction current);

/**
 * @brief Returns the tile k steps along the 8-connected straight line from origin to goal.
 *
 * k = 0 is origin and k = StepsBetween(origin, goal) is goal. Every point is
 * one step from the previous one, so walking the points in order is a valid
 * step sequence.
 */
Point LinePoint(Point origin, Point goal, int k);

/**
 * @brief The drawn point an actor should reach after its next step towards a goal.
 *
 * Moves an equal share of the remaining way from the carried offset to the goal.
 *
 * @param carry Drawn offset relative to the current tile centre.
 * @param goal Goal relative to the current tile centre.
 * @param stepsToGoal Logical steps left to reach the goal (clamped to at least 1).
 */
SubTileDisplacement IdealNextPoint(SubTileDisplacement carry, SubTileDisplacement goal, int stepsToGoal);

/** Squared length of a world sub-tile vector. */
constexpr int64_t LengthSquared(SubTileDisplacement v)
{
	return static_cast<int64_t>(v.deltaX) * v.deltaX + static_cast<int64_t>(v.deltaY) * v.deltaY;
}

/**
 * @brief Picks the step from a tile that best follows the straight line to a goal.
 *
 * The step whose tile centre is closest to IdealNextPoint() is preferred, then
 * its two neighbouring directions (closer one first). The first one canStep
 * accepts is returned. Passing the actor's carried drawn offset makes repeated
 * calls trace a true straight line (Bresenham-like) even though each call only
 * looks one step ahead.
 *
 * @param from Current tile.
 * @param goal Tile being walked towards.
 * @param carry Drawn offset relative to the centre of from (use {} if unknown).
 * @param canStep Predicate taking a Direction and returning true if the step is allowed.
 * @return The chosen direction or Direction::NoDirection if none is allowed or from == goal.
 */
template <typename CanStep>
Direction ChooseStepToward(Point from, Point goal, SubTileDisplacement carry, CanStep &&canStep)
{
	const int steps = StepsBetween(from, goal);
	if (steps == 0)
		return Direction::NoDirection;

	const SubTileDisplacement ideal = IdealNextPoint(carry, ToSubTile(goal - from), steps);
	const auto distanceSquared = [&ideal](Direction dir) {
		return LengthSquared(TileStep(dir) - ideal);
	};

	Direction best = Direction::South;
	for (int d = 1; d < 8; d++) {
		const Direction dir = static_cast<Direction>(d);
		if (distanceSquared(dir) < distanceSquared(best))
			best = dir;
	}
	if (canStep(best))
		return best;

	const Direction left = Left(best);
	const Direction right = Right(best);
	const bool leftFirst = distanceSquared(left) <= distanceSquared(right);
	const Direction first = leftFirst ? left : right;
	const Direction second = leftFirst ? right : left;
	if (canStep(first))
		return first;
	if (canStep(second))
		return second;
	return Direction::NoDirection;
}

/**
 * @brief Straightens a tile path by string pulling.
 *
 * Starting from the first tile, the furthest later tile that can be reached by
 * the 8-connected straight line (LinePoint) with every step allowed replaces
 * the run of the path in between. A single original step is always kept as is,
 * so the result is valid whenever the input is. The result never has more
 * steps than the input and ends on the same tile.
 *
 * @param tiles The path as tiles: tiles[0] is the start and each tile is one step from the previous one. Rewritten in place.
 * @param count Number of tiles including the start.
 * @param canStep Predicate taking (Point from, Point to) of a single step and returning true if the step is allowed.
 * @return The new number of tiles including the start.
 */
template <typename CanStep>
size_t StraightenPath(Point *tiles, size_t count, CanStep &&canStep)
{
	if (count < 3)
		return count;

	// The output is never longer than the input, so it can be written over the
	// part of the path that has already been read: tiles[out] always holds the
	// tile the path continues from and nothing after tiles[i] is written yet.
	size_t out = 0;
	size_t i = 0;
	while (i + 1 < count) {
		const Point from = tiles[i];
		size_t next = i + 1;
		for (size_t j = count - 1; j > i + 1; j--) {
			const Point to = tiles[j];
			const int steps = StepsBetween(from, to);
			bool ok = true;
			Point previous = from;
			for (int k = 1; k <= steps && ok; k++) {
				const Point point = LinePoint(from, to, k);
				ok = canStep(previous, point);
				previous = point;
			}
			if (ok) {
				next = j;
				break;
			}
		}

		const Point to = tiles[next];
		const int steps = StepsBetween(from, to);
		for (int k = 1; k <= steps; k++)
			tiles[out + k] = LinePoint(from, to, k);
		out += steps;
		i = next;
	}
	return out + 1;
}

/**
 * @brief Picks how far along a path the drawn position can head in a straight line.
 *
 * Returns the furthest index j such that the path from tiles[0] to tiles[j]
 * takes the minimum number of steps and every tile on the way is within half a
 * tile (per axis) of the true straight line between the two. Walking towards
 * tiles[j] with PlanFreeformStep then draws a straight line that stays inside
 * every tile on the way.
 *
 * @param tiles The path as tiles: tiles[0] is the current tile and each tile is one step from the previous one.
 * @param count Number of tiles including the current one.
 * @return Index of the goal tile; 1 if no longer straight run exists, 0 if count < 2.
 */
size_t FurthestStraightGoal(const Point *tiles, size_t count);

} // namespace devilution
