// Source/engine/render/hybrid_reference.hpp
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "engine/point.hpp"

namespace devilution::hybrid_reference {

struct Vec2 {
	float x = 0.0F, y = 0.0F;
};
struct Vec3 {
	float x = 0.0F, y = 0.0F, z = 0.0F;
};
struct Rgb {
	float r = 0.0F, g = 0.0F, b = 0.0F;
};
struct MaskTexel {
	float r = 1.0F, g = 1.0F, b = 1.0F, a = 0.0F;
};

/** Mirrors GpuHybridParams + GpuPresentParams field for field. */
struct Params {
	float originX = 0.0F, originY = 0.0F;                                          // screenOriginViewport.xy
	uint32_t viewportHeight = 0;                                                   // screenOriginViewport.z / present.values.x (output rows)
	float xx = 0.0F, xy = 0.0F, yx = 0.0F, yy = 0.0F;                              // screenToWorld
	uint32_t lightCount = 0, gridW = 112, gridH = 112, flags = 8;                  // counts; flags: 2 bloom, 8 hybrid, 16 softShadows, bits 8-11 debugView
	float ambientScale = 0.6F, heightPixelsPerWorldUnit = 32.0F, wrapCard = 0.6F; // shading.x .y .w
	float wrapSurface = 0.35F, receiverNudge = 0.05F, softTapOffset = 0.22F;       // tuning.x .y .z
	uint32_t maskScale = 2, zoom = 4, zoomOffsetX = 0, frameCounter = 0;           // misc (zoom: quarters | -offsetY << 8)
	/** ambientCurve: classic darkening per dLight level (gbuffer::BuildClassicAmbientCurve). Defaults to the linear shape. */
	std::array<float, 16> ambientCurve = { 1.0F, 14.0F / 15, 13.0F / 15, 12.0F / 15, 11.0F / 15, 10.0F / 15, 9.0F / 15, 8.0F / 15,
		7.0F / 15, 6.0F / 15, 5.0F / 15, 4.0F / 15, 3.0F / 15, 2.0F / 15, 1.0F / 15, 0.0F };
	float emissiveScale = 2.0F, exposure = 1.0F;                                   // present.values.y .z
	bool frameHasUnlit = false;                                                    // present.config2.y
};
struct Light {
	Vec3 position;
	float radius = 0.0F, intensity = 0.0F;
	Rgb color;
};
struct Planes {
	const uint8_t *aux = nullptr, *base = nullptr, *index = nullptr;
	uint32_t pitch = 0, width = 0, height = 0;
};
struct Mask {
	MaskTexel *texels = nullptr;
	uint32_t width = 0, height = 0;
}; // width = ceil(W / maskScale), height = ceil(H / maskScale)
struct Footprint {
	Vec2 lo, hi;
};
using Volume = std::span<const uint32_t>; // gridW * gridH cells (layout)
using Lut = std::span<const std::array<float, 4>, 32>;
using Emissive = std::span<const float, 256>;
using Palette = std::span<const Rgb, 256>; // sRGB 0..1

// ---- prelude ----
[[nodiscard]] bool IsWallKind(uint32_t kind);
[[nodiscard]] bool HasHeight(uint32_t kind);
[[nodiscard]] bool PassThrough(uint32_t kind);
[[nodiscard]] uint32_t HeightRows(uint32_t kind, uint32_t b, uint32_t y, uint32_t zoom); // zoom: Params::zoom
[[nodiscard]] uint32_t BaseTexelAt(const Planes &planes, Point px, const Params &p); // zoom-aware, robust (out of range → 0)
[[nodiscard]] uint32_t AuxTexelAt(const Planes &planes, Point px);
[[nodiscard]] uint32_t IndexTexelAt(const Planes &planes, Point px);

// ---- compute pass ----
[[nodiscard]] Vec2 ScreenToWorld(Vec2 screen, const Params &p);
[[nodiscard]] uint32_t CellRaw(Point c, Volume v, const Params &p);
[[nodiscard]] uint32_t CellForMarch(Point c, Volume v, const Params &p);
[[nodiscard]] float AmbientOfCell(Point c, Volume v, const Params &p);
[[nodiscard]] float LevelBilinear(Vec2 ground, Volume v, const Params &p);
[[nodiscard]] float LightGate(Vec2 ground, Volume v, const Params &p);
[[nodiscard]] float AmbientBilinear(Vec2 ground, Volume v, const Params &p);
[[nodiscard]] float AmbientBilinearClamped(Vec2 ground, Point own, Volume v, const Params &p);
[[nodiscard]] Point WallOwnCell(Vec2 ground);
[[nodiscard]] float Visibility3D(Vec3 p, Vec3 l, Point skipA, Point skipB, Volume v, const Params &params);
[[nodiscard]] float SoftVisibility3D(Vec3 p, Vec3 l, Point skipA, Point skipB, Volume v, const Params &params);
[[nodiscard]] uint32_t OrderedKey(float f); // f32 -> u32 with the same order, never 0 for a finite value
[[nodiscard]] float KeyToF32(uint32_t k);
[[nodiscard]] bool LightTouches(const Footprint &fp, const Light &l);
/** Bit i: light i is processed. Bits past params.lightCount / lights.size() are ignored, so ~0U means "no culling". */
[[nodiscard]] Rgb AccumulateMasked(Vec3 p, Vec3 n, Vec2 ground, Point own, Point skipA, float wrap, bool gated, uint32_t lightMask, Volume v, std::span<const Light> lights, const Params &params);
[[nodiscard]] uint32_t FootprintLightMask(const Footprint &fp, std::span<const Light> lights, const Params &params);
/** Footprint form (the HD sprite fragment shader): AccumulateMasked over the lights touching fp. */
[[nodiscard]] Rgb Accumulate(Vec3 p, Vec3 n, Vec2 ground, Point own, Point skipA, float wrap, bool gated, const Footprint &fp, Volume v, std::span<const Light> lights, const Params &params);
[[nodiscard]] Rgb DebugVolumeColor(Point cell, Volume v, const Params &p);
/** One mask texel before lighting: `store` is its final value unless `shade` (pass-through, debug view 5). */
struct Receiver {
	bool shade = false;
	MaskTexel store {};
	uint32_t kind = 0, nIdx = 0;
	Vec2 ground;
	float hf = 0.0F;
};
[[nodiscard]] Receiver ReceiverOf(Point maskTexel, const Planes &planes, Volume v, const Params &p);
[[nodiscard]] MaskTexel ShadeReceiver(const Receiver &r, uint32_t lightMask, Volume v, std::span<const Light> lights, Lut lut, const Params &p);
/** The workgroup reduction of hybridLightMain: the shaded receivers' footprint; `any` is false when the block shades nothing. */
[[nodiscard]] Footprint WorkgroupFootprint(Point workgroup, const Planes &planes, Volume v, const Params &p, bool &any);
/** The workgroup's light mask: the lights touching WorkgroupFootprint, 0 when nothing in the block is shaded. */
[[nodiscard]] uint32_t WorkgroupLightMask(Point workgroup, const Planes &planes, Volume v, std::span<const Light> lights, const Params &p);
/** hybridLightMain for one mask texel (mx, my), including the pass-through stores and debug view 5. */
[[nodiscard]] MaskTexel LightTexel(Point maskTexel, const Planes &planes, Volume v, std::span<const Light> lights, Lut lut, const Params &p);
void RunLightPass(Mask &mask, const Planes &planes, Volume v, std::span<const Light> lights, Lut lut, const Params &p);

// ---- present pass ----
[[nodiscard]] Rgb SrgbToLinear(Rgb c);
[[nodiscard]] Rgb LinearToSrgb(Rgb c);
[[nodiscard]] float PaletteEmissiveAt(Emissive e, uint32_t idx);
[[nodiscard]] Rgb SampleLighting(Point px, uint32_t kind, uint32_t h, const Mask &mask, const Planes &planes, const Params &p);
[[nodiscard]] Rgb FrameColor(Point px, Rgb frameSrgb, Palette palette, uint32_t idx, const Params &p);
[[nodiscard]] Rgb DebugColor(uint32_t view, Point px, uint32_t aux, uint32_t idx, Rgb frame, const Planes &planes, const Mask &mask, Emissive e, Lut lut, const Params &p);
/** fragmentMain for one output pixel: frameSrgb is the RGBA frame texel (panel rows / gamepad), palette the SDL palette. */
[[nodiscard]] Rgb Composite(Point px, Rgb frameSrgb, Palette palette, const Planes &planes, const Mask &mask, Emissive e, Lut lut, const Params &p);

} // namespace devilution::hybrid_reference
