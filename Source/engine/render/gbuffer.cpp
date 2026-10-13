/**
 * @file gbuffer.cpp
 *
 * CPU G-buffer planes for the 2.5D hybrid lighting.
 *
 * Two byte planes mirror the 8-bit back buffer byte-for-byte (same pitch): plane A carries
 * `kind:3 | normalIdx:5`, plane B the anchor row of the surface. Every writer addresses them as
 * `plane + (dst - PalSurface->pixels)`. The mode latch (`CurrentMode`) is the single gate the hot
 * blitters read; in classic mode it stays `Off` and nothing here touches the planes.
 */
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_view_transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

namespace devilution::gbuffer {

Mode CurrentMode = Mode::Off;
Planes CurrentPlanes;
uint8_t CurrentTileAux = UnlitByte;
uint8_t CurrentTileAuxBlended = UnlitByte;
std::array<uint8_t, 32> CurrentTileBaseRow {};
int16_t CurrentTileLeftX = 0;
Kind CurrentCardKind = Kind::Card;
bool CurrentArchBlended = false;

namespace {

// ---- Frame latch ----
uint32_t FrameCounterValue = 0;
const SDL_Color *LatchedPalette = nullptr;
bool LatchedEnabled = false;
bool BackBufferHybrid = false;

// ---- Plane storage ----
// The planes are over-allocated and aligned by hand: std::aligned_alloc is not available on every
// supported toolchain and the alignment only matters for the per-frame memset/memcpy throughput.
constexpr size_t PlaneAlignment = 64;

struct PlaneAllocation {
	std::unique_ptr<uint8_t[]> raw;
	uint8_t *aligned = nullptr;
};

PlaneAllocation AuxAllocation;
PlaneAllocation BaseAllocation;

PlaneAllocation AllocatePlane(size_t bytes)
{
	PlaneAllocation result;
	result.raw = std::make_unique<uint8_t[]>(bytes + PlaneAlignment);
	const auto address = reinterpret_cast<uintptr_t>(result.raw.get());
	const uintptr_t alignedAddress = (address + PlaneAlignment - 1) & ~static_cast<uintptr_t>(PlaneAlignment - 1);
	result.aligned = result.raw.get() + (alignedAddress - address);
	return result;
}

[[nodiscard]] bool PlanesAllocated()
{
	return CurrentPlanes.aux != nullptr;
}

/** Rows `[0, rows)` clamped to the allocated height. */
[[nodiscard]] int ClampRows(int rows)
{
	return std::clamp(rows, 0, static_cast<int>(CurrentPlanes.height));
}

/** Clips `[x0, x1) x [y0, y1)` (back-buffer coordinates) to the planes. Returns false when nothing is left. */
[[nodiscard]] bool ClipToPlanes(int &x0, int &y0, int &x1, int &y1)
{
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, static_cast<int>(CurrentPlanes.width));
	y1 = std::min(y1, static_cast<int>(CurrentPlanes.height));
	return x1 > x0 && y1 > y0;
}

[[nodiscard]] bool ClipRectToPlanes(Rectangle rect, int &x0, int &y0, int &x1, int &y1)
{
	x0 = rect.position.x;
	y0 = rect.position.y;
	x1 = rect.position.x + rect.size.width;
	y1 = rect.position.y + rect.size.height;
	return ClipToPlanes(x0, y0, x1, y1);
}

// ---- Normal LUT ----
struct Vec3 {
	float x, y, z;
};

[[nodiscard]] float Dot(const Vec3 &a, const Vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] Vec3 Normalized(const Vec3 &v)
{
	const float length = std::sqrt(Dot(v, v));
	return { v.x / length, v.y / length, v.z / length };
}

constexpr float Pi = 3.14159265358979323846F;

constexpr float DegToRad(float deg)
{
	return deg * Pi / 180.0F;
}

// Card basis in tile space: R = screen right, U' = screen up along the 30°-tilted card, C' = card normal.
constexpr Vec3 CardRight { 0.70710678F, -0.70710678F, 0.0F };
constexpr Vec3 CardUp { -0.35355339F, -0.35355339F, 0.86602540F };
constexpr Vec3 CardNormal { 0.61237244F, 0.61237244F, 0.5F };

/**
 * Rings of the tilted-card hemisphere, entries 4..31 (28 entries):
 *   entry(e, a) = normalize(cos e·C' + sin e·(cos a·R + sin a·U')), a = offset + 2πk/count.
 *
 * The original layout used rings {30°:6, 55°:10, 75°:12} with aligned azimuths and claims a worst-case error of ≈15°. Measured
 * on a dense sweep of the hemisphere, that layout quantises with up to 21.1° of error (the rim beyond the 75° ring is
 * uncovered and the 55° ring sits in line with the 30° one). No 32-entry LUT can reach the ≤16° the design asked for over
 * the whole hemisphere: 29 caps of 16° cover 7.06 sr against the hemisphere's 6.28 sr, a density of 1.12, below what any
 * covering of a sphere by that many equal caps achieves (≈1.2), before the wasted area at the rim. The elevations and
 * the half-step stagger of ring 2 below therefore come from a numeric sweep of the ring layouts that minimises the
 * worst case over the entire hemisphere: 18.6° (the SPEC's counts are kept). gbuffer_test asserts the measured bound.
 */
struct LutRing {
	float elevationDeg;
	int count;
	float azimuthOffsetDeg;
};
constexpr LutRing LutRings[] = {
	{ 30.0F, 6, 0.0F },
	{ 54.5F, 10, 18.0F },
	{ 79.5F, 12, 0.0F },
};
static_assert(LutRings[0].count + LutRings[1].count + LutRings[2].count == 28, "entries 4..31 hold 28 ring normals");

[[nodiscard]] std::array<NormalLutEntry, 32> BuildNormalLut()
{
	std::array<NormalLutEntry, 32> lut {};
	const auto set = [&lut](size_t i, Vec3 v) {
		v = Normalized(v);
		lut[i] = { v.x, v.y, v.z, 0.0F };
	};
	set(NormalUp, { 0.0F, 0.0F, 1.0F });
	set(NormalWallLeft, { 0.0F, 1.0F, 0.0F });
	set(NormalWallRight, { 1.0F, 0.0F, 0.0F });
	set(NormalCardFlat, CardNormal);
	size_t i = 4;
	for (const LutRing &ring : LutRings) {
		const float ce = std::cos(DegToRad(ring.elevationDeg));
		const float se = std::sin(DegToRad(ring.elevationDeg));
		for (int k = 0; k < ring.count; ++k) {
			const float a = DegToRad(ring.azimuthOffsetDeg) + 2.0F * Pi * static_cast<float>(k) / static_cast<float>(ring.count);
			const float ca = std::cos(a);
			const float sa = std::sin(a);
			set(i++, {
			            ce * CardNormal.x + se * (ca * CardRight.x + sa * CardUp.x),
			            ce * CardNormal.y + se * (ca * CardRight.y + sa * CardUp.y),
			            ce * CardNormal.z + se * (ca * CardRight.z + sa * CardUp.z),
			        });
		}
	}
	return lut;
}

/** The single quantisation rule: argmax_i dot(N, LUT[i]) over i ∈ [3, 31]. `n` must be a unit vector. */
[[nodiscard]] uint8_t ArgmaxNormal(const Vec3 &n)
{
	const std::array<NormalLutEntry, 32> &lut = NormalLut();
	uint8_t best = NormalCardFlat;
	float bestDot = -2.0F;
	for (size_t i = NormalCardFlat; i < lut.size(); ++i) {
		const float d = n.x * lut[i].x + n.y * lut[i].y + n.z * lut[i].z;
		if (d > bestDot) {
			bestDot = d;
			best = static_cast<uint8_t>(i);
		}
	}
	return best;
}

// ---- Octahedral direction -> candidate table ----
// A 64x64 octahedral map of the whole sphere. Every texel stores the SET of LUT indices that win the argmax
// somewhere inside it (as a 32-bit mask), so a lookup evaluates 1..3 dot products instead of 29 and returns the
// exact argmax rather than the argmax of the texel centre.
constexpr int OctTableSize = 64;
constexpr int OctTableSubSamples = 10; // per axis, corners included

[[nodiscard]] float SignNotZero(float v)
{
	return v >= 0.0F ? 1.0F : -1.0F;
}

/** Unit vector -> octahedral [0, 1]^2. */
void OctEncode(Vec3 n, float &u, float &v)
{
	const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
	n.x /= l1;
	n.y /= l1;
	n.z /= l1;
	if (n.z < 0.0F) {
		const float ox = (1.0F - std::fabs(n.y)) * SignNotZero(n.x);
		const float oy = (1.0F - std::fabs(n.x)) * SignNotZero(n.y);
		n.x = ox;
		n.y = oy;
	}
	u = n.x * 0.5F + 0.5F;
	v = n.y * 0.5F + 0.5F;
}

/** Octahedral [0, 1]^2 -> unit vector. */
[[nodiscard]] Vec3 OctDecode(float u, float v)
{
	Vec3 n { u * 2.0F - 1.0F, v * 2.0F - 1.0F, 0.0F };
	n.z = 1.0F - std::fabs(n.x) - std::fabs(n.y);
	if (n.z < 0.0F) {
		const float ox = (1.0F - std::fabs(n.y)) * SignNotZero(n.x);
		const float oy = (1.0F - std::fabs(n.x)) * SignNotZero(n.y);
		n.x = ox;
		n.y = oy;
	}
	return Normalized(n);
}

[[nodiscard]] int OctTexel(float t)
{
	return std::clamp(static_cast<int>(t * OctTableSize), 0, OctTableSize - 1);
}

[[nodiscard]] const std::array<uint32_t, OctTableSize * OctTableSize> &OctCandidateTable()
{
	static const std::array<uint32_t, OctTableSize * OctTableSize> Table = [] {
		std::array<uint32_t, OctTableSize * OctTableSize> table {};
		for (int iy = 0; iy < OctTableSize; ++iy) {
			for (int ix = 0; ix < OctTableSize; ++ix) {
				uint32_t mask = 0;
				for (int sy = 0; sy < OctTableSubSamples; ++sy) {
					for (int sx = 0; sx < OctTableSubSamples; ++sx) {
						const float u = (static_cast<float>(ix) + static_cast<float>(sx) / (OctTableSubSamples - 1)) / OctTableSize;
						const float v = (static_cast<float>(iy) + static_cast<float>(sy) / (OctTableSubSamples - 1)) / OctTableSize;
						mask |= uint32_t { 1 } << ArgmaxNormal(OctDecode(u, v));
					}
				}
				table[static_cast<size_t>(iy) * OctTableSize + ix] = mask;
			}
		}
		return table;
	}();
	return Table;
}

// ---- Palette emissive heuristic ----
struct PaletteEmissiveOverride {
	dungeon_type level;
	uint8_t first;
	uint8_t last; // inclusive
	float value;
};

// Aligned with what classic keeps full-bright: the light tables keep indices 0..15 identity only for the Nest and the
// Crypt (the lava). Hell's 1..31 is the animated blood-wall remap and Caves lava is darkened normally, so neither is
// listed. This table could move to assets/txtdata/lighting/ later.
constexpr PaletteEmissiveOverride PaletteEmissiveOverrides[] = {
	{ DTYPE_NEST, 1, 15, 0.4F },
	{ DTYPE_CRYPT, 1, 15, 0.4F },
};

[[nodiscard]] float SrgbToLinear(uint8_t c)
{
	const float s = static_cast<float>(c) / 255.0F;
	return s <= 0.04045F ? s / 12.92F : std::pow((s + 0.055F) / 1.055F, 2.4F);
}

} // namespace

