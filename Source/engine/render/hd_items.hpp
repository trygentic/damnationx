/**
 * @file hd_items.hpp
 *
 * Opt-in HD (4x, 60 FPS) items lying on the ground in the WebGPU browser build (`?hdSprites=1`, DamnationX HD runtime
 * contract v1 section 6): gold piles, potions, scrolls, weapons and armour.
 *
 * hd/items.txt holds `clip` lines keyed item/<cel>/<state>[_<variant>] (hd_items_core.hpp). Its clips join the HD
 * sprite index (hd_sprites.hpp) once that has settled, sharing its lazy texture loading, occlusion, lighting, zoom
 * transform and presenter pass. While an item drops, its `drop` clip follows the classic AnimationInfo
 * proportionally; once it lies still, its `rest` clip shows. Gold picks the pile of its size (small / medium / large).
 * Anything without a clip, or whose clip is still loading, is drawn classic.
 */
#pragma once

#include "engine/point.hpp"

namespace devilution {

struct Item;
struct Surface;

/**
 * True when the item was recorded as an HD instance; DrawItem then skips the classic blit and hover outline (the GPU
 * outlines the HD item when the call is wrapped in an HdSpritesOutlineScope). Item labels are unaffected.
 * @param spriteBottomLeft where the classic frame would be blitted (its bottom-left pixel, ClxDraw convention)
 * @param spriteWidth width of the classic frame
 */
bool HdSpritesDrawItem(const Surface &out, const Item &item, Point spriteBottomLeft, int spriteWidth, int lightTableIndex);

/**
 * Once per world pass: loads hd/items.txt as soon as the sprite index has settled and keeps the clips of the most
 * frequent drops (gold, healing and mana potions) streaming, so a freshly thrown item is HD from its first frame.
 */
void HdSpritesKeepItemClipsWarm();

} // namespace devilution
