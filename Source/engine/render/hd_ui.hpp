/**
 * @file hd_ui.hpp
 *
 * Opt-in HD UI for the WebGPU browser build (`?hdUi=1`): the control
 * panel, orbs, belt, side panels, item icons, cursors, game menu, stores and the DiabloUI front end are drawn from
 * 4x art at canvas resolution over the classic UI.
 *
 * The classic UI is still drawn into the back buffer unchanged (input, hit boxes and the classic fallback stay as
 * they are). The low-level draw calls report to this module through hd_ui_hooks.hpp; it maps each loaded UI frame
 * to its asset key, follows HD content through off-screen surfaces (panel buffers, SurfaceToClx composites) and
 * records back-buffer layers (hd_ui_core.hpp). RenderPresent resolves them against the final frame
 * (HdUiFinishFrame) and the presenter draws them after the HD sprites (hd_ui_webgpu.cpp).
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/render/hd_ui_core.hpp"

namespace devilution {

/**
 * Installs the draw hooks when `?hdUi=1` asks for the HD UI (WebGPU build); call before UI assets load. Also lists
 * Graphics > HD Graphics (hd_graphics.hpp) when this run has any HD subsystem.
 */
void HdUiInstall();
/** True once the hooks are installed and Graphics > HD Graphics is on. */
[[nodiscard]] bool HdUiActive();
/** The world pass redraws `rect` (back-buffer pixels) from scratch: drop the HD layers there. */
void HdUiInvalidateRect(int x, int y, int width, int height);
/** The back buffer no longer holds the UI that was recorded (movie, loading screen): drop every layer. */
void HdUiInvalidate();
/** Resolves the layers against the finished back buffer; called by RenderPresent right before presenting. */
void HdUiFinishFrame(const uint8_t *pixels, int pitch, int width, int height);

// ---- Presenter side ----
[[nodiscard]] const hd_ui::LayerTracker &HdUiLayers();
[[nodiscard]] const hd_ui::UiIndex &HdUiIndex();
[[nodiscard]] const hd_ui::TrnPool &HdUiTrns();
/** Index frame of a layer (nullptr for tints or missing art). */
[[nodiscard]] const hd_ui::FrameInfo *HdUiLayerFrame(const hd_ui::Placed &placed);
/** The classic frames (loaded and registered) whose HD images lie in `sheet`, to check an uploaded indexed sheet. */
[[nodiscard]] std::vector<hd_ui::IndexedSheetFrame> HdUiClassicFramesOfSheet(int sheet);

// ---- Implemented by hd_ui_webgpu.cpp (stubs without DEVILUTIONX_WEBGPU) ----
/** `?hdUi=1` and a presenter that can draw it. */
[[nodiscard]] bool HdUiGpuWanted();
/** Text of hd/ui.txt fetched next to the page; true when `text` holds it, `failed` once it cannot load. */
bool HdUiGpuFetchIndex(std::string &text, bool &failed);
/** True when the sheet of this frame is resident (draws this frame); otherwise starts loading it. */
bool HdUiGpuSheetReady(int sheet);

} // namespace devilution
