#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <execution>
#include <type_traits>
#include <version>

#include "engine/render/clx_normal_map.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/light_render.hpp"
#include "utils/attributes.h"
#include "utils/palette_blending.hpp"

namespace devilution {

#if __cpp_lib_execution >= 201902L
#define DEVILUTIONX_BLIT_EXECUTION_POLICY std::execution::unseq,
#else
#define DEVILUTIONX_BLIT_EXECUTION_POLICY
#endif

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillDirect(uint8_t *dst, unsigned length, uint8_t color)
{
	DVL_ASSUME(length != 0);
	std::memset(dst, color, length);
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsDirect(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length)
{
	DVL_ASSUME(length != 0);
	std::memcpy(dst, src, length);
}

struct BlitDirect {
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsDirect(dst, src, length);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillDirect(dst, length, color);
	}
};

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillWithMap(uint8_t *dst, unsigned length, uint8_t color, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	std::memset(dst, colorMap[color], length);
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsWithMap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, dst, [colorMap](uint8_t srcColor) { return colorMap[srcColor]; });
}

struct BlitWithMap {
	const uint8_t *DVL_RESTRICT colorMap;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsWithMap(dst, src, length, colorMap);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillWithMap(dst, length, color, colorMap);
	}
};

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillWithLightmap(uint8_t *dst, unsigned length, uint8_t color, const Lightmap &lightmap)
{
	DVL_ASSUME(length != 0);
	const uint8_t *light = lightmap.getLightingAt(dst);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY light, light + length, dst, [color, &lightmap](uint8_t lightLevel) {
		return lightmap.adjustColor(color, lightLevel);
	});
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsWithLightmap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const Lightmap &lightmap)
{
	DVL_ASSUME(length != 0);
	const uint8_t *light = lightmap.getLightingAt(dst);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, light, dst, [&lightmap](uint8_t srcColor, uint8_t lightLevel) {
		return lightmap.adjustColor(srcColor, lightLevel);
	});
}

struct BlitWithLightmap {
	const Lightmap &lightmap;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsWithLightmap(dst, src, length, lightmap);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillWithLightmap(dst, length, color, lightmap);
	}
};

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillBlended(uint8_t *dst, unsigned length, uint8_t color)
{
	DVL_ASSUME(length != 0);
	std::for_each(DEVILUTIONX_BLIT_EXECUTION_POLICY dst, dst + length, [tbl = paletteTransparencyLookup[color]](uint8_t &dstColor) {
		dstColor = tbl[dstColor];
	});
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlended(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length)
{
	DVL_ASSUME(length != 0);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, dst, dst, [pal = paletteTransparencyLookup](uint8_t srcColor, uint8_t dstColor) {
		return pal[srcColor][dstColor];
	});
}

struct BlitBlended {
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsBlended(dst, src, length);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillBlended(dst, length, color);
	}
};

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlendedWithMap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const uint8_t *DVL_RESTRICT colorMap)
{
	DVL_ASSUME(length != 0);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, dst, dst, [colorMap, pal = paletteTransparencyLookup](uint8_t srcColor, uint8_t dstColor) {
		return pal[dstColor][colorMap[srcColor]];
	});
}

struct BlitBlendedWithMap {
	const uint8_t *DVL_RESTRICT colorMap;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsBlendedWithMap(dst, src, length, colorMap);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillBlended(dst, length, colorMap[color]);
	}
};

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitFillBlendedWithLightmap(uint8_t *dst, unsigned length, uint8_t color, const Lightmap &lightmap)
{
	DVL_ASSUME(length != 0);
	const uint8_t *light = lightmap.getLightingAt(dst);
	std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY light, light + length, dst, dst, [color, &lightmap, pal = paletteTransparencyLookup](uint8_t lightLevel, uint8_t dstColor) {
		uint8_t srcColor = lightmap.adjustColor(color, lightLevel);
		return pal[srcColor][dstColor];
	});
}

DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void BlitPixelsBlendedWithLightmap(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src, unsigned length, const Lightmap &lightmap)
{
	DVL_ASSUME(length != 0);
	const uint8_t *light = lightmap.getLightingAt(dst);

	if (length < 1024) {
		uint8_t litSrc[1024];
		std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY src, src + length, light, litSrc, [&lightmap](uint8_t srcColor, uint8_t lightLevel) {
			return lightmap.adjustColor(srcColor, lightLevel);
		});
		std::transform(DEVILUTIONX_BLIT_EXECUTION_POLICY litSrc, litSrc + length, dst, dst, [pal = paletteTransparencyLookup](uint8_t srcColor, uint8_t dstColor) {
			return pal[dstColor][srcColor];
		});
		return;
	}

	for (size_t i = 0; i < length; i++) {
		uint8_t srcColor = src[i];
		uint8_t dstColor = dst[i];
		uint8_t lightLevel = light[i];
		uint8_t litColor = lightmap.adjustColor(srcColor, lightLevel);
		dst[i] = paletteTransparencyLookup[dstColor][litColor];
	}
}

