#include "host/host_panels.hpp"
#include "host/host_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>

#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#include "engine/palette.h"
#include "engine/backbuffer_state.hpp"
#include "inv.h"
#include "items.h"
#include "player.h"
#include "stores.h"
#include "utils/clx_decode.hpp"
#include "utils/display.h"

namespace devilution::host {
namespace {
uint32_t Generation = 0;
uint32_t SnapshotTick = 0;
uint32_t SnapshotUi = 0;
TalkID SnapshotStore = TalkID::None;
std::unordered_set<int32_t> SnapshotIcons;

bool SafePanels()
{
	if (!InGame() || IsInspectingPlayer())
		return false;
	const auto flags = BuildUiState().flags;
	return (flags & ~(kUiInventory | kUiStore | kUiAutomap)) == 0;
}

void CopyText(char (&target)[1024], const std::string_view value)
{
	const size_t size = std::min(value.size(), sizeof(target) - 1);
	std::memcpy(target, value.data(), size);
	target[size] = '\0';
}

void ExportItem(const Item &item, int cell, DxpPanelItem &output)
{
	output.icon = -1;
	if (item.isEmpty())
		return;
	const int icon = item._iCurs + CURSOR_FIRSTITEM;
	const Size pixels = GetInvItemSize(icon);
	const Size cells = GetInventorySize(item);
	output.icon = icon;
	output.width = pixels.width;
	output.height = pixels.height;
	output.cell = cell;
	output.columns = cells.width;
	output.rows = cells.height;
	output.quality = static_cast<int32_t>(item._iMagical);
	output.usable = item._iStatFlag;
	std::string text = std::string(std::string_view(item.getName()));
	if (item._itype == ItemType::Gold)
		text = std::to_string(item._ivalue) + " gold";
	if (item._iIdentified) {
		if (item._iMagical != ITEM_QUALITY_UNIQUE && item._iPrePower != -1)
			text += "\n" + std::string(std::string_view(PrintItemPower(item._iPrePower, item)));
		if (item._iSufPower != -1)
			text += "\n" + std::string(std::string_view(PrintItemPower(item._iSufPower, item)));
	}
	if (item._iMaxDam > 0)
		text += "\nDamage: " + std::to_string(item._iMinDam) + " - " + std::to_string(item._iMaxDam);
	if (item._iAC > 0)
		text += "\nArmor: " + std::to_string(item._iAC);
	if (item._iMaxDur == DUR_INDESTRUCTIBLE)
		text += "\nIndestructible";
	else if (item._iMaxDur > 0)
		text += "\nDurability: " + std::to_string(item._iDurability) + " / " + std::to_string(item._iMaxDur);
	if (item._iIdentified && item._iMaxCharges > 0)
		text += "\nCharges: " + std::to_string(item._iCharges) + " / " + std::to_string(item._iMaxCharges);
	if (!item._iIdentified && item._iMagical != ITEM_QUALITY_NORMAL)
		text += "\nNot identified";
	if (item._iMinStr > 0) text += "\nRequired Strength: " + std::to_string(item._iMinStr);
	if (item._iMinMag > 0) text += "\nRequired Magic: " + std::to_string(item._iMinMag);
	if (item._iMinDex > 0) text += "\nRequired Dexterity: " + std::to_string(item._iMinDex);
	if (!item._iStatFlag) text += "\nRequirements not met";
	CopyText(output.text, text);
}
}

bool NativeInventoryPanel()
{
	return HideInventory && SafePanels();
}

bool NativeStorePanel()
{
	return HideStore && SafePanels();
}
}

using namespace devilution;
using namespace devilution::host;

extern "C" bool dxp_panels_snapshot(DxpPanelSnapshot *output, uint32_t size)
{
	if (output == nullptr || size != sizeof(*output))
		return false;
	*output = {};
	output->version = 1;
	output->generation = ++Generation;
	output->tick = TotalTicksRun();
	SnapshotTick = output->tick;
	SnapshotUi = BuildUiState().flags;
	SnapshotStore = ActiveStore;
	SnapshotIcons.clear();
	for (auto &item : output->items) item.icon = -1;
	if (!SafePanels())
		return true;
	const Player &player = *MyPlayer;
	output->flags = (invflag ? 1U : 0U) | (IsPlayerInStore() ? 2U : 0U);
	if (output->flags == 0)
		return true;
	output->gold = IsPlayerInStore() ? StorePanelGold() : static_cast<uint32_t>(std::max(player._pGold, 0));
	output->store = static_cast<uint32_t>(ActiveStore);
	for (int slot = 0; slot < 47; ++slot) {
		const auto position = GetPanelPosition(UiPanels::Inventory, InvRect[slot].position);
		output->slots[slot][0] = position.x;
		output->slots[slot][1] = position.y;
		output->slots[slot][2] = InvRect[slot].size.width;
		output->slots[slot][3] = InvRect[slot].size.height;
	}
	for (int index = 0; index < 7; ++index)
		ExportItem(player.InvBody[index], index, output->items[index]);
	if (!player.InvBody[4].isEmpty() && player.GetItemLocation(player.InvBody[4]) == ILOC_TWOHAND) {
		ExportItem(player.InvBody[4], 5, output->items[5]);
		output->items[5].quality |= 0x100;
	}
	for (int cell = 0; cell < 40; ++cell) output->grid[cell] = player.InvGrid[cell];
	for (int index = 0; index < player._pNumInv; ++index) {
		int first = -1;
		for (int cell = 0; cell < 40; ++cell) {
			if (std::abs(static_cast<int>(player.InvGrid[cell])) == index + 1) { first = cell; break; }
		}
		if (first >= 0) ExportItem(player.InvList[index], first + 7, output->items[index + 7]);
	}
	ExportItem(player.HoldItem, -1, output->items[47]);
	if (IsPlayerInStore()) {
		for (const auto &row : ExportStorePanelRows()) {
			if (row.text.size() >= 1024) { output->flags &= ~2U; break; }
			auto &destination = output->rows[output->rowCount++];
			destination = { row.line, row.y, row.height, row.price, row.icon >= 0 ? row.icon + CURSOR_FIRSTITEM : -1, row.selectable, row.selected, row.flags, {} };
			CopyText(destination.text, row.text);
		}
		output->scrolling = StorePanelHasScrollbar();
	}
	for (const auto &item : output->items) if (item.icon >= CURSOR_FIRSTITEM) SnapshotIcons.insert(item.icon);
	for (uint32_t index = 0; index < output->rowCount; ++index) if (output->rows[index].icon >= CURSOR_FIRSTITEM) SnapshotIcons.insert(output->rows[index].icon);
	return true;
}

