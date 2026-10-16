// Diablo3D HUD core regression tests. See the module LICENSE.md / NOTICE.md.
// Test the production headers directly; no copied engine fragments or SDL mocks.
#include "diablo_hud/types.hpp"
#include "diablo_hud/layout.hpp"
#include "diablo_hud/persistence.hpp"
#include "diablo_hud/windows.hpp"
#include "diablo_hud/input.hpp"
#include "diablo_hud/frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace diablo_hud;
std::size_t checks = 0;
void Check(bool value, const char *message)
{
	++checks;
	if (!value) throw std::runtime_error(message);
}
void Near(float actual, float expected, const char *message)
{
	Check(std::isfinite(actual) && std::fabs(actual - expected) < 0.0001F, message);
}
Context Viewport(int width = 1280, int height = 720)
{
	Context c;
	c.rendererIdentity = 7;
	c.logicalWidth = c.outputWidth = c.rawViewportWidth = width;
	c.logicalHeight = c.outputHeight = c.rawViewportHeight = height;
	c.rendererLogicalWidth = width; c.rendererLogicalHeight = height;
	c.viewportWidth = static_cast<float>(width); c.viewportHeight = static_cast<float>(height);
	c.valid = true;
	return c;
}
void TypesAndIdentity()
{
	const std::array<const char *, 18> expected {{ "Health", "Mana", "Belt", "Spell", "Info", "Character", "Inventory", "Spells", "Quests", "Map", "Menu", "Chat", "Friendly", "Experience", "Level", "Durability", "Clock", "Minimap" }};
	Check(ElementCount == 18 && ClockElement == 16 && MinimapElement == 17, "stable auxiliary IDs");
	for (std::size_t i = 0; i < expected.size(); ++i)
		Check(std::strcmp(ElementNames[i], expected[i]) == 0, "stable layout names and order");
	Check(std::strcmp(ElementNames[9], "Map") == 0 && 9 != MinimapElement, "Map button and Minimap window have distinct identity");
	const Rect r {{ 10, 20 }, { 5, 6 }};
	Check(r.contains({10, 20}) && r.contains({14, 25}), "rectangle includes top-left and last interior pixel");
	Check(!r.contains({15, 25}) && !r.contains({14, 26}) && !r.contains({9, 20}), "rectangle excludes right/bottom/exterior pixels");
	Check(!Rect{{0,0},{0,1}}.contains({0,0}) && !Rect{{0,0},{1,-1}}.contains({0,0}), "empty rectangles own no pointer");
	Check(Rect{{std::numeric_limits<int>::max()-2,0},{5,1}}.contains({std::numeric_limits<int>::max(),0}), "contains uses wide edge arithmetic");
	auto c = Viewport(); auto changed = c;
	changed.layoutGeneration = 41; changed.customizationGeneration = 88;
	Check(sameTransform(c, changed) && !(c == changed), "preview/cache generations do not invalidate a physical gesture");
	changed = c; ++changed.rendererIdentity; Check(!sameTransform(c, changed), "renderer identity invalidates gesture");
	changed = c; changed.outputScaleX = 1.5F; Check(!sameTransform(c, changed), "DPI invalidates gesture");
	changed = c; changed.outputOriginX = 12; Check(!sameTransform(c, changed), "letterbox origin invalidates gesture");
	changed = c; ++changed.rawViewportY; Check(!sameTransform(c, changed), "raw viewport invalidates gesture");
}
void ElementCodec()
{
	const auto old = ParseElementLayout("D3DHUDLAYOUT1\r\nHealth 100 200 300 400\r\nMap 1000 2000 500 300\r\n");
	Check(old.has_value() && (*old)[0] && (*old)[9], "legacy names and CRLF accepted");
	Check(!(*old)[ClockElement] && !(*old)[MinimapElement], "old files do not invent new overrides");
	Overrides all;
	for (std::size_t i = 0; i < all.size(); ++i) all[i] = NormalizedRect{int(i * 10),int(i * 20),300,400};
	const auto encoded = SerializeElementLayout(all);
	const auto decoded = ParseElementLayout(encoded);
	Check(decoded && *decoded == all, "all eighteen records roundtrip exactly");
	Check(encoded.find("Clock ") != std::string::npos && encoded.find("Minimap ") != std::string::npos, "auxiliary names serialized independently");
	Check(!ParseElementLayout("D3DHUDLAYOUT2\n"), "unknown version rejected");
	Check(!ParseElementLayout("D3DHUDLAYOUT1\nUnknown 0 0 1 1\n"), "unknown name rejects whole file");
	Check(!ParseElementLayout("D3DHUDLAYOUT1\nClock 0 0 100 100\nClock 1 1 100 100\n"), "duplicate rejects whole file");
	Check(!ParseElementLayout("D3DHUDLAYOUT1\nHealth 0 0 100 100\nMana 0 0 100 100 extra\n"), "malformed later row cannot leak partial acceptance");
	Check(!ParseElementLayout("D3DHUDLAYOUT1\nMinimap 9999 0 2 100\n"), "normalized opposite edge cannot overflow");
	Check(!ParseElementLayout("D3DHUDLAYOUT1\nClock -1 0 1 1\n"), "negative position rejected");
	Check(!ParseElementLayout("D3DHUDLAYOUT1\nClock 0 0 0 1\n"), "zero size rejected");
	Check(!ParseElementLayout(std::string(8193, 'x')), "codec has bounded input");
	const Overrides empty;
	Check(SerializeElementLayout(empty) == "D3DHUDLAYOUT1\n", "empty layout has canonical version header");
}
void WindowCodec()
{
	const auto legacy = ParseWindowLayout("1 20 30\n3 200 220 1.37\n");
	Check(legacy && (*legacy)[1] && (*legacy)[3], "legacy window rows accepted");
	Near((*legacy)[1]->scale, 0, "omitted window scale retains auto-scale sentinel");
	Near((*legacy)[3]->scale, 1.37F, "explicit window scale accepted");
	const auto repeated = ParseWindowLayout("1 20 30 1.4\n1 40 50\n");
	Check(repeated && (*repeated)[1]->position == Point{40,50}, "legacy duplicate row replaces position");
	Near((*repeated)[1]->scale, 1.4F, "legacy duplicate without scale retains prior scale");
	const auto permissive = ParseWindowLayout("bad row\n0 1 2 1.5\n7 1 2 1.5\n2 -1 3\n1 9 8 99\n");
	Check(permissive && (*permissive)[1] && !(*permissive)[0] && !(*permissive)[2] && !(*permissive)[6], "legacy invalid window rows are ignored");
	Near((*permissive)[1]->scale, 2, "legacy window scale clamps at two");
	Check(!ParseWindowLayout(std::string(8193, 'x')), "window codec has bounded input");
	const auto roundtrip = ParseWindowLayout(SerializeWindowLayout(*legacy));
	Check(roundtrip && (*roundtrip)[1]->position == (*legacy)[1]->position && (*roundtrip)[3]->position == (*legacy)[3]->position, "window positions roundtrip");
	Near((*roundtrip)[1]->scale, 0, "auto sentinel roundtrips");
	Near((*roundtrip)[3]->scale, 1.37F, "window scale roundtrips");
}
void WindowGeometry()
{
	const auto c = Viewport(); const Size body {320,352}; constexpr int header = 22;
	Near(UniformWindowScale(0, body, header, c), 1.25F, "fresh tall viewport uses existing auto scale");
	Near(UniformWindowScale(1.4F, body, header, c), 1.4F, "explicit scale has no first-resize jump");
	Near(UniformWindowScale(2, body, header, c, false), 1, "disabled floating window remains native scale");
	Near(ContinuousWindowScale(1.25F,{0,0},body,header,c),1.25F,"zero movement preserves original resize scale");
	Check(ContinuousWindowScale(1.25F,{45,0},body,header,c)>1.25F,"signed horizontal outward motion grows");
	Check(ContinuousWindowScale(1.5F,{0,-40},body,header,c)<1.5F,"signed vertical inward motion shrinks");
	const auto a = ContinuousWindowScale(1.5F,{40,-39},body,header,c);
	const auto b = ContinuousWindowScale(1.5F,{40,-40},body,header,c);
	Check(std::fabs(a-b)<0.01F,"mixed-sign diagonal remains continuous at axis crossover");
	const auto small = Viewport(640,480);
	const auto scale = UniformWindowScale(2,body,header,small);
	Near(scale,480.0F/374.0F,"uniform scale fits body plus header");
	const auto clamped = ClampWindowBody({{639,479},body},header,scale,small);
	const auto bounds = WindowBounds(clamped,header,scale);
	Check(bounds.position.x>=0&&bounds.position.y>=0&&bounds.position.x+bounds.size.width<=640&&bounds.position.y+bounds.size.height<=480,"rounded 640 outer rect never overflows one pixel");
	Check(bounds.position.y+bounds.size.height==480,"fitted outer bottom shares exact viewport edge");
	const Rect native{{13,23},{53,47}},visual{{107,61},{73,69}};
	Check(NativeToVisual({33,43},native,visual)==Point{135,91},"ceil forward mapping chooses first inverse interval pixel");
	for(int y=0;y<native.size.height;++y)for(int x=0;x<native.size.width;++x) {
		const Point n=native.position+Delta{x,y};
		const auto p=NativeToVisual(n,native,visual);
		Check(visual.contains(p)&&VisualToNative(p,native,visual)==n,"every native content pixel maps to an exact visual inverse");
		Check(SurfaceSourcePoint(p,native,visual)==n,"display resampling and functional input choose the same source pixel");
	}
	Check(SurfaceSourcePoint({135,91},native,visual)==Point{33,43},"display sample has an independent fixed pixel anchor");
	Check(NativeToVisual({12,23},native,visual)==Point{12,23},"outside native points are left unchanged");
	Check(VisualToNative({4,5},{{0,0},{0,1}},visual)==Point{4,5},"invalid mapping is declined without divide by zero");
}

