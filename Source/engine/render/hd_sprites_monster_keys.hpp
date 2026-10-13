/**
 * @file hd_sprites_monster_keys.hpp
 *
 * Engine-independent naming of the HD monster clips (`monster/` keys):
 *
 *     monster/<gfx>/<anim>/<dir>           base look of a monster graphics set
 *     monster/<gfx>+<variant>/<anim>/<dir> TRN-recoloured look
 *
 * <gfx> is the monster graphics folder (first component of the monstdat sprite path, e.g. `zombie` for `zombie\zombie`,
 * `falspear` for `falspear\phall`). <anim> follows MonsterGraphic: stand, walk, attack, hit, death, special.
 * <dir> is the contract direction suffix (s sw w nw n ne e se). <variant> is the stem of the TRN file the classic
 * renderer recolours with: the monster type's trnFile (e.g. `bluered` for `zombie\bluered`, the Ghoul) or, for a
 * unique monster, `u_` + its unique TRN name (e.g. `u_genrl`, the Skeleton King).
 *
 * Covered by native unit tests (test/hd_sprites_monster_test.cpp).
 */
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devilution::hd_sprites {

/** Graphics folder of a monstdat sprite path (`zombie\zombie` -> `zombie`), lowercased; empty for an empty path. */
std::string MonsterGfxName(std::string_view spritePath);

/** TRN variant name of a TRN path or name (`zombie\bluered` / `monsters/zombie/bluered.trn` -> `bluered`), lowercased. */
std::string MonsterTrnVariant(std::string_view trnPath);

/** Variant name of a unique monster's TRN (`genrl` -> `u_genrl`); empty for an empty name. */
std::string MonsterUniqueVariant(std::string_view uniqueTrnName);

/** Contract animation name for a MonsterGraphic value (Stand..Special = 0..5); empty for anything else. */
std::string_view MonsterAnimName(int graphic);

/**
 * @brief Clip key `monster/<gfx>[+<variant>]/<anim>/<dir>`.
 * @param direction engine Direction order South..SouthEast (0..7)
 * @return empty when any part is unknown
 */
std::string MonsterClipKey(std::string_view gfx, std::string_view variant, int graphic, int direction);

struct SpriteIndex;

/** The monster looks (`<gfx>` / `<gfx>+<variant>`, the second key component) with clips in the index, sorted. */
std::vector<std::string> MonsterLooks(const SpriteIndex &index);

/** Which look draws a monster (see ChooseMonsterLook). */
struct MonsterLookChoice {
	std::string look;   ///< `<gfx>` or `<gfx>+<variant>`; empty: the graphics set has no HD art at all
	bool exact = false; ///< the first wanted variant itself (no recolour stand-in)

	/** Key prefix of the look's clips: `monster/<look>/`. */
	[[nodiscard]] std::string prefix() const { return look.empty() ? std::string {} : "monster/" + look + "/"; }
};

/**
 * @brief The look to draw a monster of graphics set `gfx` with, given the recolours it wants, best first (the unique
 * monster's `u_<trn>`, then the type's TRN variant; an empty variant is the base look).
 *
 * A wanted recolour without its own clips (a unique monster's TRN, a TRN variant the pack does not render) must not
 * send the monster back to its classic sprite: it takes the next wanted variant, else the base look, else any
 * rendered variant of the set (e.g. the Skeleton King's only look `sking+white`). Only a graphics set without any
 * clips yields an empty look (classic). `looks` is MonsterLooks().
 */
MonsterLookChoice ChooseMonsterLook(std::span<const std::string> looks, std::string_view gfx, std::span<const std::string> wantedVariants);

} // namespace devilution::hd_sprites
