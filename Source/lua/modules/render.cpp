#include "lua/modules/render.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <sol/sol.hpp>

#include "DiabloUI/ui_flags.hpp"
#include "engine/dx.h"
#include "engine/render/text_render.hpp"
#include "lua/metadoc.hpp"
#include "utils/display.h"

namespace devilution {
namespace {

struct PresentationLight {
	float x = 0;
	float y = 0;
	float z = 0;
};

std::unordered_map<std::uint32_t, PresentationLight> presentationLights;
std::uint32_t nextPresentationLightId = 1;

} // namespace

sol::table LuaRenderModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "string", "(text: string, x: integer, y: integer)",
	    "Renders a string at the given coordinates",
	    [](std::string_view text, int x, int y) { DrawString(GlobalBackBuffer(), text, { x, y }); });
	LuaSetDocFn(table, "screen_width", "()",
	    "Returns the screen width", []() { return gnScreenWidth; });
	LuaSetDocFn(table, "screen_height", "()",
	    "Returns the screen height", []() { return gnScreenHeight; });

	auto light = lua.create_table();
	LuaSetDocFn(light, "create", "(descriptor: table)", "Create a validated presentation-only light.", [](const sol::table &descriptor) {
		const auto radius = descriptor["radius"].get_or(0.0F);
		const auto intensity = descriptor["intensity"].get_or(0.0F);
		if (!std::isfinite(radius) || radius <= 0 || radius > 128) throw sol::error("render.light: radius out of range");
		if (!std::isfinite(intensity) || intensity < 0 || intensity > 100) throw sol::error("render.light: intensity out of range");
		const auto material = descriptor["material"].get_or(std::string {});
		const auto channel = descriptor["channel"].get_or(descriptor["previewChannel"].get_or(std::string {}));
		if (material != "torch_flame" && material != "spell_glow") throw sol::error("render.light: unsupported material '" + material + "'");
		if (channel != "emissive" && channel != "albedo" && channel != "normal" && channel != "height" && channel != "occlusion") throw sol::error("render.light: missing preview channel '" + channel + "'");
		const auto id = nextPresentationLightId++;
		presentationLights.emplace(id, PresentationLight { descriptor["x"].get_or(0.0F), descriptor["y"].get_or(0.0F), descriptor["z"].get_or(0.0F) });
		return id;
	});
	LuaSetDocFn(light, "update", "(id: integer, update: table)", "Update a moving presentation-only light.", [](std::uint32_t id, const sol::table &update) {
		const auto it = presentationLights.find(id);
		if (it == presentationLights.end()) throw sol::error("render.light: stale or expired light id");
		it->second.x = update["x"].get_or(it->second.x);
		it->second.y = update["y"].get_or(it->second.y);
		it->second.z = update["z"].get_or(it->second.z);
	});
	table["light"] = light;

	auto uiFlags = lua.create_table();
	uiFlags["None"] = UiFlags::None;

	uiFlags["FontSize12"] = UiFlags::FontSize12;
	uiFlags["FontSize24"] = UiFlags::FontSize24;
	uiFlags["FontSize30"] = UiFlags::FontSize30;
	uiFlags["FontSize42"] = UiFlags::FontSize42;
	uiFlags["FontSize46"] = UiFlags::FontSize46;
	uiFlags["FontSizeDialog"] = UiFlags::FontSizeDialog;

	uiFlags["ColorUiGold"] = UiFlags::ColorUiGold;
	uiFlags["ColorUiSilver"] = UiFlags::ColorUiSilver;
	uiFlags["ColorUiGoldDark"] = UiFlags::ColorUiGoldDark;
	uiFlags["ColorUiSilverDark"] = UiFlags::ColorUiSilverDark;
	uiFlags["ColorDialogWhite"] = UiFlags::ColorDialogWhite;
	uiFlags["ColorDialogYellow"] = UiFlags::ColorDialogYellow;
	uiFlags["ColorDialogRed"] = UiFlags::ColorDialogRed;
	uiFlags["ColorYellow"] = UiFlags::ColorYellow;
	uiFlags["ColorGold"] = UiFlags::ColorGold;
	uiFlags["ColorBlack"] = UiFlags::ColorBlack;
	uiFlags["ColorWhite"] = UiFlags::ColorWhite;
	uiFlags["ColorWhitegold"] = UiFlags::ColorWhitegold;
	uiFlags["ColorRed"] = UiFlags::ColorRed;
	uiFlags["ColorBlue"] = UiFlags::ColorBlue;
	uiFlags["ColorOrange"] = UiFlags::ColorOrange;
	uiFlags["ColorButtonface"] = UiFlags::ColorButtonface;
	uiFlags["ColorButtonpushed"] = UiFlags::ColorButtonpushed;

	uiFlags["AlignCenter"] = UiFlags::AlignCenter;
	uiFlags["AlignRight"] = UiFlags::AlignRight;
	uiFlags["VerticalCenter"] = UiFlags::VerticalCenter;

	uiFlags["KerningFitSpacing"] = UiFlags::KerningFitSpacing;

	uiFlags["ElementDisabled"] = UiFlags::ElementDisabled;
	uiFlags["ElementHidden"] = UiFlags::ElementHidden;

	uiFlags["PentaCursor"] = UiFlags::PentaCursor;
	uiFlags["Outlined"] = UiFlags::Outlined;

	uiFlags["NeedsNextElement"] = UiFlags::NeedsNextElement;

	table["UiFlags"] = uiFlags;

	return table;
}

} // namespace devilution