void WindowOwnership()
{
	WindowStack stack;
	stack.records = {
		{1,{{0,0},{100,122}},{{10,10},{125,153}},{{0,22},{100,100}},true},
		{2,{{100,0},{100,122}},{{20,20},{150,183}},{{100,22},{100,100}},true},
		{3,{{0,0},{100,122}},{{500,200},{100,122}},{{0,22},{100,100}},true},
	};
	Check(stack.hit({80,90},true)==2,"topmost visible body owns overlap");
	Check(stack.hit({60,45})==2&&!stack.hit({60,45},true),"foreground header blocks a body behind it");
	Check(stack.close(2)&&!stack.isOpen(2),"close changes transient visible state");
	Check(stack.hit({60,45},true)==1,"closed foreground window exposes underlying body");
	Check(!stack.focus(2),"closed window cannot acquire focus");
	const auto native=stack.records[1].native,visual=stack.records[1].visual,body=stack.records[1].body;
	Check(stack.reopen(2)&&stack.isOpen(2)&&stack.records.back().id==2,"reopen focuses preserved window");
	Check(stack.records.back().native==native&&stack.records.back().visual==visual&&stack.records.back().body==body,"close/reopen preserves native and visual layout");
	Check(stack.focus(1)&&stack.records.back().id==1,"focus moves selected identity to paint top");
	Check(stack.records[0].id==3&&stack.records[1].id==2,"focus preserves relative order of all other windows");
	Check(!stack.reopen(99)&&!stack.close(99)&&!stack.focus(99),"unsupported identities do not create windows");
	Check(!stack.hit({900,600}),"outside pointer has no window owner");
}

