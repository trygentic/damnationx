/**
 * @file hybrid_reference.cpp
 *
 * C++ transliteration of the hybrid WGSL passes, used by hybrid_reference_test as the
 * executable golden for Source/engine/render/webgpu_shaders.hpp.
 *
 * LINE-PARALLEL STRUCTURE (review by diffing against webgpu_shaders.hpp):
 *
 *   WGSL (webgpu_shaders.hpp)          C++ (this file)              notes
 *   ---- prelude ----
 *   K_* / V_* / MAX_WALL_ROWS          KUnlit.. / VBlock.. / MaxWallRows
 *   isWallKind / hasHeight             IsWallKind / HasHeight
 *   heightRows                         HeightRows                   u32 wrap-around, & 255
 *   baseTexelAt                        BaseTexelAt                  textureLoad robust access == TexelOrZero
 *   passThrough                        PassThrough
 *   (textureLoad auxTex / indexTex)    AuxTexelAt / IndexTexelAt    robust: out of range -> 0
 *   ---- compute pass ----
 *   screenToWorld                      ScreenToWorld
 *   cellRaw                            CellRaw                      out of grid -> V_BLOCK | 255
 *   cellForMarch                       CellForMarch                 unexplored -> BlockLight bit + infinite height
 *   ambientOfCell                      AmbientOfCell
 *   ambientBilinear                    AmbientBilinear              mix == Mix (a + (b - a) * t)
 *   ambientBilinearClamped             AmbientBilinearClamped
 *   wallOwnCell                        WallOwnCell                  round == std::nearbyint (round half to even, as WGSL)
 *   frontSpan / belowOn                FrontSpan / BelowOn
 *   blockedInCell                      BlockedInCell                wall cells occlude behind their face planes only
 *   visibility3d                       Visibility3D                 same DDA, same 64-step cap, any(cell != skip)
 *   softVisibility3d                   SoftVisibility3D
 *   orderedKey / keyToF32              OrderedKey / KeyToF32        bitcast == std::memcpy
 *   workgroupFootprint + wg* atomics   WorkgroupFootprint           atomicMax over the block's shaded receivers
 *   lightTouches                       LightTouches
 *   (light-mask step of the main)      WorkgroupLightMask           one bit per touching light, ascending
 *   accumulateMasked                   AccumulateMasked             same clamp/mix/smoothstep semantics; loops the mask bits
 *   footprintLightMask / accumulate    FootprintLightMask / Accumulate   footprint form (HD sprite fragment shader)
 *   debugVolumeColor                   DebugVolumeColor
 *   receiverOf / shadeReceiver         ReceiverOf / ShadeReceiver
 *   hybridLightMain                    LightTexel (one texel) + RunLightPass (the dispatch)
 *   ---- present pass ----
 *   srgbToLinear / linearToSrgb        SrgbToLinear / LinearToSrgb  per component if/else
 *   paletteEmissiveAt                  PaletteEmissiveAt            vec4 local copy + component index
 *   sampleLighting                     SampleLighting               textureLoad(hybridMask) robust == MaskRgbAt
 *   frameColor                         FrameColor
 *   debugColor                         DebugColor
 *   fragmentMain                       Composite
 *
 * Every function keeps the WGSL's statement order; the only differences are C++ spelling (vec2f ->
 * Vec2, textureLoad -> *TexelAt) and the explicit robust-access helpers that stand in for WebGPU's
 * zero result on out-of-range texture reads.
 */
#include "engine/render/hybrid_reference.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace devilution::hybrid_reference {

