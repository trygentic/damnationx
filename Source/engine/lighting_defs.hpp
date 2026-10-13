#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

#include "engine/point.hpp"

namespace devilution {

#define MAXLIGHTS 32
#define MAXVISION 4
#define NO_LIGHT -1

constexpr char LightsMax = 15;

/** @brief A light table maps palette indices, so its size is the same as the palette size. */
constexpr size_t LightTableSize = 256;

/** @brief Number of supported light levels */
constexpr size_t NumLightingLevels = LightsMax + 1;

/** Presentation-only owner categories for render-light snapshots. */
enum class RenderLightOwner : uint8_t {
	Player,
	Object,
	Missile,
	ModEffect,
};

/** Generation-safe identity for a presentation light owner. */
struct RenderLightOwnerId {
	uint32_t index = 0;
	uint32_t generation = 0;

	constexpr auto operator<=>(const RenderLightOwnerId &) const = default;
};

enum class RenderLightFalloff : uint8_t {
	Linear,
	Quadratic,
};

/** Immutable presentation data extracted after the authoritative game tick. */
struct RenderLightInstance {
	uint64_t stableId = 0;
	RenderLightOwner owner = RenderLightOwner::Player;
	RenderLightOwnerId ownerId;
	Point position;
	int height = 0;
	std::array<uint8_t, 3> color {255, 255, 255};
	uint16_t radius = 0;
	uint16_t intensity = 0;
	RenderLightFalloff falloff = RenderLightFalloff::Quadratic;
	bool visible = false;
	bool occlusionHint = false;
	uint32_t descriptorId = 0;
};

constexpr size_t RenderLightCapacityLow = 32;
constexpr size_t RenderLightCapacityHigh = 128;
constexpr size_t RenderLightSnapshotCapacity = RenderLightCapacityHigh;

struct RenderLightSnapshot {
	std::array<RenderLightInstance, RenderLightSnapshotCapacity> lights {};
	size_t count = 0;
	size_t culled = 0;
	uint8_t ambient = 0;
};

} // namespace devilution
