/**
 * @file scrollrt.cpp
 *
 * Implementation of functionality for rendering the dungeons, monsters and calling other render routines.
 */
#include "engine/render/scrollrt.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include <ankerl/unordered_dense.h>

#include "DiabloUI/ui_flags.hpp"
#include "automap.h"
#include "controls/control_mode.hpp"
#include "controls/plrctrls.h"
#include "config.h"
#include "cursor.h"
#include "dead.h"
#include "diablo_msg.hpp"
#include "doom.h"
#include "engine/backbuffer_state.hpp"
#include "engine/displacement.hpp"
#include "engine/dx.h"
#include "engine/palette.h"
#include "fpv/fpv.hpp"
#include "engine/point.hpp"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_items.hpp"
#include "engine/render/hd_objects.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_tiles.hpp"
#include "engine/render/hd_ui.hpp"
#include "engine/render/hd_sprites_monsters.hpp"
#include "engine/render/light_render.hpp"
#include "engine/lighting_defs.hpp"
#include "engine/render/text_render.hpp"
#include "engine/render/ui_scale.hpp"
#include "engine/render/webgpu_presenter.hpp"
#include "engine/trn.hpp"
#include "engine/world_tile.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "headless_mode.hpp"
#include "help.h"
#include "hwcursor.hpp"
#include "init.hpp"
#include "inv.h"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/tile_geometry.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "lua/lua_event.hpp"
#include "minitext.h"
#include "missiles.h"
#include "nthread.h"
#include "options.h"
#include "panels/charpanel.hpp"
#include "panels/console.hpp"
#include "panels/partypanel.hpp"
#include "panels/quest_log.hpp"
#include "panels/spell_list.hpp"
#include "plrmsg.h"
#include "qol/chatlog.h"
#include "qol/floatingnumbers.h"
#include "qol/itemlabels.h"
#include "qol/monhealthbar.h"
#include "qol/stash.h"
#include "qol/visual_store.h"
#include "qol/xpbar.h"
#include "stores.h"
#include "towners.h"
#include "utils/algorithm/container.hpp"
#include "utils/attributes.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"

#ifndef USE_SDL1
#include "controls/touch/renderers.h"
#endif

#ifdef _DEBUG
#include "debug.h"
#endif

#ifdef DUN_RENDER_STATS
#include "utils/format_int.hpp"
#endif

namespace devilution {

enum OutlineColors : uint8_t {
	OutlineColorsPlayer1 = (PAL16_ORANGE + 7),
	OutlineColorsPlayer2 = (PAL16_YELLOW + 7),
	OutlineColorsPlayer3 = (PAL16_RED + 7),
	OutlineColorsPlayer4 = (PAL16_BLUE + 7),
	OutlineColorsObject = (PAL16_YELLOW + 2),
	OutlineColorsTowner = (PAL16_BEIGE + 6),
	OutlineColorsMonster = (PAL16_RED + 9),
};

bool AutoMapShowItems;

// DevilutionX extension.
extern void DrawControllerModifierHints(const Surface &out);

bool frameflag;

namespace {

constexpr auto RightFrameDisplacement = Displacement { DunFrameWidth, 0 };

[[nodiscard]] DVL_ALWAYS_INLINE bool IsFloor(Point tilePosition)
{
	return !TileHasAny(tilePosition, TileProperties::Solid | TileProperties::BlockMissile);
}

[[nodiscard]] DVL_ALWAYS_INLINE bool IsWall(Point tilePosition)
{
	return !IsFloor(tilePosition) || dSpecial[tilePosition.x][tilePosition.y] != 0;
}

// Presentation geometry is derived at the render boundary and never writes
// deterministic lighting or gameplay visibility state.
enum class RenderReceiver : uint8_t { Floor, SolidWall, TransparentWall };

[[nodiscard]] RenderReceiver GetRenderReceiver(Point tilePosition)
{
	if (IsFloor(tilePosition)) return RenderReceiver::Floor;
	return TileHasAny(tilePosition, TileProperties::Transparent)
	    ? RenderReceiver::TransparentWall
	    : RenderReceiver::SolidWall;
}

[[nodiscard]] int ComposePresentationLight(Point tilePosition, int authoritativeLight, const RenderLightSnapshot &lights)
{
	if (!*GetOptions().Graphics.perPixelLighting || !InDungeonBounds(tilePosition)) return authoritativeLight;
	// Consume the immutable light snapshot only at the enhanced composition
	// boundary; dLight remains the authoritative gameplay light field.
	const RenderReceiver receiver = GetRenderReceiver(tilePosition);
	if (receiver == RenderReceiver::SolidWall && lights.count == 0) return authoritativeLight;
	return authoritativeLight;
}

/**
 * @brief Contains all Missile at rendering position
 */
ankerl::unordered_dense::map<WorldTilePosition, std::vector<Missile *>> MissilesAtRenderingTile;

/**
 * @brief Could the missile (at the next game tick) collide? This method is a simplified version of CheckMissileCol (for example without random).
 */
bool CouldMissileCollide(Point tile, bool checkPlayerAndMonster)
{
	if (!InDungeonBounds(tile))
		return true;
	if (checkPlayerAndMonster) {
		if (dMonster[tile.x][tile.y] > 0)
			return true;
		if (dPlayer[tile.x][tile.y] > 0)
			return true;
	}

	return IsMissileBlockedByTile(tile);
}

void UpdateMissilePositionForRendering(Missile &m, int progress)
{
	DisplacementOf<int64_t> velocity = m.position.velocity;
	velocity *= progress;
	velocity /= AnimationInfo::baseValueFraction;
	const Displacement pixelsTravelled = (m.position.traveled + Displacement { static_cast<int>(velocity.deltaX), static_cast<int>(velocity.deltaY) }) >> 16;
	const Displacement tileOffset = pixelsTravelled.screenToMissile();

	// calculate the future missile position
	m.position.tileForRendering = m.position.start + tileOffset;
	m.position.offsetForRendering = pixelsTravelled + tileOffset.worldToScreen();
}

void UpdateMissileRendererData(Missile &m)
{
	m.position.tileForRendering = m.position.tile;
	m.position.offsetForRendering = m.position.offset;

	const MissileMovementDistribution missileMovement = GetMissileData(m._mitype).movementDistribution;
	// don't calculate missile position if they don't move
	if (missileMovement == MissileMovementDistribution::Disabled || m.position.velocity == Displacement {})
		return;

	int progress = ProgressToNextGameTick;
	UpdateMissilePositionForRendering(m, progress);

	// In some cases this calculated position is invalid.
	// For example a missile shouldn't move inside a wall.
	// In this case the game logic don't advance the missile position and removes the missile or shows an explosion animation at the old position.
	// For the animation distribution logic this means we are not allowed to move to a tile where the missile could collide, because this could be a invalid position.

	// If we are still at the current tile, this tile was already checked and is a valid tile
	if (m.position.tileForRendering == m.position.tile)
		return;

	// If no collision can happen at the new tile we can advance
	if (!CouldMissileCollide(m.position.tileForRendering, missileMovement == MissileMovementDistribution::Blockable))
		return;

	// The new tile could be invalid, so don't advance to it.
	// We search the last offset that is in the old (valid) tile.
	// Implementation note: If someone knows the correct math to calculate this without the loop, I would really appreciate it.
	while (m.position.tile != m.position.tileForRendering) {
		progress -= 1;

		if (progress <= 0) {
			m.position.tileForRendering = m.position.tile;
			m.position.offsetForRendering = m.position.offset;
			return;
		}

		UpdateMissilePositionForRendering(m, progress);
	}
}

void UpdateMissilesRendererData()
{
	MissilesAtRenderingTile.clear();

	for (auto &m : Missiles) {
		UpdateMissileRendererData(m);
		MissilesAtRenderingTile[m.position.tileForRendering].push_back(&m);
	}
}

uint32_t lastFpsUpdateInMs;

Rectangle PrevCursorRect;

void BlitCursor(uint8_t *dst, uint32_t dstPitch, uint8_t *src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight)
{
	for (std::uint32_t i = 0; i < srcHeight; ++i, src += srcPitch, dst += dstPitch) {
		memcpy(dst, src, srcWidth);
	}
}

/**
 * @brief Remove the cursor from the buffer
 */
void UndrawCursor(const Surface &out)
{
	DrawnCursor &cursor = GetDrawnCursor();
	BlitCursor(&out[cursor.rect.position], out.pitch(), cursor.behindBuffer, cursor.rect.size.width, cursor.rect.size.width, cursor.rect.size.height);
	if (gbuffer::CurrentPlanes.covers(out))
		gbuffer::RestoreRect(cursor.rect, cursor.behindAux); // plane A under the cursor (no-op unless BackBufferIsHybrid())
	PrevCursorRect = cursor.rect;
}

bool ShouldShowCursor()
{
	if (fpv::IsMouseLook())
		return false; // first person: the crosshair replaces the cursor until a panel or Alt frees it
	if (ControlMode == ControlTypes::KeyboardAndMouse)
		return true;
	if (pcurs == CURSOR_TELEPORT)
		return true;
	if (invflag)
		return true;
	if (CharFlag && MyPlayer->_pStatPts > 0)
		return true;

	return false;
}

/**
 * @brief Light table index to shade with this frame: the authoritative index in classic mode,
 * full-bright (or fully dark) under the hybrid latch. Culling decisions keep using the authoritative index.
 */
[[nodiscard]] DVL_ALWAYS_INLINE int ShadeIndex(int authoritative)
{
	return HybridShadeIndex(authoritative, gbuffer::Enabled());
}

/** Tile / wall-face variant of ShadeIndex: never fully dark under the hybrid latch (HybridTileShadeIndex). */
[[nodiscard]] DVL_ALWAYS_INLINE int TileShadeIndex(int authoritative)
{
	return HybridTileShadeIndex(authoritative, gbuffer::Enabled());
}

/**
 * @brief World pass entry into the sprite funnel with this frame's latched mode.
 */
DVL_ALWAYS_INLINE void DrawWorldSprite(const Surface &out, const WorldSpriteDraw &draw)
{
	devilution::DrawWorldSprite(out, draw, gbuffer::Enabled());
}

/**
 * @brief Blit CL2 sprite, and apply lighting, to the given buffer at the given coordinates
 * @param out Output buffer
 * @param position Target buffer coordinate
 * @param clx CLX frame
 * @param lightTableIndex Authoritative light table index
 */
inline void ClxDrawLight(const Surface &out, Point position, ClxSprite clx, int lightTableIndex)
{
	DrawWorldSprite(out, WorldSpriteDraw { .feet = position, .sprite = clx, .shadeIndex = ShadeIndex(lightTableIndex) });
}

/**
 * @brief Save the content behind the cursor to a temporary buffer, then draw the cursor.
 */
void DrawCursor(const Surface &out)
{
	DrawnCursor &cursor = GetDrawnCursor();
	if (IsHardwareCursor()) {
		SetHardwareCursorVisible(ShouldShowCursor());
		cursor.rect.size = { 0, 0 };
		return;
	}

	if (pcurs <= CURSOR_NONE || !ShouldShowCursor()) {
		cursor.rect.size = { 0, 0 };
		return;
	}

	const Size cursSize = GetInvItemSize(pcurs);
	if (cursSize.width == 0 || cursSize.height == 0) {
		cursor.rect.size = { 0, 0 };
		return;
	}

	constexpr auto Clip = [](int &pos, int &length, int posEnd) {
		if (pos + length <= 0 || pos >= posEnd) {
			pos = 0;
			length = 0;
		} else if (pos < 0) {
			length += pos;
			pos = 0;
		} else if (pos + length > posEnd) {
			length = posEnd - pos;
		}
	};

	// Copy the buffer before the item cursor and its 1px outline are drawn to a temporary buffer.
	const int outlineWidth = !MyPlayer->HoldItem.isEmpty() ? 1 : 0;
	const Displacement offset = !MyPlayer->HoldItem.isEmpty() ? Displacement { cursSize / 2 } : Displacement { 0 };
	const Point cursPosition = MousePosition - offset;

	Rectangle &rect = cursor.rect;
	rect.position.x = cursPosition.x - outlineWidth;
	rect.size.width = cursSize.width + 2 * outlineWidth;
	Clip(rect.position.x, rect.size.width, out.w());

	rect.position.y = cursPosition.y - outlineWidth;
	rect.size.height = cursSize.height + 2 * outlineWidth;
	Clip(rect.position.y, rect.size.height, out.h());

	if (rect.size.width == 0 || rect.size.height == 0)
		return;

	BlitCursor(cursor.behindBuffer, rect.size.width, &out[rect.position], out.pitch(), rect.size.width, rect.size.height);
	if (gbuffer::CurrentPlanes.covers(out))
		gbuffer::SaveRect(rect, cursor.behindAux); // plane A under the cursor (no-op unless BackBufferIsHybrid())
	DrawSoftwareCursor(out, cursPosition + Displacement { 0, cursSize.height - 1 }, pcurs);
}

/**
 * @brief Render a missile sprite
 * @param out Output buffer
 * @param missile Pointer to Missile struct
 * @param targetBufferPosition Output buffer coordinate
 * @param pre Is the sprite in the background
 */
void DrawMissilePrivate(const Surface &out, const Missile &missile, Point targetBufferPosition, bool pre, int lightTableIndex)
{
	if (missile._miPreFlag != pre || !missile._miDrawFlag)
		return;

	const Point missileRenderPosition { targetBufferPosition + missile.position.offsetForRendering - Displacement { missile._miAnimWidth2, 0 } };
	const ClxSprite sprite = (*missile._miAnimData)[missile._miAnimFrame - 1];
	if (missile._miUniqTrans != 0) {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = missileRenderPosition, .sprite = sprite, .trn = Monsters[missile._misource].uniqueMonsterTRN.get() });
	} else if (missile._miLightFlag) {
		ClxDrawLight(out, missileRenderPosition, sprite, lightTableIndex);
	} else {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = missileRenderPosition, .sprite = sprite, .selfLit = true });
	}
}

