#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace devilution {

/** The normal Diablo section entrance, or zero for Town, set levels and Hellfire extras. */
constexpr uint8_t FreeTownPortalSectionForLevel(uint8_t level)
{
	return level >= 1 && level <= 16 ? static_cast<uint8_t>(1 + ((level - 1) / 4) * 4) : 0;
}

constexpr bool IsFreeTownPortalSection(uint8_t level)
{
	return level != 0 && FreeTownPortalSectionForLevel(level) == level;
}

constexpr uint8_t ValidFreeTownPortalSection(uint8_t section)
{
	return IsFreeTownPortalSection(section) ? section : 1;
}

/** One dedicated native portal index; player-owned portals retain slots 0..3. */
constexpr size_t FreeTownPortalIndex = 4;

/** Existing CMD_WARP uint16 payload: low byte is slot 4, high byte is destination. */
constexpr uint16_t FreeTownPortalWarp(uint8_t section)
{
	return static_cast<uint16_t>((ValidFreeTownPortalSection(section) << 8) | FreeTownPortalIndex);
}

constexpr bool IsFreeTownPortalWarp(size_t index)
{
	// The bare index remains accepted for native callers from before section routing.
	return index == FreeTownPortalIndex
	    || (index <= 0xFFFF && (index & 0xFF) == FreeTownPortalIndex && IsFreeTownPortalSection(static_cast<uint8_t>(index >> 8)));
}

constexpr uint8_t FreeTownPortalWarpDestination(size_t index, uint8_t fallback)
{
	return index == FreeTownPortalIndex ? ValidFreeTownPortalSection(fallback) : ValidFreeTownPortalSection(static_cast<uint8_t>(index >> 8));
}

/** Tagged extension in four of the twenty previously unused player save bytes. */
using FreeTownPortalSaveState = std::array<uint8_t, 4>;

constexpr FreeTownPortalSaveState EncodeFreeTownPortalState(uint8_t section)
{
	return { 'F', 'P', 1, ValidFreeTownPortalSection(section) };
}

constexpr uint8_t DecodeFreeTownPortalState(const FreeTownPortalSaveState &state, uint8_t currentLevel, bool setLevel)
{
	if (state[0] == 'F' && state[1] == 'P' && state[2] == 1 && IsFreeTownPortalSection(state[3]))
		return state[3];
	// Old files contain zeros here. A current normal dungeon floor is an actual
	// visit; Town has no ordering evidence, so migrate conservatively to level 1.
	return !setLevel && FreeTownPortalSectionForLevel(currentLevel) != 0 ? FreeTownPortalSectionForLevel(currentLevel) : 1;
}

} // namespace devilution