void GeometryAndNormalizedEdges()
{
	const auto full = resolveGeometry(Viewport(1920,1080));
	Check(full.mode == GeometryMode::Compact, "valid unmodified geometry selects accepted compact table");
	Check(full.elements[0] == Rect{{636,894},{108,139}}, "FullHD health golden rectangle");
	Check(full.elements[2] == Rect{{768,884},{384,48}}, "FullHD belt golden rectangle");
	Check(full.elements[4] == Rect{{760,940},{344,112}}, "FullHD info golden rectangle");
	Near(full.metrics.infoPhysicalGlyphHeight,16,"FullHD native twelve-pixel glyph scales to sixteen physical pixels");
	for (int height : {480,720,721,730,810,811,1080}) for (float scale : {1.0F,1.25F,1.5F}) {
		auto c = Viewport(height * 16 / 9,height);
		c.outputScaleX = c.outputScaleY = scale;
		c.viewportWidth = c.logicalWidth * scale; c.viewportHeight = c.logicalHeight * scale;
		c.outputOriginX=17; c.outputOriginY=9;
		const auto g = resolveGeometry(c);
		const auto &m = g.metrics;
		const int sourceHeight = static_cast<int>(g.elements[4].size.height / m.infoGlyphScaleY);
		Check(sourceHeight >= 5*m.infoLineHeight+2*m.infoInsetY,"five actual source line boxes and insets remain within info content");
		// R4 GetD3dHudGeometry applies overrides then rebuilds slots from the
		// final integer belt. resolveGeometry's physical fractional slots are
		// an intermediate; their independently rounded edges need not adjoin.
		std::array<Rect,8> slots{};
		for(std::size_t i=0;i<slots.size();++i)slots[i]=beltSlotRect(g.elements[2],static_cast<int>(i));
		Check(slots.front().position.x==g.elements[2].position.x&&slots.back().position.x+slots.back().size.width==g.elements[2].position.x+g.elements[2].size.width,"final belt partitions have the same outer edges as content");
		for (std::size_t i=1;i<slots.size();++i)
			Check(slots[i-1].position.x+slots[i-1].size.width==slots[i].position.x,"final adjacent belt slots share one edge without gap or overlap");
	}
	// Independent fixed golden at the failing fractional context. Binary float
	// rounding produces a one-pixel seam only in the raw resolver intermediate;
	// the accepted R4 final integer partition removes it without changing core.
	auto fractionalContext=Viewport(853,480);
	fractionalContext.outputScaleX=fractionalContext.outputScaleY=1.25F;
	fractionalContext.viewportWidth=1066.25F;fractionalContext.viewportHeight=600;
	fractionalContext.outputOriginX=17;fractionalContext.outputOriginY=9;
	const auto fractional=resolveGeometry(fractionalContext);
	Check(fractional.elements[2].position.x==341&&fractional.elements[2].size.width==171,"fractional belt outer golden matches R4 physical-edge inversion");
	Check(fractional.beltSlots[0].position.x==341&&fractional.beltSlots[0].size.width==21&&fractional.beltSlots[1].position.x==363,"raw fractional slot golden retains accepted intermediate rounding");
	const std::array<int,8> goldenStarts{{341,362,383,405,426,447,469,490}};
	const std::array<int,8> goldenWidths{{21,21,22,21,21,22,21,22}};
	for(std::size_t i=0;i<goldenStarts.size();++i){
		const auto slot=beltSlotRect(fractional.elements[2],static_cast<int>(i));
		Check(slot.position.x==goldenStarts[i]&&slot.size.width==goldenWidths[i],"final integer belt slots match independent fixed native golden");
	}
	auto c=Viewport(640,480);c.hasHudOverride=true;
	std::array<Rect,13> authored{};
	for (std::size_t i=0;i<authored.size();++i) authored[i]={{20+int(i)*3,350},{56,22}};
	const auto supplied=resolveGeometry(c,authored);
	Check(supplied.mode==GeometryMode::ExplicitAuthoredV1&&supplied.elements==authored,"authored host rectangles pass through without a second design scale");
	Overrides values;
	const Rect legacy{{11,13},{57,23}};
	values[5]=storeOverride(5,legacy,c);
	Check(*values[5]==NormalizedRect{171,270,890,479},"IDs zero through fifteen retain old normalized position-size math");
	Check(applyOverride(5,legacy,c,values)==Rect{{10,12},{56,22}},"legacy truncation semantics remain unchanged");
	const Rect clock{{11,13},{76,16}},map{{11,13},{160,400}};
	values[ClockElement]=storeOverride(ClockElement,clock,c);
	values[MinimapElement]=storeOverride(MinimapElement,map,c);
	Check(applyOverride(ClockElement,{},c,values)==clock&&applyOverride(MinimapElement,{},c,values)==map,"new Clock and Minimap edge records reload every pixel exactly");
	const auto encoded=SerializeElementLayout(values);
	const auto decoded=ParseElementLayout(encoded);
	Check(decoded&&applyOverride(ClockElement,{},c,*decoded)==clock&&applyOverride(MinimapElement,{},c,*decoded)==map,"auxiliary pixel anchors survive codec restart");
	Check(minimapViewport({{560,120},{160,400}})==Rect{{564,144},{152,372}},"tall narrow content has fixed native header and clipping inset");
	Check(minimapCoverageWidth({{564,144},{152,372}})==744,"tall narrow enumeration considers both visible vertical halves");
	Check(clockDefaultBounds(640)==Rect{{564,6},{76,16}},"Clock keeps native default position and content size");
}

