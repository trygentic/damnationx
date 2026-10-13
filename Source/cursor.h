/**
 * @file cursor.h
 *
 * Interface of cursor tracking functionality.
 */
#pragma once

#include "cursor_defs.hpp"
#include "engine/clx_sprite.hpp"
#include "engine/point.hpp"
#include "engine/size.hpp"
#include "engine/surface.hpp"
#include "utils/attributes.h"

namespace devilution {

extern int pcursmonst;
extern int8_t pcursinvitem;
extern uint16_t pcursstashitem;
extern int8_t pcursitem;

struct Object; // Defined in objects.h
extern Object *ObjectUnderCursor;

struct Player; // Defined in player.h
extern const Player *PlayerUnderCursor;
extern Point cursPosition;
extern DVL_API_FOR_TEST int pcurs;

void InitCursor();
void FreeCursor();
void ResetCursor();

struct Item;
/**
 * @brief Use the item sprite as the cursor (or show the default hand cursor if the item isEmpty)
 */
void NewCursor(const Item &item);

void NewCursor(int cursId);

void InitLevelCursor();
void CheckRportal();
void CheckTown();
void CheckCursMove();
/**
 * @brief The dungeon tile under a screen position (the mouse picking of CheckCursMove, at the current zoom level).
 * @param flipflag set when the position is in the half of the tile that prefers the neighbouring tile
 */
Point GetTileUnderScreenPosition(Point screenPosition, bool &flipflag);

void DrawSoftwareCursor(const Surface &out, Point position, int cursId);

void DrawItem(const Item &item, const Surface &out, Point position, ClxSprite clx);

/** Returns the sprite for the given inventory index. */
ClxSprite GetInvItemSprite(int cursId);

ClxSprite GetHalfSizeItemSprite(int cursId);
ClxSprite GetHalfSizeItemSpriteRed(int cursId);
void CreateHalfSizeItemSprites();
void FreeHalfSizeItemSprites();

/** Returns the width and height for an inventory index. */
Size GetInvItemSize(int cursId);

} // namespace devilution
