/**
 * @file hd_ui_core.hpp
 *
 * Engine-independent parts of the opt-in HD UI (`?hdUi=1`):
 * the hd/ui.txt index, the asset key interner, the registry that maps loaded CLX frames to asset keys, the HD
 * content of off-screen surfaces (the control panel buffers, composed char panel and buttons), CLX opacity masks,
 * the retained layer tracker that decides which HD UI texels the classic painter order shows, and the colour
 * translation (TRN) lookup tables.
 *
 * Model: the classic UI is drawn exactly as before into the 8-bit back buffer. Every classic draw of a registered UI
 * frame (or a blit of an off-screen surface holding such frames) also records a *layer*: the HD image of that frame
 * over the same rectangle, plus a snapshot of the back buffer under it right after the classic draw and the mask of
 * pixels that draw wrote. Before presenting, LayerTracker::resolve compares the final back buffer with every
 * snapshot. A pixel written later by a classic-only draw hides the layers below it there (classic text over an HD
 * panel stays visible); a pixel written later by another HD layer does not (that layer is drawn over it on the
 * GPU). The GPU draws every visible layer in record order at canvas resolution over the classic frame.
 *
 * Layers are retained across frames (the 640 px wide UI only redraws dirty components): a layer goes away when a
 * later layer covers every pixel it wrote, when its area is invalidated (the world pass redraws the viewport), or
 * when the final back buffer no longer shows it.
 *
 * Nothing here touches game state, SDL or WebGPU: covered by test/hd_ui_test.cpp.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace devilution::hd_ui {

struct Rect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	[[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }
	[[nodiscard]] int right() const { return x + w; }
	[[nodiscard]] int bottom() const { return y + h; }
	[[nodiscard]] bool contains(const Rect &o) const
	{
		return !o.empty() && o.x >= x && o.y >= y && o.right() <= right() && o.bottom() <= bottom();
	}
	[[nodiscard]] bool intersects(const Rect &o) const
	{
		return !empty() && !o.empty() && o.x < right() && x < o.right() && o.y < bottom() && y < o.bottom();
	}
	[[nodiscard]] Rect intersect(const Rect &o) const;
	[[nodiscard]] Rect translated(int dx, int dy) const { return { x + dx, y + dy, w, h }; }
	bool operator==(const Rect &) const = default;
};

/** Union of two rectangles (an empty one is ignored). */
Rect Union(const Rect &a, const Rect &b);

// ---- hd/ui.txt ----

struct SheetInfo {
	std::string png;
	int width = 0;  ///< texels at full (4x) size
	int height = 0;
	/** R = palette index, G = coverage, A = 255 where covered: drawn through the TRN and the live palette. */
	bool indexed = false;
};

struct FrameInfo {
	int sheet = -1; ///< -1: no HD image for this frame
	int x = 0;      ///< texel rectangle in the sheet at full size
	int y = 0;
	int w = 0;
	int h = 0;
};

/**
 * hd/ui.txt:
 *     scale <S>                                     texels per classic pixel (4)
 *     sheet <id> <png> <width> <height> [indexed]   ids contiguous from 0
 *     frame <key> <index> <sheet> <x> <y> <w> <h>   frame <index> (0-based) of the asset <key>
 * <key> = asset path as loaded, lowercase, '/' separated, no extension (ctrlpan/panel8, data/inv/objcurs,
 * ui_art/mainmenu, fonts/12-00); sprite sheets (lists of lists) add "#<list>" (data/foo#2).
 */
struct UiIndex {
	int scale = 4;
	std::vector<SheetInfo> sheets;
	std::unordered_map<std::string, std::vector<FrameInfo>> frames;
	std::vector<std::string> errors;

	[[nodiscard]] const FrameInfo *find(std::string_view key, int frame) const;
	[[nodiscard]] bool hasKey(std::string_view key) const;
};

UiIndex ParseUiIndex(std::string_view text);