struct BlitBlendedWithLightmap {
	const Lightmap &lightmap;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		BlitPixelsBlendedWithLightmap(dst, src, length, lightmap);
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		BlitFillBlendedWithLightmap(dst, length, color, lightmap);
	}
};

// ---- 2.5D hybrid lighting G-buffer writers ----
//
// The blit functor contract carries no coordinates, so the aux writers derive the back-buffer row from
// `dst` once per run with the exact reciprocal multiply (no div/mod: CLX runs are mostly 1-16 px wide).
// `feetY` / `left` are absolute back-buffer coordinates (the caller resolves subregion offsets).
//
// CLX runs are tiny, so a libc memset/memcpy call per plane write costs as much as the pixel work itself.
// Runs of up to 32 bytes are written with two overlapping fixed-size stores per plane (every store stays
// inside [dst, dst + n)), and for the plain `BlitDirect` inner the colour run is written by the same
// size cascade as the planes, so the hybrid path pays one run-length dispatch per run, like the classic
// libc memcpy does, instead of one per plane.

namespace gbuffer_blit {

/** Longest run written with fixed-size stores; longer runs take one libc call per plane. */
constexpr unsigned MaxFixedRun = 32;

DVL_ALWAYS_INLINE uint64_t Replicate8(uint8_t value)
{
	return static_cast<uint64_t>(value) * 0x0101010101010101ULL;
}

template <unsigned N>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void CopyFixed(uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src)
{
	std::memcpy(dst, src, N);
}

/** Writes the low N bytes of a replicated byte (N <= 16). */
template <unsigned N>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void FillFixed(uint8_t *dst, uint64_t value8)
{
	if constexpr (N == 16) {
		std::memcpy(dst, &value8, 8);
		std::memcpy(dst + 8, &value8, 8);
	} else {
		std::memcpy(dst, &value8, N);
	}
}

/**
 * @brief Writes an n-byte run through `ops`: two overlapping fixed-size writers for n <= MaxFixedRun
 * (at offsets 0 and n - N, so every byte is written and nothing outside the run is touched), otherwise
 * the variable-length fallback.
 */
template <typename Ops>
DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void WriteRun(unsigned n, const Ops &ops)
{
	DVL_ASSUME(n != 0);
	if (n > MaxFixedRun) [[unlikely]] {
		ops.variable(n);
	} else if (n >= 16) {
		ops.template fixed<16>(0);
		ops.template fixed<16>(n - 16);
	} else if (n >= 8) {
		ops.template fixed<8>(0);
		ops.template fixed<8>(n - 8);
	} else if (n >= 4) {
		ops.template fixed<4>(0);
		ops.template fixed<4>(n - 4);
	} else if (n >= 2) {
		ops.template fixed<2>(0);
		ops.template fixed<2>(n - 2);
	} else {
		ops.template fixed<1>(0);
	}
}

/** aux <- row-span normal bytes, base <- constant feet byte (BlitWithGBuffer). */
struct PlaneOps {
	uint8_t *DVL_RESTRICT aux;
	const uint8_t *DVL_RESTRICT normals;
	uint8_t *DVL_RESTRICT base;
	uint64_t base8;

	template <unsigned N>
	DVL_ALWAYS_INLINE void fixed(unsigned o) const
	{
		CopyFixed<N>(aux + o, normals + o);
		FillFixed<N>(base + o, base8);
	}
	DVL_ALWAYS_INLINE void variable(unsigned n) const
	{
		std::memcpy(aux, normals, n);
		std::memset(base, static_cast<uint8_t>(base8), n);
	}
};

/** aux <- constant byte, base <- constant byte (BlitWithConstAux). */
struct ConstOps {
	uint8_t *DVL_RESTRICT aux;
	uint8_t *DVL_RESTRICT base;
	uint64_t aux8;
	uint64_t base8;

