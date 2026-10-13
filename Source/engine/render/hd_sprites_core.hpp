/**
 * @file hd_sprites_core.hpp
 *
 * Engine-independent parts of the opt-in HD sprite replacement:
 * the hd/sprites.txt index parser, the 60 FPS frame selection, the GPU texture LRU bookkeeping and the
 * per-pixel occlusion ("last writer") plane that decides which HD texels the classic painter order hides.
 *
 * Nothing here touches game state, SDL or WebGPU so it is covered by native unit tests (test/hd_sprites_test.cpp).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/render/hd_view_transform.hpp"

namespace devilution::hd_sprites {

/** One `clip` line of hd/sprites.txt. All sizes are HD (4x) pixels. */
struct ClipInfo {
	std::string key;
	std::string png;
	int frames = 0;
	int cols = 0;
	int cellWidth = 0;
	int cellHeight = 0;
	int pivotX = 0;
	int pivotY = 0;
	bool loop = false;

	[[nodiscard]] int rows() const { return cols > 0 ? (frames + cols - 1) / cols : 0; }
	[[nodiscard]] int sheetWidth() const { return cols * cellWidth; }
	[[nodiscard]] int sheetHeight() const { return rows() * cellHeight; }
};

struct SpriteIndex {
	std::vector<ClipInfo> clips;
	std::unordered_map<std::string, int> byKey;
	std::vector<std::string> errors; // one message per rejected line

	/** Index into `clips`, or -1. */
	[[nodiscard]] int find(std::string_view key) const;
};

/** Parses hd/sprites.txt: `clip <key> <png> <frames> <cols> <cell_w> <cell_h> <pivot_x> <pivot_y> <loop 0|1>`, `#` comments. */
SpriteIndex ParseSpriteIndex(std::string_view text);

/**
 * @brief Contract frame selection: floor(frames * (currentFrame + (tick + subtick) / ticksPerFrame) / numberOfFrames).
 *
 * Loop clips wrap, one-shot clips clamp to [0, frames - 1].
 * @param subtick progress to the next game tick in [0, 1] (0 while the animation is frozen)
 */
int SelectFrame(int clipFrames, bool loop, int currentFrame, int numberOfFrames, int tick, float subtick, int ticksPerFrame);

/** Proportional selection from the progress through the whole classic animation (0..1), used while frames are distributed. */
int SelectFrameFromProgress(int clipFrames, bool loop, float progress);

/**
 * @brief SelectFrame for an animation played backwards (monsters with MFLAG_LOCK_ANIMATION: a Hidden fading back in
 * or out, a Gargoyle turning back to stone). The classic frame counts down while its tick counter counts up, so the
 * position is currentFrame + 1 - (tick + subtick) / ticksPerFrame and the HD frame keeps running backwards smoothly.
 */
int SelectFrameReversed(int clipFrames, bool loop, int currentFrame, int numberOfFrames, int tick, float subtick, int ticksPerFrame);

/**
 * @brief Downscale of the 4x clip grids (1, 2 or 4) worth loading for a view showing `canvasPixelsPerWorldPixel`
 * device pixels per world (1x game) pixel: the canvas scale times the zoom (hd_view_transform.hpp).
 *
 * The grids carry 4 texels per world pixel; more than the screen shows only costs memory, less loses detail.
 */
int SelectTextureDivisor(float canvasPixelsPerWorldPixel);

/**
 * @brief The hover outline of an HD instance (the classic ClxDrawOutline around the monster, player, towner, item or
 * object under the cursor), drawn by the GPU as a dilation of the clip's alpha behind the sprite.
 *
 * Classic outlines are one world pixel wide, so on screen they are one back-buffer pixel per world pixel: 1 without
 * zoom, the zoom factor under zoom (1x..4x in quarter steps).
 */