void EightResizeEdges()
{
	const auto c=Viewport();const Rect area{{100,100},{300,200}};
	struct Case {std::uint8_t edge;Point start;Delta delta;Rect expected;};
	const std::array<Case,8> cases{{
		{ResizeLeft,{101,180},{-50,0},{{50,100},{350,200}}},
		{ResizeRight,{398,180},{50,0},{{100,100},{350,200}}},
		{ResizeTop,{200,101},{0,-40},{{100,60},{300,240}}},
		{ResizeBottom,{200,298},{0,40},{{100,100},{300,240}}},
		{ResizeLeft|ResizeTop,{101,101},{-50,-40},{{50,60},{350,240}}},
		{ResizeRight|ResizeTop,{398,101},{50,-40},{{100,60},{350,240}}},
		{ResizeLeft|ResizeBottom,{101,298},{-50,40},{{50,100},{350,240}}},
		{ResizeRight|ResizeBottom,{398,298},{50,40},{{100,100},{350,240}}},
	}};
	for (const auto &test:cases) {
		Check(resizeEdges(area,test.start)==test.edge,"physical grip resolves the intended edge or corner");
		Check(resizePreview(MinimapElement,area,test.start,test.start+test.delta,test.edge,c)==test.expected,"all eight resize directions retain fixed opposite anchors");
		ElementState state;
		Check(state.begin(MinimapElement,area,test.start,test.edge,c)&&state.end(test.start+test.delta,c),"edge gesture commits through actual release");
		Check(applyOverride(MinimapElement,{},c,state.overrides())==test.expected,"accepted edge gesture persists exact visible rectangle");
	}
	Check(resizePreview(MinimapElement,area,{100,180},{1100,180},ResizeLeft,c)==Rect{{240,100},{160,200}},"left edge clamps to minimum and keeps original right anchor");
	Check(resizePreview(MinimapElement,area,{399,180},{-601,180},ResizeRight,c)==Rect{{100,100},{160,200}},"right edge clamps to minimum and keeps original left anchor");
	Check(resizeEdges({{0,0},{4,4}},{3,3},5)==(ResizeRight|ResizeBottom),"oversized grip clamps before selecting tiny-window corner");
	Check(resizeEdges(area,{101,101},0)==ResizeNone&&resizeEdges(area,{101,101},-1)==ResizeNone,"nonpositive grips decline resize");
	Check(resizeEdges(area,{400,200})==ResizeNone,"outside edge never owns resize");
}

