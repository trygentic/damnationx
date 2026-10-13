/**
 * @file hd_tiles_core.hpp
 *
 * Engine-independent parts of the opt-in HD level tiles: the
 * hd/tiles-<tileset>.txt manifest parser and lookup, and the "dynamic write" plane that tells which HD tile texels the
 * characters, items, missiles and UI drawn after them hide.
 *
 * Nothing here touches game state, SDL or WebGPU, so it is covered by native unit tests (test/hd_tiles_test.cpp).
 *
 * Manifest (UTF-8 text, `#` comments, whitespace separated):
 *
 *     scale <S>                                    texels per game pixel (1..4)
 *     atlas <N> <file> <width> <height>            page N, width and height 1..4096; pages contiguous from 0
 *     piece <piece> <layer> <page> <u> <v> <w> <h> <dx> <dy>
 *     at <x> <y> <piece> <layer> <page> <u> <v> <w> <h> <dx> <dy>
 *     special <frame> <page> <u> <v> <w> <h> <dx> <dy>
 *     at_special <x> <y> <frame> <page> <u> <v> <w> <h> <dx> <dy>
 *
 * <piece> is the 0-based dPiece value, <layer> `floor` (what DrawFloorTile draws: the floor triangles of a walkable
 * piece) or `cell` (everything DrawCell draws for that piece). `at` entries apply to one map position (dPiece[x][y])
 * and only while that position holds <piece>; they take precedence over the generic `piece` entries. <frame> is the
 * 1-based dSpecial value (arches, door leaves, town tree crowns); `at_special` keys it by the position whose dSpecial it
 * is. (u, v, w, h) is the image inside the page in texels; (dx, dy) the image's top-left corner relative to the
 * top-left corner of the anchor pixel, in texels: the anchor is the bottom-left pixel of the piece's bottom micro row
 * (DrawCell's targetBufferPosition) for pieces, and the classic sprite's bottom-left pixel for specials.
 * A key may repeat: every line is one part of that image (tall layers are cut into bands), drawn in file order.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace devilution::hd_tiles {

inline constexpr int MaxPageSize = 4096;
inline constexpr int MaxPages = 16;
inline constexpr int MaxScale = 4;

enum class Layer : uint8_t {
	Floor = 0,
	Cell = 1,
};

struct Page {
	/** Asset path for FindAsset, e.g. "hd\\tiles-town-0.webp" (the browser fetches "hd/tiles-town-0.webp"). */
	std::string path;
	int width = 0;
	int height = 0;
};

struct Image {
	uint8_t page = 0;
	uint16_t u = 0;
	uint16_t v = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	int16_t dx = 0;
	int16_t dy = 0;
};

/** The parts of one key: images [first, first + count) of Manifest::images. */
struct Parts {
	uint32_t first = 0;
	uint32_t count = 0;

	[[nodiscard]] bool empty() const { return count == 0; }
};

struct Manifest {
	int scale = 0;
	std::vector<Page> pages;
	/** Grouped by key: the parts of a key are contiguous, in file order. */
	std::vector<Image> images;

	/** The `at` entry of (x, y) holding `piece`, else the generic `piece` entry, else empty. */
	[[nodiscard]] Parts findPiece(int x, int y, int piece, Layer layer) const;
	/** The `at_special` entry of (x, y), else the generic `special` entry, else empty. */
	[[nodiscard]] Parts findSpecial(int x, int y, int frame) const;
	[[nodiscard]] int maxPageWidth() const;
	[[nodiscard]] int maxPageHeight() const;

	// Lookup tables (filled by ParseManifest).
	std::unordered_map<uint64_t, Parts> positional;
	std::unordered_map<uint64_t, Parts> positionalSpecials;
	std::vector<Parts> genericFloor;
	std::vector<Parts> genericCell;
	std::vector<Parts> specials;

	/**
	 * Dense form of `positional` and `positionalSpecials` (built by ParseManifest when every position is below
	 * MaxGridSize): findPiece / findSpecial run per drawn tile, so a position indexes a grid cell instead of hashing.
	 * A cell holds 1 + the first entry of a chain (0: none); a chain lists the values (piece or frame) keyed at the cell.
	 */
	struct GridEntry {
		uint32_t value = 0;
		uint32_t next = 0; // 1 + the next entry of the chain, 0 at its end
		Parts parts;
	};
	static constexpr int MaxGridSize = 1024;
	/** Grid cells per row (0: no grid, the hash maps answer). Cell of (x, y): (y * gridWidth + x) * GridSlots + slot. */
	int gridWidth = 0;
	int gridHeight = 0;
	/** Slots per position: the floor layer, the cell layer and the specials. */
	static constexpr int GridSlots = 3;
	static constexpr int SpecialSlot = 2;
	std::vector<uint32_t> grid;
	std::vector<GridEntry> gridEntries;