struct HdOutline {
	/** Width of the classic outline in world pixels. */
	static constexpr float WorldWidth = 1.0F;
	/** World-pass pixels the occlusion snapshot of an outlined instance reaches past its quad (outline + soft edge). */
	static constexpr int SnapshotPadding = 2;
	/** Back-buffer pixels of the outline's soft (antialiased) edge beyond its width. */
	static constexpr float SoftEdge = 1.0F;
};

/** Back-buffer pixels of the outline at `worldScale` back-buffer pixels per world pixel (HdSpritesFrameScale; >= 1). */
float OutlineRadius(float worldScale);

/**
 * Back-buffer pixels the outline quad reaches past the sprite quad: the radius plus the soft edge. Never more than the
 * snapshot padding at the same scale, so every outline texel has occlusion data.
 */
float OutlineQuadPadding(float worldScale);

/** How to fetch a clip's grid (hd_loader::RequestImage): the decoded size and the size the index promises. */
struct ImageRequest {
	/** Size to decode to; 0 x 0 keeps the image's own size (divisor 1). */
	int width = 0;
	int height = 0;
	/**
	 * The full-size grid the index describes (cols x cellWidth by rows x cellHeight). A PNG of any other size is from
	 * another build of the pack (a stale HTTP cache entry or a pack rebuilt mid-session): sliced with this index's
	 * cells it would show the wrong frames, so the loader refetches it and fails rather than draw it.
	 */
	int expectedWidth = 0;
	int expectedHeight = 0;
};

/** The request for `clip`'s grid at `divisor` (1, 2 or 4): downscaled sizes round up, like the texture's. */
ImageRequest ClipImageRequest(const ClipInfo &clip, int divisor);

/** Contract direction suffix for the engine's Direction order South..SouthEast (0..7); empty for anything else. */
std::string_view DirectionSuffix(int direction);

/** Appearance key from the classic player's packed armor/weapon graphic, including town-cast and death sharing. */
std::string PlayerAppearancePrefix(std::string_view className, uint8_t graphicNumber, std::string_view action, bool inTown);

/** Selects an installed appearance namespace. Legacy warrior art represents light armor with sword and shield only. */
std::string ResolvePlayerClipPrefix(const SpriteIndex &index, std::string_view className, uint8_t graphicNumber,
    std::string_view action, bool inTown);

/**
 * @brief Byte-budgeted least-recently-used bookkeeping for the lazily loaded clip textures.
 *
 * Entries used during the current frame are never evicted, so a frame that needs more than the budget keeps
 * everything it draws and the cache shrinks back once the working set does. Pinned ids (the player's core clips,
 * see PlanPlayerPreload) are never evicted at all; a pin may be set before its entry is inserted.
 */
class TextureLru {
public:
	explicit TextureLru(size_t budgetBytes = 0)
	    : budget_(budgetBytes)
	{
	}

	void setBudget(size_t budgetBytes) { budget_ = budgetBytes; }
	[[nodiscard]] size_t budget() const { return budget_; }
	[[nodiscard]] size_t totalBytes() const { return total_; }
	[[nodiscard]] bool contains(int id) const { return entries_.contains(id); }
	[[nodiscard]] size_t size() const { return entries_.size(); }

	/** Adds (or resizes) an entry and marks it used in `frame`. */
	void insert(int id, size_t bytes, uint64_t frame);
	/** Marks an existing entry used in `frame`; no-op for unknown ids. */
	void touch(int id, uint64_t frame);
	void erase(int id);
	/** Forgets all entries; pins stay. */
	void clear();
	/** Exempts `id` from eviction, or makes it evictable again. */
	void setPinned(int id, bool pinned);
	[[nodiscard]] bool pinned(int id) const { return pinned_.contains(id); }
	[[nodiscard]] const std::unordered_set<int> &pins() const { return pinned_; }
	/** Removes least recently used unpinned entries not used in `currentFrame` until the total fits the budget; returns their ids. */
	std::vector<int> evict(uint64_t currentFrame);

private:
	struct Entry {
		size_t bytes = 0;
		uint64_t lastUse = 0;
	};
	std::unordered_map<int, Entry> entries_;
	std::unordered_set<int> pinned_;
	size_t budget_;
	size_t total_ = 0;
};