	template <unsigned N>
	DVL_ALWAYS_INLINE void fixed(unsigned o) const
	{
		FillFixed<N>(aux + o, aux8);
		FillFixed<N>(base + o, base8);
	}
	DVL_ALWAYS_INLINE void variable(unsigned n) const
	{
		std::memset(aux, static_cast<uint8_t>(aux8), n);
		std::memset(base, static_cast<uint8_t>(base8), n);
	}
};

/** aux/base <- constants, except where aux already holds a card: a blended arch over a sprite keeps the sprite's lighting. */
struct ConstOpsKeepCards {
	uint8_t *DVL_RESTRICT aux;
	uint8_t *DVL_RESTRICT base;
	uint8_t auxByte;
	uint8_t baseByte;

	DVL_ALWAYS_INLINE void write(unsigned o, unsigned n) const
	{
		for (unsigned i = o; i < o + n; ++i) {
			const gbuffer::Kind kind = gbuffer::AuxKind(aux[i]);
			if (kind == gbuffer::Kind::Card || kind == gbuffer::Kind::CardEmissive) continue;
			aux[i] = auxByte;
			base[i] = baseByte;
		}
	}
	template <unsigned N>
	DVL_ALWAYS_INLINE void fixed(unsigned o) const { write(o, N); }
	DVL_ALWAYS_INLINE void variable(unsigned n) const { write(0, n); }
};

/** aux <- Unlit (BlitStampUnlit); plane B is untouched. */
struct StampOps {
	uint8_t *DVL_RESTRICT aux;

	template <unsigned N>
	DVL_ALWAYS_INLINE void fixed(unsigned o) const
	{
		FillFixed<N>(aux + o, Replicate8(gbuffer::UnlitByte));
	}
	DVL_ALWAYS_INLINE void variable(unsigned n) const
	{
		std::memset(aux, gbuffer::UnlitByte, n);
	}
};

/** `BlitDirect` pixel run fused with the plane writes: one size dispatch for all planes. */
template <typename Ops>
struct PixelsThen {
	uint8_t *DVL_RESTRICT dst;
	const uint8_t *DVL_RESTRICT src;
	Ops ops;

	template <unsigned N>
	DVL_ALWAYS_INLINE void fixed(unsigned o) const
	{
		CopyFixed<N>(dst + o, src + o);
		ops.template fixed<N>(o);
	}
	DVL_ALWAYS_INLINE void variable(unsigned n) const
	{
		std::memcpy(dst, src, n);
		ops.variable(n);
	}
};

/** `BlitDirect` fill run fused with the plane writes. */
template <typename Ops>
struct FillThen {
	uint8_t *DVL_RESTRICT dst;
	uint64_t color8;
	Ops ops;

	template <unsigned N>
	DVL_ALWAYS_INLINE void fixed(unsigned o) const
	{
		FillFixed<N>(dst + o, color8);
		ops.template fixed<N>(o);
	}
	DVL_ALWAYS_INLINE void variable(unsigned n) const
	{
		std::memset(dst, static_cast<uint8_t>(color8), n);
		ops.variable(n);
	}
};

template <typename Inner>
constexpr bool FusesWithPlanes = std::is_same_v<Inner, BlitDirect>;

} // namespace gbuffer_blit

struct GBufferCardContext {
	const uint8_t *outBase;
	uint8_t *aux;
	uint8_t *base;
	uint16_t pitch;
	uint64_t pitchRecip40;
	int left;                    // absolute back-buffer x of the sprite's left edge (may be negative when clipped)
	int feetY;                   // absolute back-buffer row of the sprite's feet (bottom) row
	uint8_t feetByte;            // feetY & 0xFF, written to plane B
	clx_normal_map::Plane plane; // row-span packed; plane.data != nullptr here
};

/** Card with a generated normal plane: copies the row-span aux bytes and writes the feet row to plane B. */
template <typename Inner>
struct BlitWithGBuffer {
	Inner inner;
	GBufferCardContext ctx;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT gbuffer_blit::PlaneOps planeOps(const uint8_t *dst) const
	{
		const ptrdiff_t off = dst - ctx.outBase;
		const auto row = static_cast<uint32_t>((static_cast<uint64_t>(off) * ctx.pitchRecip40) >> 40); // exact, no div
		const int col = static_cast<int>(off - static_cast<ptrdiff_t>(row) * ctx.pitch);
		const auto r = static_cast<unsigned>(ctx.feetY - static_cast<int>(row)); // 0 = feet row
		// The run lies inside the row's span by construction (the plane covers every opaque pixel).
		const uint8_t *normals = ctx.plane.data + ctx.plane.rowOffset[r] + (col - ctx.left - ctx.plane.rowX0[r]);
		return gbuffer_blit::PlaneOps { ctx.aux + off, normals, ctx.base + off, gbuffer_blit::Replicate8(ctx.feetByte) };
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		if constexpr (gbuffer_blit::FusesWithPlanes<Inner>) {
			gbuffer_blit::WriteRun(length, gbuffer_blit::PixelsThen<gbuffer_blit::PlaneOps> { dst, src, planeOps(dst) });
		} else {
			inner(length, dst, src);
			gbuffer_blit::WriteRun(length, planeOps(dst));
		}
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		if constexpr (gbuffer_blit::FusesWithPlanes<Inner>) {
			gbuffer_blit::WriteRun(length, gbuffer_blit::FillThen<gbuffer_blit::PlaneOps> { dst, gbuffer_blit::Replicate8(color), planeOps(dst) });
		} else {
			inner(length, color, dst);
			gbuffer_blit::WriteRun(length, planeOps(dst));
		}
	}
};

