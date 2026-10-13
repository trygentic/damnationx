/**
 * @file hd_sprites_internal.hpp
 *
 * The world-pass recording core of hd_sprites.cpp, shared with the HD sprite kinds implemented in their own files
 * (hd_sprites_monsters.cpp). Not for use outside Source/engine/render/hd_sprites*.cpp.
 */
#pragma once

#include <cstddef>
#include <span>

#include "engine/point.hpp"
#include "engine/render/hd_sprites.hpp"

namespace devilution {

struct Surface;

/** True during a world pass that may record HD instances (flag on, presenter ready, index loaded). */
[[nodiscard]] bool HdSpritesWorldPassEnabled();
/**
 * True while hd/sprites.txt is still being fetched in a pass that will draw HD sprites once it arrives (the first
 * frames in game): entities about to be drawn in HD are hidden rather than flashed classic.
 */
[[nodiscard]] bool HdSpritesIndexLoading();
/** Progress to the next game tick in [0, 1] for the 60 FPS frame selection (0 while the animation is frozen). */
[[nodiscard]] float HdSpritesSubtickProgress(bool frozen);
/** Ground (feet) point of classic actor art drawn at the tile draw position `targetBufferPosition`: the HD pivot. */
[[nodiscard]] Point HdSpritesGroundPoint(Point targetBufferPosition);
/** Places frame `frame` of clip `clipIndex` with its pivot on `pivot` (viewport pixels) and records the instance. */
bool HdSpritesRecordInstance(const Surface &out, int clipIndex, int frame, Point pivot, int lightTableIndex);
/** As above with a recolour (HdSpriteTint) and, for `selfLit`, without lighting (the classic TRN draws are unlit). */
bool HdSpritesRecordInstance(const Surface &out, int clipIndex, int frame, Point pivot, int lightTableIndex, const HdSpriteTint &tint, bool selfLit);
/**
 * Replaces the monster clips kept loaded and never evicted (hd_sprites::PlanMonsterPreload); the GPU pins their union
 * with the player's pinned clips (hd_sprites::PlanPlayerPreload).
 */
void HdSpritesSetMonsterPinnedClips(std::span<const int> clips);
/** Texture bytes of the player's pinned clips at the current resolution (the rest of the budget is for everything else). */
[[nodiscard]] size_t HdSpritesPlayerPinnedBytes();
/**
 * Like HdSpritesGpuPrefetchClips, but each clip only starts loading while it fits the free texture budget (resident
 * plus in-flight bytes); one that does not is dropped (it loads on first use). Implemented by hd_sprites_webgpu.cpp.
 */
void HdSpritesGpuPrefetchClipsIfRoom(std::span<const int> clips);

} // namespace devilution