/** Streaming state of a clip texture as the body selection sees it. */
enum class ClipStatus : uint8_t {
	Resident,    ///< drawable now
	Loading,     ///< queued or decoding: drawable soon
	Unavailable, ///< failed to load (or no GPU): it will never be drawable
};

/**
 * One HD clip that may stand in for the body the animation asks for: `<prefix><action>/<direction suffix>`.
 * `animate` false shows the clip's first frame as a still pose (an idle or walk clip standing in for another action).
 */
struct BodyClipCandidate {
	std::string_view action;
	int direction = 0;
	bool animate = true;

	bool operator==(const BodyClipCandidate &) const = default;
};

/**
 * @brief The clips that may draw a player body, best first.
 *
 * An HD body must never fall back to the classic sprite while its clip streams in or after the texture cache evicted
 * it: the classic frame would flash (at another size, pose and lighting) whenever the player turns, stops or starts
 * an action. Instead the body uses the nearest resident clip:
 *  1. the wanted clip;
 *  2. the same facing: the level's idle clip, the other idle variant, then the walk clip, as still poses (an idle
 *     clip standing in for the other idle variant keeps animating, as does a walk clip for a walk);
 *  3. the neighbouring facings (+-1, +-2, ... around the 8-direction ring): the wanted clip, then the walk clip.
 * `inTown` picks the idle variant ("town_stand" in town, "stand" in the dungeon).
 */
std::vector<BodyClipCandidate> BodyClipCandidates(std::string_view action, int direction, bool inTown, bool allowNeighbourDirections = true);

/** Which clip draws the body this frame (see ChooseBodyClip). */
struct BodyClipChoice {
	int clip = -1;       ///< index into the SpriteIndex; -1 when nothing is drawable
	bool animate = true; ///< false: draw frame 0 (a still stand-in pose)
	bool exact = false;  ///< the wanted clip itself
	/**
	 * True when no candidate is drawable yet but one is loading: draw nothing this frame (never the classic body).
	 * False with clip -1: the index holds no usable HD art for this body, so the classic sprite is the only option.
	 */
	bool hide = false;
};

/** Picks the first resident clip among BodyClipCandidates(action, direction, inTown), named `<prefix><action>/<dir>`. */
BodyClipChoice ChooseBodyClip(const SpriteIndex &index, std::string_view prefix, std::string_view action, int direction, bool inTown,
    const std::function<ClipStatus(int)> &status, bool allowNeighbourDirections = true);

/**
 * @brief Picks the first resident clip among `candidates` (named `<prefix><action>/<dir>`); the first candidate is
 * the wanted clip (`exact`). Same hide / classic rules as ChooseBodyClip.
 */
BodyClipChoice ChooseClip(const SpriteIndex &index, std::string_view prefix, std::span<const BodyClipCandidate> candidates,
    const std::function<ClipStatus(int)> &status);

/**
 * @brief The clips that may draw a monster body (or corpse), best first; actions are the contract's monster
 * animation names (stand walk attack hit death special).
 *
 * Like a player body, an HD monster never falls back to its classic sprite while a clip streams in (every action
 * and facing is its own clip, so a monster turning to attack would flash classic) or after the cache evicted it:
 *  1. the wanted clip;
 *  2. the same facing: the stand clip (animated for a stand, a still pose otherwise), then the walk clip as a still
 *     pose (animated for a walk);
 *  3. the neighbouring facings (+-1, +-2, ... around the ring): the wanted clip, the stand clip, the walk clip.
 * A death (and a corpse, the last death frame) only stands in with the death clips of other facings: a monster
 * standing up while it dies would read wrong, so it is hidden until a death clip is in.
 */
std::vector<BodyClipCandidate> MonsterClipCandidates(std::string_view action, int direction);