// ---- Lifecycle ----

void Allocate(const uint8_t *outBase, uint16_t pitch, uint16_t width, uint16_t height)
{
	Free();
	const size_t bytes = static_cast<size_t>(pitch) * height;
	if (outBase == nullptr || bytes == 0 || width > pitch)
		return;
	AuxAllocation = AllocatePlane(bytes);
	BaseAllocation = AllocatePlane(bytes);
	std::memset(AuxAllocation.aligned, UnlitByte, bytes);
	std::memset(BaseAllocation.aligned, 0, bytes);
	CurrentPlanes.aux = AuxAllocation.aligned;
	CurrentPlanes.base = BaseAllocation.aligned;
	CurrentPlanes.outBase = outBase;
	CurrentPlanes.pitch = pitch;
	CurrentPlanes.width = width;
	CurrentPlanes.height = height;
	CurrentPlanes.pitchRecip40 = ((uint64_t { 1 } << 40) + pitch - 1) / pitch;
}

void Free()
{
	CurrentPlanes = Planes {};
	AuxAllocation = PlaneAllocation {};
	BaseAllocation = PlaneAllocation {};
	BackBufferHybrid = false;
	CurrentMode = Mode::Off;
}

void LatchForFrame(const LatchInput &in)
{
	CurrentMode = Mode::Off;
	LatchedPalette = in.logicalPalette;
	// CreateBackBuffer allocates the planes before the presenter can be ready; requiring them here keeps every
	// "= Enabled()" statement of the contract memory-safe should a caller ever latch without a back buffer.
	LatchedEnabled = in.optionEnabled && in.presenterReady && !in.headless && in.gameRunning && in.level != DTYPE_TOWN
	    && PlanesAllocated();
	++FrameCounterValue;
}