	[[nodiscard]] static uint64_t PositionKey(int x, int y, int value, int layer)
	{
		return (static_cast<uint64_t>(static_cast<uint16_t>(x)) << 48) | (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 32)
		    | (static_cast<uint64_t>(static_cast<uint16_t>(value)) << 8) | static_cast<uint64_t>(layer & 0xFF);
	}

	/** Builds `grid` from the positional maps (ParseManifest); leaves it empty when a position is out of its range. */
	void buildGrid();

private:
	[[nodiscard]] const Parts *findInGrid(int x, int y, int slot, int value) const;
};

/**
 * @brief Parses hd/tiles-<tileset>.txt.
 *
 * Validates the whole file: one `scale` 1..4, pages contiguous from 0 (at most MaxPages), page sizes 1..4096, every
 * image inside its page and non-empty. Relative atlas file names resolve inside "hd".
 * @param name file name used in error messages
 * @return The manifest, or a message naming the offending line.
 */
std::expected<Manifest, std::string> ParseManifest(std::string_view text, std::string_view name);

struct PixelRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	[[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
};

/**
 * @brief Which HD tile texels are hidden by classic draws that are not tiles.
 *
 * The classic tiles stay in the 8-bit back buffer; the GPU draws each recorded HD tile instance over its own classic
 * pixels in recording order. Characters, corpses, items, objects and missiles drawn between the tiles (and the UI drawn
 * after the world) must stay in front of the tiles recorded before them. Around each such draw section `beginSection`
 * snapshots a rectangle and `endSection` marks every pixel that changed with `instanceCount` (the number of tile
 * instances recorded so far): instance i is hidden at p iff plane[p] > i. `snapshotFrame` / `finishFrame` do the same
 * for everything drawn after the world content (OOB tiles, UI, cursor) with HiddenAll.
 *
 * Tile writes never enter the plane: a later tile's HD texels are drawn after (over) the earlier ones anyway. A later
 * write of exactly the snapshot's palette index is invisible to the diff; the HD tile then shows there, which only
 * matters inside a character of the same colour as the wall behind it.
 */
class DynamicPlane {
public:
	static constexpr uint16_t HiddenAll = 0xFFFF;

	/** Clears the plane; `width` x `height` is the back buffer size. */
	void reset(int width, int height);
	/** Snapshots `rect` (clipped to the buffer); a section still open is ended first (with its own count). */
	void beginSection(const uint8_t *pixels, int pitch, PixelRect rect, uint16_t instanceCount);
	/** Marks the pixels of the open section that changed since `beginSection`; no-op without an open section. */
	void endSection(const uint8_t *pixels, int pitch);
	[[nodiscard]] bool sectionOpen() const { return sectionOpen_; }
	/** Snapshots `rect` as the end of the world content. */
	void snapshotFrame(const uint8_t *pixels, int pitch, PixelRect rect);
	/**
	 * Marks every pixel that changed since `snapshotFrame` HiddenAll; false when no snapshot was taken. Repeated calls
	 * (re-presents of the same world pass) start again from the world marks.
	 */
	bool finishFrame(const uint8_t *pixels, int pitch);

	/**
	 * Scales the plane up like scrollrt's Zoom() (HdUpscaleInPlace, `zoomQuarters` / 4 times): at 2x and even sizes
	 * source (x, y) lands on (offsetX + 2x .., 2y ..) of the `viewportWidth` x `height` zoomed viewport; plane pixels of
	 * those rows outside it are cleared. Ends an open section.
	 */
	void zoomInPlace(int viewportWidth, int offsetX, int height, int zoomQuarters = 8);

	/** Columns [first, end) of a plane row. */
	struct RowSpan {
		int first = 0;
		int end = 0;
		[[nodiscard]] bool empty() const { return end <= first; }
	};

	[[nodiscard]] const std::vector<uint16_t> &plane() const { return plane_; }
	/** Per row, a span outside of which the row is 0 (uploads and clears skip the rest). */
	[[nodiscard]] const std::vector<RowSpan> &rowSpans() const { return rowSpans_; }
	[[nodiscard]] int width() const { return width_; }
	[[nodiscard]] int height() const { return height_; }
	[[nodiscard]] uint16_t at(int x, int y) const { return plane_[static_cast<size_t>(y) * width_ + x]; }

private:
	[[nodiscard]] PixelRect clip(PixelRect rect) const;
	static void copyRect(const uint8_t *pixels, int pitch, PixelRect rect, std::vector<uint8_t> &out);
	/** `keepUndo`: saves the plane values it overwrites (undoSpans_ / undoValues_) so that finishFrame can restore them. */
	void markChanged(const uint8_t *pixels, int pitch, PixelRect rect, const std::vector<uint8_t> &snapshot, uint16_t value, bool keepUndo);

	/** Plane values [planeOffset, planeOffset + length) before the last finishFrame, at undoValues_[valuesOffset]. */
	struct UndoSpan {
		size_t planeOffset;
		uint32_t length;
		size_t valuesOffset;
	};

	std::vector<uint16_t> plane_;
	std::vector<RowSpan> rowSpans_;
	std::vector<uint8_t> section_;
	std::vector<uint8_t> frame_;
	std::vector<UndoSpan> undoSpans_;
	std::vector<uint16_t> undoValues_;
	PixelRect sectionRect_;
	PixelRect frameRect_;
	uint16_t sectionValue_ = 0;
	bool sectionOpen_ = false;
	bool frameSnapshot_ = false;
	bool finished_ = false;
	int width_ = 0;
	int height_ = 0;
};

/**
 * @brief A recolour of the whole classic view after the world pass (RedBack on the pause and death screens) that the HD
 * layers must not read as a draw over them.
 *
 * The HD trackers (DynamicPlane::finishFrame, the HD sprite occlusion, the HD floor sentinels, the HD UI layers) find what
 * was drawn over their art by diffing the final 8-bit frame with what they saw while it was drawn. RedBack rewrites every
 * viewport pixel through the pause TRN, so every HD texel looked hidden and the classic 8-bit art came back while paused.
 * `begin` / `end` bracket such a pass; `trackedFrame` then rebuilds the frame the trackers diff: a pixel unchanged since
 * `end` reads as it was before the recolour, a pixel written after it (the "Pause" text, the cursor) keeps its final value.
 * The GPU still composites the real, recoloured frame (the hybrid tile tint picks the red up from it).
 */
class RecolourPass {
public:
	/** Forgets the recorded pass (a new world pass). */
	void reset();
	/** Saves `rect` (clipped to `width` x `height`) before the recolour; a second pass in the same frame chains onto the first. */
	void begin(const uint8_t *pixels, int pitch, int width, int height, PixelRect rect);
	/** Saves the recoloured `rect`; no-op without `begin`. */
	void end(const uint8_t *pixels, int pitch);
	[[nodiscard]] bool active() const { return active_; }
	/**
	 * The frame the trackers diff: `pixels` itself (and `pitch`) when no pass is recorded, else a composed copy with
	 * pitch `width`. The copy stays valid until the next call.
	 */
	const uint8_t *trackedFrame(const uint8_t *pixels, int pitch, int width, int height, int &outPitch);

private:
	std::vector<uint8_t> before_;
	std::vector<uint8_t> after_;
	std::vector<uint8_t> composed_;
	PixelRect rect_;
	bool begun_ = false;
	bool active_ = false;
};

// ---- Classic colour match (hybrid composite) ----
//
// The HD tile art is nearly neutral grey (Cathedral atlas 2x2-block chroma 0.011 against 0.052 for the classic L1 pieces), so
// under the hybrid lighting the stone read grey where the classic palette shows warm red-brown brick and green-grey walls.
// The hybrid tile shader (hd_tiles_webgpu.cpp TileHybridFragmentSource, tileClassicTint) recolours each HD texel with the
// local classic colour normalised to unit luminance (the tint): the mean linear palette colour of the classic pixels around
// it (same G-buffer kind, not hidden by a later draw) divided by its luminance. The texel keeps its linear luminance and
// detail; its chromaticity becomes the tint plus ClassicTintHdChromaKeep of its own deviation from neutral, so the art's
// slight warm cast does not fight the level palette (L1 palettes range from red-brown to blue-grey floors). These functions
// are the C++ reference of that WGSL; keep both in sync.

inline constexpr float ClassicTintMin = 0.4F;
inline constexpr float ClassicTintMax = 2.0F;
/** Below this linear luminance the classic sample mean says nothing about hue: no tint. */
inline constexpr float ClassicTintMinLuminance = 0.002F;
/** Share of the HD texel's own chromaticity deviation (albedo / luminance - 1) kept on top of the tint. */
inline constexpr float ClassicTintHdChromaKeep = 0.5F;

/** Rec. 709 luminance of a linear RGB colour. */
[[nodiscard]] float LinearLuminance(const std::array<float, 3> &rgb);
/** Per-channel tint of the mean of the given linear classic colours, normalised to luminance 1 and clamped. */
[[nodiscard]] std::array<float, 3> ClassicTint(std::span<const std::array<float, 3>> classicLinear);
/**
 * mix(albedo, luminance(albedo) * max(tint + (albedo / luminance(albedo) - 1) * ClassicTintHdChromaKeep, 0), strength),
 * clamped to [0, 1] (linear RGB). A black texel stays black.
 */
[[nodiscard]] std::array<float, 3> ApplyClassicTint(const std::array<float, 3> &albedoLinear, const std::array<float, 3> &tint, float strength);
/** How strongly a tileset takes the classic colour: 0 = town (painted HD art keeps its own colour), 1 = Cathedral. */
[[nodiscard]] float ClassicTintStrength(int tileset);

} // namespace devilution::hd_tiles
