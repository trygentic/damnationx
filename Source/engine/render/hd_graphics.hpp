/**
 * @file hd_graphics.hpp
 *
 * The runtime HD graphics switch (Graphics > "HD Graphics", hotkey H), like Diablo II: Resurrected's legacy/HD toggle.
 *
 * The launch flags (`--hd`, `--hd-flag NAME`, the browser's `?hdSprites=1` ...) say which HD subsystems this run can
 * draw: they load the pack's indexes and art on demand. The option is the user's switch on top of them: every HD gate
 * (HdSpritesGpuAvailable, HdTilesGpuAvailable, HdTilesPreferFloorAtlas, the presenter's HD floor, HdUiActive) is
 * "launch flag && HdGraphicsEnabled()". Turning HD off keeps the GPU art resident (turning it back on is instant) and
 * keeps the HD UI's sprite tracking hooks installed (UI art loads once, so the hooks must see every load), but nothing
 * HD is recorded or drawn: the classic frame is presented as it is.
 */
#pragma once

namespace devilution {

/** True when this run was started with any HD subsystem (WebGPU builds only); the option is listed then. */
[[nodiscard]] bool HdGraphicsAvailable();
/** The Graphics > "HD Graphics" option. */
[[nodiscard]] bool HdGraphicsEnabled();
/** Flips the option (hotkey H) and shows "HD Graphics: On/Off" in a brief message box. */
void ToggleHdGraphics();
/**
 * Called once per presented frame. With the launch flag `hdToggleStress` (`--hd-flag hdToggleStress`, the browser's
 * `?hdToggleStress=1`) it flips the option every 20 frames in game, 100 times (50 off/on cycles), logging each flip:
 * a debug path for checking the switch for stale frames, GPU errors and leaks.
 */
void HdGraphicsFrameTick();
/** Lists the option in the settings menu when HdGraphicsAvailable(); call once the launch flags are known. */
void InitHdGraphicsOption();

} // namespace devilution