bool Enabled()
{
	return LatchedEnabled;
}

uint32_t FrameCounter()
{
	return FrameCounterValue;
}

const SDL_Color *CurrentPalette()
{
	return LatchedPalette;
}

bool BackBufferIsHybrid()
{
	return BackBufferHybrid;
}

void InvalidateBackBuffer()
{
	BackBufferHybrid = false;
}

void BeginWorldPass(int viewportRows)
{
	BackBufferHybrid = LatchedEnabled;
	if (!LatchedEnabled)
		return;
	// All viewport rows, also under zoom: the blitters only write the top-left half then, and nothing outside the
	// doubled column range may persist from a previous frame.
	std::memset(CurrentPlanes.aux, UnlitByte, static_cast<size_t>(CurrentPlanes.pitch) * ClampRows(viewportRows));
	CurrentMode = Mode::CaptureWorld;
}

void EndWorldPass()
{
	// A re-present (scrollrt_draw_game_screen) right after the option was switched off still presents the hybrid frame the
	// back buffer holds (the presenter reads BackBufferIsHybrid), so the cursor and the gamepad must stamp Unlit as well.
	if (LatchedEnabled || (BackBufferHybrid && PlanesAllocated()))
		CurrentMode = Mode::StampUnlit;
}

void EndFrame()
{
	CurrentMode = Mode::Off;
}