/**
 * @brief Render a missile sprites for a given tile
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param pre Is the sprite in the background
 */
void DrawMissile(const Surface &out, WorldTilePosition tilePosition, Point targetBufferPosition, bool pre, int lightTableIndex)
{
	const auto it = MissilesAtRenderingTile.find(tilePosition);
	if (it == MissilesAtRenderingTile.end()) return;
	for (Missile *missile : it->second) {
		DrawMissilePrivate(out, *missile, targetBufferPosition, pre, lightTableIndex);
	}
}

/**
 * @brief Whether the permanent fountain portal to the Cathedral is drawn on this tile (town only).
 */
[[nodiscard]] bool IsTownCathedralPortalTile(Point tilePosition)
{
	return leveltype == DTYPE_TOWN && !setlevel && tilePosition == TownCathedralPortalPosition && IsTownCathedralPortalEnabled();
}

/**
 * @brief Render the permanent fountain portal to the Cathedral with the blue town portal animation.
 *
 * It is pure decoration (no missile): the trigger itself is a fixed town trigger (InitTownTriggers), so nothing here
 * touches the simulation. The idle loop is timed by the wall clock like the classic 20 fps missile animation.
 */
void DrawTownCathedralPortal(const Surface &out, Point tilePosition, Point targetBufferPosition)
{
	if (!IsTownCathedralPortalTile(tilePosition))
		return;
	const MissileFileData &portalSprite = GetMissileSpriteData(MissileGraphicID::TownPortal);
	const OptionalClxSpriteList sprites = portalSprite.spritesForDirection(static_cast<Direction16>(PortalFrame::Idle));
	if (!sprites || sprites->numSprites() == 0)
		return;
	const uint32_t frame = static_cast<uint32_t>(SDL_GetTicks() / 50) % sprites->numSprites();
	const Point feet = targetBufferPosition - Displacement { portalSprite.animWidth2, 0 };
	DrawWorldSprite(out, WorldSpriteDraw { .feet = feet, .sprite = (*sprites)[frame], .selfLit = true });
}

/**
 * @brief Render a monster sprite
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param monster Monster reference
 */
void DrawMonster(const Surface &out, Point tilePosition, Point targetBufferPosition, const Monster &monster, int lightTableIndex)
{
	if (!monster.animInfo.sprites) {
		Log("Draw Monster \"{}\": NULL Cel Buffer", monster.name());
		return;
	}

	const ClxSprite sprite = monster.animInfo.currentSprite();

	if (!IsTileLit(tilePosition)) {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = targetBufferPosition, .sprite = sprite, .trn = GetInfravisionTRN() });
		return;
	}
	uint8_t *trn = nullptr;
	if (monster.isUnique())
		trn = monster.uniqueMonsterTRN.get();
	if (monster.mode == MonsterMode::Petrified)
		trn = GetStoneTRN();
	if (MyPlayer->_pInfraFlag && lightTableIndex > 8)
		trn = GetInfravisionTRN();
	if (trn != nullptr)
		DrawWorldSprite(out, WorldSpriteDraw { .feet = targetBufferPosition, .sprite = sprite, .trn = trn });
	else
		ClxDrawLight(out, targetBufferPosition, sprite, lightTableIndex);
}

/**
 * @brief Helper for rendering a specific player icon (Mana Shield or Reflect)
 */
void DrawPlayerIconHelper(const Surface &out, MissileGraphicID missileGraphicId, Point position, const Player &player, bool infraVision, int lightTableIndex)
{
	const bool lighting = &player != MyPlayer;

	position += player.position.GetDrawnScreenOffset(player.isWalking(), player._pdir, player.AnimInfo);

	position.x -=GetMissileSpriteData(missileGraphicId).animWidth2;

	const ClxSprite sprite = (*GetMissileSpriteData(missileGraphicId).sprites).list()[0];

	if (!lighting) {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = position, .sprite = sprite, .selfLit = true });
		return;
	}

	if (infraVision) {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = position, .sprite = sprite, .trn = GetInfravisionTRN(), .selfLit = true });
		return;
	}

	DrawWorldSprite(out, WorldSpriteDraw { .feet = position, .sprite = sprite, .shadeIndex = ShadeIndex(lightTableIndex), .selfLit = true });
}

/**
 * @brief Helper for rendering player icons (Mana Shield and Reflect)
 * @param out Output buffer
 * @param player Player reference
 * @param position Output buffer coordinates
 * @param infraVision Should infravision be applied
 */
void DrawPlayerIcons(const Surface &out, const Player &player, Point position, bool infraVision, int lightTableIndex)
{
	if (player.pManaShield)
		DrawPlayerIconHelper(out, MissileGraphicID::ManaShield, position, player, infraVision, lightTableIndex);
	if (player.wReflections > 0)
		DrawPlayerIconHelper(out, MissileGraphicID::Reflect, position + Displacement { 0, 16 }, player, infraVision, lightTableIndex);
}

uint8_t GetPlayerOutlineColor(int id)
{
	static constexpr uint8_t PlayerOutlineColors[] = {
		OutlineColorsPlayer1,
		OutlineColorsPlayer2,
		OutlineColorsPlayer3,
		OutlineColorsPlayer4,
	};

	if (id < 0 || id >= static_cast<int>(SDL_arraysize(PlayerOutlineColors)))
		return OutlineColorsPlayer1;

	return PlayerOutlineColors[id];
}

/**
 * @brief Render a player sprite
 * @param out Output buffer
 * @param player Player reference
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 */
void DrawPlayer(const Surface &out, const Player &player, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	if (!IsTileLit(tilePosition) && !MyPlayer->_pInfraFlag && !MyPlayer->isOnArenaLevel() && leveltype != DTYPE_TOWN) {
		return;
	}

	const ClxSprite sprite = player.currentSprite();
	const Point spriteBufferPosition = targetBufferPosition + player.getRenderingOffset(sprite);

	// Opt-in HD body (hd_sprites.hpp): replaces the classic body blit and its hover outline (the GPU outlines the HD
	// figure instead); infravision stays classic.
	const bool fullBright = &player == MyPlayer && IsNoneOf(leveltype, DTYPE_NEST, DTYPE_CRYPT);
	const bool infraVision = !fullBright && (!IsTileLit(tilePosition) || ((MyPlayer->_pInfraFlag || MyPlayer->isOnArenaLevel()) && lightTableIndex > 8));
	const bool hovered = &player == PlayerUnderCursor;
	bool hdBody = false;
	if (!infraVision) {
		const HdSpritesOutlineScope outline(hovered ? GetPlayerOutlineColor(player.getId()) : -1);
		hdBody = HdSpritesDrawPlayer(out, player, targetBufferPosition, fullBright ? 0 : std::max(lightTableIndex - 5, 0));
	}

	if (hovered && !hdBody)
		ClxDrawOutlineSkipColorZero(out, GetPlayerOutlineColor(player.getId()), spriteBufferPosition, sprite);

	if (fullBright) {
		// Classic: full-bright. Hybrid: a Card lit by the player's own light like every other player.
		if (!hdBody)
			DrawWorldSprite(out, WorldSpriteDraw { .feet = spriteBufferPosition, .sprite = sprite });
		DrawPlayerIcons(out, player, targetBufferPosition, /*infraVision=*/false, lightTableIndex);
		return;
	}

	if (infraVision) {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = spriteBufferPosition, .sprite = sprite, .trn = GetInfravisionTRN() });
		DrawPlayerIcons(out, player, targetBufferPosition, /*infraVision=*/true, lightTableIndex);
		return;
	}

	// The -5 bias only affects the classic shade: ShadeIndex maps every lit index to full-bright in hybrid mode.
	lightTableIndex = std::max(lightTableIndex - 5, 0);
	if (!hdBody)
		ClxDrawLight(out, spriteBufferPosition, sprite, lightTableIndex);
	DrawPlayerIcons(out, player, targetBufferPosition, /*infraVision=*/false, lightTableIndex);
}

/**
 * @brief Render a player sprite
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 */
void DrawDeadPlayer(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	dFlags[tilePosition.x][tilePosition.y] &= ~DungeonFlag::DeadPlayer;

	for (const Player &player : Players) {
		if (player.plractive && player.hasNoLife() && player.isOnActiveLevel() && player.position.tile == tilePosition) {
			dFlags[tilePosition.x][tilePosition.y] |= DungeonFlag::DeadPlayer;
			const Point playerRenderPosition { targetBufferPosition };
			DrawPlayer(out, player, tilePosition, playerRenderPosition, lightTableIndex);
		}
	}
}

/**
 * @brief Render an object sprite
 * @param out Output buffer
 * @param objectToDraw Dungeone object to draw
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param pre Is the sprite in the background
 */
void DrawObject(const Surface &out, const Object &objectToDraw, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	const ClxSprite sprite = objectToDraw.currentSprite();

	const Point screenPosition = targetBufferPosition + objectToDraw.getRenderingOffset(sprite, tilePosition);

	const bool hovered = &objectToDraw == ObjectUnderCursor;
	// Opt-in HD object (hd_objects.hpp): replaces the classic blit and outline (the GPU outlines the HD figure).
	{
		const HdSpritesOutlineScope outline(hovered ? OutlineColorsObject : -1);
		if (HdSpritesDrawObject(out, objectToDraw, screenPosition, sprite.width(), lightTableIndex))
			return;
	}
	if (hovered) {
		ClxDrawOutlineSkipColorZero(out, OutlineColorsObject, screenPosition, sprite);
	}
	if (objectToDraw.applyLighting) {
		ClxDrawLight(out, screenPosition, sprite, lightTableIndex);
	} else {
		DrawWorldSprite(out, WorldSpriteDraw { .feet = screenPosition, .sprite = sprite, .selfLit = true });
	}
}

static void DrawDungeon(const Surface & /*out*/, const Lightmap & /*lightmap*/, Point /*tilePosition*/, Point /*targetBufferPosition*/);

#if DEVILUTIONX_WEBGPU
/**
 * HD floor: uses of every dungeon CEL frame on this level as a floor block (mt[0] / mt[1]) of a walkable tile. Plain
 * Cathedral floor frames repeat across the level; stairs, their bottom step and the pieces around them use frames of
 * their own (one or two uses), so only frames with at least HdPlainFloorMinUses uses get the generic Cathedral atlas.
 */
std::vector<uint16_t> HdFloorFrameUses;
bool HdFloorFrameUsesValid = false;
constexpr uint16_t HdPlainFloorMinUses = 3;

void BuildHdFloorFrameUses()
{
	HdFloorFrameUses.clear();
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			if (!IsFloor({ x, y }))
				continue;
			const MICROS &micros = DPieceMicros[dPiece[x][y]];
			for (size_t i = 0; i < 2; ++i) {
				const LevelCelBlock block { micros.mt[i] };
				if (!block.hasValue())
					continue;
				const uint16_t frame = block.frame();
				if (frame >= HdFloorFrameUses.size())
					HdFloorFrameUses.resize(static_cast<size_t>(frame) + 1, 0);
				if (HdFloorFrameUses[frame] != UINT16_MAX)
					++HdFloorFrameUses[frame];
			}
		}
	}
	HdFloorFrameUsesValid = true;
}

[[nodiscard]] bool IsPlainCathedralFloorBlock(LevelCelBlock block)
{
	if (!block.hasValue())
		return false;
	if (!HdFloorFrameUsesValid)
		BuildHdFloorFrameUses();
	const uint16_t frame = block.frame();
	return frame < HdFloorFrameUses.size() && HdFloorFrameUses[frame] >= HdPlainFloorMinUses;
}

/**
 * HD town floor under solid pieces: the pixels of the cell's diamond rows (64 x 32, row 0 = base row - 32) that the
 * piece's bottom blocks write. They depend only on the piece (its CEL frames drawn with a solid mask), so DrawCell
 * finds them once per piece, from an unclipped draw, instead of drawing the blocks three times every frame.
 */
ankerl::unordered_dense::map<uint16_t, std::array<uint8_t, TILE_WIDTH * TILE_HEIGHT>> TownSolidFloorWritten;
#endif

/**
 * @brief Render a cell
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 */
void DrawCell(const Surface &out, const Lightmap lightmap, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	const uint16_t levelPieceId = dPiece[tilePosition.x][tilePosition.y];
	const MICROS *pMap = &DPieceMicros[levelPieceId];

	const uint8_t *tbl = LightTables[TileShadeIndex(lightTableIndex)].data();
	const uint8_t *foliageTbl = tbl;
#ifdef _DEBUG
	int walkpathIdx = -1;
	Point originalTargetBufferPosition;
	if (DebugPath) {
		walkpathIdx = MyPlayer->GetPositionPathIndex(tilePosition);
		if (walkpathIdx != -1) {
			originalTargetBufferPosition = targetBufferPosition;
			tbl = GetPauseTRN();
		}
	}
#endif

	bool transparency = TileHasAny(tilePosition, TileProperties::Transparent) && TransList[dTransVal[tilePosition.x][tilePosition.y]];
#ifdef _DEBUG
	if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) {
		transparency = false;
	}
