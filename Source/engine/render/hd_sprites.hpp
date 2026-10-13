/**
 * @file hd_sprites.hpp
 *
 * Opt-in HD (4x, 60 FPS) replacement sprites for the Warrior and the Tristram towners in the WebGPU browser build
 * (`?hdSprites=1`).
 *
 * The world pass (scrollrt.cpp) asks HdSpritesDrawPlayer / HdSpritesDrawTowner whether an entity is drawn in HD.
 * When it is, the classic body blit is skipped and an instance {clip, frame, pivot, light} is recorded together with
 * an occlusion snapshot (hd_sprites_core.hpp). RenderPresent resolves the occlusion against the finished back buffer
 * (HdSpritesFinishFrame) and the WebGPU presenter draws the instances at canvas resolution (hd_sprites_webgpu.cpp).
 *
 * Under hybrid lighting the skipped classic blit also leaves no card in the G-buffer, so the hybrid pass lights the
 * pixels under the figure as whatever lies beneath it; the HD texels are lit separately as the classic card would be.
 */
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "engine/point.hpp"
#include "engine/render/hd_sprites_core.hpp"
#include "engine/render/hd_view_transform.hpp"

namespace devilution {

struct Player;
struct Surface;
struct Towner;

/**
 * Drop shadow of an HD instance: the silhouette sheared onto the ground towards the upper left, where the shadows
 * baked into the classic player and towner art fall. A texel h back-buffer rows above the pivot lands at
 * (x - ShearX * h, pivotY - ShearY * h).
 */
struct HdSpriteShadow {
	static constexpr float ShearX = 0.42F;
	static constexpr float ShearY = 0.26F;
	static constexpr float Opacity = 0.7F;
	/** Back-buffer pixels of the shadow's edge blur. */
	static constexpr float BlurRadius = 1.25F;
};

/**
 * Recolour of an HD instance, standing in for a classic TRN the pack has no clips for: the texel colour is
 * desaturated towards its luma by 1 - `saturation`, then multiplied per channel by `rgb`. Identity by default.
 * Used for unique monsters and TRN variants drawn with another look's clips, stone curse and infravision.
 */
struct HdSpriteTint {
	std::array<float, 3> rgb { 1.0F, 1.0F, 1.0F };
	float saturation = 1.0F;