/** "ctrlpan\\Panel8.CEL" -> "ctrlpan/panel8". */
std::string NormalizeAssetKey(std::string_view path);

/** Only these asset folders hold UI art; other loads are not registered. */
bool IsUiAssetKey(std::string_view key);

/** Process-wide key interner: small ints for asset keys (registered before the index loads). */
int InternKey(std::string_view key);
const std::string &KeyName(int keyId);
size_t KeyCount();

// ---- CLX masks ----

/** One bit (byte) per pixel of `rect`; 1 = written by the draw (every byte is 0 or 1). */
struct Mask {
	Rect rect;
	std::vector<uint8_t> bits;

	[[nodiscard]] bool at(int x, int y) const
	{
		if (x < rect.x || y < rect.y || x >= rect.right() || y >= rect.bottom())
			return false;
		return bits[static_cast<size_t>(y - rect.y) * rect.w + (x - rect.x)] != 0;
	}
	[[nodiscard]] size_t count() const;
	/** True when every set pixel of `other` is set here. */
	[[nodiscard]] bool covers(const Mask &other) const;
	static Mask Full(const Rect &rect);
	/** This mask restricted to `rect` (pixels outside become 0 / are cut). The rvalue overloads reuse the storage. */
	[[nodiscard]] Mask clipped(const Rect &rect) const &;
	[[nodiscard]] Mask clipped(const Rect &rect) &&;
	[[nodiscard]] Mask translated(int dx, int dy) const &;
	[[nodiscard]] Mask translated(int dx, int dy) &&;
	/** Pixelwise AND with `other` (same coordinate space), restricted to this mask's rectangle. */
	[[nodiscard]] Mask intersected(const Mask &other) const &;
	[[nodiscard]] Mask intersected(const Mask &other) &&;
};

/**
 * Opaque pixels of a CLX frame (`pixelData` = CL2-style commands, bottom-up, `width` x `height`) drawn with its top-left
 * at (left, top), restricted to `clip`. The mask rectangle is the clipped frame rectangle.
 */
Mask ClxOpaqueMask(const uint8_t *pixelData, size_t pixelDataSize, int width, int height, int left, int top, const Rect &clip);

// ---- What a layer draws ----

/** Colour translation tables seen with UI draws, deduplicated by content (index -1 = none). */
class TrnPool {
public:
	static constexpr size_t Capacity = 64;
	/** Index of `trn` (256 entries) in the pool; -1 when the pool is full. */
	int intern(const uint8_t *trn);
	[[nodiscard]] const std::array<uint8_t, 256> &table(int index) const { return tables_[index]; }
	[[nodiscard]] size_t size() const { return tables_.size(); }
	/** Bumped whenever a table is added (GPU uploads follow it). */
	[[nodiscard]] uint32_t generation() const { return generation_; }
	void clear();

private:
	std::vector<std::array<uint8_t, 256>> tables_;
	uint32_t generation_ = 0;
};

struct Placed {
	int keyId = -1;      ///< InternKey of the asset; -1 for a tint
	int frame = 0;
	Rect rect;           ///< the classic frame's full rectangle in target pixels
	Rect clip;           ///< the part of `rect` that is visible (classic clipping)
	int trn = -1;        ///< TrnPool index
	float opacity = 1.0F;
	uint32_t tint = 0;   ///< tint layers: straight RGBA (A = opacity)

	[[nodiscard]] bool isTint() const { return keyId < 0; }
	[[nodiscard]] Placed translated(int dx, int dy) const
	{
		Placed p = *this;
		p.rect = rect.translated(dx, dy);
		p.clip = clip.translated(dx, dy);
		return p;
	}
};

/** A placed image together with the pixels its classic draw wrote (within `placed.clip`). */
struct PlacedMasked {
	Placed placed;
	Mask mask;
};

// ---- Sprite registry ----