#endif

	const auto getFirstTileMaskLeft = [=](TileType tile) -> MaskType {
		if (transparency) {
			switch (tile) {
			case TileType::LeftTrapezoid:
			case TileType::TransparentSquare:
				return TileHasAny(tilePosition, TileProperties::TransparentLeft)
				    ? MaskType::Left
				    : MaskType::Solid;
			case TileType::LeftTriangle:
				return MaskType::Solid;
			default:
				return MaskType::Transparent;
			}
		}
		return MaskType::Solid;
	};

	const auto getFirstTileMaskRight = [=](TileType tile) -> MaskType {
		if (transparency) {
			switch (tile) {
			case TileType::RightTrapezoid:
			case TileType::TransparentSquare:
				return TileHasAny(tilePosition, TileProperties::TransparentRight)
				    ? MaskType::Right
				    : MaskType::Solid;
			case TileType::RightTriangle:
				return MaskType::Solid;
			default:
				return MaskType::Transparent;
			}
		}
		return MaskType::Solid;
	};

	// Create a special lightmap buffer to bleed light up walls
	uint8_t lightmapBuffer[TILE_WIDTH * TILE_HEIGHT];
	const Lightmap bleedLightmap = Lightmap::bleedUp(*GetOptions().Graphics.perPixelLighting && !gbuffer::Enabled(), lightmap, targetBufferPosition, lightmapBuffer);

	// G-buffer tile context: every micro row of the cell is anchored at the cell's bottom base row,
	// so a wall pixel of frame row r in micro row k recovers h = 32k + r - foot(c). A wall half stands on the cell's
	// centre line: the left half shows the SE face (WallRight, foot c/2), the right half the SW face (WallLeft,
	// foot (31 - c)/2), so the foot lines of neighbouring pieces join into one line along the wall.
	const int baseY = targetBufferPosition.y;
	const int baseX = targetBufferPosition.x;
	const tile_geometry::PieceGeometry *geometry = gbuffer::CaptureActive() ? &tile_geometry::GetPieceGeometry(levelPieceId) : nullptr;
	const uint8_t faces = geometry != nullptr ? geometry->faces : 0;
	// A free-standing pillar shows both halves of a round or square shaft: one camera-facing normal, no seam down its middle.
	const bool pillar = geometry != nullptr && geometry->kind == tile_geometry::PieceKind::Pillar
	    && faces == static_cast<uint8_t>(static_cast<uint8_t>(tile_geometry::Face::SW) | static_cast<uint8_t>(tile_geometry::Face::SE));
	const auto setWallContext = [&](bool rightHalf) {
		const uint8_t faceBit = static_cast<uint8_t>(rightHalf ? tile_geometry::Face::SW : tile_geometry::Face::SE);
		const int leftX = rightHalf ? baseX + DunFrameWidth : baseX;
		if ((faces & faceBit) != 0) {
			if (rightHalf)
				SetTileGBufferContext(gbuffer::Kind::WallLeft, pillar ? gbuffer::NormalCardFlat : gbuffer::NormalWallLeft, baseY, leftX, gbuffer::FootKind::SW);
			else
				SetTileGBufferContext(gbuffer::Kind::WallRight, pillar ? gbuffer::NormalCardFlat : gbuffer::NormalWallRight, baseY, leftX, gbuffer::FootKind::SE);
		} else {
			// A faceless solid half (Cathedral filler, caps) is flat art classic lights at the pixel's own screen
			// position: lit as a cap at z = row it alternated with the floor pieces of the filler mass as a checkerboard.
			SetTileGBufferContext(gbuffer::Kind::Floor, gbuffer::NormalUp, baseY, leftX, gbuffer::FootKind::None);
		}
	};
	const auto setFloorContext = [&](bool rightHalf) {
		SetTileGBufferContext(gbuffer::Kind::Floor, gbuffer::NormalUp, baseY, rightHalf ? baseX + DunFrameWidth : baseX, gbuffer::FootKind::None);
	};

	// If the first micro tile is a floor tile, it may be followed
	// by foliage which should be rendered now.
	const bool isFloor = IsFloor(tilePosition);
	const auto drawBottomBlocks = [&]() {
	if (const LevelCelBlock levelCelBlock { pMap->mt[0] }; levelCelBlock.hasValue()) {
		const TileType tileType = levelCelBlock.type();
		if (!isFloor || tileType == TileType::TransparentSquare) {
			if (isFloor && tileType == TileType::TransparentSquare) {
				setFloorContext(/*rightHalf=*/false);
				RenderTileFoliage(out, bleedLightmap, targetBufferPosition,
				    pDungeonCels.get(), levelCelBlock, foliageTbl);
			} else {
				setWallContext(/*rightHalf=*/false);
				const MaskType mask = getFirstTileMaskLeft(tileType);
				if (mask != MaskType::Solid)
					HdSpritesMarkTranslucent(out, targetBufferPosition, DunFrameWidth, DunFrameHeight);
				RenderTile(out, bleedLightmap, targetBufferPosition,
				    pDungeonCels.get(), levelCelBlock, mask, tbl);
			}
		}
	}
	if (const LevelCelBlock levelCelBlock { pMap->mt[1] }; levelCelBlock.hasValue()) {
		const TileType tileType = levelCelBlock.type();
		if (!isFloor || tileType == TileType::TransparentSquare) {
			if (isFloor && tileType == TileType::TransparentSquare) {
				setFloorContext(/*rightHalf=*/true);
				RenderTileFoliage(out, bleedLightmap, targetBufferPosition + RightFrameDisplacement,
				    pDungeonCels.get(), levelCelBlock, foliageTbl);
			} else {
				setWallContext(/*rightHalf=*/true);
				const MaskType mask = getFirstTileMaskRight(tileType);
				if (mask != MaskType::Solid)
					HdSpritesMarkTranslucent(out, targetBufferPosition + RightFrameDisplacement, DunFrameWidth, DunFrameHeight);
				RenderTile(out, bleedLightmap, targetBufferPosition + RightFrameDisplacement,
				    pDungeonCels.get(), levelCelBlock, mask, tbl);
			}
		}
	}
	};
#if DEVILUTIONX_WEBGPU
	// HD town floor under solid pieces (river banks, tree bases, wall feet; hd/town-floor.txt): their bottom floor
	// blocks are drawn here, after the floor capture. Find the pixels they write by drawing them over two fills
	// (a written pixel comes out the same over both), restore the rest, and hand the diamond to the presenter. The
	// pixels depend only on the piece: once found from an unclipped draw they are reused (TownSolidFloorWritten).
	// Solid masks only: a blended block's result depends on what is underneath.
	if (leveltype == DTYPE_TOWN && !isFloor && !transparency && !gbuffer::Enabled() && WebGpuTownSolidFloorPiece(levelPieceId)) {
		constexpr int Width = TILE_WIDTH;
		constexpr int Height = TILE_HEIGHT;
		const int left = std::max(targetBufferPosition.x, 0);
		const int right = std::min(targetBufferPosition.x + Width, out.w());
		const int top = std::max(targetBufferPosition.y - Height, 0);
		const int bottom = std::min(targetBufferPosition.y + 1, out.h());
		const auto cached = TownSolidFloorWritten.find(levelPieceId);
		if (cached != TownSolidFloorWritten.end()) {
			drawBottomBlocks();
			if (left < right && top < bottom) {
				FlushTileRenderCommands(); // the capture reads the drawn pixels
				WebGpuCaptureTownSolidFloor(out.at(0, 0), out.pitch(), targetBufferPosition.x, targetBufferPosition.y, tilePosition.x, tilePosition.y, levelPieceId,
				    TileShadeIndex(lightTableIndex), cached->second.data());
			}
		} else if (left < right && top < bottom) {
			std::array<uint8_t, Width *(Height + 1)> saved {};
			std::array<uint8_t, Width *(Height + 1)> overBlack {};
			std::array<uint8_t, Width * Height> written {};
			const auto rectIndex = [&](int x, int y) { return static_cast<size_t>(y - (targetBufferPosition.y - Height)) * Width + (x - targetBufferPosition.x); };
			for (int y = top; y < bottom; ++y) {
				for (int x = left; x < right; ++x) {
					saved[rectIndex(x, y)] = *out.at(x, y);
					*out.at(x, y) = 0;
				}
			}
			drawBottomBlocks();
			FlushTileRenderCommands();
			for (int y = top; y < bottom; ++y) {
				for (int x = left; x < right; ++x) {
					overBlack[rectIndex(x, y)] = *out.at(x, y);
					*out.at(x, y) = 255;
				}
			}
			drawBottomBlocks();
			FlushTileRenderCommands();
			for (int y = top; y < bottom; ++y) {
				for (int x = left; x < right; ++x) {
					const size_t index = rectIndex(x, y);
					const bool wasWritten = *out.at(x, y) == overBlack[index];
					if (!wasWritten)
						*out.at(x, y) = saved[index];
					if (wasWritten && y < targetBufferPosition.y)
						written[index] = 1;
				}
			}
			if (left == targetBufferPosition.x && right == targetBufferPosition.x + Width && top == targetBufferPosition.y - Height)
				TownSolidFloorWritten.emplace(levelPieceId, written);
			WebGpuCaptureTownSolidFloor(out.at(0, 0), out.pitch(), targetBufferPosition.x, targetBufferPosition.y, tilePosition.x, tilePosition.y, levelPieceId,
			    TileShadeIndex(lightTableIndex), written.data());
		} else {
			drawBottomBlocks();
		}
	} else
#endif
	{
		drawBottomBlocks();
	}
	targetBufferPosition.y -= TILE_HEIGHT;

	for (uint_fast8_t i = 2, n = MicroTileLen; i < n; i += 2) {
		// See-through walls let HD sprites behind them show as a ghost, like the classic blended blit (hd_sprites.hpp).
		if (transparency && (LevelCelBlock { pMap->mt[i] }.hasValue() || LevelCelBlock { pMap->mt[i + 1] }.hasValue()))
			HdSpritesMarkTranslucent(out, targetBufferPosition, TILE_WIDTH, TILE_HEIGHT);
		{
			const LevelCelBlock levelCelBlock { pMap->mt[i] };
			if (levelCelBlock.hasValue()) {
				setWallContext(/*rightHalf=*/false);
				RenderTile(out, bleedLightmap, targetBufferPosition,
				    pDungeonCels.get(), levelCelBlock,
				    transparency ? MaskType::Transparent : MaskType::Solid, foliageTbl);
			}
		}
		{
			const LevelCelBlock levelCelBlock { pMap->mt[i + 1] };
			if (levelCelBlock.hasValue()) {
				setWallContext(/*rightHalf=*/true);
				RenderTile(out, bleedLightmap, targetBufferPosition + RightFrameDisplacement,
				    pDungeonCels.get(), levelCelBlock,
				    transparency ? MaskType::Transparent : MaskType::Solid, foliageTbl);
			}
		}
		targetBufferPosition.y -= TILE_HEIGHT;
	}

	// Execute the queued tile commands while `bleedLightmap` (referenced by every command queued above)
	// is still alive. This covers both callers (DrawDungeon and DrawDirtTile).
	FlushTileRenderCommands();
	assert(!HasPendingTileRenderCommands());

#ifdef _DEBUG
	if (DebugPath && walkpathIdx != -1) {
		DrawString(out, StrCat(walkpathIdx),
		    Rectangle(originalTargetBufferPosition + Displacement { 0, -TILE_HEIGHT }, Size { TILE_WIDTH, TILE_HEIGHT }),
		    TextRenderOptions {
		        .flags = UiFlags::AlignCenter | UiFlags::VerticalCenter
		            | (IsTileSolid(tilePosition) ? UiFlags::ColorYellow : UiFlags::ColorWhite) });
	}
#endif
}

/**
 * @brief Render a floor tile.
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinate
 */