/** The clips a level keeps loaded for its monsters (see PlanMonsterPreload). */
struct MonsterPreload {
	/** Never evicted. */
	std::vector<int> pinned;
	/** Loaded ahead of need but evictable. */
	std::vector<int> prefetched;
	size_t pinnedBytes = 0;
	size_t prefetchedBytes = 0;
};

/**
 * @brief Clips to load when a level's monster looks are known (level load), so monsters are drawn in HD from the
 * first frame they appear instead of streaming each action and facing on first use.
 *
 * `prefixes` are the looks on the level (`monster/<gfx>[+<variant>]/`, most important first). Clips are taken in
 * priority order: every look's stand clips (the universal stand-in, MonsterClipCandidates), then walk, death (corpses
 * stay on screen), attack, hit, special. Each look's 8 facings of one action form a group: a group is pinned while
 * the pinned bytes stay within `pinBudget`, otherwise prefetched while the prefetched bytes stay within
 * `prefetchBudget`, otherwise left to load on first use. `bytes(clip)` is the texture size at the current resolution.
 */
MonsterPreload PlanMonsterPreload(const SpriteIndex &index, std::span<const std::string> prefixes, const std::function<size_t(int)> &bytes,
    size_t pinBudget, size_t prefetchBudget);

/**
 * @brief Per-channel colour multiplier that turns art drawn like `reference` into art drawn like `wanted`, measured
 * on the classic sprite: `histogram` counts the sprite's palette indices, a pixel of index i is coloured
 * palette[wanted[i]] in the look the monster should have and palette[reference[i]] in the look whose HD clips stand
 * in for it (an empty map is the identity). This carries a TRN recolour (a unique monster's, or a TRN variant without
 * clips of its own) over to the HD clips as a tint. Channels without colour in the reference stay 1; the result is
 * clamped to [1/8, 8].
 * @param paletteRgb 256 x (r, g, b)
 */
std::array<float, 3> RecolourTint(std::span<const uint32_t, 256> histogram, std::span<const uint8_t, 768> paletteRgb,
    std::span<const uint8_t> wanted, std::span<const uint8_t> reference);

/** A map undoing a TRN as far as it can: an index the TRN maps others to goes back to the lowest of those others. */
std::array<uint8_t, 256> InverseTrn(std::span<const uint8_t, 256> trn);

/** The clips a class keeps loaded while its bodies are drawn in HD (see PlanPlayerPreload). */
struct PlayerPreload {
	/** Never evicted: the walk clips of all 8 facings, plus the level's idle clips when they fit the idle pin budget. */
	std::vector<int> pinned;
	/** Loaded ahead of need but evictable: the level's idle clips when pinning them would exceed the idle pin budget. */
	std::vector<int> prefetched;
};

/** Prefer an available town walk for this exact facing; older packs keep their ordinary walk. */
std::string_view PlayerActionForLocation(const SpriteIndex &index, std::string_view prefix, std::string_view action, int direction, bool inTown);

/**
 * @brief The resident set that keeps a player body in HD without waiting for a texture.
 *
 * The walk clips of all 8 facings are small and are the last fallback of every other action, so they are always
 * pinned. The idle clips of the current level ("town_stand" in town, "stand" in the dungeon) are pinned too while
 * their texture bytes (`bytes(clip)` at the current texture resolution) add up to at most `idlePinBudget`; otherwise
 * (a large canvas holds them at full size) they are only prefetched so they do not crowd everything else out.
 */
PlayerPreload PlanPlayerPreload(const SpriteIndex &index, std::string_view prefix, bool inTown, const std::function<size_t(int)> &bytes,
    size_t idlePinBudget);

