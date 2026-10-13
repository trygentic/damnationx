/**
 * @file fpv/fpv_hud.cpp
 *
 * First-person HUD drawn into the 8-bit UI layer after the world and before the
 * panels: crosshair, "E: name" prompt, target name / health at the top, a
 * procedural crushed-pixel melee slash, a hit marker, the help strip and the
 * level banner. Only existing text and primitive renderers are used.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "control/control.hpp"
#include "cursor.h"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/render/ui_scale.hpp"
#include "fpv/fpv.hpp"
#include "fpv/fpv_state.hpp"
#include "fpv/fpv_viewmodel_inputs.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "monster.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "towners.h"
#include "utils/format_int.hpp"
#include "utils/language.h"
#include "utils/str_cat.hpp"

namespace devilution::fpv {

namespace {

constexpr uint8_t ColorLight = PAL16_GRAY + 1;
constexpr uint8_t ColorMid = PAL16_GRAY + 5;
constexpr uint8_t ColorDim = PAL16_GRAY + 9;
constexpr uint8_t ColorShadow = PAL16_GRAY + 15;
constexpr uint8_t ColorAttack = PAL16_RED + 2;

void PutBlock(const Surface &out, int x, int y, int size, uint8_t color)
{
	const int x0 = std::max(x, 0);
	const int y0 = std::max(y, 0);
	const int x1 = std::min(x + size, out.w());
	const int y1 = std::min(y + size, out.h());
	if (x1 <= x0 || y1 <= y0)
		return;
	FillRect(out, x0, y0, x1 - x0, y1 - y0, color);
}

int UiUnit(const Surface &ui)
{
	return std::max(1, ui.h() / 480);
}

void DrawCrosshair(const Surface &ui, Point c, bool attack)
{
	const int unit = UiUnit(ui);
	const int gap = 2 * unit;
	const int arm = 4 * unit;
	const uint8_t color = attack ? ColorAttack : ColorLight;
	const auto drawArms = [&](Point centre, uint8_t col) {
		FillRect(ui, centre.x - gap - arm, centre.y, arm, unit, col);
		FillRect(ui, centre.x + gap + unit, centre.y, arm, unit, col);
		FillRect(ui, centre.x, centre.y - gap - arm, unit, arm, col);
		FillRect(ui, centre.x, centre.y + gap + unit, unit, arm, col);
		FillRect(ui, centre.x, centre.y, unit, unit, col);
	};
	drawArms(c + Displacement { unit, unit }, ColorShadow);
	drawArms(c, color);
}

void DrawHitMarker(const Surface &ui, Point c)
{
	const int unit = UiUnit(ui);
	for (const int sx : { -1, 1 }) {
		for (const int sy : { -1, 1 }) {
			for (int i = 5; i <= 9; ++i) {
				PutBlock(ui, c.x + sx * i * unit, c.y + sy * i * unit, unit, ColorLight);
				PutBlock(ui, c.x + sx * i * unit + unit, c.y + sy * i * unit + unit, unit, ColorShadow);
			}
		}
	}
}

/**
 * Crushed retro melee slash: a crescent band of big square "pixels" sweeping
 * across the lower middle of the view, alternating direction every swing.
 */
void DrawSlash(const Surface &ui, Point centre, int viewHeight, uint32_t elapsedMs, bool mirrored)
{
	constexpr float Duration = 300.0F;
	const float p = static_cast<float>(elapsedMs) / Duration;
	if (p >= 1.0F)
		return;

	const float h = static_cast<float>(viewHeight);
	const float w = static_cast<float>(ui.w());
	const int block = std::max(2, viewHeight / 110);
	const float side = mirrored ? -1.0F : 1.0F;
	// Unmirrored the arc centre sits up and to the right; the swing runs right -> left under the crosshair.
	const float cx = static_cast<float>(centre.x) + 0.06F * w;
	const float cy = static_cast<float>(centre.y) - 0.18F * h;
	const float radius = 0.42F * h;
	const float thickness = 0.11F * h;
	constexpr float AngleStart = 15.0F;
	constexpr float AngleEnd = 165.0F;

	const auto ease = [](float v) { return 1.0F - (1.0F - v) * (1.0F - v); };
	const float lead = AngleStart + (AngleEnd - AngleStart) * ease(std::min(1.0F, p * 1.6F));
	const float trail = AngleStart + (AngleEnd - AngleStart) * std::clamp((p - 0.3F) * 1.45F, 0.0F, 1.0F);
	const bool fading = p > 0.65F;

	const int xMin = static_cast<int>(cx - radius - thickness);
	const int xMax = static_cast<int>(cx + radius + thickness);
	const int yMin = static_cast<int>(cy);
	const int yMax = static_cast<int>(cy + radius + thickness);
	for (int by = yMin - (yMin % block); by <= yMax; by += block) {
		for (int bx = xMin - (xMin % block); bx <= xMax; bx += block) {
			const float dx = static_cast<float>(bx) + block * 0.5F - cx;
			const float dy = static_cast<float>(by) + block * 0.5F - cy;
			const float angle = std::atan2(dy, dx) * 180.0F / kPi;
			if (angle < trail || angle > lead)
				continue;
			const float u = (angle - AngleStart) / (AngleEnd - AngleStart);
			const float band = thickness * (0.2F + 0.8F * std::sin(kPi * u));
			const float off = std::hypot(dx, dy) - radius;
			if (std::abs(off) > band * 0.5F)
				continue;
			const int gx = bx / block;
			const int gy = by / block;
			// Thin out the tail and the whole band while it fades: ordered dither keeps it chunky.
			const bool tail = angle - trail < (lead - trail) * 0.25F;
			if ((fading || tail) && ((gx + gy) & 1) != 0)
				continue;
			const float core = std::abs(off + band * 0.15F) / (band * 0.5F);
			const uint8_t color = core < 0.35F ? ColorLight : (core < 0.7F ? ColorMid : ColorDim);
			const int px = static_cast<int>(static_cast<float>(centre.x) + side * (static_cast<float>(bx) - static_cast<float>(centre.x)));
			PutBlock(ui, mirrored ? px - block : px, by, block, color);
		}
	}
}

