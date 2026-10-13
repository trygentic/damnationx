/**
 * @file ui_scale_hit_test.cpp
 *
 * Mouse hit-testing under the in-game UI scale (engine/render/ui_scale.hpp) at several scales and screen sizes:
 * the screen pixels a scaled panel element covers must hit that element (inventory and belt slots, main panel
 * buttons, the spell button, character sheet buttons, the side panels), the pixels next to it must not, and the
 * world pickers must still see the exact screen position.
 */

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ui_test.hpp"

#include "control/control.hpp"
#include "control/control_panel.hpp"
#include "diablo.h"
#include "engine/load_file.hpp"
#include "headless_mode.hpp"
#include "engine/render/ui_scale.hpp"
#include "panels/charpanel.hpp"
#include "utils/sdl_wrap.h"
#include "utils/str_cat.hpp"
#include "utils/surface_to_png.hpp"
#include "inv.h"
#include "options.h"
#include "utils/display.h"
#include "utils/ui_fwd.h"

namespace devilution {
namespace {

struct ScreenCase {
	int width;
	int height;
	int requestedQuarters;
	int expectedQuarters;
};

constexpr std::array<ScreenCase, 9> Cases { {
	{ 1920, 1080, 4, 4 },
	{ 1920, 1080, 6, 6 },
	{ 1920, 1080, 8, 8 },
	{ 1920, 1080, 16, 9 },
	{ 2560, 1440, 7, 7 },
	{ 3440, 1440, 5, 5 },
	{ 3440, 1440, 8, 8 },
	{ 3440, 1440, 12, 12 },
	{ 3840, 2160, 16, 16 },
} };

class UiScaleGeometry : public ::testing::Test {
protected:
	void SetUp() override
	{
		savedWidth_ = gnScreenWidth;
		savedHeight_ = gnScreenHeight;
		savedScale_ = *GetOptions().Graphics.uiScale;
		ControlMode = ControlTypes::KeyboardAndMouse;
	}

	void TearDown() override
	{
		GetOptions().Graphics.uiScale.SetValue(savedScale_);
		Apply(savedWidth_, savedHeight_);
	}

	static void Apply(int width, int height)
	{
		AdjustToScreenGeometry({ width, height }); // what a window resize does
	}

	static void Apply(const ScreenCase &c)
	{
		GetOptions().Graphics.uiScale.SetValue(c.requestedQuarters);
		Apply(c.width, c.height);
	}