struct PixelRect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	[[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
};

/**
 * @brief Occlusion of HD sprite instances by everything the classic renderer writes after them.
 *
 * The classic body blit of an HD entity is skipped; instead `record` snapshots the 8-bit back buffer under the
 * instance's screen rectangle at the moment the entity would have been drawn. Once the frame is complete (world,
 * UI and cursor), `resolve` compares the final back buffer with each snapshot: a pixel that changed was written by
 * something drawn later in painter order (a wall or object in front, a panel, text, the cursor) and hides the
 * instance there. The result is the 16-bit "last writer" plane: plane[p] = 1 + the highest instance index whose
 * snapshot saw p change, so instance i is hidden at p iff plane[p] > i. Instances later in draw order are drawn
 * after (over) earlier ones on the GPU, which keeps their mutual order. Floors drawn before an entity never
 * occlude it because they are already in its snapshot.
 *
 * A later write of exactly the snapshot's palette index is invisible to the diff; `resolve` closes such isolated
 * one-pixel holes from their neighbours, and drops instances whose rectangle changed almost completely (fully
 * hidden, or a back buffer that no longer holds the world pass).
 *
 * See-through walls (TransList: classic blends or dithers them over the entity, which stays visible as a ghost) are
 * passed to `resolve` as rectangles. A hidden pixel inside one carries TranslucentBit in the plane: the GPU draws
 * the instance there at half coverage instead of hiding it, and those pixels do not count towards "fully hidden".
 */
class OcclusionTracker {
public:
	/** Plane bit: the pixel was last written by a see-through wall (draw the instance through it, see above). */
	static constexpr uint16_t TranslucentBit = 0x8000;
	/** Plane bits holding 1 + the last writer's instance index. */
	static constexpr uint16_t WriterMask = 0x7FFF;

	/** Forgets all instances; `width` x `height` is the back buffer size the plane mirrors. */
	void reset(int width, int height);
	/** Snapshots `rect` (clipped to the buffer) of `pixels`; returns the instance index or -1 when nothing is visible. */
	int record(const uint8_t *pixels, int pitch, PixelRect rect);
	[[nodiscard]] size_t count() const { return instances_.size(); }
	[[nodiscard]] PixelRect rect(size_t index) const { return instances_[index].rect; }
	/** False after `resolve` when the instance should not be drawn at all. */
	[[nodiscard]] bool visible(size_t index) const { return instances_[index].visible; }

	/**
	 * @brief Moves every recorded instance from world-pass pixels to screen pixels (zoom, hd_view_transform.hpp).
	 *
	 * Called after Zoom() doubled the world pass: each rectangle is mapped by `transform` and clipped to its viewport,
	 * each snapshot is resampled nearest exactly as Zoom() resampled the back buffer, so `resolve` against the final
	 * (zoomed) frame sees only what was drawn after the instance. An instance left with an empty rectangle is never
	 * visible. No-op for the identity transform.
	 */
	void remap(const HdWorldTransform &transform);

	/**
	 * @brief Builds the last-writer plane from the final back buffer.
	 * @param bounds receives the union of the instance rectangles: only that region of the plane is meaningful.
	 */
	void resolve(const uint8_t *pixels, int pitch, PixelRect &bounds) { resolve(pixels, pitch, {}, bounds); }
	/** As above; pixels inside a `translucent` rectangle (see-through walls, back-buffer coordinates) get TranslucentBit. */
	void resolve(const uint8_t *pixels, int pitch, std::span<const PixelRect> translucent, PixelRect &bounds);
	[[nodiscard]] const std::vector<uint16_t> &plane() const { return plane_; }
	[[nodiscard]] int width() const { return width_; }
	[[nodiscard]] int height() const { return height_; }

private:
	struct Instance {
		PixelRect rect;
		size_t snapshotOffset = 0;
		bool visible = true;
	};
	std::vector<Instance> instances_;
	std::vector<uint8_t> snapshots_;
	std::vector<uint16_t> plane_;
	std::vector<uint16_t> scratch_;       ///< resolve: the plane before the hole closing
	std::vector<uint8_t> translucent_;    ///< resolve: see-through wall coverage, all 0 between resolves
	std::vector<PixelRect> planeRects_;   ///< the plane is 0 outside these (the last resolve's instance rectangles + 1)
	std::vector<PixelRect> translucentRects_;
	int width_ = 0;
	int height_ = 0;
};

} // namespace devilution::hd_sprites