	[[nodiscard]] bool identity() const { return rgb[0] == 1.0F && rgb[1] == 1.0F && rgb[2] == 1.0F && saturation == 1.0F; }
	bool operator==(const HdSpriteTint &) const = default;
};

struct HdSpriteInstance {
	int clip = -1;
	int frame = 0;
	/**
	 * Quad in back buffer pixels: the clip cell scaled by 1/4 (x HdSpritesFrameScale() under zoom) with its pivot on
	 * the entity's tile centre. All coordinates here are screen pixels once HdSpritesApplyWorldTransform ran.
	 */
	float x = 0.0F;
	float y = 0.0F;
	float width = 0.0F;
	float height = 0.0F;
	/** Back-buffer row of the pivot (the feet), the line the drop shadow is sheared from. */
	float pivotY = 0.0F;
	/** Visible part of the quad and its drop shadow (the viewport); also the occlusion snapshot rectangle. */
	hd_sprites::PixelRect clipRect;
	/** Classic light table index the body would have been drawn with (0 = full bright). */
	int lightTableIndex = 0;
	/**
	 * Back-buffer row the classic sprite's card is anchored at (the bottom row of the classic frame, where its G-buffer
	 * base byte points). Under hybrid lighting the HD texels are lit as that camera-facing card.
	 */
	float anchorY = 0.0F;
	/** Draw the generated drop shadow (players, towners). Objects carry the shadow in their art. */
	bool dropShadow = true;
	/** Classic draws it without a light table (unlit objects such as torches): under hybrid lighting it passes through unlit. */
	bool selfLit = false;
	HdSpriteTint tint;
	/**
	 * Palette index of the hover outline (the colour classic ClxDrawOutlineSkipColorZero would use; hd_sprites::
	 * HdOutline), or -1 for none. The occlusion snapshot of an outlined instance is padded to hold the outline.
	 */
	int outlineColor = -1;
};

/**
 * While alive, every instance recorded gets the hover outline `paletteIndex` (-1: none). The world pass wraps an
 * HdSpritesDraw* call for the entity under the cursor in it and draws the classic outline only when that returns
 * false, so the outline follows the HD figure instead of the skipped classic frame.
 */
class HdSpritesOutlineScope {
public:
	explicit HdSpritesOutlineScope(int paletteIndex);
	~HdSpritesOutlineScope();
	HdSpritesOutlineScope(const HdSpritesOutlineScope &) = delete;
	HdSpritesOutlineScope &operator=(const HdSpritesOutlineScope &) = delete;

private:
	int previous_;
};

/**
 * Starts a world pass. `allowed` is false when this frame cannot use HD sprites. Instances are recorded in world-pass
 * pixels (`out` coordinates, half size under zoom); HdSpritesApplyWorldTransform moves them to the screen.
 */
void HdSpritesBeginWorldPass(const Surface &out, bool allowed);
/**
 * Maps this world pass's instances (quads, pivots, card anchors, occlusion snapshots, see-through wall rectangles)
 * from world-pass pixels to screen pixels with `transform` (hd_view_transform.hpp). DrawGame calls it right after
 * Zoom(); without zoom the transform is the identity and nothing changes.
 */
void HdSpritesApplyWorldTransform(const HdWorldTransform &transform);
/** Screen pixels per world pixel of the instances HdSpritesFrameInstances returns (2 under zoom). */
[[nodiscard]] float HdSpritesFrameScale();
/**
 * True when the player's body is HD this frame; the caller then skips the classic body blit and its outline.
 *
 * Once the index holds HD clips for the player's class, the body never falls back to the classic sprite: a clip that
 * is still streaming (or was evicted) is replaced by the nearest resident clip of the same body (hd_sprites::
 * ChooseBodyClip), and the walk and idle clips are kept loaded (hd_sprites::PlanPlayerPreload). Only while none of
 * them is resident yet (the first frames after the index loads) the body is not drawn at all. False only when there
 * is no HD art for this body (another class, HD sprites off, no clip will ever load) or under infravision.
 */
bool HdSpritesDrawPlayer(const Surface &out, const Player &player, Point targetBufferPosition, int lightTableIndex);
/** Towner variant of HdSpritesDrawPlayer (towners are drawn full bright). */
bool HdSpritesDrawTowner(const Surface &out, const Towner &towner, Point targetBufferPosition);
/** True while this world pass may record HD instances at all (`?hdSprites=1`, presenter ready). */
[[nodiscard]] bool HdSpritesWorldPassAllowed();
/** True once hd/sprites.txt has loaded or definitely failed: clips appended after that keep their indices. */
[[nodiscard]] bool HdSpritesIndexSettled();
/** Appends a clip defined outside hd/sprites.txt (hd/objects.txt); returns its index (the existing one for a known key). */
int HdSpritesAppendClip(const hd_sprites::ClipInfo &clip);
/**
 * Records an instance of `clip` frame `frame` whose pivot lands on (pivotX, pivotY) in `out` coordinates (fractional:
 * object frames anchor on the classic frame's bottom edge). `anchorY` is the classic card's bottom row (hybrid
 * lighting). True when recorded; the caller then skips the classic blit.
 */
bool HdSpritesRecord(const Surface &out, int clip, int frame, float pivotX, float pivotY, float anchorY, int lightTableIndex,
    bool dropShadow, bool selfLit);
/**
 * A see-through wall (TransList, drawn blended or dithered) covers `width` x `height` pixels of `out` with
 * `bottomLeft` as its bottom-left pixel; HD instances recorded before it show through it at half coverage there.
 */
void HdSpritesMarkTranslucent(const Surface &out, Point bottomLeft, int width, int height);
/** The back buffer no longer holds the recorded world pass (loading screen, movie, menu): drop the instances. */
void HdSpritesInvalidate();
/** Resolves occlusion against the finished 8-bit back buffer; called by RenderPresent right before presenting. */
void HdSpritesFinishFrame(const uint8_t *pixels, int pitch, int width, int height);

// ---- Presenter side ----
[[nodiscard]] std::span<const HdSpriteInstance> HdSpritesFrameInstances();
[[nodiscard]] const hd_sprites::OcclusionTracker &HdSpritesOcclusion();
/** Union of the instance rectangles resolved by the last HdSpritesFinishFrame (empty when nothing is drawn). */
[[nodiscard]] hd_sprites::PixelRect HdSpritesOcclusionBounds();
[[nodiscard]] const hd_sprites::SpriteIndex &HdSpritesIndex();

// ---- Implemented by hd_sprites_webgpu.cpp (stubs without DEVILUTIONX_WEBGPU) ----
/** `?hdSprites=1` and a presenter that can draw them. */
[[nodiscard]] bool HdSpritesGpuAvailable();
/** True when the clip's texture is resident; otherwise starts loading it (the entity stays classic meanwhile). */
bool HdSpritesGpuRequestClip(int clip);
/** Streaming state of `clip` without starting a load; a resident clip is marked used this frame (kept from eviction). */
[[nodiscard]] hd_sprites::ClipStatus HdSpritesGpuClipStatus(int clip);
/** Starts loading `clip` ahead of every queued load (a player body waiting for its clip). */
void HdSpritesGpuRequestClipUrgent(int clip);
/** Replaces the set of clips that are kept loaded and never evicted; clips in it are loaded (again) as needed. */
void HdSpritesGpuSetPinnedClips(std::span<const int> clips);
/** Starts loading `clips` (behind the queued loads) unless resident; they stay evictable. */
void HdSpritesGpuPrefetchClips(std::span<const int> clips);
/** Texture bytes `clip` takes at the current texture resolution. */
[[nodiscard]] size_t HdSpritesGpuClipBytes(int clip);
/** Byte budget of the clip texture cache. */
[[nodiscard]] size_t HdSpritesGpuTextureBudget();
/** Text of hd/sprites.txt when it is not packaged with the assets (fetched next to the page in the browser). */
bool HdSpritesGpuFetchIndex(std::string &text, bool &failed);
/** As HdSpritesGpuFetchIndex for another small text file next to the page (e.g. "hd/objects.txt"). */
bool HdSpritesGpuFetchText(const char *url, std::string &text, bool &failed);

} // namespace devilution