// ---- Tile context ----

void SetTileContext(Kind kind, uint8_t normalIdx, int anchorRow, int leftX, FootKind foot)
{
	CurrentTileAux = PackAux(kind, normalIdx);
	CurrentTileAuxBlended = (kind == Kind::WallLeft || kind == Kind::WallRight)
	    ? PackAux(Kind::WallBlended, normalIdx)
	    : CurrentTileAux;
	BuildBaseRow(CurrentTileBaseRow, anchorRow, foot);
	CurrentTileLeftX = static_cast<int16_t>(leftX);
}

void BuildBaseRow(std::array<uint8_t, 32> &out, int anchorRow, FootKind foot)
{
	for (int c = 0; c < 32; ++c)
		out[c] = static_cast<uint8_t>((anchorRow - FootRows(foot, c)) & 0xFF);
}

void AnchorArchOnWallLine(const Surface &out, Rectangle rect, uint8_t auxByte, int anchorRow, int leftX, FootKind foot)
{
	if (!CaptureActive() || !CurrentPlanes.covers(out) || foot == FootKind::None)
		return;
	const int x0 = std::max(rect.position.x, 0);
	const int y0 = std::max(rect.position.y, 0);
	const int x1 = std::min(rect.position.x + rect.size.width, out.w());
	const int y1 = std::min(rect.position.y + rect.size.height, out.h());
	const auto constBase = static_cast<uint8_t>(anchorRow & 0xFF);
	for (int y = y0; y < y1; ++y) {
		const uint8_t *row = out.at(0, y);
		const uint8_t *aux = CurrentPlanes.auxAt(row);
		uint8_t *base = CurrentPlanes.baseAt(row);
		for (int x = x0; x < x1; ++x) {
			if (aux[x] != auxByte || base[x] != constBase)
				continue;
			const int c = std::clamp(x - leftX, 0, 63);
			base[x] = static_cast<uint8_t>((anchorRow - ArchFootRows(foot, c)) & 0xFF);
		}
	}
}

// ---- UI stamping ----