namespace {

// ---- prelude constants ----
constexpr uint32_t KUnlit = 0;
constexpr uint32_t KFloor = 1;
constexpr uint32_t KWallL = 2;
constexpr uint32_t KWallR = 3;
constexpr uint32_t KCard = 4;
constexpr uint32_t KCardEmissive = 5;
constexpr uint32_t KWallBlended = 6;
constexpr uint32_t KWallTop = 7;
constexpr uint32_t VFaceSW = 256;
constexpr uint32_t VFaceSE = 512;
constexpr uint32_t VBlock = 1024;
constexpr uint32_t VExplored = 8192;
constexpr uint32_t VArch = 16384;
constexpr uint32_t VDoorOpen = 32768;
constexpr uint32_t MaxWallRows = 224; // plane-B wrap guard
constexpr float CellPickOffset = 0.35F; // CELL_PICK_OFFSET in the WGSL

// ---- vector helpers standing in for the WGSL built-ins ----
Vec2 Sub(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
Vec2 Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
Vec2 Scale(Vec2 a, float s) { return { a.x * s, a.y * s }; }
float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 Scale(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
Rgb Add(Rgb a, Rgb b) { return { a.r + b.r, a.g + b.g, a.b + b.b }; }
Rgb Scale(Rgb a, float s) { return { a.r * s, a.g * s, a.b * s }; }
Rgb Mul(Rgb a, Rgb b) { return { a.r * b.r, a.g * b.g, a.b * b.b }; }
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec2 Normalize(Vec2 a)
{
	const float len = std::sqrt(Dot(a, a));
	return { a.x / len, a.y / len };
}
int FloorToInt(float v) { return static_cast<int>(std::floor(v)); }
Point FloorToPoint(Vec2 v) { return { FloorToInt(v.x), FloorToInt(v.y) }; }
Vec2 ToVec2(Point p) { return { static_cast<float>(p.x), static_cast<float>(p.y) }; }
float Mix(float a, float b, float t) { return a + (b - a) * t; }
float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
float Smoothstep(float e0, float e1, float x)
{
	const float t = Clamp((x - e0) / (e1 - e0), 0.0F, 1.0F);
	return t * t * (3.0F - 2.0F * t);
}
Vec2 ClampVec(Vec2 v, Vec2 lo, Vec2 hi) { return { Clamp(v.x, lo.x, hi.x), Clamp(v.y, lo.y, hi.y) }; }
bool AnyNotEqual(Point a, Point b) { return a.x != b.x || a.y != b.y; } // any(a != b)
bool AllEqual(Point a, Point b) { return a.x == b.x && a.y == b.y; }    // all(a == b)

// textureLoad on an r8uint plane: WebGPU robust access returns 0 for any out-of-range coordinate.
uint32_t TexelOrZero(const uint8_t *plane, const Planes &planes, Point px)
{
	if (plane == nullptr || px.x < 0 || px.y < 0 || px.x >= static_cast<int>(planes.width) || px.y >= static_cast<int>(planes.height))
		return 0;
	return plane[static_cast<size_t>(px.y) * planes.pitch + static_cast<size_t>(px.x)];
}
// textureLoad on hybridMask: robust access returns (0, 0, 0, 0) out of range.
Rgb MaskRgbAt(const Mask &mask, Point m)
{
	if (mask.texels == nullptr || m.x < 0 || m.y < 0 || m.x >= static_cast<int>(mask.width) || m.y >= static_cast<int>(mask.height))
		return Rgb {};
	const MaskTexel &t = mask.texels[static_cast<size_t>(m.y) * mask.width + static_cast<size_t>(m.x)];
	return { t.r, t.g, t.b };
}
Vec3 LutNormal(Lut lut, uint32_t idx)
{
	const std::array<float, 4> &e = lut[idx & 31U];
	return { e[0], e[1], e[2] };
}

} // namespace

// ============================================================ prelude ============================================================

bool IsWallKind(uint32_t kind) { return kind == KWallL || kind == KWallR || kind == KWallBlended; }
bool HasHeight(uint32_t kind) { return IsWallKind(kind) || kind == KCard || kind == KWallTop; }
// Rows above the anchor for a plane texel. B holds unzoomed rows and is never scaled up.
uint32_t ZoomQuarters(uint32_t zoom) { return std::max(zoom & 255U, 4U); }
uint32_t ZoomRow(uint32_t y, uint32_t zoom) { return ((y + (zoom >> 8)) * 4U) / ZoomQuarters(zoom); }
uint32_t HeightRows(uint32_t kind, uint32_t b, uint32_t y, uint32_t zoom)
{
	if (!HasHeight(kind)) return 0;
	return (b - ZoomRow(y, zoom)) & 255U;
}
// Zoom-aware B fetch: the world blitters wrote B at world size starting at column 0; zoomOffsetX is the transform's
// offsetX. A negative x after the shift is out of range: robust access returns 0 (h wraps -> guard).
uint32_t BaseTexelAt(const Planes &planes, Point px, const Params &p)
{
	if (ZoomQuarters(p.zoom) == 4) return TexelOrZero(planes.base, planes, px);
	const int sx = px.x - static_cast<int>(p.zoomOffsetX);
	if (sx < 0) return 0;
	return TexelOrZero(planes.base, planes,
	    { (sx * 4) / static_cast<int>(ZoomQuarters(p.zoom)), static_cast<int>(ZoomRow(static_cast<uint32_t>(std::max(px.y, 0)), p.zoom)) });
}
uint32_t AuxTexelAt(const Planes &planes, Point px) { return TexelOrZero(planes.aux, planes, px); }
uint32_t IndexTexelAt(const Planes &planes, Point px) { return TexelOrZero(planes.index, planes, px); }
bool PassThrough(uint32_t kind) { return kind == KUnlit || kind == KCardEmissive; }

// ========================================================= compute pass ==========================================================

Vec2 ScreenToWorld(Vec2 screen, const Params &p)
{
	const Vec2 d = Sub(screen, { p.originX, p.originY });
	return { p.xx * d.x + p.xy * d.y,
		p.yx * d.x + p.yy * d.y };
}
uint32_t CellRaw(Point c, Volume v, const Params &p)
{
	const int w = static_cast<int>(p.gridW);
	const int h = static_cast<int>(p.gridH);
	if (c.x < 0 || c.y < 0 || c.x >= w || c.y >= h) return VBlock | 255U; // out of grid: solid, tall
	const size_t index = static_cast<size_t>(c.y) * p.gridW + static_cast<size_t>(c.x);
	if (index >= v.size()) return VBlock | 255U; // a short span is treated like the grid edge (robust access)
	return v[index];
}
// Hidden-information rule: an unexplored cell exposes only its gameplay BlockLight bit with infinite height and no arch.
uint32_t CellForMarch(Point c, Volume v, const Params &p)
{
	const uint32_t raw = CellRaw(c, v, p);
	if ((raw & VExplored) == 0) return (raw & VBlock) | 255U;
	return raw;
}
// Classic light level of one cell as a float (0 bright .. 15 dark); an unexplored cell is fully dark (levelOfCell).
float LevelOfCell(Point c, Volume v, const Params &p)
{
	const uint32_t raw = CellRaw(c, v, p);
	if ((raw & VExplored) == 0) return 15.0F;
	return static_cast<float>((raw >> 17U) & 15U);
}
// The curve at a fractional level, linear between entries (ambientAtLevel).
float AmbientAtLevel(float level, const Params &p)
{
	const float l = std::clamp(level, 0.0F, 15.0F);
	const size_t i0 = static_cast<size_t>(std::floor(l));
	const size_t i1 = std::min<size_t>(i0 + 1, 15);
	return Mix(p.ambientCurve[i0], p.ambientCurve[i1], l - static_cast<float>(i0));
}
// Classic ambient for one cell from the level's darkening curve (params.ambientCurve); 0 when unexplored.
float AmbientOfCell(Point c, Volume v, const Params &p)
{
	if ((CellRaw(c, v, p) & VExplored) == 0) return 0.0F;
	return AmbientAtLevel(LevelOfCell(c, v, p), p);
}
// Bilinear light level over the 4 nearest tile centres (levelBilinear).
float LevelBilinear(Vec2 ground, Volume v, const Params &p)
{
	const Vec2 q = Sub(ground, { 0.5F, 0.5F });
	const Point c0 = FloorToPoint(q);
	const Vec2 f = Sub(q, ToVec2(c0));
	const float l00 = LevelOfCell(c0, v, p);
	const float l10 = LevelOfCell({ c0.x + 1, c0.y }, v, p);
	const float l01 = LevelOfCell({ c0.x, c0.y + 1 }, v, p);
	const float l11 = LevelOfCell({ c0.x + 1, c0.y + 1 }, v, p);
	return Mix(Mix(l00, l10, f.x), Mix(l01, l11, f.x), f.y);
}
// Interpolate the light level, then map it (ambientBilinear).
float AmbientBilinear(Vec2 ground, Volume v, const Params &p)
{
	return AmbientAtLevel(LevelBilinear(ground, v, p), p);
}
// Direct light fades to 0 at level 15 along the interpolated level (lightGate).
float LightGate(Vec2 ground, Volume v, const Params &p)
{
	return std::clamp(15.0F - LevelBilinear(ground, v, p), 0.0F, 1.0F);
}
// Surfaces: bilinear like cards, zero only for an unexplored own cell (a fully dark explored cell bleeds from lit neighbours).
float AmbientBilinearClamped(Vec2 ground, Point own, Volume v, const Params &p)
{
	if ((CellRaw(own, v, p) & VExplored) == 0) return 0.0F;
	return AmbientBilinear(ground, v, p);
}
// Gate cell of a wall face (wallGateCell in the WGSL): the own cell, or the cell in front of the wall line (half a tile further,
// the line is the cell's centre line) when the own cell carries no Explored bit.
Point WallGateCell(Vec2 ground, Vec2 n, Volume v, const Params &p)
{
	const Point own = FloorToPoint(Sub(ground, Scale(n, CellPickOffset)));
	if ((CellRaw(own, v, p) & VExplored) != 0) return own;
	return FloorToPoint(Add(ground, Scale(n, CellPickOffset + 0.5F)));
}
// Own cell of a foot-less wall receiver (WallTop): u + v == 1.9375 on the base row.
Point WallOwnCell(Vec2 ground)
{
	Point c = FloorToPoint(ground);
	const Vec2 f = Sub(ground, ToVec2(c));
	const int s = static_cast<int>(std::nearbyint(ground.x + ground.y - 1.9375F));
	if (c.x + c.y > s) {
		if (f.x < f.y) {
			c.x -= 1;
		} else {
			c.y -= 1;
		}
	}
	return c;
}
namespace {
// t range in which a + t * da > k: the ray is in front of a face plane at coordinate k (frontSpan).
Vec2 FrontSpan(float a, float da, float k)
{
	if (std::fabs(da) <= 1e-6F) {
		if (a > k) return { -1e9F, 1e9F };
		return { 1e9F, -1e9F };
	}
	const float t = (k - a) / da;
	if (da > 0.0F) return { t, 1e9F };
	return { -1e9F, t };
}
// True when the ray's z is below h somewhere on [ta, tb] (belowOn).
bool BelowOn(Vec3 p, Vec3 d, float ta, float tb, float h) { return std::min(p.z + ta * d.z, p.z + tb * d.z) < h; }
// BlockLight test of one explored cell over [tIn, tOut] (blockedInCell): a wall half stands on the cell's centre line, so only
// the part of the span behind its face planes occludes; a cell without faces occludes as a whole.
bool BlockedInCell(Vec3 p, Vec3 d, Point cell, uint32_t v, float tIn, float tOut, float h)
{
	float lo = tIn;
	float hi = tOut; // the open part in front of every face plane
	if ((v & VFaceSE) != 0) {
		const Vec2 r = FrontSpan(p.x, d.x, static_cast<float>(cell.x) + 0.5F);
		lo = std::max(lo, r.x);
		hi = std::min(hi, r.y);
	}
	if ((v & VFaceSW) != 0) {
		const Vec2 r = FrontSpan(p.y, d.y, static_cast<float>(cell.y) + 0.5F);
		lo = std::max(lo, r.x);
		hi = std::min(hi, r.y);
	}
	if ((v & (VFaceSW | VFaceSE)) == 0 || lo >= hi) return BelowOn(p, d, tIn, tOut, h);
	return (lo > tIn && BelowOn(p, d, tIn, lo, h)) || (hi < tOut && BelowOn(p, d, hi, tOut, h));
}
} // namespace
// Visibility of light l from receiver p (tile space; z in world height units). 3D DDA over the 2D grid:
// an explored cell blocks when BlockLight is set and the ray's z is below the cell's height inside it, behind its face planes
// (BlockedInCell); an explored Arch cell blocks when the ray is below its height AND rises above its lintel (independent of
// BlockLight); unexplored cells block by BlockLight with infinite height and no faces. Receiver and light cells are skipped (any(): sharing one coordinate is not enough).
float Visibility3D(Vec3 p, Vec3 l, Point skipA, Point skipB, Volume v, const Params &params)
{
	const Vec3 d = Sub(l, p);
	if (d.x * d.x + d.y * d.y < 1e-6F) return 1.0F;
	Point cell = FloorToPoint({ p.x, p.y });
	const Point endCell = FloorToPoint({ l.x, l.y });
	const Point step { d.x > 0.0F ? 1 : -1, d.y > 0.0F ? 1 : -1 };
	Vec2 invD { 1e9F, 1e9F };
	if (std::fabs(d.x) > 1e-6F) invD.x = 1.0F / std::fabs(d.x);
	if (std::fabs(d.y) > 1e-6F) invD.y = 1.0F / std::fabs(d.y);
	Vec2 tMax { (p.x - static_cast<float>(cell.x)) * invD.x, (p.y - static_cast<float>(cell.y)) * invD.y };
	if (d.x > 0.0F) tMax.x = (static_cast<float>(cell.x + 1) - p.x) * invD.x;
	if (d.y > 0.0F) tMax.y = (static_cast<float>(cell.y + 1) - p.y) * invD.y;
	const float hScale = params.heightPixelsPerWorldUnit;
	float tIn = 0.0F;
	for (uint32_t i = 0; i < 64U; i++) {
		const float tOut = std::min(std::min(tMax.x, tMax.y), 1.0F);
		if (AnyNotEqual(cell, skipA) && AnyNotEqual(cell, skipB)) {
			const uint32_t cv = CellForMarch(cell, v, params);
			const float h = static_cast<float>(cv & 255U) / hScale;
			const float zIn = p.z + tIn * d.z;
			const float zOut = p.z + tOut * d.z;
			const float zMin = std::min(zIn, zOut);
			const float zMax = std::max(zIn, zOut);
			if ((cv & VArch) != 0) {
				const float lintel = static_cast<float>((cv >> 21U) & 127U) * 2.0F / hScale;
				if (zMin < h && zMax > lintel) return 0.0F;
			} else if ((cv & VBlock) != 0 && BlockedInCell(p, d, cell, cv, tIn, tOut, h)) {
				return 0.0F;
			}
		}
		if (AllEqual(cell, endCell) || tOut >= 1.0F) break;
		tIn = tOut;
		if (tMax.x < tMax.y) {
			cell.x += step.x;
			tMax.x += invD.x;
		} else {
			cell.y += step.y;
			tMax.y += invD.y;
		}
	}
	return 1.0F;
}
float SoftVisibility3D(Vec3 p, Vec3 l, Point skipA, Point skipB, Volume v, const Params &params)
{
	const Vec2 d = Sub(Vec2 { l.x, l.y }, Vec2 { p.x, p.y });
	if (Dot(d, d) < 1e-4F) return Visibility3D(p, l, skipA, skipB, v, params);
	const Vec2 t = Scale(Normalize({ -d.y, d.x }), params.softTapOffset);
	const float vis = Visibility3D(p, l, skipA, skipB, v, params)
	    + Visibility3D(p, { l.x + t.x, l.y + t.y, l.z }, skipA, skipB, v, params)
	    + Visibility3D(p, { l.x - t.x, l.y - t.y, l.z }, skipA, skipB, v, params);
	return vis / 3.0F;
}
// Per-workgroup light culling: the real world footprint of this 8x8 block's shaded receivers (the workgroup atomics of
// the WGSL) and the lights touching it as a bitmask built once per workgroup, so every branch on it is workgroup-uniform.
uint32_t OrderedKey(float f)
{
	uint32_t b;
	std::memcpy(&b, &f, sizeof(b)); // bitcast<u32>
	if ((b & 0x80000000U) != 0) return ~b;
	return b | 0x80000000U;
}
float KeyToF32(uint32_t k)
{
	const uint32_t b = (k & 0x80000000U) != 0 ? (k & 0x7fffffffU) : ~k;
	float f;
	std::memcpy(&f, &b, sizeof(f)); // bitcast<f32>
	return f;
}
// The receivers' ground grown by the shading nudge: p.xy = ground + n.xy * receiverNudge with |n.x|, |n.y| <= 1. A light whose
// radius misses this box misses every receiver of the block (dist >= radius in AccumulateMasked), so culling it changes nothing.
// Texels past the mask edge need no bounds check here: their outPixel lies past the planes or the viewport, never shaded.
Footprint WorkgroupFootprint(Point workgroup, const Planes &planes, Volume v, const Params &p, bool &any)
{
	std::array<uint32_t, 2> groundMax {};    // wgGroundMax (workgroup memory starts zeroed)
	std::array<uint32_t, 2> groundMinInv {}; // wgGroundMinInv
	for (int ly = 0; ly < 8; ly++) {
		for (int lx = 0; lx < 8; lx++) {
			const Receiver r = ReceiverOf({ workgroup.x * 8 + lx, workgroup.y * 8 + ly }, planes, v, p);
			if (!r.shade) continue;
			const uint32_t kx = OrderedKey(r.ground.x);
			const uint32_t ky = OrderedKey(r.ground.y);
			groundMax[0] = std::max(groundMax[0], kx);
			groundMax[1] = std::max(groundMax[1], ky);
			groundMinInv[0] = std::max(groundMinInv[0], ~kx);
			groundMinInv[1] = std::max(groundMinInv[1], ~ky);
		}
	}
	any = groundMax[0] != 0;
	const Vec2 lo { KeyToF32(~groundMinInv[0]), KeyToF32(~groundMinInv[1]) };
	const Vec2 hi { KeyToF32(groundMax[0]), KeyToF32(groundMax[1]) };
	return { Sub(lo, { p.receiverNudge, p.receiverNudge }), Add(hi, { p.receiverNudge, p.receiverNudge }) };
}
uint32_t WorkgroupLightMask(Point workgroup, const Planes &planes, Volume v, std::span<const Light> lights, const Params &p)
{
	bool any = false;
	const Footprint fp = WorkgroupFootprint(workgroup, planes, v, p, any);
	uint32_t mask = 0;
	const uint32_t lightCount = std::min<uint32_t>({ p.lightCount, 32U, static_cast<uint32_t>(lights.size()) });
	for (uint32_t li = 0; li < lightCount && any; li++) { // one light per invocation; nothing shaded -> 0
		if (LightTouches(fp, lights[li])) mask |= 1U << li;
	}
	return mask;
}
bool LightTouches(const Footprint &fp, const Light &l)
{
	const Vec2 lxy { l.position.x, l.position.y };
	const Vec2 q = Sub(ClampVec(lxy, fp.lo, fp.hi), lxy);
	return Dot(q, q) < l.radius * l.radius;
}
// Shared light loop over the workgroup's light mask (ascending index, the order of the full list).
// own: cell for ambient clamp + gate (surfaces); gated = false -> free bilinear ambient, no gate (cards).
Rgb AccumulateMasked(Vec3 p, Vec3 n, Vec2 ground, Point own, Point skipA, float wrap, bool gated, uint32_t lightMask, Volume v, std::span<const Light> lights, const Params &params)
{
	const uint32_t flags = params.flags;
	float ambient;
	if (gated) {
		ambient = AmbientBilinearClamped(ground, own, v, params);
	} else {
		ambient = AmbientBilinear(ground, v, params);
	}
	ambient = ambient * params.ambientScale;
	float gate = 1.0F;
	if (gated) gate = (CellRaw(own, v, params) & VExplored) == 0 ? 0.0F : LightGate(ground, v, params); // never light what classic never lit
	Rgb radiance {};
	float peak = 0.0F;
	const uint32_t lightCount = std::min<uint32_t>({ params.lightCount, 32U, static_cast<uint32_t>(lights.size()) }); // params.lights has 32 slots on the GPU
	uint32_t pending = lightCount >= 32U ? lightMask : (lightMask & ((1U << lightCount) - 1U)); // the GPU mask never has these bits
	if (gate <= 0.0F) pending = 0;                                                               // radiance * gate == 0: skip every light and its marches
	while (pending != 0) {
		const uint32_t i = static_cast<uint32_t>(std::countr_zero(pending));
		pending = pending & (pending - 1U);
		const Light &light = lights[i];
		const Vec3 toL = Sub(light.position, p);
		const float dist = Length(toL);
		if (dist >= light.radius) continue;
		const Vec3 ldir = Scale(toL, 1.0F / std::max(dist, 1e-4F));
		const float ndl = Clamp((Dot(n, ldir) + wrap) / (1.0F + wrap), 0.0F, 1.0F);
		if (ndl <= 0.0F) continue;
		const Point lightCell = FloorToPoint({ light.position.x, light.position.y });
		float vis;
		if ((flags & 16U) != 0) {
			vis = SoftVisibility3D(p, light.position, skipA, lightCell, v, params);
		} else {
			vis = Visibility3D(p, light.position, skipA, lightCell, v, params);
		}
		const float att = 1.0F - dist / light.radius;
		const float contribution = att * att * vis * ndl * light.intensity;
		radiance = Add(radiance, Scale(light.color, contribution));
		peak = std::max(peak, contribution);
	}
	Rgb lighting = Add({ ambient, ambient, ambient }, Scale(radiance, gate));
	if ((flags & 2U) != 0) lighting = Add(lighting, Scale(radiance, gate * (0.12F * Smoothstep(0.45F, 1.0F, peak)))); // bloom: additive only
	return lighting;
}
// Footprint form for callers without a workgroup (the HD sprite fragment shader, hd_sprites_webgpu.cpp).
uint32_t FootprintLightMask(const Footprint &fp, std::span<const Light> lights, const Params &params)
{
	uint32_t mask = 0;
	const uint32_t lightCount = std::min<uint32_t>({ params.lightCount, 32U, static_cast<uint32_t>(lights.size()) });
	for (uint32_t i = 0; i < lightCount; i++) {
		if (LightTouches(fp, lights[i])) mask = mask | (1U << i);
	}
	return mask;
}
Rgb Accumulate(Vec3 p, Vec3 n, Vec2 ground, Point own, Point skipA, float wrap, bool gated, const Footprint &fp, Volume v, std::span<const Light> lights, const Params &params)
{
	return AccumulateMasked(p, n, ground, own, skipA, wrap, gated, FootprintLightMask(fp, lights, params), v, lights, params);
}
Rgb DebugVolumeColor(Point cell, Volume v, const Params &p) // Volume view, computed in the compute pass
{
	const uint32_t raw = CellRaw(cell, v, p);
	if ((raw & VExplored) == 0) return { 1.0F, 0.0F, 1.0F }; // magenta = leak detector
	const float hgt = static_cast<float>(raw & 255U) / 255.0F;
	if ((raw & VArch) != 0) return Scale(Rgb { 0.2F, 0.6F, 1.0F }, 0.3F + hgt);
	if ((raw & VDoorOpen) != 0) return Scale(Rgb { 0.2F, 1.0F, 0.4F }, 0.3F + hgt);
	if ((raw & VBlock) != 0) return Scale(Rgb { 1.0F, 0.35F, 0.35F }, 0.3F + hgt); // existing red occluder tint
	const float g = 0.15F + 0.5F * static_cast<float>(15U - ((raw >> 17U) & 15U)) / 15.0F;
	return { g, g, g };
}

// receiverOf: one mask texel before lighting; the dispatch bounds check (id >= dims) lives in RunLightPass.
Receiver ReceiverOf(Point maskTexel, const Planes &planes, Volume v, const Params &p)
{
	Receiver r { false, { 1.0F, 1.0F, 1.0F, 0.0F }, KUnlit, 0, {}, 0.0F };
	const uint32_t flags = p.flags;
	const uint32_t maskScale = p.maskScale;
	const uint32_t zoom = p.zoom;
	const uint32_t debugView = (flags >> 8U) & 15U;
	const Point outPixel { static_cast<int>(static_cast<uint32_t>(maskTexel.x) * maskScale + (maskScale / 2U)),
		static_cast<int>(static_cast<uint32_t>(maskTexel.y) * maskScale + (maskScale / 2U)) }; // texel centre in output pixels
	const int viewportH = static_cast<int>(p.viewportHeight);
	if ((flags & 8U) == 0 || outPixel.y >= viewportH) return r;
	const uint32_t a = AuxTexelAt(planes, outPixel); // odd-W last column: robust access returns 0 (Unlit) past the edge
	uint32_t kind = a >> 5U;
	const uint32_t nIdx = a & 31U;
	const uint32_t b = BaseTexelAt(planes, outPixel, p);
	uint32_t h = HeightRows(kind, b, static_cast<uint32_t>(outPixel.y), zoom);
	if (IsWallKind(kind) && h > MaxWallRows) { // plane-B wrap guard
		kind = KFloor;
		h = 0;
	}
	r.kind = kind;
	r.nIdx = nIdx;
	r.hf = static_cast<float>(h);
	const Vec2 floorScreen { static_cast<float>(outPixel.x), static_cast<float>(outPixel.y) + r.hf * static_cast<float>(ZoomQuarters(zoom)) / 4.0F };
	r.ground = ScreenToWorld(floorScreen, p);
	if (debugView == 5U) {
		const Rgb c = DebugVolumeColor(FloorToPoint(r.ground), v, p);
		r.store = { c.r, c.g, c.b, 1.0F };
		return r;
	}
	if (PassThrough(kind)) return r;
	r.shade = true;
	return r;
}
MaskTexel ShadeReceiver(const Receiver &r, uint32_t lightMask, Volume v, std::span<const Light> lights, Lut lut, const Params &p)
{
	const uint32_t kind = r.kind;
	const Vec2 ground = r.ground;
	const Vec3 n = LutNormal(lut, r.nIdx);
	const float z = r.hf / p.heightPixelsPerWorldUnit;
	const Vec2 nudge { n.x * p.receiverNudge, n.y * p.receiverNudge };
	// Cell picks use a wider offset than the shading nudge (CELL_PICK_OFFSET in the WGSL): the foot line runs through the cell's
	// centre but is quantised to rows, and 0.35 tiles keeps the skip and own picks on the wall's own cell for every column.
	const Vec2 cellNudge { n.x * CellPickOffset, n.y * CellPickOffset };
	Rgb lit;
	if (IsWallKind(kind)) { // foot-table walls, see-through walls and arches: WallBlended is lit like the face it continues
		const Point skipA = FloorToPoint(Add(ground, cellNudge));
		const Point own = WallGateCell(ground, { n.x, n.y }, v, p);
		lit = AccumulateMasked({ ground.x + nudge.x, ground.y + nudge.y, z }, n, ground, own, skipA, p.wrapSurface, true, lightMask, v, lights, p);
	} else if (kind == KWallTop) {
		const Point own = WallOwnCell(ground);
		lit = AccumulateMasked({ ground.x, ground.y, z }, n, ground, own, own, p.wrapSurface, true, lightMask, v, lights, p);
	} else if (kind == KCard) { // no gate, no clamp: CPU culling already enforces no-reveal
		const Point c = FloorToPoint(ground);
		lit = AccumulateMasked({ ground.x, ground.y, z }, n, ground, c, c, p.wrapCard, false, lightMask, v, lights, p);
	} else { // KFloor
		const Point c = FloorToPoint(ground);
		lit = AccumulateMasked({ ground.x, ground.y, 0.0F }, n, ground, c, c, p.wrapSurface, true, lightMask, v, lights, p);
	}
	return { lit.r, lit.g, lit.b, 1.0F }; // debugView 4 (Lighting) reads this mask unchanged in the composite
}

// hybridLightMain for one mask texel: its workgroup's light mask, then its own receiver.
MaskTexel LightTexel(Point maskTexel, const Planes &planes, Volume v, std::span<const Light> lights, Lut lut, const Params &p)
{
	const Point wg { maskTexel.x / 8, maskTexel.y / 8 }; // @builtin(workgroup_id) for @workgroup_size(8, 8)
	const uint32_t lightMask = WorkgroupLightMask(wg, planes, v, lights, p);
	const Receiver r = ReceiverOf(maskTexel, planes, v, p);
	if (!r.shade) return r.store;
	return ShadeReceiver(r, lightMask, v, lights, lut, p);
}

void RunLightPass(Mask &mask, const Planes &planes, Volume v, std::span<const Light> lights, Lut lut, const Params &p)
{
	// Dispatched over the FULL mask (rows >= viewport store pass-through), one light mask per 8x8 workgroup.
	for (uint32_t wy = 0; wy < (mask.height + 7) / 8; wy++) {
		for (uint32_t wx = 0; wx < (mask.width + 7) / 8; wx++) {
			const uint32_t lightMask = WorkgroupLightMask({ static_cast<int>(wx), static_cast<int>(wy) }, planes, v, lights, p);
			for (uint32_t my = wy * 8; my < std::min(wy * 8 + 8, mask.height); my++) {
				for (uint32_t mx = wx * 8; mx < std::min(wx * 8 + 8, mask.width); mx++) {
					const Receiver r = ReceiverOf({ static_cast<int>(mx), static_cast<int>(my) }, planes, v, p);
					mask.texels[static_cast<size_t>(my) * mask.width + mx] = r.shade ? ShadeReceiver(r, lightMask, v, lights, lut, p) : r.store;
				}
			}
		}
	}
}

// ====================================================== present pass =======================================================

namespace {
// Per-component sRGB <-> linear with if/else (mirrors srgbToLinear1 / linearToSrgb1).
float SrgbToLinear1(float c)
{
	if (c > 0.04045F) return std::pow((c + 0.055F) / 1.055F, 2.4F);
	return c / 12.92F;
}
float LinearToSrgb1(float c)
{
	if (c > 0.0031308F) return 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
	return c * 12.92F;
}
} // namespace

Rgb SrgbToLinear(Rgb c) { return { SrgbToLinear1(c.r), SrgbToLinear1(c.g), SrgbToLinear1(c.b) }; }
Rgb LinearToSrgb(Rgb c) { return { LinearToSrgb1(c.r), LinearToSrgb1(c.g), LinearToSrgb1(c.b) }; }
float PaletteEmissiveAt(Emissive e, uint32_t idx) // present.paletteEmissive[idx >> 2][idx & 3] via a local vec4 copy
{
	const uint32_t i = idx & 255U;
	const std::array<float, 4> v { e[(i >> 2U) * 4U + 0U], e[(i >> 2U) * 4U + 1U], e[(i >> 2U) * 4U + 2U], e[(i >> 2U) * 4U + 3U] };
	return v[i & 3U];
}

// Bilateral upsample of the half-res mask: 4 taps weighted bilinearly, kept only when the tap's centre sample has the same
// kind, a height within 8 rows and lies inside the viewport; nearest (m0) when every weight is zero.
Rgb SampleLighting(Point px, uint32_t kind, uint32_t h, const Mask &mask, const Planes &planes, const Params &p)
{
	const int s = static_cast<int>(p.maskScale);
	if (s == 1) return MaskRgbAt(mask, px);
	const Vec2 mf = Scale(Sub(ToVec2(px), { 1.0F, 1.0F }), 0.5F);
	const Point m0 = FloorToPoint(mf);
	const Vec2 f = Sub(mf, ToVec2(m0));
	const uint32_t zoom = p.zoom;
	const int viewportH = static_cast<int>(p.viewportHeight);
	Rgb sum {};
	float wsum = 0.0F;
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			const Point m { m0.x + i, m0.y + j };
			const Point sp { m.x * 2 + 1, m.y * 2 + 1 }; // negative sp -> robust access returns 0: kind Unlit -> rejected
			if (sp.y >= viewportH) continue;             // rows below the viewport hold pass-through, never lighting
			const uint32_t ta = AuxTexelAt(planes, sp);
			const uint32_t tb = BaseTexelAt(planes, sp, p);
			const uint32_t th = HeightRows(ta >> 5U, tb, static_cast<uint32_t>(std::max(sp.y, 0)), zoom);
			float wx = 1.0F - f.x;
			if (i == 1) wx = f.x;
			float wy = 1.0F - f.y;
			if (j == 1) wy = f.y;
			const float w = wx * wy;
			const bool ok = ((ta >> 5U) == kind) && (std::abs(static_cast<int>(th) - static_cast<int>(h)) < 8);
			if (ok) {
				sum = Add(sum, Scale(MaskRgbAt(mask, m), w));
				wsum += w;
			}
		}
	}
	if (wsum <= 0.0F) return MaskRgbAt(mask, m0);
	return Scale(sum, 1.0F / wsum);
}
// Viewport rows: palette expansion (byte-identical to BltFast); panel rows / gamepad: the RGBA frame texel.
Rgb FrameColor(Point px, Rgb frameSrgb, Palette palette, uint32_t idx, const Params &p)
{
	if (px.y >= static_cast<int>(p.viewportHeight) || p.frameHasUnlit) return frameSrgb;
	return palette[idx & 255U];
}
Rgb DebugColor(uint32_t view, Point px, uint32_t aux, uint32_t idx, Rgb frame, const Planes &planes, const Mask &mask, Emissive e, Lut lut, const Params &p)
{
	const uint32_t kind = aux >> 5U;
	const uint32_t nIdx = aux & 31U;
	if (view == 1U) { // Kind: 8-colour palette
		const std::array<Rgb, 8> kindColors { Rgb { 0.0F, 0.0F, 0.0F }, Rgb { 0.25F, 0.25F, 0.25F }, Rgb { 1.0F, 0.0F, 0.0F }, Rgb { 0.0F, 0.0F, 1.0F },
			Rgb { 0.0F, 1.0F, 0.0F }, Rgb { 1.0F, 1.0F, 0.0F }, Rgb { 1.0F, 0.0F, 1.0F }, Rgb { 0.0F, 1.0F, 1.0F } };
		return kindColors[kind & 7U];
	}
	if (view == 2U) { // Height: grey h / 255
		const uint32_t b = BaseTexelAt(planes, px, p);
		const uint32_t h = HeightRows(kind, b, static_cast<uint32_t>(px.y), p.zoom);
		const float g = static_cast<float>(h) / 255.0F;
		return { g, g, g };
	}
	if (view == 3U) { // Normal
		const Vec3 n = LutNormal(lut, nIdx);
		return { n.x * 0.5F + 0.5F, n.y * 0.5F + 0.5F, n.z * 0.5F + 0.5F };
	}
	if (view == 4U || view == 5U) { // Lighting / Volume: the mask alone
		const Point m { px.x / static_cast<int>(p.maskScale), px.y / static_cast<int>(p.maskScale) };
		return MaskRgbAt(mask, m);
	}
	if (view == 6U) { // Emissive
		const float g = PaletteEmissiveAt(e, idx);
		return { g, g, g };
	}
	return frame;
}

