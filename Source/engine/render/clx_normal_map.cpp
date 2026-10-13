/**
 * @file clx_normal_map.cpp
 *
 * Per-sprite normal-plane generation and cache for the 2.5D hybrid lighting.
 *
 * Generation: the CLX frame is decoded into dense mask/luminance arrays held in one reused Scratch (with a
 * one-pixel zero border so no neighbourhood pass needs a bounds check); a 3-4 chamfer distance transform drives a
 * silhouette dome (a table by chamfer distance), a Sobel of the palette luminance (de-lit by its opaque mean) adds
 * detail, the two are blended, mirrored on request and quantised through a 128x128 card-space direction->index table
 * built once from gbuffer::CardToTile + gbuffer::QuantizeNormal (the octahedral table specialised to the
 * card hemisphere, which is all a sprite normal can span). Every pass visits only the pixels of each row's opaque
 * span, and the result is packed straight into the row-span layout (one byte per span column).
 *
 * Cache: keyed by pointer identity {pixelData, pixelDataSize, width, height, mirrored} with an
 * open-addressing table and an intrusive LRU; every entry header and payload is carved from one arena
 * reserved by SetCacheBudget with a first-fit free list. HashPixels runs once per generated entry to match
 * authored overrides and to alias memcpy-duplicated frames (refcounted payload). Sprite memory that is freed
 * evicts its entries (EvictNormalPlanes), which defeats ABA reuse of the pointer key.
 */
#include "engine/render/clx_normal_map.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/render/hd_ui_hooks.hpp"
#include "utils/attributes.h"
#include "utils/clx_decode.hpp"

namespace devilution::clx_normal_map {

// ---------------------------------------------------------------------------------------------------------
// Scratch
// ---------------------------------------------------------------------------------------------------------

constexpr uint8_t MaskBody = 1;   // opaque, colour != 0: part of the silhouette (0 = transparent)
constexpr uint8_t MaskShadow = 2; // opaque, colour 0: ShadowByte, outside the silhouette

/**
 * Dense per-pixel working arrays for one frame, row 0 = feet row, with a one-pixel zero border on every side so the
 * neighbourhood passes need no bounds checks: pixel (x, y) lives at (y + 1) * stride + x + 1, stride = width + 2.
 * Grows to the largest frame seen and is reused.
 */
struct Scratch {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t stride = 0;
	std::vector<uint8_t> mask;    // 0 (transparent) / MaskBody / MaskShadow
	std::vector<float> body;      // 1 for body pixels, 0 elsewhere (the Sobel's silhouette weight)
	std::vector<float> lum;       // linear luminance of the palette entry (written for opaque pixels, read for body pixels)
	std::vector<uint16_t> dist;   // 3-4 chamfer distance from a body pixel to the nearest non-body pixel (0 elsewhere)
	std::vector<float> dome;      // silhouette dome height H_s per pixel (0 outside the silhouette)
	std::vector<float> gx, gy;    // Sobel of lum over the spans; after BlendNormals: card-space cx, cy (and lum holds cz)
	std::vector<uint16_t> rowX0;  // per-row span [x0, x1) over every opaque pixel (x1 <= x0: empty row)
	std::vector<uint16_t> rowX1;
	std::vector<float> domeTable; // silhouette dome height H_s by chamfer distance (flat beyond 3R), sized for any distance the frame can hold
	std::array<float, 256> lumTable {};

	[[nodiscard]] size_t index(uint32_t x, uint32_t y) const
	{
		return (static_cast<size_t>(y) + 1) * stride + x + 1;
	}