void StampUnlit(const Surface &out, Rectangle rect)
{
	if (!StampActive() || !CurrentPlanes.covers(out))
		return;

	// Clip to `out` first (its own coordinates), then translate to back-buffer coordinates: `out` may be a subregion.
	int x0 = std::max(rect.position.x, 0);
	int y0 = std::max(rect.position.y, 0);
	int x1 = std::min(rect.position.x + rect.size.width, out.w());
	int y1 = std::min(rect.position.y + rect.size.height, out.h());
	if (x1 <= x0 || y1 <= y0)
		return;

	const ptrdiff_t originOffset = out.begin() - CurrentPlanes.outBase;
	const int originY = static_cast<int>(CurrentPlanes.rowOf(originOffset));
	const int originX = static_cast<int>(originOffset - static_cast<ptrdiff_t>(originY) * CurrentPlanes.pitch);
	x0 += originX;
	x1 += originX;
	y0 += originY;
	y1 += originY;
	if (!ClipToPlanes(x0, y0, x1, y1))
		return;

	uint8_t *dst = CurrentPlanes.aux + static_cast<size_t>(y0) * CurrentPlanes.pitch + x0;
	const size_t width = static_cast<size_t>(x1 - x0);
	for (int y = y0; y < y1; ++y) {
		std::memset(dst, UnlitByte, width);
		dst += CurrentPlanes.pitch;
	}
}

// ---- Zoom / cursor ----

void ZoomInPlace(int viewportWidth, int viewportOffsetX, int outHeight, int zoomQuarters)
{
	// Only plane A is scaled: UI stamping and the cursor write A at output coordinates after the world pass, while
	// B is only read by the GPU, which samples it at world size under zoom.
	if (!BackBufferHybrid || !PlanesAllocated())
		return;
	if (viewportWidth <= 0 || outHeight <= 0 || viewportOffsetX < 0
	    || viewportOffsetX + viewportWidth > CurrentPlanes.width || outHeight > CurrentPlanes.height)
		return;

	// Exactly Zoom()'s mapping (scrollrt.cpp, HdUpscaleInPlace).
	HdUpscaleInPlace(CurrentPlanes.aux, static_cast<size_t>(CurrentPlanes.pitch),
	    ComputeHdWorldTransform(zoomQuarters, viewportOffsetX + viewportWidth, outHeight, viewportWidth, viewportOffsetX));
}

void SaveRect(Rectangle rect, std::span<uint8_t> auxOut)
{
	if (!BackBufferHybrid || !PlanesAllocated())
		return;
	int x0, y0, x1, y1;
	if (!ClipRectToPlanes(rect, x0, y0, x1, y1))
		return;
	const size_t width = static_cast<size_t>(x1 - x0);
	const size_t rows = std::min(static_cast<size_t>(y1 - y0), auxOut.size() / width);
	const uint8_t *src = CurrentPlanes.aux + static_cast<size_t>(y0) * CurrentPlanes.pitch + x0;
	uint8_t *dst = auxOut.data();
	for (size_t y = 0; y < rows; ++y) {
		std::memcpy(dst, src, width);
		src += CurrentPlanes.pitch;
		dst += width;
	}
}

void RestoreRect(Rectangle rect, std::span<const uint8_t> aux)
{
	if (!BackBufferHybrid || !PlanesAllocated())
		return;
	int x0, y0, x1, y1;
	if (!ClipRectToPlanes(rect, x0, y0, x1, y1))
		return;
	const size_t width = static_cast<size_t>(x1 - x0);
	const size_t rows = std::min(static_cast<size_t>(y1 - y0), aux.size() / width);
	const uint8_t *src = aux.data();
	uint8_t *dst = CurrentPlanes.aux + static_cast<size_t>(y0) * CurrentPlanes.pitch + x0;
	for (size_t y = 0; y < rows; ++y) {
		std::memcpy(dst, src, width);
		src += width;
		dst += CurrentPlanes.pitch;
	}
}

// ---- Upload views ----

std::span<const uint8_t> AuxRows(int rows)
{
	if (!PlanesAllocated())
		return {};
	return { CurrentPlanes.aux, static_cast<size_t>(CurrentPlanes.pitch) * ClampRows(rows) };
}

std::span<const uint8_t> BaseRows(int rows)
{
	if (!PlanesAllocated())
		return {};
	return { CurrentPlanes.base, static_cast<size_t>(CurrentPlanes.pitch) * ClampRows(rows) };
}