/** Cheap identity check against a reused address: frame size, data size and the first pixel bytes. */
struct SpriteFingerprint {
	uint16_t width = 0;
	uint16_t height = 0;
	uint32_t size = 0;
	uint32_t head = 0;
	bool operator==(const SpriteFingerprint &) const = default;
};

SpriteFingerprint FingerprintOf(const uint8_t *frameData, uint32_t frameSize);

class SpriteRegistry {
public:
	struct Entry {
		SpriteFingerprint fingerprint;
		int keyId = -1;
		int frame = 0;
		/** Composite frames (made from a surface by SurfaceToClx): their HD content, relative to the frame's top-left. */
		std::vector<PlacedMasked> children;
	};

	void registerFrame(const void *frameData, SpriteFingerprint fingerprint, int keyId, int frame);
	void registerComposite(const void *frameData, SpriteFingerprint fingerprint, std::vector<PlacedMasked> children);
	[[nodiscard]] const Entry *find(const void *frameData, SpriteFingerprint fingerprint) const;
	/** Calls fn(frameData, entry) for every registered frame. */
	template <typename Fn>
	void forEach(Fn &&fn) const
	{
		for (const auto &[frameData, entry] : entries_)
			fn(frameData, entry);
	}
	/** Forgets every frame inside [begin, begin + size) (the buffer is being freed). */
	void forget(const void *begin, size_t size);
	[[nodiscard]] size_t size() const { return entries_.size(); }

private:
	std::unordered_map<const void *, Entry> entries_;
};

// ---- Off-screen surfaces ----

/** HD content drawn into surfaces other than the back buffer, keyed by surface identity. */
class OffscreenStore {
public:
	static constexpr size_t MaxEntriesPerSurface = 512;
	/** A new (or recycled) surface: forget what it held. */
	void reset(const void *surface);
	/** Adds an entry; drops older entries of that surface whose written pixels it covers. */
	void add(const void *surface, PlacedMasked entry);
	/** Entries of `surface` visible inside `srcRect`, clipped to it and moved by (dx, dy). */
	[[nodiscard]] std::vector<PlacedMasked> collect(const void *surface, const Rect &srcRect, int dx, int dy) const;
	[[nodiscard]] bool has(const void *surface) const;
	[[nodiscard]] size_t size(const void *surface) const;

private:
	std::unordered_map<const void *, std::vector<PlacedMasked>> surfaces_;
};

// ---- Back-buffer layers ----

class LayerTracker {
public:
	static constexpr size_t MaxLayers = 4096;
	static constexpr uint16_t NoLayer = 0xFFFF;
	/** A layer is dropped when fewer than this share of the pixels it wrote is still visible. */
	static constexpr float MinVisibleShare = 0.2F;

	struct Layer {
		Placed placed;
		Mask mask;                     ///< pixels the classic draw wrote (rect = placed.clip)
		std::vector<uint8_t> snapshot; ///< back buffer over placed.clip right after the draw
	};

	/** Forgets everything; the plane mirrors a `width` x `height` back buffer. */
	void reset(int width, int height);
	[[nodiscard]] int width() const { return width_; }
	[[nodiscard]] int height() const { return height_; }

	/**
	 * Records a layer right after its classic draw. `entry.mask` lists the pixels that draw wrote; `entry.placed.clip`
	 * is cut to the buffer. Older layers whose written pixels the new one covers completely are dropped.
	 * Returns false when nothing of it is inside the buffer.
	 */
	bool record(const uint8_t *pixels, int pitch, PlacedMasked entry);
	/** Drops every layer that intersects `rect` (the area is redrawn from scratch, e.g. by the world pass). */
	void invalidate(const Rect &rect);
	void clear();