struct TargetText {
	std::string text;
	UiFlags color = UiFlags::ColorWhite;
};

TargetText TargetName(const CrosshairTarget &target)
{
	TargetText result;
	switch (target.kind) {
	case TargetKind::Monster: {
		const Monster &monster = Monsters[target.id];
		result.text = std::string(monster.name());
		result.color = monster.isUnique() ? UiFlags::ColorWhitegold : UiFlags::ColorWhite;
		break;
	}
	case TargetKind::Towner:
		result.text = std::string(Towners[target.id].name);
		break;
	case TargetKind::Player:
		result.text = std::string(Players[target.id].name());
		break;
	case TargetKind::Item: {
		const Item &item = Items[target.id];
		if (item._itype == ItemType::Gold)
			result.text = StrCat(FormatInteger(item._ivalue), " ", _("gold"));
		else
			result.text = std::string(item.getName().str());
		result.color = item.getTextColor();
		break;
	}
	case TargetKind::Object:
	case TargetKind::Door:
		result.text = std::string(Objects[target.id].name().str());
		break;
	case TargetKind::Trigger:
		result.text = std::string(InfoString.str());
		break;
	default:
		break;
	}
	return result;
}

bool HasUsePrompt(const CrosshairTarget &target)
{
	if (!target.inRange)
		return false;
	switch (target.kind) {
	case TargetKind::Item:
	case TargetKind::Object:
	case TargetKind::Door:
	case TargetKind::Trigger:
	case TargetKind::Towner:
		return true;
	case TargetKind::Monster:
		return CanTalkToMonst(Monsters[target.id]);
	default:
		return false;
	}
}

void DrawTextBox(const Surface &ui, std::string_view text, int y, UiFlags color, bool background)
{
	const int width = GetLineWidth(text) + 8;
	const int x = (ui.w() - width) / 2;
	if (background)
		DrawHalfTransparentRectTo(ui, x, y - 2, width, 16);
	DrawString(ui, text, { { 0, y }, { ui.w(), 14 } }, { .flags = color | UiFlags::AlignCenter });
}

} // namespace

void DrawHud(const Surface &ui)
{
	if (!IsActive())
		return;
	HudState &hud = GetHud();
	const uint32_t now = NowMs();
	const Point centre = ScreenToUi({ ViewWidth() / 2, ViewHeight() / 2 });
	const int viewHeight = ScreenToUi({ 0, ViewHeight() }).y;
	const CrosshairTarget &target = GetCrosshairTarget();
	const Rectangle &mainPanel = GetMainPanel();

	if (hud.slashActive && ShowSlashOverlay())
		DrawSlash(ui, centre, viewHeight, now - hud.slashStart, hud.slashMirrored);

	const bool attackable = (target.kind == TargetKind::Monster || target.kind == TargetKind::Player) && target.inRange;
	DrawCrosshair(ui, centre, attackable);
	if (now < hud.hitMarkerUntil)
		DrawHitMarker(ui, centre);

	// Target name (and a health bar when the "Enemy Health Bar" option is off; that one draws its own) at the top.
	if (target.kind != TargetKind::None && target.kind != TargetKind::Trigger) {
		const bool optionBar = target.kind == TargetKind::Monster && *GetOptions().Gameplay.enemyHealthBar;
		if (!optionBar) {
			const TargetText name = TargetName(target);
			DrawTextBox(ui, name.text, 6, name.color, false);
			if (target.kind == TargetKind::Monster) {
				const Monster &monster = Monsters[target.id];
				const int barWidth = 120 * UiUnit(ui);
				const int x = (ui.w() - barWidth) / 2;
				const int fill = monster.maxHitPoints > 0 ? std::clamp(barWidth * monster.hitPoints / monster.maxHitPoints, 0, barWidth) : 0;
				FillRect(ui, x - 1, 23, barWidth + 2, 5, ColorShadow);
				FillRect(ui, x, 24, fill, 3, PAL16_RED + 4);
			}
		}
	}

	// "E: <name>" prompt above the panel.
	if (HasUsePrompt(target)) {
		const TargetText name = TargetName(target);
		if (!name.text.empty()) {
			const int y = std::min(mainPanel.position.y, viewHeight) - 36;
			DrawTextBox(ui, StrCat("E: ", name.text), y, UiFlags::ColorWhitegold, true);
		}
	}

	if (now < hud.helpUntil) {
		const int y = std::min(mainPanel.position.y, viewHeight) - 58;
		DrawTextBox(ui, _("WASD move   Mouse look   LMB attack   RMB spell   E use   Alt cursor   G view"), y, UiFlags::ColorWhite, true);
	}

	if (now < hud.bannerUntil && !hud.bannerText.empty()) {
		DrawString(ui, hud.bannerText, { { 0, viewHeight / 4 }, { ui.w(), 40 } },
		    { .flags = UiFlags::FontSize30 | UiFlags::ColorGold | UiFlags::AlignCenter });
	}
}

} // namespace devilution::fpv