// fragmentMain for one output pixel: frameSrgb is the RGBA frame texel (panel rows / gamepad), palette the SDL palette.
// The opt-in HD floor (HdFloorWgsl: hdFloorAlbedo, floorFrameIndex, bindings 16-22) is not modelled: without floor marks it is the identity.
Rgb Composite(Point px, Rgb frameSrgb, Palette palette, const Planes &planes, const Mask &mask, Emissive e, Lut lut, const Params &p)
{
	if ((p.flags & 8U) == 0) return frameSrgb; // legacy path (frame * legacy 2D mask): the 2D mask is not modelled here
	const uint32_t a = AuxTexelAt(planes, px);
	const uint32_t kind = a >> 5U;
	const uint32_t idx = IndexTexelAt(planes, px);
	const Rgb frame = FrameColor(px, frameSrgb, palette, idx, p);
	const uint32_t view = (p.flags >> 8U) & 15U; // present.config.z
	if (view != 0) return DebugColor(view, px, a, idx, frame, planes, mask, e, lut, p);
	if (PassThrough(kind) || px.y >= static_cast<int>(p.viewportHeight)) return frame;
	const uint32_t b = BaseTexelAt(planes, px, p);
	uint32_t h = HeightRows(kind, b, static_cast<uint32_t>(px.y), p.zoom);
	uint32_t k = kind;
	if (IsWallKind(k) && h > MaxWallRows) {
		k = KFloor;
		h = 0;
	}
	const Rgb lighting = SampleLighting(px, k, h, mask, planes, p);
	const float emissive = PaletteEmissiveAt(e, idx);
	const Rgb albedo = SrgbToLinear(frame);
	Rgb color = Add(Mul(albedo, lighting), Scale(albedo, emissive * p.emissiveScale));
	color = Scale(color, p.exposure);
	return LinearToSrgb({ Clamp(color.r, 0.0F, 1.0F), Clamp(color.g, 0.0F, 1.0F), Clamp(color.b, 0.0F, 1.0F) });
}

} // namespace devilution::hybrid_reference
