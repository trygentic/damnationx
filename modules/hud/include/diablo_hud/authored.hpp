// Extracted authored HUD table from Diablo3D / DevilutionX. See LICENSE.md / NOTICE.md.
#pragma once
#include "types.hpp"
#include <algorithm>
#include <array>
#include <string_view>
namespace diablo_hud {
struct AuthoredEntry {
	std::string_view name;
	bool rightAnchor;
	int x, y, width, height;
	bool utility = false;
};

// Logical pixels, identical to the editable Control nodes in ui/hud.tscn.
inline constexpr std::array<AuthoredEntry, 13> AuthoredEntries {{
	{ "HudHealthOrb", false, -226, -122, 88, 113 },
	{ "HudManaOrb", false, 138, -122, 88, 113 },
	{ "HudBelt", false, -116, -103, 232, 29 },
	{ "HudSpell", false, 252, -58, 44, 44 },
	{ "HudInfo", false, -132, -72, 264, 64 },
	{ "HudCharacter", false, -310, -106, 71, 20, true },
	{ "HudInventory", false, 239, -106, 71, 20, true },
	{ "HudSpellbook", false, 239, -82, 71, 20, true },
	{ "HudQuests", false, -310, -82, 71, 20, true },
	{ "HudMap", false, -310, -54, 71, 20, true },
	{ "HudMenu", false, -310, -30, 71, 20, true },
	{ "HudChat", false, -199, -150, 33, 24, true },
	{ "HudFriendly", false, 166, -150, 33, 24, true },
}};

inline int safeWidth(int width, int height)
{
	return std::min(width, height * 16 / 9);
}

inline Rect defaultRect(const AuthoredEntry &entry, int safeWidth, int height)
{
	return { { (entry.rightAnchor ? safeWidth : safeWidth / 2) + entry.x, height + entry.y }, { entry.width, entry.height } };
}

inline Rect placeRect(Rect rect, const AuthoredEntry &, int width, int height)
{
	const int safe = safeWidth(width, height);
	rect.size.width = std::clamp(rect.size.width, 1, std::max(1, safe));
	rect.size.height = std::clamp(rect.size.height, 1, std::max(1, height));
	rect.position.x = std::clamp(rect.position.x, 0, safe - rect.size.width) + (width - safe) / 2;
	rect.position.y = std::clamp(rect.position.y, 0, height - rect.size.height);
	return rect;
}

inline Rect scaleRect(Rect rect, int height)
{
	// Scale edges together so adjacent cells share their boundary exactly.
	const int left = rect.position.x * height / 480;
	const int top = rect.position.y * height / 480;
	const int right = (rect.position.x + rect.size.width) * height / 480;
	const int bottom = (rect.position.y + rect.size.height) * height / 480;
	return { { left, top }, { std::max(1, right - left), std::max(1, bottom - top) } };
}


inline std::array<Rect,13> authoredDefaults(const Context&c){std::array<Rect,13> result{};const int width=c.logicalWidth*480/std::max(1,c.logicalHeight);const int safe=safeWidth(width,480);for(std::size_t i=0;i<result.size();++i)result[i]=scaleRect(placeRect(defaultRect(AuthoredEntries[i],safe,480),AuthoredEntries[i],width,480),c.logicalHeight);return result;}
}
