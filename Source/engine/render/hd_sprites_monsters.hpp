/**
 * @file hd_sprites_monsters.hpp
 *
 * Opt-in HD (4x, 60 FPS) monster sprites for the WebGPU browser build (`?hdSprites=1`), DamnationX HD runtime
 * contract v1 section 2, keys `monster/<gfx>[+<variant>]/<anim>/<dir>` (hd_sprites_monster_keys.hpp).
 *
 * Same pipeline as the HD Warrior (hd_sprites.hpp): the classic body blit is skipped and an instance is recorded with
 * an occlusion snapshot. The clip follows the monster's current classic animation (which MonsterGraphic and direction
 * its AnimationInfo holds) with the contract's proportional frame mapping, and its pivot sits on the same ground
 * point as the player's, including the walk offset.
 *
 * Stays classic: no clip for the monster's look (graphics set, TRN recolour or unique TRN), infravision, stone curse
 * (the grey stone TRN), and any clip whose texture is still loading.
 */
#pragma once

#include "engine/direction.hpp"
#include "engine/point.hpp"

namespace devilution {

struct Corpse;
struct Monster;
struct Surface;

/**
 * @brief True when the monster's body was recorded as an HD instance; the caller then skips the classic body blit.
 * @param targetBufferPosition tile draw position (without the sprite centring and walk offsets)
 * @param infraVision the classic renderer would draw the monster with the infravision TRN (stays classic)
 */
bool HdSpritesDrawMonster(const Surface &out, const Monster &monster, Point targetBufferPosition, bool infraVision, int lightTableIndex);

/** Called at the start of every world pass that draws HD sprites: keeps the level's monster clips loaded. */
void HdSpritesMonstersBeginPass();

/** Corpse variant: the last frame of the monster type's HD death clip in `direction`. */
bool HdSpritesDrawCorpse(const Surface &out, const Corpse &corpse, Direction direction, Point targetBufferPosition, int lightTableIndex);

} // namespace devilution