// ---- Normals ----

const std::array<NormalLutEntry, 32> &NormalLut()
{
	static const std::array<NormalLutEntry, 32> Lut = BuildNormalLut();
	return Lut;
}

uint8_t QuantizeNormal(float x, float y, float z)
{
	const float length2 = x * x + y * y + z * z;
	if (!(length2 > 0.0F) || !std::isfinite(length2))
		return NormalCardFlat;
	const float invLength = 1.0F / std::sqrt(length2);
	const Vec3 n { x * invLength, y * invLength, z * invLength };

	float u, v;
	OctEncode(n, u, v);
	uint32_t candidates = OctCandidateTable()[static_cast<size_t>(OctTexel(v)) * OctTableSize + OctTexel(u)];
	if ((candidates & (candidates - 1)) == 0) {
		// Single candidate: no dot product needed. (The table never stores an empty mask.)
		uint8_t idx = 0;
		while ((candidates >>= 1) != 0)
			++idx;
		return idx;
	}

	const std::array<NormalLutEntry, 32> &lut = NormalLut();
	uint8_t best = NormalCardFlat;
	float bestDot = -2.0F;
	while (candidates != 0) {
		uint8_t idx = 0;
		while (((candidates >> idx) & 1U) == 0)
			++idx;
		candidates &= candidates - 1;
		const float d = n.x * lut[idx].x + n.y * lut[idx].y + n.z * lut[idx].z;
		if (d > bestDot) {
			bestDot = d;
			best = idx;
		}
	}
	return best;
}

void CardToTile(float cx, float cy, float cz, float &tx, float &ty, float &tz)
{
	tx = cx * CardRight.x + cy * CardUp.x + cz * CardNormal.x;
	ty = cx * CardRight.y + cy * CardUp.y + cz * CardNormal.y;
	tz = cx * CardRight.z + cy * CardUp.z + cz * CardNormal.z;
}

// ---- Palette emissive heuristic ----

void BuildClassicAmbientCurve(std::span<const SDL_Color, 256> palette,
    std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels> lightTables, std::span<float, NumLightingLevels> out)
{
	const auto luminance = [&](uint8_t index) {
		const SDL_Color &c = palette[index];
		return 0.2126F * static_cast<float>(c.r) + 0.7152F * static_cast<float>(c.g) + 0.0722F * static_cast<float>(c.b);
	};
	for (size_t level = 0; level < NumLightingLevels; ++level) {
		float sum = 0.0F;
		int count = 0;
		for (size_t i = 1; i < 256; ++i) { // index 0 is transparent/black in every table
			const float source = luminance(static_cast<uint8_t>(i));
			if (source <= 0.0F) continue;
			sum += std::min(1.0F, luminance(lightTables[level][i]) / source);
			++count;
		}
		out[level] = count == 0 ? 0.0F : sum / static_cast<float>(count);
	}
}

void BuildPaletteEmissive(std::span<const SDL_Color, 256> logicalPalette, dungeon_type level, std::span<float, 256> out)
{
	// Level entries 0..127 are never emissive; 128..255 hold the global fire/glow ramps.
	for (size_t i = 0; i < 128; ++i)
		out[i] = 0.0F;
	for (size_t i = 128; i < 256; ++i) {
		const SDL_Color &c = logicalPalette[i];
		const float r = SrgbToLinear(c.r);
		const float g = SrgbToLinear(c.g);
		const float b = SrgbToLinear(c.b);
		const float luminance = 0.2126F * r + 0.7152F * g + 0.0722F * b;
		const float maxChannel = std::max({ r, g, b });
		const float minChannel = std::min({ r, g, b });
		const float saturation = (maxChannel - minChannel) / std::max(maxChannel, 1e-4F);
		const float brightness = std::clamp((luminance - 0.45F) / 0.55F, 0.0F, 1.0F);
		out[i] = std::pow(brightness, 1.5F) * (0.3F + 0.7F * saturation);
	}
	for (const PaletteEmissiveOverride &override : PaletteEmissiveOverrides) {
		if (override.level != level)
			continue;
		for (size_t i = override.first; i <= override.last; ++i)
			out[i] = override.value;
	}
	out[0] = 0.0F;
}

} // namespace devilution::gbuffer