	/** Every screen pixel of `uiRect`'s scaled image hits it; the screen pixels just outside it do not. */
	static void ExpectScreenImageHits(const Rectangle &uiRect, const char *what)
	{
		const Rectangle screen = UiToScreen(uiRect);
		ASSERT_GT(screen.size.width, 0) << what;
		ASSERT_GT(screen.size.height, 0) << what;
		const int left = screen.position.x;
		const int top = screen.position.y;
		const int right = left + screen.size.width - 1;
		const int bottom = top + screen.size.height - 1;
		for (const Point p : { Point { left, top }, Point { right, top }, Point { left, bottom }, Point { right, bottom },
		         Point { (left + right) / 2, (top + bottom) / 2 } }) {
			SetMousePositionFromScreen(p);
			EXPECT_TRUE(uiRect.contains(MousePosition)) << what << " screen " << p.x << "," << p.y << " -> ui " << MousePosition.x << "," << MousePosition.y;
			EXPECT_EQ(GetMouseScreenPosition(), p) << what;
		}
		for (const Point p : { Point { left - 1, (top + bottom) / 2 }, Point { right + 1, (top + bottom) / 2 }, Point { (left + right) / 2, top - 1 },
		         Point { (left + right) / 2, bottom + 1 } }) {
			if (p.x < 0 || p.y < 0 || p.x >= gnScreenWidth || p.y >= gnScreenHeight)
				continue;
			SetMousePositionFromScreen(p);
			EXPECT_FALSE(uiRect.contains(MousePosition)) << what << " outside at screen " << p.x << "," << p.y;
		}
	}

private:
	int savedWidth_ = 0;
	int savedHeight_ = 0;
	int savedScale_ = 4;
};

TEST_F(UiScaleGeometry, ScaleIsLimitedAndPanelsLiveInUiSpace)
{
	for (const ScreenCase &c : Cases) {
		SCOPED_TRACE(testing::Message() << c.width << "x" << c.height << " requested " << c.requestedQuarters);
		Apply(c);
		EXPECT_EQ(GetUiScaleQuarters(), c.expectedQuarters);
		EXPECT_EQ(GetUiWidth(), c.width * 4 / c.expectedQuarters);
		EXPECT_EQ(GetUiHeight(), c.height * 4 / c.expectedQuarters);
		// The world keeps the whole screen: the scaled UI space is always wider than the panel.
		EXPECT_EQ(gnViewportHeight, c.height);
		EXPECT_FALSE(CanPanelsCoverView());

		// The control panel sits at the bottom centre of UI space and of the screen.
		const Rectangle &mainPanel = GetMainPanel();
		EXPECT_EQ(mainPanel.position.y + mainPanel.size.height, GetUiHeight());
		const Rectangle mainOnScreen = UiToScreen(mainPanel);
		EXPECT_EQ(mainOnScreen.position.y + mainOnScreen.size.height, c.height);
		EXPECT_NEAR(mainOnScreen.position.x + mainOnScreen.size.width / 2, c.width / 2, c.expectedQuarters);
		EXPECT_EQ(GetMainPanelScreenHeight(), mainOnScreen.size.height);
		EXPECT_NEAR(mainOnScreen.size.width, 640 * c.expectedQuarters / 4, 1);

		// The dialogs' 640 x 480 rectangle is centred in UI space.
		const Rectangle &dialog = GetGameUIRectangle();
		EXPECT_EQ(dialog.position.x, (GetUiWidth() - 640) / 2);
		EXPECT_EQ(dialog.position.y, (GetUiHeight() - 480) / 2);
		// Side panels stay inside UI space.
		EXPECT_GE(GetLeftPanel().position.x, 0);
		EXPECT_LE(GetRightPanel().position.x + GetRightPanel().size.width, GetUiWidth());
	}
}

TEST_F(UiScaleGeometry, OneXChangesNothing)
{
	GetOptions().Graphics.uiScale.SetValue(4);
	Apply(1920, 1080);
	EXPECT_FALSE(IsUiScaled());
	EXPECT_EQ(GetUiWidth(), 1920);
	EXPECT_EQ(GetUiHeight(), 1080);
	EXPECT_EQ(GetMainPanel().position, Point((1920 - 640) / 2, 1080 - 128));
	EXPECT_EQ(GetGameUIRectangle().position, GetUIRectangle().position);
	EXPECT_EQ(GetGameUIRectangle().size, GetUIRectangle().size);
	SetMousePositionFromScreen({ 1001, 503 });
	EXPECT_EQ(MousePosition, Point(1001, 503));
	MousePosition = { 17, 23 }; // set directly, as tests and demos do
	EXPECT_EQ(GetMouseScreenPosition(), Point(17, 23));
}

TEST_F(UiScaleGeometry, PanelElementsAreHitWhereTheyAreDrawn)
{
	for (const ScreenCase &c : Cases) {
		SCOPED_TRACE(testing::Message() << c.width << "x" << c.height << " q=" << c.requestedQuarters);
		Apply(c);
		// Inventory, equipment and belt slots (CheckInvHLight's rectangles).
		for (int r = 0; r < NUM_XY_SLOTS; ++r) {
			const Point origin = r >= SLOTXY_BELT_FIRST ? GetMainPanel().position : GetRightPanel().position;
			ExpectScreenImageHits({ origin + Displacement { InvRect[r].position.x, InvRect[r].position.y }, InvRect[r].size }, "inventory slot");
		}
		// Main panel buttons and the spell button (CheckMainPanelButton).
		for (const Rectangle &button : MainPanelButtonRect) {
			Rectangle placed = button;
			placed.position = GetPanelPosition(UiPanels::Main, button.position);
			ExpectScreenImageHits(placed, "main panel button");
		}
		{
			Rectangle placed = SpellButtonRect;
			placed.position = GetPanelPosition(UiPanels::Main, SpellButtonRect.position);
			ExpectScreenImageHits(placed, "spell button");
		}
		// Character sheet stat buttons.
		for (const Rectangle &button : CharPanelButtonRect) {
			Rectangle placed = button;
			placed.position = GetPanelPosition(UiPanels::Character, button.position);
			ExpectScreenImageHits(placed, "char panel button");
		}
		// Whole panels: the "mouse over a panel or over the world" decision.
		ExpectScreenImageHits(GetMainPanel(), "main panel");
		ExpectScreenImageHits(GetLeftPanel(), "left panel");
		ExpectScreenImageHits(GetRightPanel(), "right panel");
	}
}

TEST_F(UiScaleGeometry, WorldPickingSeesTheExactScreenPosition)
{
	for (const ScreenCase &c : Cases) {
		Apply(c);
		for (const Point p : { Point { 0, 0 }, Point { c.width - 1, c.height - 1 }, Point { c.width / 2 + 1, c.height / 3 }, Point { 333, 211 } }) {
			SetMousePositionFromScreen(p);
			EXPECT_EQ(GetMouseScreenPosition(), p);
			EXPECT_LT(MousePosition.x, GetUiWidth());
			EXPECT_LT(MousePosition.y, GetUiHeight());
		}
		// A cursor placed in UI space (gamepad navigation, SetCursorPos) is picked at the centre of its UI pixel.
		MousePosition = GetMainPanel().position + Displacement { 10, 10 };
		EXPECT_EQ(ScreenToUi(GetMouseScreenPosition()), MousePosition);
	}
}

/** CheckInvHLight end to end: the engine's own slot lookup under a screen position. */
class UiScaleInventoryHitTest : public UITest {
protected:
	void SetUp() override
	{
		UITest::SetUp();
		savedWidth_ = gnScreenWidth;
		savedHeight_ = gnScreenHeight;
		savedScale_ = *GetOptions().Graphics.uiScale;
		InspectPlayer = MyPlayer;
	}