void GestureCommitAndRollback()
{
	const auto c=Viewport(640,480);const Rect original{{200,100},{76,16}};const Point start{220,108};
	Overrides initial;initial[ClockElement]=storeOverride(ClockElement,original,c);
	initial[MinimapElement]=storeOverride(MinimapElement,{{400,30},{160,300}},c);
	const auto originalBytes=SerializeElementLayout(initial);
	ElementState state;state.replace(initial);
	std::size_t persisted=0;std::string bytes=originalBytes;
	const auto commit=[&](bool accepted){if(accepted){++persisted;bytes=SerializeElementLayout(state.overrides());}};
	Check(state.begin(ClockElement,original,start,ResizeNone,c),"valid gesture captures original snapshot");
	Check(state.update(start+Delta{-50,25},c)&&state.dragging(),"MOVE changes preview without ending gesture");
	Check(bytes==originalBytes&&persisted==0,"preview produces no persistence effect");
	state.cancel();Check(state.overrides()==initial&&!state.dragging()&&bytes==originalBytes,"cancel restores exact previous override and stored bytes");
	Check(state.begin(ClockElement,original,start,ResizeNone,c),"new gesture begins after cancellation");
	commit(state.end(start+Delta{-50,25},c));
	Check(persisted==1&&!state.dragging()&&applyOverride(ClockElement,{},c,state.overrides())==Rect{{150,125},{76,16}},"UP without prior MOVE applies and persists final pointer once");
	const auto committed=state.overrides();const auto committedBytes=bytes;
	std::vector<Context> changes;
	auto changed=c;changed.outputScaleX=1.5F;changes.push_back(changed);
	changed=c;changed.outputOriginX=15;changes.push_back(changed);
	changed=c;++changed.rawViewportX;changes.push_back(changed);
	changed=c;++changed.rawViewportHeight;changes.push_back(changed);
	changed=c;++changed.outputWidth;changes.push_back(changed);
	changed=c;++changed.rendererIdentity;changes.push_back(changed);
	for (const auto &context:changes) {
		state.replace(committed);const Rect accepted=applyOverride(ClockElement,{},c,committed);
		Check(state.begin(ClockElement,accepted,start,ResizeNone,c)&&state.update(start+Delta{30,30},c),"transform-cancel case has an actual pending preview");
		commit(state.end(start+Delta{30,30},context));
		Check(state.overrides()==committed&&!state.dragging()&&bytes==committedBytes&&persisted==1,"changed physical context plus UP rolls back without another MOVE or write");
	}
	for (int cause=0;cause<5;++cause) {
		Eligibility eligible; if(cause==0)eligible.enabled=false;else if(cause==1)eligible.focused=false;else if(cause==2)eligible.modal=true;else if(cause==3)eligible.heldItem=true;else eligible.visible=false;
		state.replace(committed);const Rect accepted=applyOverride(ClockElement,{},c,committed);
		Check(state.begin(ClockElement,accepted,start,ResizeNone,c)&&state.update(start+Delta{20,20},c),"eligibility-cancel case previews before ownership changes");
		commit(state.end(start+Delta{20,20},c,eligible));
		Check(state.overrides()==committed&&persisted==1&&bytes==committedBytes,"focus/modal/held/hidden/disabled release cancels with no persistence");
	}
	state.replace(initial);Check(state.begin(ClockElement,original,start,ResizeNone,c)&&state.update(start+Delta{20,20},c),"repeated-down case has a pending preview");
	Check(!state.begin(MinimapElement,{{400,30},{160,300}},{420,50},ResizeNone,c)&&state.overrides()==initial&&!state.dragging(),"second DOWN cancels old preview instead of adopting it as origin");
	Check(state.begin(ClockElement,original,start,ResizeNone,c)&&state.update(start+Delta{20,20},c),"mixed-button reset has pending edit A");
	Check(state.reset(MinimapElement)&&state.overrides()[ClockElement]==initial[ClockElement]&&!state.overrides()[MinimapElement],"RMB reset B restores pending LMB A before reset effect");
	Check(!state.end(start+Delta{20,20},c),"later LMB release cannot commit cancelled mixed-button preview");
	state.replace(initial);Check(state.begin(MinimapElement,{{400,30},{160,300}},{420,50},ResizeNone,c)&&state.update({390,80},c),"Minimap visibility cancellation starts from actual preview");
	Eligibility hidden;hidden.visible=false;
	Check(!state.end({390,80},c,hidden)&&state.overrides()==initial,"host map hidden/type transition cancels ID17 on UP");
	Check(state.begin(ClockElement,original,start,ResizeNone,c)&&state.update(start+Delta{20,20},c),"profile replacement starts with pending preview");
	Overrides nextProfile;state.replace(nextProfile);
	Check(!state.dragging()&&!state.customized()&&bytes==committedBytes,"explicit profile replacement drops pending state without a save");
	Check(!state.begin(ElementCount,original,start,ResizeNone,c),"invalid identity cannot start a gesture");
	state.replace(initial);
	Check(state.begin(ClockElement,original,start,ResizeNone,c)&&state.update(start+Delta{-40,20},c),"explicit commit has the last dispatched preview");
	const auto generation=state.generation();const auto dispatched=state.overrides();
	Check(state.commit(c)&&state.overrides()==dispatched&&state.generation()==generation&&!state.dragging(),"adapter commit does not synthesize a second MOVE or generation");
	Check(!state.commit(c),"explicit commit fires once");
	state.replace(initial);
	Check(state.begin(ClockElement,original,start,ResizeNone,c)&&state.update(start+Delta{-40,20},c),"late context commit starts with preview");
	auto newDpi=c;newDpi.outputScaleY=1.5F;
	Check(!state.commit(newDpi)&&state.overrides()==initial,"explicit commit still revalidates physical output transform");
}