extern "C" void dxp_panels_enable(uint32_t mask)
{
	const bool inventory = (mask & 1) != 0;
	const bool store = (mask & 2) != 0;
	if (HideInventory != inventory || HideStore != store) {
		HideInventory = inventory;
		HideStore = store;
		if (InGame()) RedrawEverything();
	}
}

extern "C" bool dxp_panels_action(uint32_t generation, uint32_t action, int32_t target, float horizontal, float vertical, uint32_t modifiers)
{
	if (generation != Generation || SnapshotTick != TotalTicksRun() || !SafePanels()
	    || SnapshotUi != BuildUiState().flags || SnapshotStore != ActiveStore)
		return false;
	bool accepted = false;
	if (action >= 10 && action <= 15 && HideStore && IsPlayerInStore()) {
		switch (action) {
		case 10: accepted = ActivateStorePanelRow(target); break;
		case 11: StorePrior(); accepted = true; break;
		case 12: StoreNext(); accepted = true; break;
		case 13: StoreESC(); accepted = true; break;
		case 14: if (target > 0) StoreUp(); else StoreDown(); accepted = true; break;
		case 15:
			for (int depth = 0; depth < 32 && IsPlayerInStore(); ++depth) StoreESC();
			accepted = true;
			break;
		}
	} else if (action == 3 && HideInventory && invflag && !IsPlayerInStore() && MyPlayer->HoldItem.isEmpty()) {
		invflag = false;
		accepted = true;
	} else if ((action == 1 || action == 2) && HideInventory && invflag && !IsPlayerInStore()
	    && target >= 0 && target < 47 && std::isfinite(horizontal) && std::isfinite(vertical)
	    && horizontal >= 0 && horizontal < 1 && vertical >= 0 && vertical < 1) {
		if (pcurs != CURSOR_HAND && pcurs < CURSOR_FIRSTITEM && pcurs != CURSOR_IDENTIFY
		    && pcurs != CURSOR_REPAIR && pcurs != CURSOR_RECHARGE && pcurs != CURSOR_OIL)
			return false;
		const Point previous = MousePosition;
		const auto &rect = InvRect[target];
		MousePosition = GetPanelPosition(UiPanels::Inventory, rect.position)
		    + Displacement { static_cast<int>(horizontal * rect.size.width), static_cast<int>(vertical * rect.size.height) };
		pcursinvitem = CheckInvHLight();
		if (!TryIconCurs()) {
			if (action == 1) CheckInvItem((modifiers & 1) != 0, (modifiers & 2) != 0);
			else if (pcursinvitem >= 0 && MyPlayer->HoldItem.isEmpty()) UseInvItem(pcursinvitem);
		}
		MousePosition = previous;
		accepted = true;
	}
	if (accepted) {
		++Generation;
		RedrawEverything();
	}
	return accepted;
}

extern "C" uint32_t dxp_panels_icon(int32_t icon, uint8_t *output, uint32_t capacity, uint32_t *outWidth, uint32_t *outHeight)
{
	if (!InGame() || SnapshotIcons.count(icon) == 0 || outWidth == nullptr || outHeight == nullptr)
		return 0;
	const auto sprite = GetInvItemSprite(icon);
	const uint32_t width = sprite.width(), height = sprite.height();
	*outWidth = width;
	*outHeight = height;
	const uint32_t required = width * height * 4;
	if (output == nullptr || capacity < required) return required;
	std::memset(output, 0, required);
	const uint8_t *source = sprite.pixelData();
	const uint8_t *end = source + sprite.pixelDataSize();
	uint32_t horizontal = 0, vertical = 0;
	const auto put = [&](uint32_t column, uint8_t index) {
		if (column >= width || vertical >= height) return;
		const auto color = logical_palette[index];
		const size_t offset = ((height - 1 - vertical) * width + column) * 4;
		output[offset] = color.r; output[offset + 1] = color.g; output[offset + 2] = color.b; output[offset + 3] = 255;
	};
	while (source < end && vertical < height && width != 0) {
		const uint8_t command = *source++;
		if (!IsClxOpaque(command)) horizontal += command;
		else if (IsClxOpaqueFill(command)) {
			if (source >= end) return 0;
			const auto count = GetClxOpaqueFillWidth(command);
			const uint8_t color = *source++;
			for (uint32_t index = 0; index < count; ++index) put(horizontal + index, color);
			horizontal += count;
		} else {
			const auto count = GetClxOpaquePixelsWidth(command);
			if (end - source < count) return 0;
			for (uint32_t index = 0; index < count; ++index) put(horizontal + index, source[index]);
			source += count;
			horizontal += count;
		}
		while (horizontal >= width) { horizontal -= width; ++vertical; }
	}
	return required;
}