/** Constant aux + base per run: CardEmissive, flat-tier cards (plane.data == nullptr), arches (WallLeft/WallRight, foot None). */
template <typename Inner>
struct BlitWithConstAux {
	Inner inner;
	uint8_t *aux;
	uint8_t *base;
	const uint8_t *outBase;
	uint8_t auxByte;
	uint8_t baseByte;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT gbuffer_blit::ConstOps constOps(const uint8_t *dst) const
	{
		const ptrdiff_t off = dst - outBase;
		return gbuffer_blit::ConstOps { aux + off, base + off, gbuffer_blit::Replicate8(auxByte), gbuffer_blit::Replicate8(baseByte) };
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		if constexpr (gbuffer_blit::FusesWithPlanes<Inner>) {
			gbuffer_blit::WriteRun(length, gbuffer_blit::PixelsThen<gbuffer_blit::ConstOps> { dst, src, constOps(dst) });
		} else {
			inner(length, dst, src);
			gbuffer_blit::WriteRun(length, constOps(dst));
		}
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		if constexpr (gbuffer_blit::FusesWithPlanes<Inner>) {
			gbuffer_blit::WriteRun(length, gbuffer_blit::FillThen<gbuffer_blit::ConstOps> { dst, gbuffer_blit::Replicate8(color), constOps(dst) });
		} else {
			inner(length, color, dst);
			gbuffer_blit::WriteRun(length, constOps(dst));
		}
	}
};

/** Arches blended over sprites: constant aux/base like BlitWithConstAux, but card pixels underneath keep their aux/base. */
template <typename Inner>
struct BlitWithConstAuxKeepCards {
	Inner inner;
	uint8_t *aux;
	uint8_t *base;
	const uint8_t *outBase;
	uint8_t auxByte;
	uint8_t baseByte;

	DVL_ALWAYS_INLINE gbuffer_blit::ConstOpsKeepCards ops(const uint8_t *dst) const
	{
		const ptrdiff_t off = dst - outBase;
		return gbuffer_blit::ConstOpsKeepCards { aux + off, base + off, auxByte, baseByte };
	}
	DVL_ALWAYS_INLINE void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		inner(length, dst, src);
		gbuffer_blit::WriteRun(length, ops(dst));
	}
	DVL_ALWAYS_INLINE void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		inner(length, color, dst);
		gbuffer_blit::WriteRun(length, ops(dst));
	}
};

/** UI stamping (StampUnlit mode): every written pixel gets aux = Unlit; plane B is untouched. */
template <typename Inner>
struct BlitStampUnlit {
	Inner inner;
	uint8_t *aux;
	const uint8_t *outBase;

	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t *DVL_RESTRICT dst, const uint8_t *DVL_RESTRICT src) const
	{
		const gbuffer_blit::StampOps ops { aux + (dst - outBase) };
		if constexpr (gbuffer_blit::FusesWithPlanes<Inner>) {
			gbuffer_blit::WriteRun(length, gbuffer_blit::PixelsThen<gbuffer_blit::StampOps> { dst, src, ops });
		} else {
			inner(length, dst, src);
			gbuffer_blit::WriteRun(length, ops);
		}
	}
	DVL_ALWAYS_INLINE DVL_ATTRIBUTE_HOT void operator()(unsigned length, uint8_t color, uint8_t *DVL_RESTRICT dst) const
	{
		const gbuffer_blit::StampOps ops { aux + (dst - outBase) };
		if constexpr (gbuffer_blit::FusesWithPlanes<Inner>) {
			gbuffer_blit::WriteRun(length, gbuffer_blit::FillThen<gbuffer_blit::StampOps> { dst, gbuffer_blit::Replicate8(color), ops });
		} else {
			inner(length, color, dst);
			gbuffer_blit::WriteRun(length, ops);
		}
	}
};

} // namespace devilution