void PhysicalOwnerAndPointerScope()
{
	std::vector<WindowRecord> windows{
		{1,{{10,10},{100,122}},{{100,100},{150,183}},{{10,32},{100,100}},true},
		{2,{{200,10},{100,122}},{{110,110},{150,183}},{{200,32},{100,100}},false},
	};
	const Overlay clock{ClockElement,{{14,34},{20,16}},true};
	const Overlay map{MinimapElement,{{300,30},{160,300}},true};
	const std::vector<Overlay> elements{{9,{{300,30},{160,300}},true},{5,{{600,400},{60,20}},true}};
	const Point physical{110,140};
	auto owner=ResolvePhysicalOwner(physical,windows,clock,map,elements);
	Check(owner.kind==OwnerKind::WindowBody&&owner.id==1,"physical point resolves visible native panel before lower modules");
	Check(ResolvePhysicalOwner({15,35},windows,clock,map,elements).kind==OwnerKind::Clock,"visible Clock is physical topmost overlay");
	Check(ResolvePhysicalOwner({310,40},windows,clock,map,elements).kind==OwnerKind::Minimap,"Minimap window wins over overlapping Map utility button");
	Check(ResolvePhysicalOwner({610,410},windows,clock,map,elements).kind==OwnerKind::Element,"lower native element owns uncovered point");
	Check(ResolvePhysicalOwner(physical,windows,clock,map,elements,true).kind==OwnerKind::Modal,"native modal ownership precedes all movable overlays");
	Check(!ResolvePhysicalOwner(physical,windows,clock,map,elements,false,true).owns(),"relative capture declines physical UI picking until host releases capture");
	PointerState state;state.pointer=physical;
	{
		PointerScope outer(state,windows);
		Check(state.pointer==Point{16,36}&&IsNativePointer(state),"outer scope converts physical pointer once to fixed native anchor");
		Check(clock.bounds.contains(state.pointer)&&!clock.bounds.contains(physical),"inverse regression witness numerically collides with physical Clock");
		Check(CapturedWindowAt(state,true)==1,"captured native identity owns numerical Clock collision");
		windows[1].visible=true;FocusWindows(windows,2);
		{
			PointerScope inner(state,windows);
			Check(state.depth==2&&state.pointer==Point{16,36}&&CapturedWindowAt(state,true)==1,"nested handler retains captured identity despite new focus/order");
		}
		Check(state.depth==1&&state.pointer==Point{16,36},"inner scope does not remap or release outer pointer");
		Check(!CapturedWindowAt(state,true,false)&&state.pointer==Point{16,36},"host reports closed captured owner without remapping into another window or Clock");
	}
	Check(state.depth==0&&state.pointer==physical&&!state.captured&&!IsNativePointer(state),"outer scope restores exact physical point and clears native owner");
	const Overlay overlappingClock{ClockElement,{{105,130},{20,16}},true};
	state.pointer=physical;
	const bool clockOwns=ResolvePhysicalOwner(physical,windows,overlappingClock).kind==OwnerKind::Clock;
	Check(clockOwns&&FindWindowHit(windows,physical).has_value(),"physical Clock witness overlaps a real selectable window");
	{
		PointerScope clockScope(state,windows,clockOwns);
		Check(state.pointer==physical&&!IsNativePointer(state)&&!CapturedWindowAt(state),"Clock owner keeps pointer physical through native scopes");
	}
	state.pointer=physical;
	try {PointerScope scope(state,windows);throw std::runtime_error("owned scope unwind");}
	catch(const std::runtime_error &) {}
	Check(state.depth==0&&state.pointer==physical&&!state.captured,"pointer scope restores state on exceptions");
	const std::vector<WindowRecord> headers{
		{1,{{0,0},{100,122}},{{10,10},{125,153}},{{0,22},{100,100}},true},
		{2,{{100,0},{100,122}},{{20,20},{150,183}},{{100,22},{100,100}},true},
	};
	owner=ResolvePhysicalOwner({60,45},headers);
	Check(owner.kind==OwnerKind::WindowHeader&&owner.id==2,"top header gets consuming ownership instead of background body");
}

