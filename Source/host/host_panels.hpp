#pragma once

#include <cstdint>

struct DxpPanelItem {
	int32_t icon, width, height, cell, columns, rows, quality, usable;
	char text[1024];
};

struct DxpPanelRow {
	int32_t line, y, height, price, icon, selectable, selected;
	uint32_t flags;
	char text[1024];
};

struct DxpPanelSnapshot {
	uint32_t version, generation, tick, flags, gold, store, rowCount, scrolling;
	int32_t grid[40];
	float slots[47][4];
	DxpPanelItem items[48];
	DxpPanelRow rows[104];
};

static_assert(sizeof(DxpPanelItem) == 1056);
static_assert(sizeof(DxpPanelRow) == 1056);
static_assert(sizeof(DxpPanelSnapshot) == 161456);

extern "C" {
bool dxp_panels_snapshot(DxpPanelSnapshot *output, uint32_t size);
void dxp_panels_enable(uint32_t mask);
bool dxp_panels_action(uint32_t generation, uint32_t action, int32_t target, float horizontal, float vertical, uint32_t modifiers);
uint32_t dxp_panels_icon(int32_t icon, uint8_t *output, uint32_t capacity, uint32_t *width, uint32_t *height);
}
