/**
 * @file ui_scale.cpp
 *
 * The in-game UI scale: UI space, mouse mapping and the UI layer (ui_scale.hpp).
 */
#include "engine/render/ui_scale.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "control/control.hpp"
#include "diablo.h"
#include "engine/backbuffer_state.hpp"
#include "engine/dx.h"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_ui.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/ui_scale_core.hpp"
#include "game_mode.hpp"
#include "hwcursor.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/sdl_compat.h"

namespace devilution {

namespace {

int UiScaleQuarters = 4;
HdWorldTransform UiTransform;
Rectangle GameUIRectangle;
/** The last mouse position from the mouse itself (screen pixels) and the UI pixel it gave. */
Point LastMouseScreen;
Point LastMouseUi;

struct UiLayer {
	std::optional<OwnedSurface> ui;
	/** What the layer started as (the world sampled into UI space): UI pixels that still equal it were not drawn. */
	std::optional<OwnedSurface> baseline;
	/** The back buffer the layer started over (pitch of the back buffer), for re-presents. */
	std::vector<uint8_t> world;
	bool valid = false;
};

UiLayer Layer;

/** The UI space follows the screen size even when something sets it without AdjustToScreenGeometry (tests). */
void Refresh()
{
	if (UiTransform.viewport.width != gnScreenWidth || UiTransform.viewport.height != gnScreenHeight)
		UpdateUiScale();
}

void EnsureLayerSurfaces()
{
	const int width = UiTransform.world.width;
	const int height = UiTransform.world.height;
	if (Layer.ui && Layer.ui->w() == width && Layer.ui->h() == height)
		return;
	Layer.ui.emplace(width, height);
	Layer.baseline.emplace(width, height);
	for (const OwnedSurface *surface : { &*Layer.ui, &*Layer.baseline }) {
		if (Palette != nullptr)
			SDLC_SetSurfacePalette(surface->surface, Palette.get());
	}
	// The cursor was saved from (and restores into) the old layer.
	GetDrawnCursor().rect.size = { 0, 0 };
	Layer.valid = false;
}

void OptionUiScaleChanged()
{
	const int previous = UiScaleQuarters;
	UpdateUiScale();
	CalculatePanelAreas();
	if (UiScaleQuarters == previous)
		return;
	// MousePosition is in the old UI space: map the last mouse position again.
	MousePosition = ScreenToUi(LastMouseScreen);
	LastMouseUi = MousePosition;
	GetDrawnCursor().rect.size = { 0, 0 };
	Layer.valid = false;
	if (gbRunGame) {
		CalcViewportGeometry(); // the player stays centred above the control panel
		RedrawEverything();
	}
	ReinitializeHardwareCursor();
}

const auto OptionChangeHandlerUiScale = (GetOptions().Graphics.uiScale.SetValueChangedCallback(OptionUiScaleChanged), true);

} // namespace

void UpdateUiScale()
{
	UiScaleQuarters = EffectiveUiScaleQuarters(*GetOptions().Graphics.uiScale, gnScreenWidth, gnScreenHeight);
	UiTransform = ComputeUiTransform(UiScaleQuarters, gnScreenWidth, gnScreenHeight);
	constexpr Size UISize { 640, 480 };
	GameUIRectangle = {
		{ (GetUiWidth() - UISize.width) / 2, (GetUiHeight() - UISize.height) / 2 },
		UISize
	};
}

int GetUiScaleQuarters()
{
	Refresh();
	return UiScaleQuarters;
}

const HdWorldTransform &GetUiTransform()
{
	Refresh();
	return UiTransform;
}

uint16_t GetUiWidth()
{
	Refresh();
	return static_cast<uint16_t>(UiTransform.world.width);
}

uint16_t GetUiHeight()
{
	Refresh();
	return static_cast<uint16_t>(UiTransform.world.height);
}

uint16_t GetUiViewportHeight()
{
	const Rectangle &mainPanel = GetMainPanel();
	if (GetUiWidth() <= mainPanel.size.width)
		return static_cast<uint16_t>(GetUiHeight() - mainPanel.size.height);
	return GetUiHeight();
}

const Rectangle &GetGameUIRectangle()
{
	Refresh();
	return GameUIRectangle;
}

Point ScreenToUi(Point screen)
{
	if (!IsUiScaled())
		return screen;
	return { ScreenToUiX(UiTransform, screen.x), ScreenToUiY(UiTransform, screen.y) };
}

Point UiToScreen(Point ui)
{
	if (!IsUiScaled())
		return ui;
	return { UiToScreenCentreX(UiTransform, ui.x), UiToScreenCentreY(UiTransform, ui.y) };
}

Rectangle UiToScreen(const Rectangle &ui)
{
	if (!IsUiScaled())
		return ui;
	const int left = UiEdgeX(UiTransform, ui.position.x);
	const int top = UiEdgeY(UiTransform, ui.position.y);
	return { { left, top }, { UiEdgeX(UiTransform, ui.position.x + ui.size.width) - left, UiEdgeY(UiTransform, ui.position.y + ui.size.height) - top } };
}

int GetMainPanelScreenHeight()
{
	const Rectangle &mainPanel = GetMainPanel();
	if (!IsUiScaled())
		return mainPanel.size.height;
	return gnScreenHeight - UiEdgeY(UiTransform, mainPanel.position.y);
}

void SetMousePositionFromScreen(Point screen)
{
	LastMouseScreen = screen;
	LastMouseUi = ScreenToUi(screen);
	MousePosition = LastMouseUi;
}

Point GetMouseScreenPosition()
{
	if (!IsUiScaled())
		return MousePosition;
	// Something else moved the cursor (gamepad navigation, SetCursorPos without a mouse): the centre of its UI pixel.
	if (MousePosition != LastMouseUi)
		return UiToScreen(MousePosition);
	return LastMouseScreen;
}

Surface BeginUiLayer(const Surface &backBuffer)
{
	if (!IsUiScaled()) {
		Layer.valid = false;
		return backBuffer;
	}
	EnsureLayerSurfaces();
	UiSampleScreen(backBuffer.begin(), static_cast<size_t>(backBuffer.pitch()), Layer.baseline->begin(),
	    static_cast<size_t>(Layer.baseline->pitch()), UiTransform);
	for (int y = 0; y < Layer.ui->h(); ++y)
		std::memcpy(Layer.ui->at(0, y), Layer.baseline->at(0, y), static_cast<size_t>(Layer.ui->w()));
	const size_t worldBytes = static_cast<size_t>(backBuffer.pitch()) * static_cast<size_t>(backBuffer.h());
	Layer.world.resize(worldBytes);
	std::memcpy(Layer.world.data(), backBuffer.begin(), worldBytes);
	// The HD UI records its layers in the UI layer while scaled (hd_ui.cpp); the layer is redrawn from scratch.
	HdUiInvalidate();
	Layer.valid = true;
	return *Layer.ui;
}

Surface ResumeUiLayer(const Surface &backBuffer, bool &resumed)
{
	resumed = IsUiScaled() && Layer.valid && Layer.ui && Layer.ui->w() == UiTransform.world.width && Layer.ui->h() == UiTransform.world.height
	    && Layer.world.size() == static_cast<size_t>(backBuffer.pitch()) * static_cast<size_t>(backBuffer.h());
	if (resumed)
		return *Layer.ui;
	return BeginUiLayer(backBuffer);
}

void CompositeUiLayer(const Surface &backBuffer, bool restore)
{
	if (!IsUiScaled() || !Layer.valid)
		return;
	const bool stamp = gbuffer::StampActive() && gbuffer::CurrentPlanes.covers(backBuffer);
	UiComposite(backBuffer.begin(), static_cast<size_t>(backBuffer.pitch()), restore ? Layer.world.data() : nullptr,
	    Layer.ui->begin(), Layer.baseline->begin(), static_cast<size_t>(Layer.ui->pitch()), UiTransform,
	    [stamp](const uint8_t *dst, int n) {
		    if (stamp)
			    gbuffer::StampUnlitRun(dst, static_cast<unsigned>(n));
	    });
}

void InvalidateUiLayer()
{
	Layer.valid = false;
}

bool IsUiLayer(const Surface &out)
{
	return Layer.ui && out.surface == Layer.ui->surface;
}

SDL_Surface *GetUiLayerSurface()
{
	// Only in game and while the back buffer shows the layer (not the menus, loading screens and movies).
	return IsUiScaled() && gbRunGame && Layer.valid && Layer.ui ? Layer.ui->surface : nullptr;
}

Surface UiLayerBackBuffer()
{
	return GlobalBackBuffer();
}

void RemapUiLayer(const uint8_t map[256])
{
	if (!Layer.ui)
		return;
	for (OwnedSurface *surface : { &*Layer.ui, &*Layer.baseline }) {
		for (int y = 0; y < surface->h(); ++y) {
			uint8_t *row = surface->at(0, y);
			for (int x = 0; x < surface->w(); ++x)
				row[x] = map[row[x]];
		}
	}
	for (uint8_t &pixel : Layer.world)
		pixel = map[pixel];
}

} // namespace devilution