void MixedInputPairs()
{
	InputPairs pairs;ElementState state;const auto c=Viewport();const Rect area{{200,200},{76,16}};const Point start{220,208};
	pairs.own(1);Check(state.begin(ClockElement,area,start,ResizeNone,c)&&state.update(start+Delta{20,20},c),"LMB owns actual edit preview");
	pairs.own(3);Check(state.reset(MinimapElement),"second RMB reset cancels preview before its effect");
	Check(pairs.owns(1)&&pairs.owns(3)&&pairs.release(3),"second button release consumes only its own pair");
	Check(pairs.owns(1)&&pairs.release(1)&&!state.end(start+Delta{20,20},c),"first release is still consumed but cannot commit cancelled preview");
	Check(pairs.mask()==0&&!pairs.release(1)&&!pairs.release(2),"owned release fires once and unrelated button is not adopted");
	pairs.own(0);pairs.own(32);Check(pairs.mask()==0,"invalid button identities cannot shift or own bits");
	pairs.own(31);Check(pairs.mask()==(std::uint32_t(1)<<30)&&pairs.release(31),"upper valid button identity retains exact mask");
	pairs.own(1);pairs.own(3);pairs.cancel();Check(pairs.mask()==0,"explicit input suspension clears transient pairs");
}

void FrameSequence()
{
	FrameContract frame;
	Check(!frame.begin(1,0,480)&&!frame.begin(1,640,-1)&&!frame.begin(1,8192,8192),"invalid or unbounded frames are declined before capture");
	Check(frame.begin(42,640,480)&&frame.phase()==FramePhase::Capturing,"bounded frame begins with identity");
	Check(frame.beginPanel(0)&&!frame.beginPanel(2)&&!frame.sealPanels(),"one captured panel must seal before another or the frame");
	Check(frame.sealPanel()&&frame.beginPanel(2)&&frame.sealPanel(),"panel zero identity and subsequent capture seal independently");
	Check(!frame.cursorDrawn()&&!frame.prepare(42)&&!frame.present(42),"early cursor/prepare/present cannot bypass panel seal");
	Check(frame.sealPanels()&&!frame.beginPanel(3),"panel seal ends normal capture phase");
	Check(!frame.lateOverlay()&&frame.cursorDrawn()&&frame.preservesCursor(),"cursor checkpoint precedes late overlays");
	Check(frame.lateOverlay()&&frame.lateOverlay(),"multiple late overlays share the existing frame");
	Check(!frame.prepare(43)&&frame.prepare(42),"prepared output must use captured frame identity");
	Check(!frame.present(43)&&frame.present(42)&&!frame.present(42),"presentation occurs once for matching prepared identity");
	frame.invalidate();Check(frame.phase()==FramePhase::Empty&&!frame.preservesCursor()&&!frame.recover(),"new lifecycle drops stale identity and cursor marker");
	Check(frame.begin(9,640,480)&&frame.sealPanels()&&frame.cursorDrawn()&&frame.lateOverlay()&&frame.recover(),"late overlap recovery is modeled after actual cursor checkpoint");
	Check(frame.preservesCursor()&&frame.phase()==FramePhase::Recovered,"recovery retains fact that native cursor was already painted");
	Check(!frame.present(9),"recovery status alone cannot claim presenter success");
	FrameContract transported;
	Check(transported.begin(77,640,480)&&transported.beginPanel(1)&&transported.materialize(),"first panel can materialize its transport during capture");
	Check(transported.materialized()&&transported.phase()==FramePhase::Capturing&&!transported.beginPanel(2),"materialization keeps current open recipe and frame phase");
	Check(transported.sealPanel()&&transported.beginPanel(2)&&transported.sealPanel(),"materialized first panel does not prevent later scaled panel recipe");
	Check(transported.sealPanels()&&transported.cursorDrawn()&&transported.lateOverlay()&&transported.materialize(),"late transport recovery occurs after cursor paint");
	Check(transported.phase()==FramePhase::LateOverlay&&transported.preservesCursor(),"transport materialization retains late phase and cursor checkpoint");
	Check(transported.prepare(77)&&transported.present(77),"materialized frame still prepares and presents in ordinary order");
	Check(!transported.materialize(),"presented frame cannot acquire new materialization work");
	transported.invalidate();Check(!transported.materialized(),"new frame invalidation clears transport fallback state");
}
}
int main()
{
	try {
		TypesAndIdentity(); ElementCodec(); WindowCodec(); WindowGeometry(); WindowOwnership();
		GeometryAndNormalizedEdges(); EightResizeEdges(); GestureCommitAndRollback();
		PhysicalOwnerAndPointerScope(); MixedInputPairs(); FrameSequence();
		std::cout << "PASS HUD_CORE checks=" << checks << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "FAIL HUD_CORE after=" << checks << " reason=" << error.what() << '\n';
		return 1;
	}
}