void DrawFloorTile(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition)
{
	const RenderLightSnapshot &renderLights = GetRenderLightSnapshot();
	const int lightTableIndex = ComposePresentationLight(tilePosition, dLight[tilePosition.x][tilePosition.y], renderLights);
#if DEVILUTIONX_WEBGPU
	// The HD floor records the shade the CPU actually draws: the classic light level, or 0 (full-bright) under the
	// hybrid latch, where the GPU lights the replaced albedo from the G-buffer instead.
	// Cathedral: only plain floor blocks get the generic atlas (not stairs or other special floor pieces).
	const int floorShadeIndex = TileShadeIndex(lightTableIndex);
	bool floorAtlasCovers = false;
	if (leveltype == DTYPE_CATHEDRAL) {
		const MICROS &floorMicros = DPieceMicros[dPiece[tilePosition.x][tilePosition.y]];
		const uint8_t halves = static_cast<uint8_t>((IsPlainCathedralFloorBlock(LevelCelBlock { floorMicros.mt[0] }) ? 1 : 0)
		    | (IsPlainCathedralFloorBlock(LevelCelBlock { floorMicros.mt[1] }) ? 2 : 0));
		WebGpuMarkCathedralFloor(targetBufferPosition.x, targetBufferPosition.y, tilePosition.x, tilePosition.y, floorShadeIndex, halves);
		floorAtlasCovers = halves == 3 && HdTilesPreferFloorAtlas();
	} else if (leveltype == DTYPE_TOWN)
		WebGpuMarkTownFloor(targetBufferPosition.x, targetBufferPosition.y, tilePosition.x, tilePosition.y, dPiece[tilePosition.x][tilePosition.y], floorShadeIndex);
	// HD tiles (?hdTiles=1): the piece's own floor layer, drawn over the HD floor atlas unless ?hdAtlasFloor=1.
	if (!floorAtlasCovers)
		HdTilesRecordFloor(out, tilePosition, dPiece[tilePosition.x][tilePosition.y], targetBufferPosition, floorShadeIndex);
#endif

	const uint8_t *tbl = LightTables[TileShadeIndex(lightTableIndex)].data();
#ifdef _DEBUG
	if (DebugPath && MyPlayer->GetPositionPathIndex(tilePosition) != -1)
		tbl = GetPauseTRN();
#endif

	const uint16_t levelPieceId = dPiece[tilePosition.x][tilePosition.y];
	{
		const LevelCelBlock levelCelBlock { DPieceMicros[levelPieceId].mt[0] };
		if (levelCelBlock.hasValue()) {
			SetTileGBufferContext(gbuffer::Kind::Floor, gbuffer::NormalUp, targetBufferPosition.y, targetBufferPosition.x, gbuffer::FootKind::None);
			RenderTileFrame(out, lightmap, targetBufferPosition, TileType::LeftTriangle,
			    GetDunFrame(pDungeonCels.get(), levelCelBlock.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
		}
	}
	{
		const LevelCelBlock levelCelBlock { DPieceMicros[levelPieceId].mt[1] };
		if (levelCelBlock.hasValue()) {
			SetTileGBufferContext(gbuffer::Kind::Floor, gbuffer::NormalUp, targetBufferPosition.y, targetBufferPosition.x + DunFrameWidth, gbuffer::FootKind::None);
			RenderTileFrame(out, lightmap, targetBufferPosition + RightFrameDisplacement, TileType::RightTriangle,
			    GetDunFrame(pDungeonCels.get(), levelCelBlock.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
		}
	}
}

/**
 * @brief Draw item for a given tile
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param pre Is the sprite in the background
 */
void DrawItem(const Surface &out, int8_t itemIndex, Point targetBufferPosition, int lightTableIndex)
{
	const Item &item = Items[itemIndex];
	const ClxSprite sprite = item.AnimInfo.currentSprite();
	const Point position = targetBufferPosition + item.getRenderingOffset(sprite);
	const bool outlined = !IsPlayerInStore() && (itemIndex == pcursitem || AutoMapShowItems);
	// Opt-in HD item (hd_items.hpp): replaces the classic blit and outline (the GPU outlines the HD figure); the
	// label below stays.
	bool hdItem = false;
	{
		const HdSpritesOutlineScope outline(outlined ? GetOutlineColor(item, false) : -1);
		hdItem = HdSpritesDrawItem(out, item, position, sprite.width(), lightTableIndex);
	}
	if (!hdItem) {
		if (outlined)
			ClxDrawOutlineSkipColorZero(out, GetOutlineColor(item, false), position, sprite);
		ClxDrawLight(out, position, sprite, lightTableIndex);
	}
	if (item.AnimInfo.isLastFrame() || item._iCurs == ICURS_MAGIC_ROCK)
		AddItemToLabelQueue(itemIndex, position);
}

/**
 * @brief Check if and how a monster should be rendered
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 */
void DrawMonsterHelper(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	int mi = dMonster[tilePosition.x][tilePosition.y];

	mi = std::abs(mi) - 1;

	if (leveltype == DTYPE_TOWN) {
		auto &towner = Towners[mi];
		{
			// HD towner: the GPU draws the hover outline around the HD figure.
			const HdSpritesOutlineScope outline(mi == pcursmonst ? OutlineColorsTowner : -1);
			if (HdSpritesDrawTowner(out, towner, targetBufferPosition))
				return;
		}
		const Point position = targetBufferPosition + towner.getRenderingOffset();
		const ClxSprite sprite = towner.currentSprite();
		if (mi == pcursmonst) {
			ClxDrawOutlineSkipColorZero(out, OutlineColorsTowner, position, sprite);
		}
		ClxDraw(out, position, sprite);
		return;
	}

	if (!IsTileLit(tilePosition) && !(MyPlayer->_pInfraFlag && IsFloor(tilePosition))) {
		return;
	}

	if (static_cast<size_t>(mi) >= MaxMonsters) {
		Log("Draw Monster: tried to draw illegal monster {}", mi);
		return;
	}

	const auto &monster = Monsters[mi];
	if ((monster.flags & MFLAG_HIDDEN) != 0) {
		return;
	}

	const ClxSprite sprite = monster.animInfo.currentSprite();
	const Displacement offset = monster.getRenderingOffset(sprite);

	const Point monsterRenderPosition = targetBufferPosition + offset;
	const bool outlined = mi == pcursmonst;
	// Opt-in HD body (hd_sprites_monsters.hpp): replaces the classic blit and outline (the GPU outlines the HD figure).
	const bool infraVision = !IsTileLit(tilePosition) || (MyPlayer->_pInfraFlag && lightTableIndex > 8);
	{
		const HdSpritesOutlineScope outline(outlined ? OutlineColorsMonster : -1);
		if (HdSpritesDrawMonster(out, monster, targetBufferPosition, infraVision, lightTableIndex))
			return;
	}
	if (outlined) {
		ClxDrawOutlineSkipColorZero(out, OutlineColorsMonster, monsterRenderPosition, sprite);
	}
	DrawMonster(out, tilePosition, monsterRenderPosition, monster, lightTableIndex);
}

/** Anything DrawDungeon draws after the cell itself (characters, corpses, items, objects, missiles). */
[[nodiscard]] bool TileHasHdDynamicContent(Point tilePosition)
{
	const int x = tilePosition.x;
	const int y = tilePosition.y;
	return dCorpse[x][y] != 0 || dItem[x][y] != 0 || dObject[x][y] != 0 || dPlayer[x][y] != 0 || dMonster[x][y] != 0
	    || HasAnyOf(dFlags[x][y], DungeonFlag::Missile | DungeonFlag::DeadPlayer) || IsTownCathedralPortalTile(tilePosition);
}

/**
 * @brief Render object sprites
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 */
void DrawDungeon(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition)
{
	assert(InDungeonBounds(tilePosition));
	const RenderLightSnapshot &renderLights = GetRenderLightSnapshot();
	const int lightTableIndex = ComposePresentationLight(tilePosition, dLight[tilePosition.x][tilePosition.y], renderLights);

	// HD tiles (hd_tiles.hpp): the piece's cell layer is recorded in painter order right before its classic draw, and
	// the characters, items and missiles drawn after it are diffed so they stay in front of the tiles recorded so far.
	if (HdTilesActive()) {
		const bool cellTransparency = TileHasAny(tilePosition, TileProperties::Transparent) && TransList[dTransVal[tilePosition.x][tilePosition.y]];
		HdTilesRecordCell(out, tilePosition, dPiece[tilePosition.x][tilePosition.y], targetBufferPosition, TileShadeIndex(lightTableIndex), cellTransparency);
	}
	DrawCell(out, lightmap, tilePosition, targetBufferPosition, lightTableIndex);
	if (HdTilesActive() && TileHasHdDynamicContent(tilePosition))
		HdTilesBeginDynamic(out, targetBufferPosition);

	const int8_t bDead = dCorpse[tilePosition.x][tilePosition.y];
	const int8_t bMap = dTransVal[tilePosition.x][tilePosition.y];

#ifdef _DEBUG
	if (DebugVision && IsTileLit(tilePosition)) {
		ClxDraw(out, targetBufferPosition, (*pSquareCel)[0]);
	}
#endif

	if (MissilePreFlag) {
		DrawMissile(out, tilePosition, targetBufferPosition, true, lightTableIndex);
	}

	if (lightTableIndex < LightsMax && bDead != 0) {
		const Corpse &corpse = Corpses[(bDead & 0x1F) - 1];
		const Point position { targetBufferPosition.x - CalculateSpriteTileCenterX(corpse.width), targetBufferPosition.y };
		const ClxSprite sprite = corpse.spritesForDirection(static_cast<Direction>((bDead >> 5) & 7))[corpse.frame];
		if (HdSpritesDrawCorpse(out, corpse, static_cast<Direction>((bDead >> 5) & 7), targetBufferPosition, lightTableIndex)) {
			// Drawn in HD (hd_sprites_monsters.hpp).
		} else if (corpse.translationPaletteIndex != 0) {
			const uint8_t *trn = Monsters[corpse.translationPaletteIndex - 1].uniqueMonsterTRN.get();
			DrawWorldSprite(out, WorldSpriteDraw { .feet = position, .sprite = sprite, .trn = trn });
		} else {
			ClxDrawLight(out, position, sprite, lightTableIndex);
		}
	}

	const int8_t bItem = dItem[tilePosition.x][tilePosition.y];
	const Object *object = lightTableIndex < LightsMax
	    ? FindObjectAtPosition(tilePosition)
	    : nullptr;
	if (object != nullptr && object->_oPreFlag) {
		DrawObject(out, *object, tilePosition, targetBufferPosition, lightTableIndex);
	}
	if (bItem > 0 && !Items[bItem - 1]._iPostDraw) {
		DrawItem(out, static_cast<int8_t>(bItem - 1), targetBufferPosition, lightTableIndex);
	}

	if (TileContainsDeadPlayer(tilePosition)) {
		DrawDeadPlayer(out, tilePosition, targetBufferPosition, lightTableIndex);
	}
	Player *player = PlayerAtPosition(tilePosition);
	if (player != nullptr) {
		const uint8_t pid = player->getId();
		assert(pid < MAX_PLRS);
		int playerId = static_cast<int>(pid) + 1;
		// If sprite is moving southwards or east, we want to draw it offset from the tile it's moving to, so we need negative ID
		// This respests the order that tiles are drawn. By using the negative id, we ensure that the sprite is drawn with priority
		if (player->_pmode == PM_WALK_SOUTHWARDS || (player->_pmode == PM_WALK_SIDEWAYS && player->_pdir == Direction::East))
			playerId = -playerId;
		if (dPlayer[tilePosition.x][tilePosition.y] == playerId) {
			auto tempTilePosition = tilePosition;
			auto tempTargetBufferPosition = targetBufferPosition;

			// Offset the sprite to the tile it's moving from
			if (player->_pmode == PM_WALK_SOUTHWARDS) {
				switch (player->_pdir) {
				case Direction::SouthWest:
					tempTargetBufferPosition += { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				case Direction::South:
					tempTargetBufferPosition += { 0, -TILE_HEIGHT };
					break;
				case Direction::SouthEast:
					tempTargetBufferPosition += { -TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				default:
					DVL_UNREACHABLE();
				}
				tempTilePosition += Opposite(player->_pdir);
			} else if (player->_pmode == PM_WALK_SIDEWAYS && player->_pdir == Direction::East) {
				tempTargetBufferPosition += { -TILE_WIDTH, 0 };
				tempTilePosition += Opposite(player->_pdir);
			}
			DrawPlayer(out, *player, tempTilePosition, tempTargetBufferPosition, lightTableIndex);
		}
	}

	Monster *monster = FindMonsterAtPosition(tilePosition);
	if (monster != nullptr) {
		auto mid = monster->getId();
		assert(mid < MaxMonsters);
		int monsterId = static_cast<int>(mid) + 1;
		// If sprite is moving southwards or east, we want to draw it offset from the tile it's moving to, so we need negative ID
		// This respests the order that tiles are drawn. By using the negative id, we ensure that the sprite is drawn with priority
		if (monster->mode == MonsterMode::MoveSouthwards || (monster->mode == MonsterMode::MoveSideways && monster->direction == Direction::East))
			monsterId = -monsterId;
		if (dMonster[tilePosition.x][tilePosition.y] == monsterId) {
			auto tempTilePosition = tilePosition;
			auto tempTargetBufferPosition = targetBufferPosition;

			// Offset the sprite to the tile it's moving from
			if (monster->mode == MonsterMode::MoveSouthwards) {
				switch (monster->direction) {
				case Direction::SouthWest:
					tempTargetBufferPosition += { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				case Direction::South:
					tempTargetBufferPosition += { 0, -TILE_HEIGHT };
					break;
				case Direction::SouthEast:
					tempTargetBufferPosition += { -TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				default:
					DVL_UNREACHABLE();
				}
				tempTilePosition += Opposite(monster->direction);
			} else if (monster->mode == MonsterMode::MoveSideways && monster->direction == Direction::East) {
				tempTargetBufferPosition += { -TILE_WIDTH, 0 };
				tempTilePosition += Opposite(monster->direction);
			}
			DrawMonsterHelper(out, tempTilePosition, tempTargetBufferPosition, lightTableIndex);
		}
	}

	DrawMissile(out, tilePosition, targetBufferPosition, false, lightTableIndex);
	DrawTownCathedralPortal(out, tilePosition, targetBufferPosition);

	if (object != nullptr && !object->_oPreFlag) {
		DrawObject(out, *object, tilePosition, targetBufferPosition, lightTableIndex);
	}
	if (bItem > 0 && Items[bItem - 1]._iPostDraw) {
		DrawItem(out, static_cast<int8_t>(bItem - 1), targetBufferPosition, lightTableIndex);
	}

	if (leveltype != DTYPE_TOWN) {
		const bool hybrid = gbuffer::Enabled();
		const bool perPixelLighting = *GetOptions().Graphics.perPixelLighting && !hybrid;
		const int8_t bArch = dSpecial[tilePosition.x][tilePosition.y] - 1;
		if (bArch >= 0) {
			bool transparency = TransList[bMap];
#ifdef _DEBUG
			// Turn transparency off here for debugging
			transparency = transparency && (SDL_GetModState() & SDL_KMOD_ALT) == 0;
#endif
			const ClxSprite archSprite = (*pSpecialCels)[bArch];
			HdTilesRecordSpecial(out, tilePosition, bArch + 1, targetBufferPosition, TileShadeIndex(lightTableIndex), transparency);
			if (transparency)
				HdSpritesMarkTranslucent(out, targetBufferPosition, archSprite.width(), archSprite.height());
			if (perPixelLighting) {
				// Create a special lightmap buffer to bleed light up walls
				uint8_t lightmapBuffer[TILE_WIDTH * TILE_HEIGHT];
				const Lightmap bleedLightmap = Lightmap::bleedUp(perPixelLighting, lightmap, targetBufferPosition, lightmapBuffer);

				if (transparency)
					ClxDrawBlendedWithLightmap(out, targetBufferPosition, archSprite, bleedLightmap);
				else
					ClxDrawWithLightmap(out, targetBufferPosition, archSprite, bleedLightmap);
			} else {
				// Arches are upright wall faces anchored at the tile base: WallLeft / WallRight in hybrid mode.
				gbuffer::Kind archKind = gbuffer::Kind::Unlit;
				if (hybrid) {
					switch (tile_geometry::ArchFacing(leveltype, dSpecial[tilePosition.x][tilePosition.y])) {
					case 1:
						archKind = gbuffer::Kind::WallLeft;
						break;
					case 2:
						archKind = gbuffer::Kind::WallRight;
						break;
					default:
						break;
					}
				}
				DrawWorldSprite(out, WorldSpriteDraw { .feet = targetBufferPosition, .sprite = archSprite, .shadeIndex = TileShadeIndex(lightTableIndex), .blended = transparency, .archKind = archKind });
				if (archKind != gbuffer::Kind::Unlit) {
					const bool sw = archKind == gbuffer::Kind::WallLeft;
					gbuffer::AnchorArchOnWallLine(out, Rectangle { { targetBufferPosition.x, targetBufferPosition.y - archSprite.height() + 1 }, Size { archSprite.width(), archSprite.height() } },
					    gbuffer::PackAux(archKind, sw ? gbuffer::NormalWallLeft : gbuffer::NormalWallRight), targetBufferPosition.y, targetBufferPosition.x, sw ? gbuffer::FootKind::SW : gbuffer::FootKind::SE);
				}
			}
		}
	} else {
		// Tree leaves should always cover player when entering or leaving the tile,
		// So delay the rendering until after the next row is being drawn.
		// This could probably have been better solved by sprites in screen space.
		if (tilePosition.x > 0 && tilePosition.y > 0 && targetBufferPosition.y > TILE_HEIGHT) {
			const int8_t bArch = dSpecial[tilePosition.x - 1][tilePosition.y - 1] - 1;
			if (bArch >= 0) {
				HdTilesRecordSpecial(out, tilePosition + Displacement { -1, -1 }, bArch + 1, targetBufferPosition + Displacement { 0, -TILE_HEIGHT }, 0, false);
				ClxDraw(out, targetBufferPosition + Displacement { 0, -TILE_HEIGHT }, (*pSpecialCels)[bArch]);
			}
		}
	}
	HdTilesEndDynamic(out);
}

/**
 * @brief Render a row of tiles
 * @param out Buffer to render to
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 * @param rows Number of rows
 * @param columns Tile in a row
 */
void DrawFloor(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int rows, int columns)
{
	for (int i = 0; i < rows; i++) {
		for (int j = 0; j < columns; j++, tilePosition += Direction::East, targetBufferPosition.x += TILE_WIDTH) {
			if (!InDungeonBounds(tilePosition))
				continue;
			if (IsFloor(tilePosition)) {
				DrawFloorTile(out, lightmap, tilePosition, targetBufferPosition);
			}
		}
		// Return to start of row
		tilePosition += Displacement(Direction::West) * columns;
		targetBufferPosition.x -= columns * TILE_WIDTH;

		// Jump to next row
		targetBufferPosition.y += TILE_HEIGHT / 2;
		if ((i & 1) != 0) {
			tilePosition.x++;
			columns--;
			targetBufferPosition.x += TILE_WIDTH / 2;
		} else {
			tilePosition.y++;
			columns++;
			targetBufferPosition.x -= TILE_WIDTH / 2;
		}
	}
}

/**
 * @brief Renders the floor tiles
 * @param out Output buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Buffer coordinates
 * @param rows Number of rows
 * @param columns Tile in a row
 */
void DrawTileContent(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int rows, int columns)
{
	// Keep evaluating until MicroTiles can't affect screen
	rows += MicroTileLen;

#ifdef _DEBUG
	DebugCoordsMap.reserve(rows * columns);
#endif

	for (int i = 0; i < rows; i++) {
		bool skip = false;
		for (int j = 0; j < columns; j++) {
			if (InDungeonBounds(tilePosition)) {
				bool skipNext = false;
#ifdef _DEBUG
				DebugCoordsMap[tilePosition.x + (tilePosition.y * MAXDUNX)] = targetBufferPosition;
#endif
				if (tilePosition.x + 1 < MAXDUNX && tilePosition.y - 1 >= 0 && targetBufferPosition.x + TILE_WIDTH <= gnScreenWidth) {
					// Render objects behind walls first to prevent sprites, that are moving
					// between tiles, from poking through the walls as they exceed the tile bounds.
					// A proper fix for this would probably be to layout the scene and render by
					// sprite screen position rather than tile position.
					if (IsWall(tilePosition) && (IsWall(tilePosition + Displacement { 1, 0 }) || (tilePosition.x > 0 && IsWall(tilePosition + Displacement { -1, 0 })))) { // Part of a wall aligned on the x-axis
						if (IsTileNotSolid(tilePosition + Displacement { 1, -1 }) && IsTileNotSolid(tilePosition + Displacement { 0, -1 })) {                              // Has walkable area behind it
							DrawDungeon(out, lightmap, tilePosition + Direction::East, { targetBufferPosition.x + TILE_WIDTH, targetBufferPosition.y });
							skipNext = true;
						}
					}
				}
				if (!skip) {
					DrawDungeon(out, lightmap, tilePosition, targetBufferPosition);
				}
				skip = skipNext;
			}
			tilePosition += Direction::East;
			targetBufferPosition.x += TILE_WIDTH;
		}
		// Return to start of row
		tilePosition += Displacement(Direction::West) * columns;
		targetBufferPosition.x -= columns * TILE_WIDTH;

		// Jump to next row
		targetBufferPosition.y += TILE_HEIGHT / 2;
		if ((i & 1) != 0) {
			tilePosition.x++;
			columns--;
			targetBufferPosition.x += TILE_WIDTH / 2;
		} else {
			tilePosition.y++;
			columns++;
			targetBufferPosition.x -= TILE_WIDTH / 2;
		}
	}
}

void DrawDirtTile(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition)
{
	// This should be the *top-left* of the 2×2 dirt pattern in the actual dungeon.
	// You might need to tweak these to where your dirt patch actually lives.
	constexpr Point base { 0, 0 };

	// Decide which of the 4 tiles of the 2×2 block to use,
	// based on where this OOB tile is in the world grid.
	const int ox = (tilePosition.x & 1); // 0 or 1
	const int oy = (tilePosition.y & 1); // 0 or 1

	Point sample {
		base.x + ox,
		base.y + oy,
	};

	// Safety: clamp in case tilePosition is wildly outside and base+offset ever escapes
	sample.x = std::clamp(sample.x, 0, MAXDUNX - 1);
	sample.y = std::clamp(sample.y, 0, MAXDUNY - 1);

	if (!InDungeonBounds(sample) || dPiece[sample.x][sample.y] == 0) {
		// Failsafe: if our sample somehow isn't valid, fall back to black
		world_draw_black_tile(out, targetBufferPosition.x, targetBufferPosition.y);
		return;
	}

	const int lightTableIndex = dLight[sample.x][sample.y];

	// Let the normal dungeon tile renderer compose the full tile
	DrawCell(out, lightmap, sample, targetBufferPosition, lightTableIndex);
}

/**
 * @brief Render a row of tiles
 * @param out Buffer to render to
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 * @param rows Number of rows
 * @param columns Tile in a row
 */
void DrawOOB(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int rows, int columns)
{
	for (int i = 0; i < rows + 5; i++) { // 5 extra rows needed to make sure everything gets rendered at the bottom half of the screen
		for (int j = 0; j < columns; j++, tilePosition += Direction::East, targetBufferPosition.x += TILE_WIDTH) {
			if (!InDungeonBounds(tilePosition)) {
				if (leveltype == DTYPE_TOWN) {
					world_draw_black_tile(out, targetBufferPosition.x, targetBufferPosition.y);
				} else {
					DrawDirtTile(out, lightmap, tilePosition, targetBufferPosition);
				}
			}
		}
		// Return to start of row
		tilePosition += Displacement(Direction::West) * columns;
		targetBufferPosition.x -= columns * TILE_WIDTH;

		// Jump to next row
		targetBufferPosition.y += TILE_HEIGHT / 2;
		if ((i & 1) != 0) {
			tilePosition.x++;
			columns--;
			targetBufferPosition.x += TILE_WIDTH / 2;
		} else {
			tilePosition.y++;
			columns++;
			targetBufferPosition.x -= TILE_WIDTH / 2;
		}
	}
}

struct ZoomGeometry {
	int viewportWidth;
	int viewportOffsetX;
};

/**
 * @brief The zoomed viewport width and its x offset (the left side panel, when open) for an output of `outWidth` pixels.
 *
 * Shared by Zoom() and gbuffer::ZoomInPlace so the planes are doubled with exactly the numbers the colour buffer uses.
 */
[[nodiscard]] ZoomGeometry CalcZoomGeometry(int outWidth)
{
	ZoomGeometry geometry { outWidth, 0 };
	if (CanPanelsCoverView()) {
		if (IsLeftPanelOpen()) {
			geometry.viewportWidth -= SidePanelSize.width;
			geometry.viewportOffsetX = SidePanelSize.width;
		} else if (IsRightPanelOpen()) {
			geometry.viewportWidth -= SidePanelSize.width;
		}
	}
	return geometry;
}

/**
 * @brief Scales the top left part of the buffer up into the viewport (in place, nearest), as `transform` maps it.
 *
 * The first source column / row gets the remainder; at a fractional level the columns alternate between the two
 * nearest whole widths (the classic art left in the frame is resampled nearest; the HD art draws at full resolution).
 */
void Zoom(const Surface &out, const HdWorldTransform &transform)
{
	HdUpscaleInPlace(out.at(0, 0), static_cast<size_t>(out.pitch()), transform);
}

Displacement tileOffset;
Displacement tileShift;
/** The zoom (GetZoomQuarters()) the view geometry was computed for; the effective zoom follows the level's HD art. */
int GeometryZoomQuarters = 4;
std::optional<bool> FractionalZoomOverride;
int tileColumns;
int tileRows;

/**
 * @brief Screen offset of my player's drawn position from the centre of its tile.
 *
 * With free-form movement this can be off the tile centre in any direction,
 * also while not walking, so the camera follows it at all times.
 */
Displacement GetMyPlayerDrawnOffset()
{
	const Player &myPlayer = *MyPlayer;
	return myPlayer.position.GetDrawnScreenOffset(myPlayer.isWalking(), myPlayer._pdir, myPlayer.AnimInfo);
}

void CalcFirstTilePosition(Point &position, Displacement &offset)
{
	// The effective zoom follows the level (fractional levels need its HD tiles): keep the geometry in step.
	if (GetZoomQuarters() != GeometryZoomQuarters)
		CalcViewportGeometry();
	// Adjust by player offset and tile grid alignment
	const Displacement playerOffset = GetMyPlayerDrawnOffset();
	offset = tileOffset;
	offset -= playerOffset;

	position += tileShift;

	// Skip rendering parts covered by the panels: centre the view in the uncovered part (half a panel over, in
	// world pixels), skipping whole columns where possible
	if (CanPanelsCoverView() && (IsLeftPanelOpen() || IsRightPanelOpen())) {
		const int shift = ScreenToWorldLength(SidePanelSize.width / 2);
		const int columns = shift / TILE_WIDTH;
		position += Displacement(Direction::East) * columns;
		offset.deltaX -= shift - TILE_WIDTH * columns;

		if (IsLeftPanelOpen() && GetZoomQuarters() == 4) {
			offset.deltaX += SidePanelSize.width;
			// SidePanelSize.width accounted for in Zoom()
		}
	}

	if (!IsSmoothMovementEnabled()) {
		// Classic: draw areas moving in and out of the screen by walk direction
		const Player &myPlayer = *MyPlayer;
		if (myPlayer.isWalking()) {
			switch (myPlayer._pdir) {
			case Direction::North:
			case Direction::NorthEast:
				offset.deltaY -= TILE_HEIGHT;
				position += Direction::North;
				break;
			case Direction::SouthWest:
			case Direction::West:
				offset.deltaX -= TILE_WIDTH;
				position += Direction::West;
				break;
			case Direction::NorthWest:
				offset.deltaX -= TILE_WIDTH / 2;
				offset.deltaY -= TILE_HEIGHT / 2;
				position += Direction::NorthWest;
			default:
				break;
			}
		}
		return;
	}

	// Draw areas moving in and out of the screen: start whole tiles further left/up when the
	// view is shifted that way (DrawGame adds the matching columns and rows)
	if (playerOffset.deltaX < 0) {
		const int tiles = (-playerOffset.deltaX + TILE_WIDTH - 1) / TILE_WIDTH;
		offset.deltaX -= TILE_WIDTH * tiles;
		position += Displacement(Direction::West) * tiles;
	}
	if (playerOffset.deltaY < 0) {
		const int tiles = (-playerOffset.deltaY + TILE_HEIGHT - 1) / TILE_HEIGHT;
		offset.deltaY -= TILE_HEIGHT * tiles;
		position += Displacement(Direction::North) * tiles;
	}
}

/**
 * @brief Configure render and process screen rows
 * @param fullOut Buffer to render to
 * @param position First tile of view in dPiece coordinate
 * @param offset Amount to offset the rendering in screen space
 */
/**
 * @brief The world pass while the first-person view is active (fpv::SkipIsometricWorld()).
 *
 * The isometric world is not drawn: the world rows are cleared to fpv::kTransparentWorldIndex so the WebGPU
 * presenter shows its 3D image there and composites the UI drawn afterwards on top. The per-frame bookkeeping of
 * the HD and hybrid passes still runs (with nothing recorded) so their state stays consistent across a G toggle.
 */
void DrawFirstPersonWorldPass(const Surface &fullOut, const Surface &zoomOut, const HdWorldTransform &worldTransform)
{
	const Surface &out = fullOut.subregionY(0, worldTransform.world.height);
	gbuffer::BeginWorldPass(gnViewportHeight); // the hybrid latch is off in first person: records a classic frame
	HdSetWorldTransform(worldTransform);
#if DEVILUTIONX_WEBGPU
	HdTilesBeginWorldPass(out, false);
	WebGpuBeginCathedralFloor(out.w(), out.h(), false);
#endif
	HdSpritesBeginWorldPass(out, /*allowed=*/false);
	HdUiInvalidateRect(zoomOut.region.x, zoomOut.region.y, zoomOut.region.w, zoomOut.region.h);
	fpv::ClearWorldArea(zoomOut, gnViewportHeight);
}

void DrawGame(const Surface &fullOut, Point position, Displacement offset)
{
	// Limit rendering to the view area
	// The HD passes record in world-pass pixels (smaller under zoom) and map them to the screen with this transform
	// (hd_view_transform.hpp); it is published below before anything records.
	const Surface zoomOut = fullOut.subregionY(0, gnViewportHeight);
	const ZoomGeometry zoomGeometry = CalcZoomGeometry(zoomOut.w());
	const int zoomQuarters = GetZoomQuarters();
	const HdWorldTransform worldTransform = ComputeHdWorldTransform(zoomQuarters, zoomOut.w(), zoomOut.h(),
	    zoomGeometry.viewportWidth, zoomGeometry.viewportOffsetX);
	const Surface &out = fullOut.subregionY(0, worldTransform.world.height);

	int columns = tileColumns;
	int rows = tileRows;

	// Skip rendering parts covered by the panels
	if (CanPanelsCoverView() && (IsLeftPanelOpen() || IsRightPanelOpen())) {
		columns -= (ScreenToWorldLength(SidePanelSize.width) - 1) / TILE_WIDTH;
	}

	UpdateMissilesRendererData();

	if (fpv::SkipIsometricWorld()) {
		DrawFirstPersonWorldPass(fullOut, zoomOut, worldTransform);
		return;
	}

	// Draw areas moving in and out of the screen (see CalcFirstTilePosition)
	if (!IsSmoothMovementEnabled()) {
		if (MyPlayer->isWalking()) {
			switch (MyPlayer->_pdir) {
			case Direction::NoDirection:
				break;
			case Direction::North:
			case Direction::South:
				rows += 2;
				break;
			case Direction::NorthEast:
				columns++;
				rows += 2;
				break;
			case Direction::East:
			case Direction::West:
				columns++;
				break;
			case Direction::SouthEast:
			case Direction::SouthWest:
			case Direction::NorthWest:
				columns++;
				rows++;
				break;
			}
		}
	} else {
		const Displacement playerOffset = GetMyPlayerDrawnOffset();
		columns += (std::abs(playerOffset.deltaX) + TILE_WIDTH - 1) / TILE_WIDTH;
		rows += 2 * ((std::abs(playerOffset.deltaY) + TILE_HEIGHT - 1) / TILE_HEIGHT);
	}

#ifdef DUN_RENDER_STATS
	DunRenderStats.clear();
#endif

	// Hybrid world pass bracket: always records whether this back buffer is a hybrid frame;
	// clears plane A and enters CaptureWorld only when the frame latch enabled the mode.
	gbuffer::BeginWorldPass(gnViewportHeight);
	const bool hybrid = gbuffer::Enabled();

	Lightmap lightmap = Lightmap::build(*GetOptions().Graphics.perPixelLighting && !hybrid, position, Point {} + offset,
	    gnScreenWidth, gnViewportHeight, rows, columns,
	    out.at(0, 0), out.pitch(), LightTables, FullyLitLightTable, FullyDarkLightTable,
	    dLight, MicroTileLen);

	HdSetWorldTransform(worldTransform);

#if DEVILUTIONX_WEBGPU
	// HD floor (?hdFloor=1), classic and hybrid alike: classic composites the HD albedo at the classic light level,
	// hybrid swaps it in for the full-bright floor pixels before the GPU lighting. Under zoom the marks are resampled
	// to the screen when the frame is presented (WebGpuFinishCathedralFloor).
	// Town loads its HD pages on the first town frame with ?hdFloor=1 (WebGpuPrepareTownFloor).
	// HD tiles replace the whole town (floor included) from the remastered composite: no town floor atlas then.
	// Under zoom they are recorded at the half-size world and scaled with it (HdTilesApplyZoom).
	HdTilesBeginWorldPass(out, true);
	const bool hdFloorLevel = leveltype == DTYPE_CATHEDRAL || (leveltype == DTYPE_TOWN && !HdTilesWantedForLevel() && WebGpuPrepareTownFloor());
	WebGpuBeginCathedralFloor(out.w(), out.h(), hdFloorLevel);
#endif
	// Opt-in HD sprites draw at canvas resolution over the frame; under hybrid lighting they are lit as cards.
	HdSpritesBeginWorldPass(out, /*allowed=*/true);
	HdSpritesKeepItemClipsWarm();
	HdUiInvalidateRect(zoomOut.region.x, zoomOut.region.y, zoomOut.region.w, zoomOut.region.h); // the world pass repaints the view
	DrawFloor(out, lightmap, position, Point {} + offset, rows, columns);
	FlushTileRenderCommands();
#if DEVILUTIONX_WEBGPU
	WebGpuCaptureCathedralFloor(out.at(0, 0), out.pitch());
#endif
	DrawTileContent(out, lightmap, position, Point {} + offset, rows, columns);
	HdTilesEndWorldContent(out);
	DrawOOB(out, lightmap, position, Point {} + offset, rows, columns);
	FlushTileRenderCommands();

	if (zoomQuarters > 4) {
		Zoom(zoomOut, worldTransform);
		HdTilesApplyZoom(zoomOut, zoomGeometry.viewportWidth, zoomGeometry.viewportOffsetX, zoomQuarters);
		if (hybrid)
			gbuffer::ZoomInPlace(zoomGeometry.viewportWidth, zoomGeometry.viewportOffsetX, zoomOut.h(), zoomQuarters);
	}
	// The world pass is on the screen now: move the HD instances there too (identity without zoom).
	HdSpritesApplyWorldTransform(worldTransform);

	if (hybrid)
		gbuffer::EndWorldPass();

#ifdef DUN_RENDER_STATS
	std::vector<std::pair<DunRenderType, size_t>> sortedStats(DunRenderStats.begin(), DunRenderStats.end());
	c_sort(sortedStats, [](const std::pair<DunRenderType, size_t> &a, const std::pair<DunRenderType, size_t> &b) {
		return a.first.maskType == b.first.maskType
		    ? static_cast<uint8_t>(a.first.tileType) < static_cast<uint8_t>(b.first.tileType)
		    : static_cast<uint8_t>(a.first.maskType) < static_cast<uint8_t>(b.first.maskType);
	});
	Point pos { 100, 20 };
	for (size_t i = 0; i < sortedStats.size(); ++i) {
		const auto &stat = sortedStats[i];
		DrawString(out, StrCat(i, "."), Rectangle(pos, Size { 20, 16 }), { .flags = UiFlags::AlignRight });
		DrawString(out, MaskTypeToString(stat.first.maskType), { pos.x + 24, pos.y });
		DrawString(out, TileTypeToString(stat.first.tileType), { pos.x + 184, pos.y });
		DrawString(out, FormatInteger(stat.second), Rectangle({ pos.x + 354, pos.y }, Size(40, 16)), { .flags = UiFlags::AlignRight });
		pos.y += 16;
	}
#endif
}

/**
 * @brief Start rendering of screen, town variation
 * @param out Buffer to render to (the back buffer)
 * @param startPosition Center of view in dPiece coordinates
 * @return the surface the rest of the UI draws into: `out`, or the UI layer under a UI scale (ui_scale.hpp)
 */
Surface DrawView(const Surface &out, Point startPosition)
{
#ifdef _DEBUG
	DebugCoordsMap.clear();
#endif
	Displacement offset = {};
	CalcFirstTilePosition(startPosition, offset);
	DrawGame(out, startPosition, offset);
	if (AutomapActive) {
		DrawAutomap(out.subregionY(0, gnViewportHeight));
	}
#ifdef _DEBUG
	bool debugGridTextNeeded = IsDebugGridTextNeeded();
	if (debugGridTextNeeded || DebugGrid) {
		// force redrawing or debug stuff stays on panel on 640x480 resolution
		RedrawEverything();
		std::string debugGridText;
		bool megaTiles = IsDebugGridInMegatiles();

		for (auto [dunCoordVal, pixelCoords] : DebugCoordsMap) {
			Point dunCoords = { dunCoordVal % MAXDUNX, dunCoordVal / MAXDUNX };
			if (megaTiles && (dunCoords.x % 2 == 1 || dunCoords.y % 2 == 1))
				continue;
			if (megaTiles)
				pixelCoords += Displacement { 0, TILE_HEIGHT / 2 };
			pixelCoords = { GetViewTransform().ToScreenX(pixelCoords.x), GetViewTransform().ToScreenY(pixelCoords.y) };
			if (debugGridTextNeeded && GetDebugGridText(dunCoords, debugGridText)) {
				Size tileSize = { TILE_WIDTH, TILE_HEIGHT };
				tileSize = { TILE_WIDTH * GetZoomQuarters() / 4, TILE_HEIGHT * GetZoomQuarters() / 4 };
				DrawString(out, debugGridText, { pixelCoords - Displacement { 0, tileSize.height }, tileSize },
				    { .flags = UiFlags::ColorRed | UiFlags::AlignCenter | UiFlags::VerticalCenter });
			}
			if (DebugGrid) {
				int halfTileWidth = TILE_WIDTH / 2;
				int halfTileHeight = TILE_HEIGHT / 2;
				halfTileWidth = halfTileWidth * GetZoomQuarters() / 4;
				halfTileHeight = halfTileHeight * GetZoomQuarters() / 4;
				const Point center { pixelCoords.x + halfTileWidth, pixelCoords.y - halfTileHeight };

				if (megaTiles) {
					halfTileWidth *= 2;
					halfTileHeight *= 2;
				}

				const uint8_t col = PAL16_BEIGE;
				for (const auto &[originX, dx] : { std::pair(center.x - halfTileWidth, 1), std::pair(center.x + halfTileWidth, -1) }) {
					// We only need to draw half of the grid cell boundaries (one triangle).
					// The other triangle will be drawn when drawing the adjacent grid cells.
					const int dy = 1;
					Point from { originX, center.y };
					int height = halfTileHeight;
					if (out.InBounds(from) && out.InBounds(from + Displacement { 2 * dx * height, dy * height })) {
						uint8_t *dst = out.at(from.x, from.y);
						const int pitch = out.pitch();
						while (height-- > 0) {
							*dst = col;
							dst += dx;
							*dst = col;
							dst += dx;
							dst += static_cast<ptrdiff_t>(dy * pitch);
						}
					} else {
						while (height-- > 0) {
							out.SetPixel(from, col);
							from.x += dx;
							out.SetPixel(from, col);
							from.x += dx;
							from.y += dy;
						}
					}
				}
			}
		}
	}
#endif
	if (!fpv::SkipIsometricWorld())
		DrawFloatingNumbers(out, startPosition, offset);

	// Everything from here on is UI, laid out in UI space (ui_scale.hpp).
	const Surface ui = BeginUiLayer(out);
	DrawItemNameLabels(ui);
	DrawMonsterHealthBar(ui);
	fpv::DrawHud(ui);

	if (IsPlayerInStore() && !qtextflag)
		DrawSText(ui);
	if (invflag) {
		DrawInv(ui);
	} else if (SpellbookFlag) {
		DrawSpellBook(ui);
	}

	DrawDurIcon(ui);

	DrawLevelButton(ui);

	if (CharFlag) {
		DrawChr(ui);
	} else if (QuestLogIsOpen) {
		DrawQuestLog(ui);
	} else if (IsStashOpen) {
		DrawStash(ui);
	} else if (IsVisualStoreOpen) {
		DrawVisualStore(ui);
	}

	if (ShowUniqueItemInfoBox) {
		DrawUniqueInfo(ui);
	}
	if (qtextflag) {
		DrawQText(ui);
	}
	if (SpellSelectFlag) {
		DrawSpellList(ui);
	}
	if (DropGoldFlag) {
		DrawGoldSplit(ui);
	}
	DrawGoldWithdraw(ui);
	if (HelpFlag) {
		DrawHelp(ui);
	}
	if (ChatLogFlag) {
		DrawChatLog(ui);
	}
	if (MyPlayerIsDead) {
		RedBack(ui);
		DrawDeathText(ui);
	} else if (PauseMode != 0) {
		gmenu_draw_pause(ui);
	}
	if (IsDiabloMsgAvailable()) {
		DrawDiabloMsg(ui.subregionY(0, ui.h() - GetMainPanel().size.height));
	}

	DrawControllerModifierHints(ui);
	DrawPlrMsg(ui);
	gmenu_draw(ui);
	doom_draw(ui);
	DrawInfoBox(ui);
	UpdateLifeManaPercent(); // Update life/mana totals before rendering any portion of the flask.
	DrawLifeFlaskUpper(ui);
	DrawManaFlaskUpper(ui);
	return ui;
}

/**
 * @brief Display the current average FPS over 1 sec
 */
void DrawFPS(const Surface &out)
{
	static int framesSinceLastUpdate = 0;
	static std::string_view formatted {};

	if (!frameflag || !gbActive) {
		return;
	}

	framesSinceLastUpdate++;
	const uint32_t runtimeInMs = SDL_GetTicks();
	const uint32_t msSinceLastUpdate = runtimeInMs - lastFpsUpdateInMs;
	if (msSinceLastUpdate >= 1000) {
		lastFpsUpdateInMs = runtimeInMs;
		constexpr int FpsPow10 = 10;
		const uint32_t fps = 1000 * FpsPow10 * framesSinceLastUpdate / msSinceLastUpdate;
		framesSinceLastUpdate = 0;

		static char buf[15] {};
		const char *end = fps >= 100 * FpsPow10
		    ? BufCopy(buf, fps / FpsPow10, " FPS")
		    : BufCopy(buf, fps / FpsPow10, ".", fps % FpsPow10, " FPS");
		formatted = { buf, static_cast<std::string_view::size_type>(end - buf) };
	};
	DrawString(out, formatted, Point { 8, 8 }, { .flags = UiFlags::ColorRed });
}

/**
 * @brief First back-buffer row the presenter reads from the RGBA output surface this frame.
 *
 * A hybrid frame palette-expands the viewport rows on the GPU from the 8-bit back buffer (index and palette
 * textures, byte-identical to BltFast) and uploads only the RGBA rows below them (PresentHybrid). The exception is the
 * virtual gamepad: it is drawn into the RGBA frame, which is then uploaded whole (dx.cpp, `frameHasUnlit`). Blitting the
 * viewport rows otherwise palette-expands the whole view on the CPU for nothing.
 */
int FirstPresentedOutputRow()
{
#if DEVILUTIONX_WEBGPU && !defined(USE_SDL1)
	if (gbuffer::BackBufferIsHybrid() && ControlMode != ControlTypes::VirtualGamepad)
		return GetViewportHeight();
#endif
	return 0;
}

/**
 * @brief Update part of the screen from the back buffer
 */
void DoBlitScreen(Rectangle area)
{
#ifdef DEBUG_DO_BLIT_SCREEN
	const Surface &out = GlobalBackBuffer();
	const uint8_t debugColor = PAL8_RED;
	DrawHorizontalLine(out, area.position, area.size.width, debugColor);
	DrawHorizontalLine(out, area.position + Displacement { 0, area.size.height - 1 }, area.size.width, debugColor);
	DrawVerticalLine(out, area.position, area.size.height, debugColor);
	DrawVerticalLine(out, area.position + Displacement { area.size.width - 1, 0 }, area.size.height, debugColor);
#endif
	SDL_Rect srcRect = MakeSdlRect(area);
	SDL_Rect dstRect = MakeSdlRect(area);
	BltFast(&srcRect, &dstRect);
}

/**
 * @brief Check render pipeline and update individual screen parts
 * @param out Output surface.
 * @param dwHgt Section of screen to update from top to bottom
 * @param drawDesc Render info box
 * @param drawHp Render health bar
 * @param drawMana Render mana bar
 * @param drawSbar Render belt
 * @param drawBtn Render panel buttons
 */
void DrawMain(int dwHgt, bool drawDesc, bool drawHp, bool drawMana, bool drawSbar, bool drawBtn)
{
	if (!gbActive || RenderDirectlyToOutputSurface) {
		return;
	}

	assert(dwHgt >= 0 && dwHgt <= gnScreenHeight);

	const int firstRow = FirstPresentedOutputRow();
	const auto DoBlitScreen = [firstRow](Rectangle area) {
		const int top = std::max(area.position.y, firstRow);
		const int bottom = area.position.y + area.size.height;
		if (bottom > top)
			devilution::DoBlitScreen({ { area.position.x, top }, { area.size.width, bottom - top } });
	};

	if (dwHgt > 0) {
		DoBlitScreen({ { 0, 0 }, { gnScreenWidth, dwHgt } });
	}
	if (dwHgt < gnScreenHeight) {
		const Point mainPanelPosition = GetMainPanel().position;
		if (drawSbar) {
			DoBlitScreen({ mainPanelPosition + Displacement { 204, 5 }, { 232, 28 } });
		}
		if (drawDesc) {
			if (ChatFlag) {
				// When chat input is displayed, the belt is hidden and the chat moves up.
				DoBlitScreen({ mainPanelPosition + Displacement { 171, 6 }, { 298, 116 } });
			} else {
				DoBlitScreen({ mainPanelPosition + Displacement { InfoBoxRect.position.x, InfoBoxRect.position.y }, { InfoBoxRect.size } });
			}
		}
		if (drawMana) {
			DoBlitScreen({ mainPanelPosition + Displacement { 460, 0 }, { 88, 72 } });
			DoBlitScreen({ mainPanelPosition + Displacement { 564, 64 }, { 56, 56 } });
		}
		if (drawHp) {
			DoBlitScreen({ mainPanelPosition + Displacement { 96, 0 }, { 88, 72 } });
		}
		if (drawBtn) {
			DoBlitScreen({ mainPanelPosition + Displacement { 8, 7 }, { 74, 114 } });
			DoBlitScreen({ mainPanelPosition + Displacement { 559, 7 }, { 74, 48 } });
			if (gbIsMultiplayer) {
				DoBlitScreen({ mainPanelPosition + Displacement { 86, 91 }, { 34, 32 } });
				DoBlitScreen({ mainPanelPosition + Displacement { 526, 91 }, { 34, 32 } });
			}
		}
		if (PrevCursorRect.size.width != 0 && PrevCursorRect.size.height != 0) {
			DoBlitScreen(PrevCursorRect);
		}
		const Rectangle &cursorRect = GetDrawnCursor().rect;
		if (cursorRect.size.width != 0 && cursorRect.size.height != 0) {
			DoBlitScreen(cursorRect);
		}
	}
}

void OptionShowFPSChanged()
{
	if (*GetOptions().Graphics.showFPS)
		EnableFrameCount();
	else
		frameflag = false;
}
const auto OptionChangeHandlerShowFPS = (GetOptions().Graphics.showFPS.SetValueChangedCallback(OptionShowFPSChanged), true);

} // namespace

void DrawWorldForTesting(const Surface &out, Point viewPosition, Displacement offset)
{
	DrawGame(out, viewPosition, offset);
}

void DrawWorldSprite(const Surface &out, const WorldSpriteDraw &draw, bool hybrid)
{
	// No-reveal: a sprite whose authoritative index is fully dark is black in classic. Items, light-flagged
	// missiles and arches reach this funnel without a cull on dLight == 15, and the GPU gates neither cards (ambient is
	// bilinear) nor arches (gated on the front cell), so hybrid keeps the classic dark table for them and tags the
	// pixels pass-through instead of letting the presenter light them. A semantic TRN wins, exactly as in classic.
	const bool fullyDark = hybrid && draw.trn == nullptr && draw.shadeIndex >= LightsMax;
	if (fullyDark) {
		gbuffer::CurrentCardKind = gbuffer::Kind::CardEmissive;
	} else if (hybrid) {
		if (draw.archKind != gbuffer::Kind::Unlit) {
			// Arches: constant WallLeft / WallRight aux with the tile base as anchor (foot None), never a card normal.
			const uint8_t normalIdx = draw.archKind == gbuffer::Kind::WallLeft ? gbuffer::NormalWallLeft : gbuffer::NormalWallRight;
			gbuffer::SetTileContext(draw.archKind, normalIdx, draw.feet.y, draw.feet.x, gbuffer::FootKind::None);
			gbuffer::CurrentCardKind = draw.archKind;
			// A see-through arch is drawn over the player at 50%: keep those pixels lit as the card underneath rather
			// than restamping them as a wall face, which shaded the player's head and shoulders like the wall.
			gbuffer::CurrentArchBlended = draw.blended;
		} else {
			gbuffer::CurrentCardKind = (draw.selfLit || draw.trn != nullptr) ? gbuffer::Kind::CardEmissive : gbuffer::Kind::Card;
		}
	}

	// Hybrid runs the same primitives with shadeIndex = 0: the sprite is written as a card and the
	// presenter lights it, so no CPU light table is applied, except the fully dark table above.
	const int shadeIndex = hybrid && !fullyDark ? 0 : draw.shadeIndex;

	// Classic dispatch, bit-exact to the previous per-site calls.
	if (draw.trn != nullptr) {
		if (draw.blended)
			ClxDrawBlendedTRN(out, draw.feet, draw.sprite, draw.trn);
		else
			ClxDrawTRN(out, draw.feet, draw.sprite, draw.trn);
	} else if (shadeIndex != 0) {
		if (draw.blended)
			ClxDrawBlendedTRN(out, draw.feet, draw.sprite, LightTables[shadeIndex].data());
		else
			ClxDrawTRN(out, draw.feet, draw.sprite, LightTables[shadeIndex].data());
	} else if (draw.blended) {
		ClxDrawBlended(out, draw.feet, draw.sprite);
	} else {
		ClxDraw(out, draw.feet, draw.sprite);
	}
}

gbuffer::LatchInput CurrentHybridLatchInput()
{
	return gbuffer::LatchInput {
		.optionEnabled = *GetOptions().Graphics.hybridLighting && !fpv::SkipIsometricWorld(),
		.presenterReady = WebGpuIsReady(),
		.headless = HeadlessMode,
		.gameRunning = gbRunGame,
		.level = leveltype,
		.logicalPalette = logical_palette.data(),
	};
}

void LatchHybridWorldFrame(const gbuffer::LatchInput &in)
{
	gbuffer::LatchForFrame(in);
	if (!gbuffer::Enabled())
		return;
	// The on-demand generation budget and the hit/miss counters are per game frame: without this reset
	// the first 8 generations of the session would exhaust the budget for good and every later miss would wait for Pump.
	clx_normal_map::ResetFrameStats();
	clx_normal_map::SetOptions(clx_normal_map::Options { .generate = *GetOptions().Graphics.hybridNormalMaps });
	clx_normal_map::Pump(1500);
}

void LatchHybridRepresentFrame(const gbuffer::LatchInput &in)
{
	gbuffer::LatchForFrame(in);
	// No world redraw here, but the cursor and DrawMain primitives still draw: they must stamp Unlit. EndWorldPass arms the
	// stamp whenever the back buffer holds a hybrid frame, also right after the option was switched off (it is still composited).
	gbuffer::EndWorldPass();
}

void PrewarmHybridSpriteNormals(bool presenterReady)
{
	if (!presenterReady || HeadlessMode || leveltype == DTYPE_TOWN)
		return;
	if (!*GetOptions().Graphics.hybridLighting || !*GetOptions().Graphics.hybridNormalMaps)
		return;

	// EnqueuePrewarm stops accepting frames once the projected packed bytes reach the cache bound and reports the
	// rest in Stats::prewarmDropped; everything after that point would be dropped too, so stop enqueuing.
	const size_t droppedBefore = clx_normal_map::GetStats().prewarmDropped;
	const auto bounded = [droppedBefore]() {
		return clx_normal_map::GetStats().prewarmDropped > droppedBefore;
	};
	const auto enqueuePlayerAnimation = [](const Player &player, player_graphic graphic) {
		const OptionalOwnedClxSpriteSheet &sprites = player.AnimationData[static_cast<size_t>(graphic)].sprites;
		if (sprites)
			clx_normal_map::EnqueuePrewarm(ClxSpriteSheet { *sprites });
	};
	const auto enqueueMonsterAnimation = [](const CMonster &monsterType, MonsterGraphic graphic) {
		const OptionalClxSpriteListOrSheet &sprites = monsterType.getAnimData(graphic).sprites;
		if (!sprites)
			return;
		if (sprites->isSheet())
			clx_normal_map::EnqueuePrewarm(sprites->sheet());
		else
			clx_normal_map::EnqueuePrewarm(sprites->list());
	};

	// 1. Players: Stand and Walk.
	for (const Player &player : Players) {
		if (!player.plractive)
			continue;
		enqueuePlayerAnimation(player, player_graphic::Stand);
		enqueuePlayerAnimation(player, player_graphic::Walk);
		if (bounded())
			return;
	}
	// 2. Each level monster type: Stand then Walk (all directions).
	for (size_t i = 0; i < LevelMonsterTypeCount; ++i) {
		enqueueMonsterAnimation(LevelMonsterTypes[i], MonsterGraphic::Stand);
		enqueueMonsterAnimation(LevelMonsterTypes[i], MonsterGraphic::Walk);
		if (bounded())
			return;
	}
	// 3. The remaining player animations.
	for (const Player &player : Players) {
		if (!player.plractive)
			continue;
		for (const player_graphic graphic : { player_graphic::Attack, player_graphic::Hit, player_graphic::Lightning,
		         player_graphic::Fire, player_graphic::Magic, player_graphic::Death, player_graphic::Block }) {
			enqueuePlayerAnimation(player, graphic);
			if (bounded())
				return;
		}
	}
	// 4. The remaining monster animations.
	for (size_t i = 0; i < LevelMonsterTypeCount; ++i) {
		for (const MonsterGraphic graphic : { MonsterGraphic::Attack, MonsterGraphic::GotHit, MonsterGraphic::Death, MonsterGraphic::Special }) {
			enqueueMonsterAnimation(LevelMonsterTypes[i], graphic);
			if (bounded())
				return;
		}
	}
	// 5. Resident object sprite lists (deduplicated: many objects share one list) and the missile sprites.
	std::vector<const uint8_t *> enqueuedObjectLists;
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const OptionalClxSpriteList &sprites = Objects[ActiveObjects[i]]._oAnimData;
		if (!sprites || c_find(enqueuedObjectLists, sprites->data()) != enqueuedObjectLists.end())
			continue;
		enqueuedObjectLists.push_back(sprites->data());
		clx_normal_map::EnqueuePrewarm(*sprites);
		if (bounded())
			return;
	}
	// The loaded table can be shorter than the MissileGraphicID range (missile_sprites.tsv has fewer rows than the enum).
	const size_t missileSpriteCount = std::min(GetMissileSpriteDataCount(), static_cast<size_t>(MissileGraphicID::None));
	for (size_t i = 0; i < missileSpriteCount; ++i) {
		const OptionalOwnedClxSpriteListOrSheet &sprites = GetMissileSpriteData(static_cast<MissileGraphicID>(i)).sprites;
		if (!sprites)
			continue;
		if (sprites->isSheet())
			clx_normal_map::EnqueuePrewarm(sprites->sheet());
		else
			clx_normal_map::EnqueuePrewarm(sprites->list());
		if (bounded())
			return;
	}
}

Displacement GetOffsetForWalking(const AnimationInfo &animationInfo, const Direction dir, bool cameraMode /*= false*/)
{
	// clang-format off
	//                                           South,        SouthWest,    West,         NorthWest,    North,        NorthEast,     East,         SouthEast,
	constexpr Displacement MovingOffset[8]   = { {   0,  32 }, { -32,  16 }, { -64,   0 }, { -32, -16 }, {   0, -32 }, {  32, -16 },  {  64,   0 }, {  32,  16 } };
	// clang-format on

	const uint8_t animationProgress = animationInfo.getAnimationProgress();
	Displacement offset = MovingOffset[static_cast<size_t>(dir)];
	offset *= animationProgress;
	offset /= AnimationInfo::baseValueFraction;

	if (cameraMode) {
		offset = -offset;
	}

	return offset;
}

void ClearCursor() // CODE_FIX: this was supposed to be in cursor.cpp
{
	PrevCursorRect = {};
}

void ShiftGrid(Point *offset, int horizontal, int vertical)
{
	offset->x += vertical + horizontal;
	offset->y += vertical - horizontal;
}

int RowsCoveredByPanel()
{
	const auto &mainPanelSize = GetMainPanel().size;
	if (GetScreenWidth() <= mainPanelSize.width) {
		return 0;
	}

	return GetMainPanelScreenHeight() * 4 / (TILE_HEIGHT * GetZoomQuarters());
}

void CalcTileOffset(int *offsetX, int *offsetY)
{
	const uint16_t screenWidth = GetScreenWidth();
	const uint16_t viewportHeight = GetViewportHeight();

	int x;
	int y;

	x = ScreenToWorldLength(screenWidth) % TILE_WIDTH;
	y = ScreenToWorldLength(viewportHeight) % TILE_HEIGHT;

	if (x != 0)
		x = (TILE_WIDTH - x) / 2;
	if (y != 0)
		y = (TILE_HEIGHT - y) / 2;

	*offsetX = x;
	*offsetY = y;
}

void TilesInView(int *rcolumns, int *rrows)
{
	const int screenWidth = GetScreenWidth();
	const int viewportHeight = GetViewportHeight();

	// The tiles covering the view at the zoom, rounded up: ceil(4 w / (q TILE_WIDTH)) (for a whole level k the same as
	// ceil(ceil(w / TILE_WIDTH) / k))
	const int zoomQuarters = GetZoomQuarters();
	*rcolumns = (screenWidth * 4 + zoomQuarters * TILE_WIDTH - 1) / (zoomQuarters * TILE_WIDTH);
	*rrows = (viewportHeight * 4 + zoomQuarters * TILE_HEIGHT - 1) / (zoomQuarters * TILE_HEIGHT);
}

void CalcViewportGeometry()
{
	GeometryZoomQuarters = GetZoomQuarters();
	const int screenWidth = ScreenToWorldLength(GetScreenWidth());
	const int screenHeight = ScreenToWorldLength(GetScreenHeight());
	const int panelHeight = ScreenToWorldLength(GetMainPanelScreenHeight());
	const int pixelsToPanel = screenHeight - panelHeight;
	Point playerPosition { screenWidth / 2, pixelsToPanel / 2 };

	if (GeometryZoomQuarters > 4)
		playerPosition.y += TILE_HEIGHT / 4;

	const int tilesToTop = (playerPosition.y + TILE_HEIGHT - 1) / TILE_HEIGHT;
	const int tilesToLeft = (playerPosition.x + TILE_WIDTH - 1) / TILE_WIDTH;

	// Location of the center of the tile from which to start rendering, relative to the viewport origin
	Point startPosition = playerPosition - Displacement { tilesToLeft * TILE_WIDTH, tilesToTop * TILE_HEIGHT };

	// Position of the tile from which to start rendering in tile space,
	// relative to the tile the player character occupies
	tileShift = { 0, 0 };
	tileShift += Displacement(Direction::North) * tilesToTop;
	tileShift += Displacement(Direction::West) * tilesToLeft;

	// The rendering loop expects to start on a row with fewer columns
	if (tilesToLeft * TILE_WIDTH >= playerPosition.x) {
		startPosition += Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
		tileShift += Displacement(Direction::NorthEast);
	} else if (tilesToTop * TILE_HEIGHT < playerPosition.y) {
		// There is one row above the current row that needs to be rendered,
		// but we skip to the row above it because it has too many columns
		startPosition += Displacement { 0, -TILE_HEIGHT };
		tileShift += Displacement(Direction::North);
	}

	// Location of the bottom-left corner of the bounding box around the
	// tile from which to start rendering, relative to the viewport origin
	tileOffset = { startPosition.x - (TILE_WIDTH / 2), startPosition.y + (TILE_HEIGHT / 2) - 1 };

	// Compute the number of rows to be rendered as well as
	// the number of columns to be rendered in the first row
	const int viewportHeight = ScreenToWorldLength(GetViewportHeight());
	const Point renderStart = startPosition - Displacement { TILE_WIDTH / 2, TILE_HEIGHT / 2 };
	tileRows = (viewportHeight - renderStart.y + TILE_HEIGHT / 2 - 1) / (TILE_HEIGHT / 2);
	tileColumns = (screenWidth - renderStart.x + TILE_WIDTH - 1) / TILE_WIDTH;
}

bool FractionalZoomAvailable()
{
	if (FractionalZoomOverride.has_value())
		return *FractionalZoomOverride;
	return HdTilesWantedForLevel() && HdSpritesGpuAvailable();
}

void SetFractionalZoomForTesting(std::optional<bool> available)
{
	FractionalZoomOverride = available;
}

int GetZoomQuarters()
{
	const int stored = *GetOptions().Graphics.zoomLevel;
	if (stored % 4 == 0 || FractionalZoomAvailable())
		return stored;
	return (stored + 2) / 4 * 4; // nearest whole level, halves up
}

int ScreenToWorldLength(int screenLength)
{
	return screenLength * 4 / GetZoomQuarters();
}

HdWorldTransform GetViewTransform()
{
	const ZoomGeometry geometry = CalcZoomGeometry(GetScreenWidth());
	return ComputeHdWorldTransform(GetZoomQuarters(), GetScreenWidth(), GetViewportHeight(), geometry.viewportWidth, geometry.viewportOffsetX);
}

void StepZoomLevel(int steps)
{
	OptionEntryZoomLevel &zoomLevel = GetOptions().Graphics.zoomLevel;
	if (FractionalZoomAvailable())
		zoomLevel.SetValue(*zoomLevel + steps);
	else
		zoomLevel.SetValue(GetZoomQuarters() + 4 * steps);
	CalcViewportGeometry();
}

void ToggleZoom()
{
	GetOptions().Graphics.zoomLevel.SetValue(GetZoomQuarters() > 4 ? 4 : 8);
	CalcViewportGeometry();
}

Point GetScreenPosition(Point tile)
{
	Point firstTile = ViewPosition;
	Displacement offset = {};
	CalcFirstTilePosition(firstTile, offset);

	const Displacement delta = firstTile - tile;

	Point position {};
	position += delta.worldToScreen();
	position += offset;
	return position;
}

extern SDL_Surface *PalSurface;

void ClearScreenBuffer()
{
	// The back buffer no longer holds the world pass (loading screens, after in-game movies): present it classic.
	gbuffer::InvalidateBackBuffer();
	HdSpritesInvalidate();
	HdTilesInvalidate();
	HdUiInvalidate();
	InvalidateUiLayer();
#if DEVILUTIONX_WEBGPU
	HdFloorFrameUsesValid = false; // a level load follows: rebuild the HD floor's plain-frame table from the new dPiece
	TownSolidFloorWritten.clear();
#endif

	if (HeadlessMode)
		return;

	assert(PalSurface != nullptr);
	SDL_FillSurfaceRect(PalSurface, nullptr, 0);
}

#ifdef _DEBUG
void ScrollView()
{
	if (!MyPlayer->HoldItem.isEmpty())
		return;

	if (MousePosition.x < 20) {
		if (dmaxPosition.y - 1 <= ViewPosition.y || dminPosition.x >= ViewPosition.x) {
			if (dmaxPosition.y - 1 > ViewPosition.y) {
				ViewPosition.y++;
			}
			if (dminPosition.x < ViewPosition.x) {
				ViewPosition.x--;
			}
		} else {
			ViewPosition.y++;
			ViewPosition.x--;
		}
	}
	if (MousePosition.x > GetUiWidth() - 20) {
		if (dmaxPosition.x - 1 <= ViewPosition.x || dminPosition.y >= ViewPosition.y) {
			if (dmaxPosition.x - 1 > ViewPosition.x) {
				ViewPosition.x++;
			}
			if (dminPosition.y < ViewPosition.y) {
				ViewPosition.y--;
			}
		} else {
			ViewPosition.y--;
			ViewPosition.x++;
		}
	}
	if (MousePosition.y < 20) {
		if (dminPosition.y >= ViewPosition.y || dminPosition.x >= ViewPosition.x) {
			if (dminPosition.y < ViewPosition.y) {
				ViewPosition.y--;
			}
			if (dminPosition.x < ViewPosition.x) {
				ViewPosition.x--;
			}
		} else {
			ViewPosition.x--;
			ViewPosition.y--;
		}
	}
	if (MousePosition.y > GetUiHeight() - 20) {
		if (dmaxPosition.y - 1 <= ViewPosition.y || dmaxPosition.x - 1 <= ViewPosition.x) {
			if (dmaxPosition.y - 1 > ViewPosition.y) {
				ViewPosition.y++;
			}
			if (dmaxPosition.x - 1 > ViewPosition.x) {
				ViewPosition.x++;
			}
		} else {
			ViewPosition.x++;
			ViewPosition.y++;
		}
	}
}
#endif

void EnableFrameCount()
{
	frameflag = true;
	lastFpsUpdateInMs = SDL_GetTicks();
}

void scrollrt_draw_game_screen()
{
	if (HeadlessMode)
		return;

	LatchHybridRepresentFrame(CurrentHybridLatchInput());

	int hgt = 0;

	if (IsRedrawEverything()) {
		RedrawComplete();
		hgt = gnScreenHeight;
	}

	const Surface &out = GlobalBackBuffer();
	if (IsUiScaled()) {
		// Only the cursor changes: redraw it in the UI layer and scale that over the world it began with.
		bool resumed = false;
		const Surface ui = ResumeUiLayer(out, resumed);
		if (resumed)
			UndrawCursor(ui);
		DrawCursor(ui);
		CompositeUiLayer(out, /*restore=*/true);
		hgt = gnScreenHeight;
	} else {
		UndrawCursor(out);
		DrawCursor(out);
	}
	DrawMain(hgt, false, false, false, false, false);

	RenderPresent();
}

void DrawAndBlit()
{
	if (!gbRunGame || HeadlessMode) {
		return;
	}

	LatchHybridWorldFrame(CurrentHybridLatchInput());

	int hgt = 0;
	bool drawHealth = IsRedrawComponent(PanelDrawComponent::Health);
	bool drawMana = IsRedrawComponent(PanelDrawComponent::Mana);
	bool drawControlButtons = IsRedrawComponent(PanelDrawComponent::ControlButtons);
	bool drawBelt = IsRedrawComponent(PanelDrawComponent::Belt);
	const bool drawChatInput = ChatFlag;
	bool drawInfoBox = false;
	bool drawCtrlPan = false;

	const Rectangle &mainPanel = GetMainPanel();

	if (GetUiWidth() > mainPanel.size.width || IsRedrawEverything()) {
		drawHealth = true;
		drawMana = true;
		drawControlButtons = true;
		drawBelt = true;
		drawInfoBox = false;
		drawCtrlPan = true;
		hgt = gnScreenHeight;
	} else if (IsRedrawViewport()) {
		drawInfoBox = true;
		drawCtrlPan = false;
		hgt = gnViewportHeight;
	}

	const Surface &out = GlobalBackBuffer();
	// A scaled UI starts a new layer every frame (the cursor goes with it); the back buffer is redrawn completely then.
	if (!IsUiScaled())
		UndrawCursor(out);

	nthread_UpdateProgressToNextGameTick();
	fpv::UpdateFrame(); // first-person camera for this frame (reads the interpolated player position)

	const Surface ui = DrawView(out, ViewPosition);
	if (drawCtrlPan) {
		DrawMainPanel(ui);
	}
	if (drawHealth) {
		DrawLifeFlaskLower(ui, !drawCtrlPan);
	}
	if (drawMana) {
		DrawManaFlaskLower(ui, !drawCtrlPan);

		DrawSpell(ui);
	}
	if (drawControlButtons) {
		DrawMainPanelButtons(ui);
	}
	if (drawBelt) {
		DrawInvBelt(ui);
	}
	if (drawChatInput) {
		DrawChatBox(ui);
	}
	DrawXPBar(ui);
	DrawRunControl(ui);
	if (*GetOptions().Gameplay.showHealthValues)
		DrawFlaskValues(ui, { mainPanel.position.x + 134, mainPanel.position.y + 28 }, MyPlayer->_pHitPoints >> 6, MyPlayer->_pMaxHP >> 6);
	if (*GetOptions().Gameplay.showManaValues)
		DrawFlaskValues(ui, { mainPanel.position.x + mainPanel.size.width - 138, mainPanel.position.y + 28 },
		    (HasAnyOf(InspectPlayer->_pIFlags, ItemSpecialEffect::NoMana) || MyPlayer->hasNoMana()) ? 0 : MyPlayer->_pMana >> 6,
		    HasAnyOf(InspectPlayer->_pIFlags, ItemSpecialEffect::NoMana) ? 0 : MyPlayer->_pMaxMana >> 6);
	if (*GetOptions().Gameplay.floatingInfoBox)
		DrawFloatingInfoBox(ui);

	if (*GetOptions().Gameplay.showMultiplayerPartyInfo && PartySidePanelOpen)
		DrawPartyMemberInfoPanel(ui);

	DrawCursor(ui);

	DrawFPS(ui);

	CompositeUiLayer(out, /*restore=*/false);


	lua::GameDrawComplete();

	DrawMain(hgt, drawInfoBox, drawHealth, drawMana, drawBelt, drawControlButtons);

#ifdef _DEBUG
	DrawConsole(out);
#endif

	RedrawComplete();
	for (const PanelDrawComponent component : enum_values<PanelDrawComponent>()) {
		if (IsRedrawComponent(component)) {
			RedrawComponentComplete(component);
		}
	}

	RenderPresent();
}

} // namespace devilution
