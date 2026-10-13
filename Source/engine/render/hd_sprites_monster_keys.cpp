#include "engine/render/hd_sprites_monster_keys.hpp"

#include <algorithm>
#include <array>
#include <functional>

#include "engine/render/hd_sprites_core.hpp"

namespace devilution::hd_sprites {

namespace {

std::string Lowercase(std::string_view text)
{
	std::string result(text);
	for (char &c : result) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return result;
}

constexpr std::string_view PathSeparators = "\\/";

} // namespace

std::string MonsterGfxName(std::string_view spritePath)
{
	const size_t separator = spritePath.find_first_of(PathSeparators);
	return Lowercase(spritePath.substr(0, separator));
}

std::string MonsterTrnVariant(std::string_view trnPath)
{
	const size_t separator = trnPath.find_last_of(PathSeparators);
	if (separator != std::string_view::npos)
		trnPath.remove_prefix(separator + 1);
	std::string stem = Lowercase(trnPath);
	if (stem.size() > 4 && stem.ends_with(".trn"))
		stem.resize(stem.size() - 4);
	return stem;
}

std::string MonsterUniqueVariant(std::string_view uniqueTrnName)
{
	const std::string stem = MonsterTrnVariant(uniqueTrnName);
	return stem.empty() ? std::string {} : "u_" + stem;
}

std::string_view MonsterAnimName(int graphic)
{
	static constexpr std::array<std::string_view, 6> Names = { "stand", "walk", "attack", "hit", "death", "special" };
	if (graphic < 0 || graphic >= static_cast<int>(Names.size()))
		return {};
	return Names[static_cast<size_t>(graphic)];
}

std::string MonsterClipKey(std::string_view gfx, std::string_view variant, int graphic, int direction)
{
	const std::string_view anim = MonsterAnimName(graphic);
	const std::string_view dir = DirectionSuffix(direction);
	if (gfx.empty() || anim.empty() || dir.empty())
		return {};
	std::string key;
	key.reserve(8 + gfx.size() + 1 + variant.size() + 1 + anim.size() + 1 + dir.size());
	key.append("monster/").append(gfx);
	if (!variant.empty())
		key.append("+").append(variant);
	key.append("/").append(anim).append("/").append(dir);
	return key;
}

std::vector<std::string> MonsterLooks(const SpriteIndex &index)
{
	constexpr std::string_view Prefix = "monster/";
	std::vector<std::string> looks;
	for (const ClipInfo &clip : index.clips) {
		const std::string_view key = clip.key;
		if (!key.starts_with(Prefix))
			continue;
		const size_t end = key.find('/', Prefix.size());
		if (end == std::string_view::npos || end == Prefix.size())
			continue;
		looks.emplace_back(key.substr(Prefix.size(), end - Prefix.size()));
	}
	std::sort(looks.begin(), looks.end());
	looks.erase(std::unique(looks.begin(), looks.end()), looks.end());
	return looks;
}

MonsterLookChoice ChooseMonsterLook(std::span<const std::string> looks, std::string_view gfx, std::span<const std::string> wantedVariants)
{
	MonsterLookChoice choice;
	if (gfx.empty())
		return choice;
	const auto has = [&looks](std::string_view look) { return std::binary_search(looks.begin(), looks.end(), look, std::less<>()); };
	const auto lookName = [gfx](std::string_view variant) {
		std::string look(gfx);
		if (!variant.empty())
			look.append("+").append(variant);
		return look;
	};
	bool first = true;
	for (const std::string &variant : wantedVariants) {
		std::string look = lookName(variant);
		if (has(look)) {
			choice.look = std::move(look);
			choice.exact = first;
			return choice;
		}
		first = false;
	}
	if (std::string base = lookName({}); has(base)) {
		choice.look = std::move(base);
		choice.exact = wantedVariants.empty();
		return choice;
	}
	// Any rendered recolour of the set (sorted, so the choice is stable).
	const std::string variantPrefix = lookName({}) + "+";
	const auto it = std::lower_bound(looks.begin(), looks.end(), variantPrefix);
	if (it != looks.end() && it->starts_with(variantPrefix))
		choice.look = *it;
	return choice;
}

} // namespace devilution::hd_sprites