	void TearDown() override
	{
		GetOptions().Graphics.uiScale.SetValue(savedScale_);
		AdjustToScreenGeometry({ savedWidth_, savedHeight_ });
		UITest::TearDown();
	}

	int savedWidth_ = 0;
	int savedHeight_ = 0;
	int savedScale_ = 4;
};

TEST_F(UiScaleInventoryHitTest, BeltAndInventorySlotsUnderScaledScreenPositions)
{
	StripPlayer();
	Item potion {};
	InitializeItem(potion, IDI_HEAL);
	MyPlayer->SpdList[3] = potion;
	const int invIndex = PlaceItemInInventory(potion);
	ASSERT_GE(invIndex, 0);
	invflag = true;

	for (const ScreenCase &c : Cases) {
		SCOPED_TRACE(testing::Message() << c.width << "x" << c.height << " q=" << c.requestedQuarters);
		GetOptions().Graphics.uiScale.SetValue(c.requestedQuarters);
		AdjustToScreenGeometry({ c.width, c.height });

		const Rectangle beltSlot { GetMainPanel().position + Displacement { InvRect[SLOTXY_BELT_FIRST + 3].position.x, InvRect[SLOTXY_BELT_FIRST + 3].position.y },
			InvRect[SLOTXY_BELT_FIRST + 3].size };
		SetMousePositionFromScreen(UiToScreen(beltSlot).Center());
		EXPECT_EQ(CheckInvHLight(), INVITEM_BELT_FIRST + 3);

		const Rectangle gridSlot { GetRightPanel().position + Displacement { InvRect[SLOTXY_INV_FIRST].position.x, InvRect[SLOTXY_INV_FIRST].position.y },
			InvRect[SLOTXY_INV_FIRST].size };
		SetMousePositionFromScreen(UiToScreen(gridSlot).Center());
		EXPECT_EQ(CheckInvHLight(), INVITEM_INV_FIRST + invIndex);

		// The empty belt slot next to the potion.
		const Rectangle emptySlot { GetMainPanel().position + Displacement { InvRect[SLOTXY_BELT_FIRST + 4].position.x, InvRect[SLOTXY_BELT_FIRST + 4].position.y },
			InvRect[SLOTXY_BELT_FIRST + 4].size };
		SetMousePositionFromScreen(UiToScreen(emptySlot).Center());
		EXPECT_EQ(CheckInvHLight(), -1);
	}
}

/**
 * Not a check: renders the control panel, belt, orbs, inventory and character sheet over a synthetic full-resolution
 * "world" at 1x and at the scaled cases and writes them as PNGs into $UI_SCALE_PREVIEW_DIR (skipped without it).
 */
TEST_F(UiScaleInventoryHitTest, WritePreviews)
{
	const char *dir = std::getenv("UI_SCALE_PREVIEW_DIR");
	if (dir == nullptr)
		GTEST_SKIP() << "set UI_SCALE_PREVIEW_DIR to write previews";

	std::array<uint8_t, 768> palData;
	LoadFileInMem("levels\\towndata\\town.pal", palData);
	SDLPaletteUniquePtr palette = SDLWrap::AllocPalette(256);
	for (int i = 0; i < 256; ++i)
		palette->colors[i] = SDL_Color { palData[i * 3], palData[i * 3 + 1], palData[i * 3 + 2], SDL_ALPHA_OPAQUE };

	HeadlessMode = false; // load the panel art
	StripPlayer();
	Item potion {};
	InitializeItem(potion, IDI_HEAL);
	for (int i = 0; i < 4; ++i)
		MyPlayer->SpdList[i] = potion;
	Item sword {};
	InitializeItem(sword, IDI_BARDSWORD);
	PlaceItemInInventory(sword);

	for (const ScreenCase &c : { ScreenCase { 1920, 1080, 4, 4 }, ScreenCase { 1920, 1080, 8, 8 }, ScreenCase { 3440, 1440, 6, 6 }, ScreenCase { 3440, 1440, 8, 8 } }) {
		GetOptions().Graphics.uiScale.SetValue(c.requestedQuarters);
		AdjustToScreenGeometry({ c.width, c.height });
		ASSERT_TRUE(InitMainPanel().has_value());
		InitInv();
		invflag = true;
		CharFlag = true;

		OwnedSurface backBuffer { c.width, c.height };
		SDL_SetSurfacePalette(backBuffer.surface, palette.get());
		// The "world": a full-resolution checkerboard with 1 px lines, so a scaled world would show.
		for (int y = 0; y < c.height; ++y) {
			uint8_t *row = backBuffer.at(0, y);
			for (int x = 0; x < c.width; ++x)
				row[x] = (x % 8 == 0 || y % 8 == 0) ? 0 : static_cast<uint8_t>(((x / 32 + y / 32) % 2 != 0) ? 0xF4 : 0xE8);
		}
		const Surface ui = BeginUiLayer(backBuffer);
		DrawInv(ui);
		DrawChr(ui);
		DrawMainPanel(ui);
		UpdateLifeManaPercent();
		DrawLifeFlaskLower(ui, true);
		DrawManaFlaskLower(ui, true);
		DrawLifeFlaskUpper(ui);
		DrawManaFlaskUpper(ui);
		DrawInvBelt(ui);
		CompositeUiLayer(backBuffer, /*restore=*/false);

		const std::string path = StrCat(dir, "/ui-scale-", c.width, "x", c.height, "-", c.requestedQuarters * 25, ".png");
		SDL_IOStream *file = SDL_IOFromFile(path.c_str(), "wb");
		ASSERT_NE(file, nullptr) << path;
		ASSERT_TRUE(WriteSurfaceToFilePng(backBuffer, file).has_value()) << path;
	}
	invflag = false;
	CharFlag = false;
	HeadlessMode = true;
}

} // namespace
} // namespace devilution
