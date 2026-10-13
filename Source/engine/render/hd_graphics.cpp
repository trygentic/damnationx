/**
 * @file hd_graphics.cpp
 *
 * The runtime HD graphics switch (hd_graphics.hpp).
 */
#include "engine/render/hd_graphics.hpp"

#include "config.h"
#include "diablo_msg.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_tiles.hpp"
#include "engine/render/hd_ui.hpp"
#include "game_mode.hpp"
#include "options.h"
#include "utils/language.h"
#include "utils/log.hpp"

#if DEVILUTIONX_WEBGPU
#include "engine/render/webgpu_platform.hpp"
#endif

namespace devilution {

namespace {

/**
 * Drops every HD record that describes the frame on screen (sprite instances and occlusion, tile instances and the
 * dynamic plane, UI layers) and redraws the whole frame: the next present is built from scratch in the new mode, so no
 * HD image outlives the switch and none is missing after it. The zoom follows by itself: GetZoomQuarters() rounds a
 * fractional level to a whole one while FractionalZoomAvailable() (HD tiles and sprites) is false, and
 * CalcFirstTilePosition recomputes the view geometry when the effective zoom changes. The stored level is kept, so
 * turning HD back on restores it.
 */
void OnHdGraphicsChanged()
{
	HdSpritesInvalidate();
	HdTilesInvalidate();
	HdUiInvalidate();
	RedrawEverything();
	Log("HD graphics: {}", HdGraphicsEnabled() ? "on" : "off");
}

const auto OptionChangeHandlerHdGraphics = (GetOptions().Graphics.hdGraphics.SetValueChangedCallback(OnHdGraphicsChanged), true);

} // namespace

bool HdGraphicsAvailable()
{
#if DEVILUTIONX_WEBGPU
	const WebGpuPlatform &platform = GetWebGpuPlatform();
	static const bool Available = platform.queryFlag("hdSprites") || platform.queryFlag("hdTiles")
	    || platform.queryFlag("hdUi") || platform.queryFlag("hdFloor");
	return Available;
#else
	return false;
#endif
}

bool HdGraphicsEnabled()
{
	return *GetOptions().Graphics.hdGraphics;
}

void ToggleHdGraphics()
{
	if (!HdGraphicsAvailable())
		return;
	OptionEntryBoolean &option = GetOptions().Graphics.hdGraphics;
	option.SetValue(!*option);
	if (gbRunGame)
		InitDiabloMsg(*option ? _("HD Graphics: On") : _("HD Graphics: Off"), 1200);
}

void HdGraphicsFrameTick()
{
#if DEVILUTIONX_WEBGPU
	static const bool Stress = GetWebGpuPlatform().queryFlag("hdToggleStress");
	static int Frames = 0;
	static int Toggles = 0;
	if (!Stress || !gbRunGame || Toggles >= 100 || ++Frames % 20 != 0)
		return;
	ToggleHdGraphics();
	++Toggles;
	Log("HD toggle stress: {} of 100", Toggles);
#endif
}

void InitHdGraphicsOption()
{
	OptionEntryBoolean &option = GetOptions().Graphics.hdGraphics;
	if (HdGraphicsAvailable())
		option.flags &= ~OptionEntryFlags::Invisible;
	else
		option.flags |= OptionEntryFlags::Invisible;
}

} // namespace devilution
