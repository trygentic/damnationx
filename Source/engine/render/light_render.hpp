#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/lighting_defs.hpp"
#include "engine/point.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

// Shared native/browser forward-lighting contract. Values crossing the renderer
// boundary are linear (not sRGB) and use the world-isometric coordinate frame.
struct LinearColor {
	float red = 0.0F;
	float green = 0.0F;
	float blue = 0.0F;
};

struct LightRenderNormal {
	// x points east, y points south, and z points up in world-isometric space.
	// Heights are world units above the tile plane; mirrored sprites must negate
	// x after transforming their frame-space normal into this basis.
	//
	// Relation to the tile-space basis of the 2.5D hybrid G-buffer (gbuffer.hpp:
	// tx = tile +x = Direction::SouthEast = screen (+32,+16) px per tile, ty = tile +y =
	// Direction::SouthWest = screen (-32,+16), tz = up; 1 unit = one tile edge horizontally,
	// heightPixelsPerWorldUnit (32) screen rows vertically):
	//   east  = (tx - ty) / sqrt(2)   (screen right)
	//   south = (tx + ty) / sqrt(2)   (screen down, toward the viewer)
	//   up    = tz
	float x = 0.0F;
	float y = 0.0F;
	float z = 1.0F;
	float height = 0.0F;
};

enum class LightRenderQuality : uint8_t {
	Low,
	High,
};

struct LightRenderCapabilities {
	bool normalMaps = false;
	bool emissiveMaps = false;
	bool highQuality = false;
};

struct LightRenderBudget {
	uint16_t maxLightsPerPrimitive;
	uint16_t maxLightsPerFragment;

	[[nodiscard]] static constexpr LightRenderBudget For(LightRenderQuality quality)
	{
		return quality == LightRenderQuality::High
		    ? LightRenderBudget { 64, 32 }
		    : LightRenderBudget { 16, 8 };
	}
};

struct ForwardLight {
	Point position;
	float height = 0.0F;
	float radius = 0.0F;
	LinearColor color;
	float intensity = 1.0F;
};

struct LightRenderMaterial {
	LinearColor ambient { 1.0F, 1.0F, 1.0F };
	LinearColor emissive;
	bool hasNormalMap = false;
	bool hasEmissiveMap = false;
	bool legacyPrelit = false;
	bool ui = false;
};

constexpr size_t MaxForwardLights = 64;

struct ForwardLightList {
	std::array<const ForwardLight *, MaxForwardLights> lights {};
	size_t count = 0;

	[[nodiscard]] std::span<const ForwardLight *const> span() const { return { lights.data(), count }; }
};

[[nodiscard]] ForwardLightList BuildForwardLightList(std::span<const ForwardLight> visibleLights, Point primitivePosition, LightRenderQuality quality);

[[nodiscard]] constexpr LightRenderNormal MirroredNormal(LightRenderNormal normal, bool mirrored)
{
	if (mirrored)
		normal.x = -normal.x;
	return normal;
}

// A missing normal map is a flat, upward-facing surface. Emissive is additive,
// while legacy-prelit and UI materials bypass dynamic lighting entirely.
[[nodiscard]] constexpr LightRenderNormal ResolveNormal(LightRenderNormal sampledNormal, bool hasNormalMap)
{
	return hasNormalMap ? sampledNormal : LightRenderNormal {};
}

[[nodiscard]] constexpr LightRenderNormal ResolveNormal(bool hasNormalMap)
{
	// Callers without a sampled map use the documented flat-normal default.
	return ResolveNormal(LightRenderNormal {}, hasNormalMap);
}
[[nodiscard]] constexpr bool UsesDynamicLighting(const LightRenderMaterial &material, const LightRenderCapabilities &capabilities)
{
	return !material.ui && !material.legacyPrelit && (capabilities.normalMaps || !material.hasNormalMap);
}

class Lightmap {
public:
	explicit Lightmap(const uint8_t *outBuffer, std::span<const uint8_t> lightmapBuffer, uint16_t pitch,
	    std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels> lightTables,
	    const uint8_t *fullyLitLightTable, const uint8_t *fullyDarkLightTable)
	    : Lightmap(outBuffer, pitch, lightmapBuffer, pitch, lightTables, fullyLitLightTable, fullyDarkLightTable)
	{
	}

	explicit Lightmap(const uint8_t *outBuffer, uint16_t outPitch,
	    std::span<const uint8_t> lightmapBuffer, uint16_t lightmapPitch,
	    std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels> lightTables,
	    const uint8_t *fullyLitLightTable, const uint8_t *fullyDarkLightTable);

	[[nodiscard]] uint8_t adjustColor(uint8_t color, uint8_t lightLevel) const
	{
		return lightTables[lightLevel][color];
	}

	const uint8_t *getLightingAt(const uint8_t *outLoc) const
	{
		const ptrdiff_t outDist = outLoc - outBuffer;
		const ptrdiff_t rowOffset = outDist % outPitch;

		if (outDist < 0) {
			// In order to support "bleed up" for wall tiles,
			// reuse the first row whenever outLoc is out of bounds
			const int modOffset = rowOffset < 0 ? outPitch : 0;
			return lightmapBuffer.data() + rowOffset + modOffset;
		}

		const ptrdiff_t row = outDist / outPitch;
		return lightmapBuffer.data() + row * lightmapPitch + rowOffset;
	}

	[[nodiscard]] bool isFullyLitLightTable(const uint8_t *lightTable) const { return lightTable == fullyLitLightTable_; }
	[[nodiscard]] bool isFullyDarkLightTable(const uint8_t *lightTable) const { return lightTable == fullyDarkLightTable_; }

	static Lightmap build(bool perPixelLighting, Point tilePosition, Point targetBufferPosition,
	    int viewportWidth, int viewportHeight, int rows, int columns,
	    const uint8_t *outBuffer, uint16_t outPitch,
	    std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels> lightTables,
	    const uint8_t *fullyLitLightTable, const uint8_t *fullyDarkLightTable,
	    const uint8_t tileLights[MAXDUNX][MAXDUNY],
	    uint_fast8_t microTileLen);

	static Lightmap bleedUp(bool perPixelLighting, const Lightmap &source, Point targetBufferPosition, std::span<uint8_t> lightmapBuffer);

private:
	const uint8_t *outBuffer;
	const uint16_t outPitch;

	std::span<const uint8_t> lightmapBuffer;
	const uint16_t lightmapPitch;

	std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels> lightTables;
	const uint8_t *fullyLitLightTable_;
	const uint8_t *fullyDarkLightTable_;
};

} // namespace devilution