	/** Sizes every array for a w x h frame and clears the ones that are read where the decoder does not write. */
	void prepare(uint32_t w, uint32_t h)
	{
		const size_t n = (static_cast<size_t>(w) + 2) * (static_cast<size_t>(h) + 2);
		if (n > mask.size()) {
			mask.resize(n);
			body.resize(n);
			lum.resize(n);
			dist.resize(n);
			dome.resize(n);
			gx.resize(n);
			gy.resize(n);
		}
		if (h > rowX0.size()) {
			rowX0.resize(h);
			rowX1.resize(h);
		}
		width = w;
		height = h;
		stride = w + 2;
		std::memset(mask.data(), 0, n);
		std::memset(body.data(), 0, n * sizeof(float));
		std::memset(dist.data(), 0, n * sizeof(uint16_t));
		std::memset(dome.data(), 0, n * sizeof(float));
		std::fill_n(rowX0.data(), h, static_cast<uint16_t>(w));
		std::fill_n(rowX1.data(), h, static_cast<uint16_t>(0));
	}
};

namespace {

// ---------------------------------------------------------------------------------------------------------
// Luminance
// ---------------------------------------------------------------------------------------------------------

const std::array<float, 256> &SrgbToLinearTable()
{
	static const std::array<float, 256> Table = [] {
		std::array<float, 256> table {};
		for (size_t i = 0; i < table.size(); ++i) {
			const float c = static_cast<float>(i) / 255.0F;
			table[i] = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
		}
		return table;
	}();
	return Table;
}

/** Luminance per palette entry: linear-light Y, or (perceptual) the same weights over the gamma-encoded sRGB bytes. */
void BuildLumTable(const SDL_Color *palette, bool perceptual, std::array<float, 256> &out)
{
	if (palette == nullptr) {
		out.fill(0.0F);
		return;
	}
	if (perceptual) {
		for (size_t i = 0; i < out.size(); ++i) {
			const SDL_Color &c = palette[i];
			out[i] = (0.2126F * static_cast<float>(c.r) + 0.7152F * static_cast<float>(c.g) + 0.0722F * static_cast<float>(c.b)) * (1.0F / 255.0F);
		}
		return;
	}
	const std::array<float, 256> &lin = SrgbToLinearTable();
	for (size_t i = 0; i < out.size(); ++i) {
		const SDL_Color &c = palette[i];
		out[i] = 0.2126F * lin[c.r] + 0.7152F * lin[c.g] + 0.0722F * lin[c.b];
	}
}

// ---------------------------------------------------------------------------------------------------------
// Decoder (walker shape as IsPointWithinClx, clx_render.cpp)
// ---------------------------------------------------------------------------------------------------------

/** Fills mask/lum and the per-row spans. Returns false for a degenerate (0-sized) frame. */
bool DecodeFrame(ClxSprite sprite, const SDL_Color *palette, bool perceptual, Scratch &s)
{
	const uint32_t w = sprite.width();
	const uint32_t h = sprite.height();
	if (w == 0 || h == 0)
		return false;
	s.prepare(w, h);
	BuildLumTable(palette, perceptual, s.lumTable);

	const uint8_t *src = sprite.pixelData();
	const uint8_t *const end = src + sprite.pixelDataSize();
	uint8_t *const mask = s.mask.data();
	float *const body = s.body.data();
	float *const lum = s.lum.data();
	uint32_t x = 0;
	uint32_t y = 0; // row from the feet (the first encoded row is the bottom row)

	while (src < end && y < h) {
		const uint8_t control = *src++;
		if (!IsClxOpaque(control)) {
			// Transparent runs may cross line boundaries.
			x += control;
			while (x >= w) {
				x -= w;
				++y;
			}
			continue;
		}
		const bool fill = IsClxOpaqueFill(control);
		const uint32_t n = fill ? GetClxOpaqueFillWidth(control) : GetClxOpaquePixelsWidth(control);
		// Opaque runs never cross a line; clamp anyway so corrupt data cannot write past the row.
		const uint32_t x1 = std::min(x + n, w);
		if (x < s.rowX0[y])
			s.rowX0[y] = static_cast<uint16_t>(x);
		if (x1 > s.rowX1[y])
			s.rowX1[y] = static_cast<uint16_t>(x1);
		size_t i = s.index(x, y);
		if (fill) {
			if (src >= end)
				break;
			const uint8_t color = *src++;
			const uint8_t m = color == 0 ? MaskShadow : MaskBody;
			const float b = color == 0 ? 0.0F : 1.0F;
			const float l = s.lumTable[color];
			for (uint32_t k = x; k < x1; ++k, ++i) {
				mask[i] = m;
				body[i] = b;
				lum[i] = l;
			}
		} else {
			const auto avail = static_cast<uint32_t>(std::min<ptrdiff_t>(n, end - src));
			for (uint32_t k = 0; k < avail; ++k) {
				const uint8_t color = src[k];
				if (x + k < x1) {
					mask[i] = color == 0 ? MaskShadow : MaskBody;
					body[i] = color == 0 ? 0.0F : 1.0F;
					lum[i] = s.lumTable[color];
					++i;
				}
			}
			src += avail;
		}
		x += n;
		if (x >= w) {
			x = 0;
			++y;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------------------------------------

/**
 * Two-pass 3-4 chamfer distance from every body pixel to the nearest non-body pixel, over the row spans only.
 * Everything outside the spans (including the frame edge) holds 0 from Scratch::prepare, so it counts as outside.
 * Each pass is split into a per-pixel gather from the neighbouring row (independent per pixel, vectorisable) and a
 * propagation along the row through a register-carried running value, so no pixel waits on a store to its
 * neighbour. The last stage also writes the silhouette dome height from the finished distance.
 */
void ChamferDistance(Scratch &s)
{
	const uint32_t h = s.height;
	const auto stride = static_cast<ptrdiff_t>(s.stride);
	const uint8_t *const mask = s.mask.data();
	uint16_t *const dist = s.dist.data();
	float *const dome = s.dome.data();
	const float *const domeTable = s.domeTable.data();
	// Forward pass (feet row first, left to right): below, below-left, below-right, then left.
	for (uint32_t y = 0; y < h; ++y) {
		const uint32_t x0 = s.rowX0[y];
		const uint32_t x1 = s.rowX1[y];
		if (x1 <= x0)
			continue;
		const uint32_t len = x1 - x0;
		const size_t start = s.index(x0, y);
		uint16_t *d = dist + start;
		const uint8_t *m = mask + start;
		for (uint32_t k = 0; k < len; ++k) {
			const uint16_t *b = d + k - stride;
			const uint16_t below = std::min(std::min<uint16_t>(b[0] + 3, b[-1] + 4), static_cast<uint16_t>(b[1] + 4));
			d[k] = m[k] == MaskBody ? below : uint16_t { 0 };
		}
		uint32_t run = 0; // distance carried from the left (left of the span is outside)
		for (uint32_t k = 0; k < len; ++k) {
			run = m[k] == MaskBody ? std::min<uint32_t>(d[k], run + 3) : 0;
			d[k] = static_cast<uint16_t>(run);
		}
	}
	// Backward pass (top row first, right to left): above, above-right, above-left, then right.
	for (uint32_t y = h; y-- > 0;) {
		const uint32_t x0 = s.rowX0[y];
		const uint32_t x1 = s.rowX1[y];
		if (x1 <= x0)
			continue;
		const uint32_t len = x1 - x0;
		const size_t start = s.index(x0, y);
		uint16_t *d = dist + start;
		const uint8_t *m = mask + start;
		float *o = dome + start;
		for (uint32_t k = 0; k < len; ++k) {
			const uint16_t *a = d + k + stride;
			const uint16_t above = std::min(std::min<uint16_t>(a[0] + 3, a[1] + 4), static_cast<uint16_t>(a[-1] + 4));
			d[k] = std::min(d[k], above);
		}
		uint32_t run = 0; // distance carried from the right
		for (uint32_t k = len; k-- > 0;) {
			run = m[k] == MaskBody ? std::min<uint32_t>(d[k], run + 3) : 0;
			d[k] = static_cast<uint16_t>(run);
			o[k] = domeTable[run];
		}
	}
}

/**
 * Fills the dome table H_s(d) = R * sqrt(1 - ((R - d) / R)^2) for chamfer distances 0..3R (3 units per pixel) and
 * H_s = R beyond, so a lookup needs no clamp. A 3-4 chamfer distance never exceeds 4 pixel steps to the nearest
 * non-body pixel, which is at most (min(w, h) + 1) / 2 steps away, so the table covers every value the frame can hold.
 */
void BuildDomeTable(Scratch &s, const Options &opts)
{
	const int maxRadius = std::max<int>(4, opts.maxInflateRadius);
	const int radius = std::clamp<int>(static_cast<int>(std::min(s.width, s.height)) / 4, 4, maxRadius);
	const auto cap = static_cast<uint32_t>(radius) * 3U;
	const uint32_t size = std::max(cap + 1, 4U * ((std::min(s.width, s.height) + 1) / 2) + 8U);
	if (s.domeTable.size() < size)
		s.domeTable.resize(size);
	const auto r = static_cast<float>(radius);
	for (uint32_t k = 0; k <= cap; ++k) {
		const float d = static_cast<float>(k) / 3.0F; // pixels from the outside
		const float t = (r - d) / r;
		s.domeTable[k] = r * std::sqrt(std::max(0.0F, 1.0F - t * t));
	}
	std::fill(s.domeTable.begin() + cap + 1, s.domeTable.begin() + size, r);
}

/**
 * Sobel of the luminance over the row spans (a neighbour outside the silhouette takes the centre value, i.e. its
 * difference is weighted by body = 0) and the mean gradient over body pixels. The Sobel loop is pure float
 * arithmetic so it vectorises; the values it leaves at non-body span pixels are never read.
 */
void SobelLuminance(Scratch &s, float &meanGx, float &meanGy, float &meanSquare)
{
	const uint32_t h = s.height;
	const auto st = static_cast<ptrdiff_t>(s.stride);
	const uint8_t *const mask = s.mask.data();
	const float *const body = s.body.data();
	const float *const lum = s.lum.data();
	float *const gx = s.gx.data();
	float *const gy = s.gy.data();
	double sumGx[2] = { 0.0, 0.0 };
	double sumGy[2] = { 0.0, 0.0 };
	double sumSq[2] = { 0.0, 0.0 };
	size_t count = 0;
	for (uint32_t y = 0; y < h; ++y) {
		const uint32_t x0 = s.rowX0[y];
		const uint32_t x1 = s.rowX1[y];
		if (x1 <= x0)
			continue;
		const uint32_t len = x1 - x0;
		const size_t start = s.index(x0, y);
		const uint8_t *m = mask + start;
		const float *b = body + start;
		const float *l = lum + start;
		float *ox = gx + start;
		float *oy = gy + start;
		for (uint32_t k = 0; k < len; ++k) {
			const float c = l[k];
			const auto t = [&](ptrdiff_t off) -> float {
				const auto j = static_cast<ptrdiff_t>(k) + off;
				return b[j] * (l[j] - c);
			};
			// y up = +stride (row 0 is the feet row). Normalised by 1/8 so Gx/Gy are luminance per pixel.
			ox[k] = (t(-st + 1) + 2.0F * t(1) + t(st + 1) - t(-st - 1) - 2.0F * t(-1) - t(st - 1)) * (1.0F / 8.0F);
			oy[k] = (t(st - 1) + 2.0F * t(st) + t(st + 1) - t(-st - 1) - 2.0F * t(-st) - t(-st + 1)) * (1.0F / 8.0F);
		}
		// Means over body pixels: two interleaved accumulators keep the add chains short; summed in a fixed order.
		uint32_t k = 0;
		for (; k + 1 < len; k += 2) {
			const bool b0 = m[k] == MaskBody;
			const bool b1 = m[k + 1] == MaskBody;
			sumGx[0] += b0 ? ox[k] : 0.0F;
			sumGy[0] += b0 ? oy[k] : 0.0F;
			sumSq[0] += b0 ? ox[k] * ox[k] + oy[k] * oy[k] : 0.0F;
			sumGx[1] += b1 ? ox[k + 1] : 0.0F;
			sumGy[1] += b1 ? oy[k + 1] : 0.0F;
			sumSq[1] += b1 ? ox[k + 1] * ox[k + 1] + oy[k + 1] * oy[k + 1] : 0.0F;
			count += (b0 ? 1 : 0) + (b1 ? 1 : 0);
		}
		if (k < len && m[k] == MaskBody) {
			sumGx[0] += ox[k];
			sumGy[0] += oy[k];
			sumSq[0] += ox[k] * ox[k] + oy[k] * oy[k];
			++count;
		}
	}
	const double n = count > 0 ? static_cast<double>(count) : 1.0;
	meanGx = static_cast<float>((sumGx[0] + sumGx[1]) / n);
	meanGy = static_cast<float>((sumGy[0] + sumGy[1]) / n);
	meanSquare = static_cast<float>((sumSq[0] + sumSq[1]) / n);
}

/** Everything the per-pixel normal needs after the passes over the frame. */
struct NormalParams {
	float silhouetteWeight;
	float detailWeight;
	float detailStrength;
	float meanGx;
	float meanGy;
	float mirrorSign; // -1 negates card x
};

/**
 * Reciprocal square root from the bit-hack seed refined by Newton steps: one step is within 0.18 % (far inside the
 * ~15 degree LUT cells), two are within 5e-6. A handful of pipelined, vectorisable multiplies instead of an
 * fsqrt + fdiv chain per normalisation. Deterministic: fixed IEEE single-precision operations, no platform
 * intrinsics.
 */
template <int Steps>
DVL_ALWAYS_INLINE float FastRsqrt(float x)
{
	uint32_t bits;
	std::memcpy(&bits, &x, sizeof(bits));
	bits = 0x5F375A86U - (bits >> 1);
	float y;
	std::memcpy(&y, &bits, sizeof(y));
	for (int i = 0; i < Steps; ++i)
		y = y * (1.5F - 0.5F * x * y * y);
	return y;
}

/**
 * Dome normal, detail normal, blend and mirror for every span pixel, in place: gx/gy/lum
 * become the unit card-space normal (cx right, cy up, cz toward the camera). Pure float arithmetic over the spans
 * (vectorises); what it leaves at non-body span pixels is never read.
 */
void BlendNormals(Scratch &s, const NormalParams &p)
{
	const uint32_t h = s.height;
	const auto st = static_cast<ptrdiff_t>(s.stride);
	const float *const dome = s.dome.data();
	float *const gx = s.gx.data();
	float *const gy = s.gy.data();
	float *const gz = s.lum.data();
	const float wS = p.silhouetteWeight;
	const float wD = p.detailWeight;
	const float negStrength = -p.detailStrength;
	for (uint32_t y = 0; y < h; ++y) {
		const uint32_t x0 = s.rowX0[y];
		const uint32_t x1 = s.rowX1[y];
		if (x1 <= x0)
			continue;
		const uint32_t len = x1 - x0;
		const size_t start = s.index(x0, y);
		const float *d = dome + start;
		float *ox = gx + start;
		float *oy = gy + start;
		float *oz = gz + start;
		for (uint32_t k = 0; k < len; ++k) {
			// 3. Silhouette dome normal from central differences. A neighbour outside the silhouette (or the frame)
			// holds dome 0, so every rim pixel slopes outward, including the staircase pixels of a diagonal edge
			// whose opaque neighbours are rim pixels themselves.
			const float *dk = d + k;
			float sx = (dk[-1] - dk[1]) * 0.5F;   // -dH/dx
			float sy = (dk[-st] - dk[st]) * 0.5F; // -dH/dy
			const float sInv = FastRsqrt<1>(sx * sx + sy * sy + 1.0F);
			sx *= sInv;
			sy *= sInv;
			const float sz = sInv;
			// 4b. Detail normal from the de-lit Sobel gradient.
			float dx = negStrength * (ox[k] - p.meanGx);
			float dy = negStrength * (oy[k] - p.meanGy);
			const float dInv = FastRsqrt<1>(dx * dx + dy * dy + 1.0F);
			dx *= dInv;
			dy *= dInv;
			const float dz = dInv;
			// 5. Blend (both terms have z > 0, so the sum never vanishes). The result is what gets quantised, so it is
			// normalised to unit length within 5e-6.
			const float cx = wS * sx + wD * dx;
			const float cy = wS * sy + wD * dy;
			const float cz = wS * sz + wD * dz;
			const float inv = FastRsqrt<2>(cx * cx + cy * cy + cz * cz);
			// 6. Mirror.
			ox[k] = cx * inv * p.mirrorSign;
			oy[k] = cy * inv;
			oz[k] = cz * inv;
		}
	}
}

/** Runs every frame-wide pass on a decoded frame; afterwards gx/gy/lum hold the card normals. */
void ComputeCardNormals(Scratch &s, const Options &opts, bool mirrored)
{
	BuildDomeTable(s, opts);
	ChamferDistance(s);
	NormalParams p {};
	float meanGx;
	float meanGy;
	float meanSquare;
	SobelLuminance(s, meanGx, meanGy, meanSquare);
	p.meanGx = opts.delight ? meanGx : 0.0F;
	p.meanGy = opts.delight ? meanGy : 0.0F;
	p.silhouetteWeight = opts.silhouetteWeight;
	p.detailWeight = 1.0F - opts.silhouetteWeight;
	p.detailStrength = opts.detailStrength;
	if (opts.normalizeDetail) {
		// RMS of the gradient the detail normal sees (de-lit when delight is on): E[|g - mean|^2] = E[|g|^2] - |mean|^2.
		const float variance = std::max(0.0F, meanSquare - p.meanGx * p.meanGx - p.meanGy * p.meanGy);
		p.detailStrength = opts.detailStrength / std::max(std::sqrt(variance), std::max(opts.detailContrastFloor, 1e-4F));
	}
	p.mirrorSign = mirrored ? -1.0F : 1.0F;
	BlendNormals(s, p);
}

// ---------------------------------------------------------------------------------------------------------
// Quantisation: 128x128 card-space direction -> LUT index table built once from
// gbuffer::CardToTile + gbuffer::QuantizeNormal. Every normal that reaches it is a unit vector with cz > 0 (generated
// and authored alike), so (cx, cy) identifies the direction and the lookup needs neither the card->tile multiply
// nor a projection: one table load per pixel.
// ---------------------------------------------------------------------------------------------------------

constexpr int CardTableSize = 128;

const std::array<uint8_t, CardTableSize * CardTableSize> &CardTable()
{
	static const std::array<uint8_t, CardTableSize * CardTableSize> Table = [] {
		std::array<uint8_t, CardTableSize * CardTableSize> table {};
		for (int v = 0; v < CardTableSize; ++v) {
			for (int u = 0; u < CardTableSize; ++u) {
				float cx = (static_cast<float>(u) + 0.5F) * (2.0F / CardTableSize) - 1.0F;
				float cy = (static_cast<float>(v) + 0.5F) * (2.0F / CardTableSize) - 1.0F;
				const float rr = cx * cx + cy * cy;
				float cz;
				if (rr < 1.0F) {
					cz = std::sqrt(1.0F - rr);
				} else {
					// Outside the unit disc (never hit by a unit normal): the horizon direction.
					const float inv = 1.0F / std::sqrt(rr);
					cx *= inv;
					cy *= inv;
					cz = 0.0F;
				}
				float tx;
				float ty;
				float tz;
				gbuffer::CardToTile(cx, cy, cz, tx, ty, tz);
				const float inv = 1.0F / std::sqrt(tx * tx + ty * ty + tz * tz);
				table[static_cast<size_t>(v) * CardTableSize + u] = gbuffer::QuantizeNormal(tx * inv, ty * inv, tz * inv);
			}
		}
		return table;
	}();
	return Table;
}

/** The quantisation table, fetched once per generation. */
struct Quantizer {
	const uint8_t *table;
};

Quantizer MakeQuantizer()
{
	return Quantizer { CardTable().data() };
}

/** Unit card-space normal (cz > 0) -> LUT index in [3, 31]. */
DVL_ALWAYS_INLINE uint8_t QuantizeCardNormal(const Quantizer &q, float cx, float cy, float /*cz*/)
{
	constexpr float Scale = CardTableSize / 2.0F;
	const int u = std::clamp(static_cast<int>((cx + 1.0F) * Scale), 0, CardTableSize - 1);
	const int v = std::clamp(static_cast<int>((cy + 1.0F) * Scale), 0, CardTableSize - 1);
	const uint8_t idx = q.table[static_cast<size_t>(v) * CardTableSize + u];
	return idx < gbuffer::NormalCardFlat ? gbuffer::NormalCardFlat : static_cast<uint8_t>(idx & gbuffer::NormalMask);
}

// ---------------------------------------------------------------------------------------------------------
// Row-span packing
// ---------------------------------------------------------------------------------------------------------

/** Byte layout of a packed plane: uint32 rowOffset[h+1] | uint16 rowX0[h] | pad to 4 | span bytes. */
struct PackedLayout {
	uint32_t rowOffsetBytes;
	uint32_t rowX0Bytes;
	uint32_t dataOffset;
};

inline PackedLayout LayoutFor(uint32_t height)
{
	PackedLayout l;
	l.rowOffsetBytes = (height + 1) * 4;
	l.rowX0Bytes = height * 2;
	l.dataOffset = (l.rowOffsetBytes + l.rowX0Bytes + 3U) & ~3U;
	return l;
}

inline Plane ViewPacked(const uint8_t *bytes, uint16_t width, uint16_t height)
{
	const PackedLayout l = LayoutFor(height);
	Plane p;
	p.rowOffset = reinterpret_cast<const uint32_t *>(bytes);
	p.rowX0 = reinterpret_cast<const uint16_t *>(bytes + l.rowOffsetBytes);
	p.data = bytes + l.dataOffset;
	p.width = width;
	p.height = height;
	return p;
}

/**
 * Lays `out` out as a packed plane over the decoded spans and fills every span byte from `byteAt(i)`, `i` being the
 * Scratch index of that pixel. Returns a view into `out`.
 */
template <typename ByteAt>
Plane PackRowSpans(const Scratch &s, std::vector<uint8_t> &out, ByteAt byteAt)
{
	const uint32_t w = s.width;
	const uint32_t h = s.height;
	const PackedLayout l = LayoutFor(h);
	size_t total = 0;
	for (uint32_t y = 0; y < h; ++y)
		total += s.rowX1[y] > s.rowX0[y] ? s.rowX1[y] - s.rowX0[y] : 0;
	out.resize(l.dataOffset + total);
	auto *rowOffset = reinterpret_cast<uint32_t *>(out.data());
	auto *rowX0 = reinterpret_cast<uint16_t *>(out.data() + l.rowOffsetBytes);
	uint8_t *data = out.data() + l.dataOffset;
	// Zero the pad bytes so the buffer is byte-deterministic.
	std::memset(out.data() + l.rowOffsetBytes + l.rowX0Bytes, 0, l.dataOffset - l.rowOffsetBytes - l.rowX0Bytes);
	uint32_t cursor = 0;
	for (uint32_t y = 0; y < h; ++y) {
		rowOffset[y] = cursor;
		const uint32_t x0 = s.rowX0[y];
		const uint32_t x1 = s.rowX1[y];
		if (x1 <= x0) {
			rowX0[y] = 0;
			continue;
		}
		rowX0[y] = static_cast<uint16_t>(x0);
		size_t i = s.index(x0, y);
		for (uint32_t x = x0; x < x1; ++x, ++i)
			data[cursor++] = byteAt(i);
	}
	rowOffset[h] = cursor;
	return ViewPacked(out.data(), static_cast<uint16_t>(w), static_cast<uint16_t>(h));
}

/** Packs the computed card normals: Card byte / ShadowByte / 0. */
Plane PackGenerated(const Scratch &s, const Quantizer &q, std::vector<uint8_t> &out)
{
	const uint8_t *const mask = s.mask.data();
	const float *const cx = s.gx.data();
	const float *const cy = s.gy.data();
	return PackRowSpans(s, out, [&](size_t i) -> uint8_t {
		const uint8_t m = mask[i];
		if (m == MaskBody)
			return gbuffer::PackAux(gbuffer::Kind::Card, QuantizeCardNormal(q, cx[i], cy[i], 0.0F));
		return m == MaskShadow ? gbuffer::ShadowByte : 0;
	});
}

// ---------------------------------------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------------------------------------

struct Key {
	const uint8_t *pixelData = nullptr;
	uint32_t pixelDataSize = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	bool mirrored = false;

	bool operator==(const Key &other) const
	{
		return pixelData == other.pixelData && pixelDataSize == other.pixelDataSize && width == other.width
		    && height == other.height && mirrored == other.mirrored;
	}
};

inline Key MakeKey(ClxSprite sprite, bool mirrored)
{
	Key k;
	k.pixelData = sprite.pixelData();
	k.pixelDataSize = sprite.pixelDataSize();
	k.width = sprite.width();
	k.height = sprite.height();
	k.mirrored = mirrored;
	return k;
}

/** Cheap mix of the pointer key (no pixel hashing on lookup). */
inline uint64_t HashKey(const Key &k)
{
	uint64_t h = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(k.pixelData));
	h ^= (static_cast<uint64_t>(k.pixelDataSize) << 32) ^ (static_cast<uint64_t>(k.width) << 16) ^ k.height;
	if (k.mirrored)
		h ^= 0x8000000000000000ULL;
	h *= 0x9E3779B97F4A7C15ULL;
	return h ^ (h >> 29);
}

/** Authored override: packed lazily per mirrored flag over the first sprite seen with that content hash; never in the arena. */
struct AuthoredPlane {
	uint16_t width = 0;
	uint16_t height = 0;
	std::vector<uint8_t> rgbTopDown;
	std::vector<uint8_t> packed[2];
	bool built[2] = { false, false };
};

/** Arena block holding one packed plane; shared (refcounted) by entries whose frames hash equal. */
struct Payload {
	uint32_t refCount;
	uint32_t blockBytes;
	uint64_t contentKey; // contentHash64 ^ mirrored salt
	uint16_t width;
	uint16_t height;
	// followed at PayloadHeaderBytes by the packed bytes (uint32 rowOffset[h+1] | uint16 rowX0[h] | pad | data)
};
constexpr uint32_t PayloadHeaderBytes = (sizeof(Payload) + 7U) & ~7U;

inline const uint8_t *PayloadBytes(const Payload *p)
{
	return reinterpret_cast<const uint8_t *>(p) + PayloadHeaderBytes;
}

enum class EntryState : uint8_t { Queued, Ready };

/** Payload::contentKey = contentHash ^ (mirrored ? MirroredContentSalt : 0): mirrored planes alias separately. */
constexpr uint64_t MirroredContentSalt = 0x5bd1e9955bd1e995ULL;

inline uint64_t ContentKeyFor(uint64_t contentHash, bool mirrored)
{
	return contentHash ^ (mirrored ? MirroredContentSalt : 0ULL);
}

/** Arena block: the per-key header and intrusive LRU node. */
struct Entry {
	Key key;
	ClxSprite sprite; // rebuilds the frame for deferred generation (Pump); valid while the key is (evict-on-free)
	uint32_t blockBytes;
	uint32_t projectedBytes; // prewarm projection charged against the 70 % line until built/dropped
	Entry *lruPrev;
	Entry *lruNext;
	Payload *payload;
	const AuthoredPlane *authored;
	EntryState state;
};
constexpr uint32_t EntryBlockBytes = (sizeof(Entry) + 15U) & ~15U;

struct FreeBlock {
	uint32_t offset;
	uint32_t size;
};

[[maybe_unused]] constexpr size_t DefaultNativeBudget = 64U * 1024U * 1024U;
/**
 * Emscripten default, derived from the packed/dense ratio measured by clx_normal_map_benchmark
 * (BM_GeneratePlane/128 monster-like frame 0.27, disc.clx 0.37, ramp.clx 0.45; see test/fixtures/normal_map/README.md):
 *   frames      = 8 types x 616 frames/type x (2 of 6 anims: Stand, Walk) = 1643 frames at 128 px
 *   bytes/frame = 128*128*ratio + rowOffset (129*4) + rowX0 (128*2) + pad + payload header + entry header
 *               ~= 7.4 KB at the fixtures' worst ratio 0.45 (9.0 KB at a pessimistic 0.55 for wide poses)
 *   1.5 x 1643 x 7.4 KB ~= 17.8 MiB (21.7 MiB at 0.55) -> smallest multiple of 8 MiB >= that = 24 MiB
 */
[[maybe_unused]] constexpr size_t DefaultEmscriptenBudget = 24U * 1024U * 1024U;

constexpr size_t DefaultBudget()
{
#ifdef __EMSCRIPTEN__
	return DefaultEmscriptenBudget;
#else
	return DefaultNativeBudget;
#endif
}

constexpr size_t MinBudget = 16U * 1024U;
constexpr float PrewarmFillFraction = 0.7F;

struct Cache {
	// Arena
	std::unique_ptr<uint8_t[]> arena;
	size_t budget = 0;
	std::vector<FreeBlock> freeList; // sorted by offset, coalesced
	size_t usedBytes = 0;

	// Table (open addressing, linear probing, backward-shift delete)
	std::vector<Entry *> slots;
	size_t entryCount = 0;
	uint32_t slotMask = 0;

	// LRU
	Entry *lruHead = nullptr;
	Entry *lruTail = nullptr;

	// Content index for aliasing (contentKey -> payload)
	std::unordered_map<uint64_t, Payload *> byContent;

	// Authored registry (never in the arena, never LRU-evicted)
	std::unordered_map<uint64_t, std::unique_ptr<AuthoredPlane>> authored;
	uint64_t authoredVersion = 0;

	// Queue of pending keys (drained by Pump)
	std::vector<Key> queue;
	size_t queueHead = 0;
	size_t pendingProjected = 0;

	// Measured packed/dense ratio
	double packedSum = 0.0;
	double denseSum = 0.0;

	// Address bounds of every cached pixelData pointer (fast reject for EvictNormalPlanes)
	uintptr_t minPtr = UINTPTR_MAX;
	uintptr_t maxPtr = 0;

	// Options / budget
	Options options;
	uint32_t framesPerGameFrame = 8;
	uint32_t microsPerGameFrame = 1500;

	// Stats
	size_t generatedThisFrame = 0;
	double microsThisFrame = 0.0;
	/** Measured generation cost (moving average, microseconds per frame pixel): Pump's estimate of the next item. */
	double microsPerPixel = 0.0;
	size_t prewarmDropped = 0;
	size_t evictions = 0;
	size_t hits = 0;
	size_t misses = 0;
	size_t authoredHits = 0;

	std::vector<uint8_t> genBuffer;
	Scratch scratch;
};

/**
 * Immortal singleton. EvictNormalPlanes runs from the destructors of every Owned* sprite, including namespace-scope
 * globals (Players' animation sheets, the gmenu/cursor/inventory/towner sprites, ...) that were constructed at
 * static-init time and are therefore destroyed AFTER a function-local static first touched during play. The cache is
 * heap-allocated once and never freed so that those late evictions (and ClearNormalPlaneCache) always see a live
 * object; the OS reclaims the memory at exit.
 */
Cache &TheCache()
{
	static Cache *const cache = new Cache;
	return *cache;
}

PlaneProvider TestProvider = nullptr;

using Clock = std::chrono::steady_clock;

inline double MicrosSince(Clock::time_point t0)
{
	return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// ---- Arena ----

void ResetArena(Cache &c)
{
	c.freeList.clear();
	if (c.budget > 0)
		c.freeList.push_back(FreeBlock { 0, static_cast<uint32_t>(c.budget) });
	c.usedBytes = 0;
}

void *ArenaAlloc(Cache &c, uint32_t bytes)
{
	bytes = (bytes + 15U) & ~15U;
	for (size_t i = 0; i < c.freeList.size(); ++i) {
		FreeBlock &fb = c.freeList[i];
		if (fb.size < bytes)
			continue;
		const uint32_t offset = fb.offset;
		fb.offset += bytes;
		fb.size -= bytes;
		if (fb.size == 0)
			c.freeList.erase(c.freeList.begin() + static_cast<ptrdiff_t>(i));
		c.usedBytes += bytes;
		return c.arena.get() + offset;
	}
	return nullptr;
}

void ArenaFree(Cache &c, void *ptr, uint32_t bytes)
{
	bytes = (bytes + 15U) & ~15U;
	const uint32_t offset = static_cast<uint32_t>(static_cast<uint8_t *>(ptr) - c.arena.get());
	c.usedBytes -= bytes;
	auto it = std::lower_bound(c.freeList.begin(), c.freeList.end(), offset,
	    [](const FreeBlock &fb, uint32_t off) { return fb.offset < off; });
	// Coalesce with the previous block.
	if (it != c.freeList.begin()) {
		auto prev = it - 1;
		if (prev->offset + prev->size == offset) {
			prev->size += bytes;
			if (it != c.freeList.end() && prev->offset + prev->size == it->offset) {
				prev->size += it->size;
				c.freeList.erase(it);
			}
			return;
		}
	}
	// Coalesce with the next block.
	if (it != c.freeList.end() && offset + bytes == it->offset) {
		it->offset = offset;
		it->size += bytes;
		return;
	}
	c.freeList.insert(it, FreeBlock { offset, bytes });
}

// ---- Table ----

void RehashTable(Cache &c, size_t newCapacity)
{
	std::vector<Entry *> old = std::move(c.slots);
	c.slots.assign(newCapacity, nullptr);
	c.slotMask = static_cast<uint32_t>(newCapacity - 1);
	for (Entry *e : old) {
		if (e == nullptr)
			continue;
		uint32_t i = static_cast<uint32_t>(HashKey(e->key)) & c.slotMask;
		while (c.slots[i] != nullptr)
			i = (i + 1) & c.slotMask;
		c.slots[i] = e;
	}
}

inline Entry *FindEntry(Cache &c, const Key &key)
{
	if (c.slots.empty())
		return nullptr;
	uint32_t i = static_cast<uint32_t>(HashKey(key)) & c.slotMask;
	while (true) {
		Entry *e = c.slots[i];
		if (e == nullptr)
			return nullptr;
		if (e->key == key)
			return e;
		i = (i + 1) & c.slotMask;
	}
}

void InsertEntry(Cache &c, Entry *e)
{
	if (c.slots.empty())
		RehashTable(c, 64);
	else if ((c.entryCount + 1) * 10 > c.slots.size() * 7)
		RehashTable(c, c.slots.size() * 2);
	uint32_t i = static_cast<uint32_t>(HashKey(e->key)) & c.slotMask;
	while (c.slots[i] != nullptr)
		i = (i + 1) & c.slotMask;
	c.slots[i] = e;
	++c.entryCount;
	const uintptr_t p = reinterpret_cast<uintptr_t>(e->key.pixelData);
	c.minPtr = std::min(c.minPtr, p);
	c.maxPtr = std::max(c.maxPtr, p);
}

void RemoveFromTable(Cache &c, Entry *e)
{
	uint32_t i = static_cast<uint32_t>(HashKey(e->key)) & c.slotMask;
	while (c.slots[i] != e)
		i = (i + 1) & c.slotMask;
	c.slots[i] = nullptr;
	--c.entryCount;
	// Backward-shift deletion keeps every probe chain intact without tombstones.
	uint32_t j = i;
	while (true) {
		j = (j + 1) & c.slotMask;
		Entry *other = c.slots[j];
		if (other == nullptr)
			break;
		const uint32_t home = static_cast<uint32_t>(HashKey(other->key)) & c.slotMask;
		// `other` may move into the hole at i unless its home lies in (i, j] (cyclically).
		const bool homeAfterHole = i <= j ? (home > i && home <= j) : (home > i || home <= j);
		if (!homeAfterHole) {
			c.slots[i] = other;
			c.slots[j] = nullptr;
			i = j;
		}
	}
}

// ---- LRU ----

inline void LruUnlink(Cache &c, Entry *e)
{
	if (e->lruPrev != nullptr)
		e->lruPrev->lruNext = e->lruNext;
	else
		c.lruHead = e->lruNext;
	if (e->lruNext != nullptr)
		e->lruNext->lruPrev = e->lruPrev;
	else
		c.lruTail = e->lruPrev;
	e->lruPrev = nullptr;
	e->lruNext = nullptr;
}

inline void LruPushFront(Cache &c, Entry *e)
{
	e->lruPrev = nullptr;
	e->lruNext = c.lruHead;
	if (c.lruHead != nullptr)
		c.lruHead->lruPrev = e;
	c.lruHead = e;
	if (c.lruTail == nullptr)
		c.lruTail = e;
}

inline void LruTouch(Cache &c, Entry *e)
{
	if (c.lruHead == e)
		return;
	LruUnlink(c, e);
	LruPushFront(c, e);
}

// ---- Payloads / entries ----

void ReleasePayload(Cache &c, Payload *p)
{
	if (p == nullptr)
		return;
	if (--p->refCount > 0)
		return;
	c.byContent.erase(p->contentKey);
	ArenaFree(c, p, p->blockBytes);
}

/** Removes an entry from every structure and returns its block to the arena. Does not count as an LRU eviction. */
void DestroyEntry(Cache &c, Entry *e)
{
	RemoveFromTable(c, e);
	LruUnlink(c, e);
	ReleasePayload(c, e->payload);
	if (e->state == EntryState::Queued)
		c.pendingProjected -= std::min<size_t>(c.pendingProjected, e->projectedBytes);
	ArenaFree(c, e, e->blockBytes);
}

/** Evicts the least recently used entry (never `protect`). Returns false when nothing could be evicted. */
bool EvictOne(Cache &c, const Entry *protect)
{
	Entry *victim = c.lruTail;
	while (victim != nullptr && victim == protect)
		victim = victim->lruPrev;
	if (victim == nullptr)
		return false;
	DestroyEntry(c, victim);
	++c.evictions;
	return true;
}

void *ArenaAllocEvicting(Cache &c, uint32_t bytes, const Entry *protect)
{
	while (true) {
		void *p = ArenaAlloc(c, bytes);
		if (p != nullptr)
			return p;
		if (!EvictOne(c, protect))
			return nullptr;
	}
}

void SizeTable(Cache &c)
{
	size_t cap = 64;
	while (cap < c.budget / 2048)
		cap *= 2;
	c.slots.assign(cap, nullptr);
	c.slotMask = static_cast<uint32_t>(cap - 1);
}

void EnsureArena(Cache &c)
{
	if (c.arena != nullptr)
		return;
	c.budget = DefaultBudget();
	c.arena.reset(new uint8_t[c.budget]);
	ResetArena(c);
	SizeTable(c);
}

Entry *NewEntry(Cache &c, const Key &key, ClxSprite sprite)
{
	void *block = ArenaAllocEvicting(c, EntryBlockBytes, nullptr);
	if (block == nullptr)
		return nullptr;
	Entry *e = new (block) Entry { key, sprite, EntryBlockBytes, 0, nullptr, nullptr, nullptr, nullptr, EntryState::Queued };
	InsertEntry(c, e);
	LruPushFront(c, e);
	return e;
}

inline size_t ProjectedPackedBytes(const Cache &c, uint32_t w, uint32_t h)
{
	const double ratio = c.denseSum > 0.0 ? c.packedSum / c.denseSum : 0.5;
	const PackedLayout l = LayoutFor(h);
	return static_cast<size_t>(static_cast<double>(w) * h * ratio) + l.dataOffset + PayloadHeaderBytes + EntryBlockBytes;
}

AuthoredPlane *FindAuthored(Cache &c, uint64_t contentHash, uint16_t w, uint16_t h)
{
	auto it = c.authored.find(contentHash);
	if (it == c.authored.end())
		return nullptr;
	AuthoredPlane *a = it->second.get();
	if (a->width != w || a->height != h)
		return nullptr;
	return a;
}

/** Packs the authored RGB normals over the sprite's opaque pixels (mirrored/converted/quantised like generated ones). */
bool BuildAuthoredPacked(Cache &c, AuthoredPlane &a, ClxSprite sprite, bool mirrored)
{
	const size_t m = mirrored ? 1 : 0;
	if (a.built[m])
		return true;
	Scratch &s = c.scratch;
	if (!DecodeFrame(sprite, nullptr, /*perceptual=*/false, s))
		return false;
	const Quantizer q = MakeQuantizer();
	const uint32_t w = s.width;
	const uint32_t h = s.height;
	const uint8_t *const mask = s.mask.data();
	const uint8_t *const rgbTopDown = a.rgbTopDown.data();
	(void)PackRowSpans(s, a.packed[m], [&](size_t i) -> uint8_t {
		const uint8_t kind = mask[i];
		if (kind != MaskBody)
			return kind == MaskShadow ? gbuffer::ShadowByte : 0;
		// Scratch index -> (x, y from the feet) -> authored row (top-down).
		const uint32_t y = static_cast<uint32_t>(i / s.stride) - 1;
		const uint32_t x = static_cast<uint32_t>(i % s.stride) - 1;
		const uint8_t *rgb = rgbTopDown + (static_cast<size_t>(h - 1 - y) * w + x) * 3;
		float nx = static_cast<float>(rgb[0]) * (2.0F / 255.0F) - 1.0F;
		float ny = static_cast<float>(rgb[1]) * (2.0F / 255.0F) - 1.0F;
		float nz = std::max(static_cast<float>(rgb[2]) / 255.0F, 1e-3F);
		const float inv = 1.0F / std::sqrt(nx * nx + ny * ny + nz * nz);
		nx *= inv;
		ny *= inv;
		nz *= inv;
		if (mirrored)
			nx = -nx;
		return gbuffer::PackAux(gbuffer::Kind::Card, QuantizeCardNormal(q, nx, ny, nz));
	});
	a.built[m] = true;
	return true;
}

inline Plane EntryView(Cache &c, const Entry *e)
{
	if (e->authored != nullptr) {
		++c.authoredHits;
		return ViewPacked(e->authored->packed[e->key.mirrored ? 1 : 0].data(), e->key.width, e->key.height);
	}
	return ViewPacked(PayloadBytes(e->payload), e->key.width, e->key.height);
}

/**
 * Resolves a Queued entry: authored override first, then a content alias, then a fresh generation.
 * Returns false when the entry could not be filled (it is destroyed). `generated` is set when real work ran.
 */
bool BuildEntry(Cache &c, Entry *e, bool &generated)
{
	generated = false;
	const ClxSprite sprite = e->sprite;
	const uint64_t contentHash = HashPixels(sprite);
	const uint64_t contentKey = ContentKeyFor(contentHash, e->key.mirrored);

	c.pendingProjected -= std::min<size_t>(c.pendingProjected, e->projectedBytes);
	e->projectedBytes = 0;

	// Keep the entry off the LRU while allocating so it can never be evicted underneath itself.
	LruUnlink(c, e);

	if (AuthoredPlane *a = FindAuthored(c, contentHash, e->key.width, e->key.height); a != nullptr) {
		if (BuildAuthoredPacked(c, *a, sprite, e->key.mirrored)) {
			e->authored = a;
			e->state = EntryState::Ready;
			LruPushFront(c, e);
			return true;
		}
	}

	if (auto it = c.byContent.find(contentKey); it != c.byContent.end()) {
		Payload *p = it->second;
		if (p->width == e->key.width && p->height == e->key.height) {
			++p->refCount;
			e->payload = p;
			e->state = EntryState::Ready;
			LruPushFront(c, e);
			return true;
		}
	}

	const Plane gen = GeneratePlane(sprite, std::span<const SDL_Color, 256>(gbuffer::CurrentPalette(), 256), c.options, e->key.mirrored, c.genBuffer, c.scratch);
	generated = true;
	if (gen.rowOffset == nullptr) {
		LruPushFront(c, e);
		DestroyEntry(c, e);
		return false;
	}
	c.packedSum += static_cast<double>(gen.bytes());
	c.denseSum += static_cast<double>(e->key.width) * e->key.height;

	const uint32_t blockBytes = PayloadHeaderBytes + static_cast<uint32_t>(c.genBuffer.size());
	void *block = ArenaAllocEvicting(c, blockBytes, e);
	if (block == nullptr) {
		LruPushFront(c, e);
		DestroyEntry(c, e);
		return false;
	}
	Payload *p = static_cast<Payload *>(block);
	p->refCount = 1;
	p->blockBytes = blockBytes;
	p->contentKey = contentKey;
	p->width = e->key.width;
	p->height = e->key.height;
	std::memcpy(static_cast<uint8_t *>(block) + PayloadHeaderBytes, c.genBuffer.data(), c.genBuffer.size());
	c.byContent[contentKey] = p;
	e->payload = p;
	e->state = EntryState::Ready;
	LruPushFront(c, e);
	return true;
}

/** Folds one generation into the cost estimate Pump uses to stop before an item would overrun its budget. */
void NoteGenerationCost(Cache &c, const Key &key, double micros)
{
	const double pixels = std::max(1.0, static_cast<double>(key.width) * key.height);
	const double perPixel = micros / pixels;
	c.microsPerPixel = c.microsPerPixel == 0.0 ? perPixel : c.microsPerPixel * 0.875 + perPixel * 0.125;
}

void EnqueueList(Cache &c, ClxSpriteList list)
{
	EnsureArena(c);
	// Prewarm runs while a level loads: build the quantisation tables (the card-space table and gbuffer's octahedral
	// candidate table, tens of ms in the browser) there, not inside the first Pump or lookup of the level's first frame.
	(void)CardTable();
	const size_t limit = static_cast<size_t>(static_cast<double>(c.budget) * PrewarmFillFraction);
	for (const ClxSprite sprite : list) {
		const Key key = MakeKey(sprite, false);
		if (FindEntry(c, key) != nullptr)
			continue;
		const size_t projected = ProjectedPackedBytes(c, sprite.width(), sprite.height());
		if (c.usedBytes + c.pendingProjected + projected > limit) {
			++c.prewarmDropped;
			continue;
		}
		Entry *e = NewEntry(c, key, sprite);
		if (e == nullptr) {
			++c.prewarmDropped;
			continue;
		}
		e->projectedBytes = static_cast<uint32_t>(std::min<size_t>(projected, UINT32_MAX));
		c.pendingProjected += projected;
		c.queue.push_back(key);
	}
}

void ClearCache(Cache &c)
{
	std::fill(c.slots.begin(), c.slots.end(), nullptr);
	c.entryCount = 0;
	c.lruHead = nullptr;
	c.lruTail = nullptr;
	c.byContent.clear();
	c.queue.clear();
	c.queueHead = 0;
	c.pendingProjected = 0;
	c.minPtr = UINTPTR_MAX;
	c.maxPtr = 0;
	c.prewarmDropped = 0;
	c.evictions = 0;
	ResetArena(c);
}

inline bool UnderFrameBudget(const Cache &c)
{
	return c.generatedThisFrame < c.framesPerGameFrame && c.microsThisFrame < static_cast<double>(c.microsPerGameFrame);
}

/** Resolves a Queued entry synchronously, charging the per-game-frame budget for real generation work. */
bool BuildEntryNow(Cache &c, Entry *e)
{
	// Build the quantisation tables (the card-space table and gbuffer's octahedral candidate table) before the timed
	// region: charging their one-time cost (several ms) to the first generated entry would exhaust that game frame's
	// microsecond budget and turn every further lookup in the frame into a flat-tier miss.
	(void)CardTable();
	const Clock::time_point t0 = Clock::now();
	const Key key = e->key; // a failed build destroys the entry
	bool generated = false;
	const bool ok = BuildEntry(c, e, generated);
	if (generated) {
		NoteGenerationCost(c, key, MicrosSince(t0));
		++c.generatedThisFrame;
		c.microsThisFrame += MicrosSince(t0);
	}
	return ok;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------------

Plane GetOrBuildNormalPlane(ClxSprite sprite, bool mirrored)
{
	if (TestProvider != nullptr)
		return TestProvider(sprite, mirrored);
	Cache &c = TheCache();
	if (!c.options.generate)
		return Plane {};
	EnsureArena(c);

	const Key key = MakeKey(sprite, mirrored);
	if (Entry *e = FindEntry(c, key); e != nullptr) {
		if (e->state == EntryState::Ready) {
			++c.hits;
			LruTouch(c, e);
			return EntryView(c, e);
		}
		// Queued (prewarm, or over budget in an earlier frame) but not yet generated: a miss under budget generates
		// now instead of waiting for Pump to reach it in FIFO order. Pump skips entries that are no longer Queued.
		++c.misses;
		if (UnderFrameBudget(c) && BuildEntryNow(c, e))
			return EntryView(c, e);
		return Plane {};
	}
	++c.misses;

	if (UnderFrameBudget(c)) {
		Entry *e = NewEntry(c, key, sprite);
		if (e == nullptr)
			return Plane {};
		if (!BuildEntryNow(c, e))
			return Plane {};
		return EntryView(c, e);
	}

	// Over budget: queue for Pump.
	Entry *e = NewEntry(c, key, sprite);
	if (e == nullptr)
		return Plane {};
	c.queue.push_back(key);
	return Plane {};
}

Plane GeneratePlane(ClxSprite sprite, std::span<const SDL_Color, 256> logicalPalette, const Options &opts, bool mirrored, std::vector<uint8_t> &out, Scratch &scratch)
{
	if (!opts.generate)
		return Plane {};
	if (!DecodeFrame(sprite, logicalPalette.data(), opts.perceptualDetail, scratch))
		return Plane {};
	ComputeCardNormals(scratch, opts, mirrored);
	return PackGenerated(scratch, MakeQuantizer(), out);
}

uint64_t HashPixels(ClxSprite sprite)
{
	const uint8_t *p = sprite.pixelData();
	const uint8_t *const end = p + sprite.pixelDataSize();
	uint64_t h = 0xcbf29ce484222325ULL;
	for (; p != end; ++p) {
		h ^= *p;
		h *= 0x100000001b3ULL;
	}
	return h;
}

void RegisterAuthoredNormalMap(uint64_t contentHash, uint64_t materialVersion, std::span<const uint8_t> rgbTopDown, uint16_t width, uint16_t height)
{
	Cache &c = TheCache();
	const size_t needed = static_cast<size_t>(width) * height * 3;
	if (width == 0 || height == 0 || rgbTopDown.size() < needed)
		return;
	// Entries that reference an authored plane about to be replaced or dropped must be re-resolved.
	const auto dropEntriesBackedBy = [&](const AuthoredPlane *a) {
		std::vector<Entry *> victims;
		for (Entry *e : c.slots) {
			if (e != nullptr && e->authored == a)
				victims.push_back(e);
		}
		for (Entry *e : victims)
			DestroyEntry(c, e);
	};
	if (materialVersion != c.authoredVersion) {
		// One package per materialVersion: a new pack replaces the previous one wholesale.
		for (auto &[hash, plane] : c.authored)
			dropEntriesBackedBy(plane.get());
		c.authored.clear();
		c.authoredVersion = materialVersion;
	} else if (auto it = c.authored.find(contentHash); it != c.authored.end()) {
		dropEntriesBackedBy(it->second.get());
		c.authored.erase(it);
	}
	// Generated planes already cached for this content are superseded so that "authored wins" holds regardless of
	// registration order. Queued entries resolve through FindAuthored when they are built.
	{
		std::vector<Entry *> victims;
		for (Entry *e : c.slots) {
			if (e != nullptr && e->payload != nullptr && e->payload->contentKey == ContentKeyFor(contentHash, e->key.mirrored))
				victims.push_back(e);
		}
		for (Entry *e : victims)
			DestroyEntry(c, e);
	}
	auto plane = std::make_unique<AuthoredPlane>();
	plane->width = width;
	plane->height = height;
	plane->rgbTopDown.assign(rgbTopDown.begin(), rgbTopDown.begin() + static_cast<ptrdiff_t>(needed));
	c.authored[contentHash] = std::move(plane);
}

void SetOptions(const Options &opts)
{
	Cache &c = TheCache();
	const Options &cur = c.options;
	const bool same = cur.generate == opts.generate && cur.silhouetteWeight == opts.silhouetteWeight
	    && cur.detailStrength == opts.detailStrength && cur.maxInflateRadius == opts.maxInflateRadius && cur.delight == opts.delight
	    && cur.perceptualDetail == opts.perceptualDetail && cur.normalizeDetail == opts.normalizeDetail
	    && cur.detailContrastFloor == opts.detailContrastFloor;
	if (same)
		return;
	c.options = opts;
	// Planes are a pure function of frame + palette + options: anything cached under the old options is stale.
	if (c.arena != nullptr)
		ClearCache(c);
}

void SetBudget(uint32_t framesPerGameFrame, uint32_t microsPerGameFrame)
{
	Cache &c = TheCache();
	c.framesPerGameFrame = framesPerGameFrame;
	c.microsPerGameFrame = microsPerGameFrame;
}

void SetCacheBudget(size_t bytes)
{
	Cache &c = TheCache();
	bytes = std::clamp<size_t>(bytes, MinBudget, static_cast<size_t>(0xFFFFFFF0U));
	c.budget = bytes;
	c.arena.reset(new uint8_t[bytes]);
	SizeTable(c);
	ClearCache(c);
}

void EnqueuePrewarm(ClxSpriteList list)
{
	Cache &c = TheCache();
	if (!c.options.generate)
		return;
	EnqueueList(c, list);
}

void EnqueuePrewarm(ClxSpriteSheet sheet)
{
	Cache &c = TheCache();
	if (!c.options.generate)
		return;
	for (const ClxSpriteList list : sheet)
		EnqueueList(c, list);
}

size_t Pump(uint32_t budgetMicros)
{
	Cache &c = TheCache();
	size_t generatedCount = 0;
	if (c.queueHead >= c.queue.size())
		return 0;
	const Clock::time_point t0 = Clock::now();
	// The quantisation tables are built once, normally by the level's prewarm (EnqueueList); building them here spends
	// this call's budget.
	(void)CardTable();
	bool worked = false;
	while (c.queueHead < c.queue.size()) {
		const double elapsed = MicrosSince(t0);
		if (elapsed >= static_cast<double>(budgetMicros))
			break;
		const Key key = c.queue[c.queueHead];
		Entry *e = FindEntry(c, key);
		const bool queued = e != nullptr && e->state == EntryState::Queued;
		// The budget is checked between items, so also stop before an item whose estimated cost would overrun it (the
		// first item of a call always runs: a frame larger than the whole budget still gets built).
		if (queued && worked && elapsed + c.microsPerPixel * key.width * key.height > static_cast<double>(budgetMicros))
			break;
		++c.queueHead;
		if (!queued)
			continue; // evicted or freed since it was queued: dropped
		worked = true;
		bool generated = false;
		const Clock::time_point itemStart = Clock::now();
		if (BuildEntry(c, e, generated) && generated) {
			++generatedCount;
			NoteGenerationCost(c, key, MicrosSince(itemStart));
		}
	}
	if (c.queueHead >= c.queue.size()) {
		c.queue.clear();
		c.queueHead = 0;
	} else if (c.queueHead > 1024 && c.queueHead * 2 > c.queue.size()) {
		c.queue.erase(c.queue.begin(), c.queue.begin() + static_cast<ptrdiff_t>(c.queueHead));
		c.queueHead = 0;
	}
	return generatedCount;
}

void ClearNormalPlaneCache()
{
	Cache &c = TheCache();
	if (c.arena == nullptr)
		return;
	ClearCache(c);
}

void EvictNormalPlanes(const void *dataBegin, size_t dataSize)
{
	if (HdUiHooks != nullptr) [[unlikely]]
		HdUiHooks->spritesFreed(dataBegin, dataSize);
	Cache &c = TheCache();
	if (c.entryCount == 0 || dataBegin == nullptr || dataSize == 0)
		return;
	const uintptr_t begin = reinterpret_cast<uintptr_t>(dataBegin);
	const uintptr_t end = begin + dataSize;
	if (end <= c.minPtr || begin > c.maxPtr)
		return;
	std::vector<Entry *> victims;
	for (Entry *e : c.slots) {
		if (e == nullptr)
			continue;
		const uintptr_t p = reinterpret_cast<uintptr_t>(e->key.pixelData);
		if (p >= begin && p < end)
			victims.push_back(e);
	}
	for (Entry *e : victims)
		DestroyEntry(c, e);
}

Stats GetStats()
{
	Cache &c = TheCache();
	Stats s {};
	s.entries = c.entryCount;
	s.bytes = c.usedBytes;
	s.arenaBytes = c.arena != nullptr ? c.budget : 0;
	s.generatedThisFrame = c.generatedThisFrame;
	s.queued = c.queue.size() - c.queueHead;
	s.prewarmDropped = c.prewarmDropped;
	s.evictions = c.evictions;
	s.hits = c.hits;
	s.misses = c.misses;
	s.authoredHits = c.authoredHits;
	return s;
}

void ResetFrameStats()
{
	Cache &c = TheCache();
	c.generatedThisFrame = 0;
	c.microsThisFrame = 0.0;
	c.hits = 0;
	c.misses = 0;
	c.authoredHits = 0;
}

void SetNormalPlaneProviderForTesting(PlaneProvider provider)
{
	TestProvider = provider;
}

// ---------------------------------------------------------------------------------------------------------
// Test seams (declared by test/clx_normal_map_test.cpp and the benchmark; not part of the public header)
// ---------------------------------------------------------------------------------------------------------

Scratch &SharedScratchForTesting()
{
	return TheCache().scratch;
}

/** Card-space normals before quantisation: out[(y * w + x) * 3 + {0,1,2}] for body pixels, zero elsewhere. Row 0 = feet. */
bool GenerateCardNormalsForTesting(ClxSprite sprite, std::span<const SDL_Color, 256> logicalPalette, const Options &opts, bool mirrored, std::vector<float> &out, Scratch &scratch)
{
	if (!DecodeFrame(sprite, logicalPalette.data(), opts.perceptualDetail, scratch))
		return false;
	ComputeCardNormals(scratch, opts, mirrored);
	const uint32_t w = scratch.width;
	const uint32_t h = scratch.height;
	out.assign(static_cast<size_t>(w) * h * 3, 0.0F);
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			const size_t i = scratch.index(x, y);
			if (scratch.mask[i] != MaskBody)
				continue;
			float *n = out.data() + (static_cast<size_t>(y) * w + x) * 3;
			n[0] = scratch.gx[i];
			n[1] = scratch.gy[i];
			n[2] = scratch.lum[i];
		}
	}
	return true;
}

/** Table quantisation of a unit card-space normal (what the packed bytes carry). */
uint8_t QuantizeCardNormalForTesting(float cx, float cy, float cz)
{
	return QuantizeCardNormal(MakeQuantizer(), cx, cy, cz);
}

} // namespace devilution::clx_normal_map
