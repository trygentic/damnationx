/**
 * @file ui_scale.hpp
 *
 * The in-game UI scale (Graphics "UI Scale", 1x .. 4x in quarters), independent of the world zoom.
 *
 * Design: the in-game UI keeps its classic pixel layout but lives in a smaller "UI space", the screen divided by the
 * scale (ui_scale_core.hpp). Everything that lays out or hit-tests the UI uses UI-space coordinates:
 *  - GetMainPanel(), GetLeftPanel(), GetRightPanel() and GetGameUIRectangle() are laid out in GetUiWidth() x
 *    GetUiHeight(), so every panel, slot and button rectangle derived from them is in UI pixels;
 *  - MousePosition is in UI pixels (SetMousePositionFromScreen at the input boundary), so panel hit tests compare like
 *    with like; the world pickers use GetMouseScreenPosition() (full screen resolution) instead.
 * The world pass still draws into the back buffer at screen resolution. The UI draws into a UI-space layer that starts
 * as a nearest sample of the finished world (half transparent boxes blend with it) and is scaled up nearest onto the
 * back buffer afterwards: only the UI pixels the UI changed are written, so the world keeps its full resolution.
 *
 * The scale is limited so the UI space stays wider than 640 and at least 480 high (EffectiveUiScaleQuarters): the
 * panels never cover the view then and the world geometry does not depend on the UI scale (apart from the player
 * staying centred above the taller control panel). At 1x nothing changes: UI space is the screen and the layer is the
 * back buffer itself.
 */
#pragma once

#include <cstdint>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/hd_view_transform.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** Recomputes the effective scale and UI space from the option and the screen size (AdjustToScreenGeometry). */
void UpdateUiScale();

/** The scale that applies, in quarters (4 = 1x): the option limited to what the screen fits. */
[[nodiscard]] int GetUiScaleQuarters();
[[nodiscard]] inline bool IsUiScaled() { return GetUiScaleQuarters() > 4; }
/** UI space to screen: `world` is the UI space, `viewport` the screen. Identity at 1x. */
[[nodiscard]] const HdWorldTransform &GetUiTransform();

/** Size of the UI space (the screen at 1x). */
[[nodiscard]] uint16_t GetUiWidth();
[[nodiscard]] uint16_t GetUiHeight();
/** UI rows not covered by the control panel when it covers the view (the 640 px wide layout); else GetUiHeight(). */
[[nodiscard]] uint16_t GetUiViewportHeight();
/** The 640 x 480 rectangle centred in UI space where the in-game dialogs (stores, quest text, help, game menu) go. */
[[nodiscard]] const Rectangle &GetGameUIRectangle();

[[nodiscard]] Point ScreenToUi(Point screen);
/** The screen pixel at the centre of a UI pixel. */
[[nodiscard]] Point UiToScreen(Point ui);
/** The screen pixels a UI rectangle covers. */
[[nodiscard]] Rectangle UiToScreen(const Rectangle &ui);
/** Screen rows covered by the control panel (its UI height scaled; the player is centred above them). */
[[nodiscard]] int GetMainPanelScreenHeight();

/** Input boundary: the mouse is at `screen` (logical screen pixels); sets MousePosition (UI pixels). */
void SetMousePositionFromScreen(Point screen);
/** The mouse in screen pixels for the world pickers: exact when MousePosition came from the mouse, else its centre. */
[[nodiscard]] Point GetMouseScreenPosition();

// ---- The UI layer (scrollrt.cpp) ----

/**
 * @brief Starts the UI of a frame over the finished world in `backBuffer`.
 * @return the surface the UI draws into: the back buffer at 1x, else the UI layer sampled from it
 */
[[nodiscard]] Surface BeginUiLayer(const Surface &backBuffer);
/** The layer of the last frame when the back buffer still shows it (a re-present only redraws the cursor), else BeginUiLayer. */
[[nodiscard]] Surface ResumeUiLayer(const Surface &backBuffer, bool &resumed);
/** Scales the UI layer onto the back buffer. `restore`: rewrite the untouched pixels from the world the layer began over. */
void CompositeUiLayer(const Surface &backBuffer, bool restore);
/** The back buffer no longer shows the last UI layer (ClearScreenBuffer). */
void InvalidateUiLayer();
/** True when `out` draws into the UI layer (not the back buffer). */
[[nodiscard]] bool IsUiLayer(const Surface &out);
/** The back buffer the UI layer is composited onto. */
[[nodiscard]] Surface UiLayerBackBuffer();
/** The UI layer surface while a scaled in-game UI shows (the HD UI records its layers there), else nullptr. */
[[nodiscard]] SDL_Surface *GetUiLayerSurface();
/** Recolours the layer, its baseline and the world copy through `map` (RedBack over a scaled UI: see control_panel.cpp). */
void RemapUiLayer(const uint8_t map[256]);

} // namespace devilution
