// Minimal console backend for the extracted HUD core. No OS input or renderer.
// See LICENSE.md / NOTICE.md for inherited terms and credits.
#include "diablo_hud/layout.hpp"
#include "diablo_hud/persistence.hpp"
#include "diablo_hud/windows.hpp"
#include "diablo_hud/input.hpp"
#include "diablo_hud/frame.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace diablo_hud;
struct ConsoleBackend {
	void draw(std::size_t id, Rect rect) const
	{
		std::cout << "draw id=" << id << " rect=" << rect.position.x << ',' << rect.position.y
			<< ',' << rect.size.width << ',' << rect.size.height << '\n';
	}
	void persist(const Overrides &values) const
	{
		std::cout << "persist command (memory text only)\n" << SerializeElementLayout(values);
	}
};
void Require(bool ok, const char *what)
{
	if (!ok) throw std::runtime_error(what);
}
}
int main()
{
	try {
		Context context;
		context.rendererIdentity=1;
		context.logicalWidth=context.outputWidth=context.rawViewportWidth=1280;
		context.logicalHeight=context.outputHeight=context.rawViewportHeight=720;
		context.viewportWidth=1280;context.viewportHeight=720;context.valid=true;
		ConsoleBackend backend;
		const auto geometry=resolveGeometry(context);
		for (std::size_t id=0;id<geometry.elements.size();++id)
			backend.draw(id,geometry.elements[id]);

		ElementState elements;
		const auto clock=clockDefaultBounds(context.logicalWidth);
		const Point pointer=clock.position+Delta{20,8};
		Require(elements.begin(ClockElement,clock,pointer,ResizeNone,context),"begin Clock move");
		Require(elements.end(pointer+Delta{-200,40},context),"commit Clock move");
		const auto moved=applyOverride(ClockElement,clock,context,elements.overrides());
		backend.draw(ClockElement,moved);backend.persist(elements.overrides());

		// A host output change is a cancel, even when no MOVE precedes release.
		Require(elements.begin(ClockElement,moved,moved.position,ResizeNone,context),"begin pending edit");
		Require(elements.update(moved.position+Delta{20,20},context),"preview pending edit");
		auto dpiChanged=context;dpiChanged.outputScaleX=1.5F;
		Require(!elements.end(moved.position+Delta{20,20},dpiChanged),"DPI release must cancel");
		Require(applyOverride(ClockElement,clock,context,elements.overrides())==moved,"cancel restores committed Clock");
		std::cout << "cancel reason=output-transform changed; no persist command\n";

		FrameContract frame;
		Require(frame.begin(1,1280,720),"begin bounded frame");
		Require(frame.beginPanel(1),"begin native panel capture");
		const Rect body=ClampWindowBody({{12,30},{320,352}},22,1.25F,context);
		backend.draw(1,WindowBounds(body,22,1.25F));
		Require(frame.sealPanel()&&frame.sealPanels(),"seal panel content");
		Require(frame.cursorDrawn()&&frame.lateOverlay(),"cursor precedes late Clock");
		backend.draw(ClockElement,moved);
		Require(frame.prepare(1)&&frame.present(1),"same command-frame identity presents once");
		std::cout << "present commands only; no pixels/window/game/profile/assets\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL HUD_HEADLESS " << error.what() << '\n';
		return 1;
	}
}