	/**
	 * Builds the visibility plane from the final back buffer and drops layers that are no longer shown.
	 * `drawable(layer)` says whether the GPU can draw that layer this frame: a layer that cannot is treated like a
	 * classic-only draw (its pixels hide the layers below, so the classic pixels show instead).
	 * After resolve, layer i is visible at pixel p iff plane[p] != NoLayer && i >= plane[p].
	 */
	void resolve(const uint8_t *pixels, int pitch, const std::function<bool(const Layer &)> &drawable);

	[[nodiscard]] const std::vector<Layer> &layers() const { return layers_; }
	[[nodiscard]] const std::vector<uint16_t> &plane() const { return plane_; }
	/** Union of the layer rectangles after the last resolve (only that part of the plane is meaningful). */
	[[nodiscard]] Rect bounds() const { return bounds_; }
	/** Layers that resolve found drawable (indices into layers()). */
	[[nodiscard]] const std::vector<uint8_t> &drawableFlags() const { return drawable_; }

private:
	std::vector<Layer> layers_;
	std::vector<uint16_t> plane_;
	std::vector<Rect> planeRects_; ///< layer rectangles of the last resolve: the plane is 0 outside them
	std::vector<uint8_t> drawable_;
	Rect bounds_;
	int width_ = 0;
	int height_ = 0;
};

// ---- Uploaded indexed sheets ----

/** A classic CLX frame and the rectangle of its HD image in an indexed sheet. */
struct IndexedSheetFrame {
	FrameInfo hd;
	const uint8_t *pixelData = nullptr; ///< CLX commands (after the frame header), bottom-up
	size_t pixelDataSize = 0;
	int width = 0;
	int height = 0;
};

struct IndexedSheetReport {
	size_t covered = 0;   ///< texels with alpha 255
	size_t badTexels = 0; ///< texels whose alpha is neither 0 nor 255
	size_t compared = 0;  ///< classic opaque pixels sampled (the HD texel at the centre of each scale x scale block)
	size_t matched = 0;   ///< ... whose HD texel is covered and holds the classic palette index
	bool ok = false;
};

/**
 * Checks the texels of an uploaded indexed sheet (`width` x `height` RGBA8, rows `pitch` bytes apart) as the GPU holds
 * them: alpha is 0 or 255, something is covered, and at the centre of every classic pixel the HD red channel is the
 * classic palette index (a colour-managed or failed upload shifts or loses them). A few texels differ by design (the
 * art spreads indices into the anti-aliased rim), so 90 % of the samples must match.
 */
IndexedSheetReport CheckIndexedSheet(const uint8_t *texels, size_t pitch, int width, int height, int scale, std::span<const IndexedSheetFrame> frames);

// ---- Colour translations for RGBA art ----

constexpr int TrnLutSize = 32;

/** Nearest palette index of every 32^3 grid colour (index (b * 32 + g) * 32 + r); built once per palette. */
std::vector<uint8_t> BuildNearestGrid(std::span<const uint8_t, 768> palette);

/**
 * 32x32x32 RGBA lookup table that applies a palette colour translation to true-colour art: each grid colour c is
 * moved by the translation of its nearest palette colour n: c + (palette[trn[n]] - palette[n]). Identity entries of
 * the table leave colours alone; a table that maps everything to red tints the art red keeping its detail.
 * `palette` = 256 x RGB, `nearest` = BuildNearestGrid(palette). Layout: index (b * 32 + g) * 32 + r, 4 bytes each.
 */
std::vector<uint8_t> BuildTrnLut(std::span<const uint8_t, 256> trn, std::span<const uint8_t, 768> palette, std::span<const uint8_t> nearest);
/** As above, building the nearest grid on the fly. */
std::vector<uint8_t> BuildTrnLut(std::span<const uint8_t, 256> trn, std::span<const uint8_t, 768> palette);

/** Texture divisor for UI sheets shown at `canvasPixelsPerUiPixel` (4x art): 1, 2 or 4. */
int SelectUiTextureDivisor(float canvasPixelsPerUiPixel);

} // namespace devilution::hd_ui
