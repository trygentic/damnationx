/**
 * @file hd_objects.hpp
 *
 * Opt-in HD (4x, 60 FPS) dungeon object sprites in the WebGPU browser build (`?hdSprites=1`, DamnationX HD runtime
 * contract v1 section 3): chests, sarcophagi, barrels, doors, levers, shrines, torches, candles, bookcases, ...
 *
 * hd/objects.txt binds HD clips to the classic CEL frames of an object file (hd_objects_core.hpp). Its clips join the
 * HD sprite index (hd_sprites.hpp) once that has settled, so they share its lazy texture loading, occlusion, lighting
 * and presenter pass. Objects get no generated drop shadow (the shadow is part of the art), unlit objects (torches,
 * fires) stay unlit under hybrid lighting, and a `trans` clip plays once when the object's frame jumps (a chest or a
 * door opening, a lever being pulled) before the new resting frame shows. Anything without an HD binding, or whose
 * clip is still loading, is drawn classic.
 */
#pragma once

#include "engine/point.hpp"

namespace devilution {

struct Object;
struct Surface;

/**
 * True when the object was recorded as an HD instance; DrawObject then skips the classic blit and hover outline (the
 * GPU outlines the HD figure when the call is wrapped in an HdSpritesOutlineScope).
 * @param spriteBottomLeft where the classic frame would be blitted (its bottom-left pixel, ClxDraw convention)
 * @param spriteWidth width of the classic frame
 */
bool HdSpritesDrawObject(const Surface &out, const Object &object, Point spriteBottomLeft, int spriteWidth, int lightTableIndex);

} // namespace devilution
