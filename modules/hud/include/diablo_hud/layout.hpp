// Extracted from the Diablo3D / DevilutionX HUD (24080e8), with host-owned
// state and numeric context. See LICENSE.md / NOTICE.md for inherited terms.
#pragma once
#include "types.hpp"
#include "authored.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace diablo_hud {
inline Rect beltSlotRect(Rect belt,int slot) {
    const int left=belt.size.width*slot/8,right=belt.size.width*(slot+1)/8;
    return {belt.position+Delta{left,0},{right-left,belt.size.height}};
}
namespace detail {
struct PhysicalRect {
	float x, y, width, height;
};
constexpr std::array<PhysicalRect, 13> CompactEntries {{
	{ -324, -186, 108, 139 }, { 216, -186, 108, 139 },
	{ -192, -196, 384, 48 }, { 152, -108, 48, 48 },
	{ -200, -140, 344, 112 },
	{ -440, -196, 100, 48 }, { 340, -196, 100, 48 },
	{ 340, -140, 100, 48 }, { -440, -140, 100, 48 },
	{ -440, -84, 100, 48 }, { 340, -84, 100, 48 },
	{ -294, -260, 48, 48 }, { 246, -260, 48, 48 },
}};

inline Rect Unite(Rect a, Rect b)
{
	const int right = std::max(a.position.x + a.size.width, b.position.x + b.size.width);
	const int bottom = std::max(a.position.y + a.size.height, b.position.y + b.size.height);
	a.position.x = std::min(a.position.x, b.position.x);
	a.position.y = std::min(a.position.y, b.position.y);
	a.size = { right - a.position.x, bottom - a.position.y };
	return a;
}

inline Rect AuthoredFrame(const Geometry &geometry)
{
	Rect frame = geometry.elements[2];
	for (size_t index : { size_t { 3 }, size_t { 4 }, size_t { 5 }, size_t { 6 }, size_t { 7 }, size_t { 8 }, size_t { 9 }, size_t { 10 } })
		frame = Unite(frame, geometry.elements[index]);
	const auto &context = geometry.context;
	const int horizontal = std::max(1, 10 * context.logicalHeight / 480);
	const int top = std::max(1, 6 * context.logicalHeight / 480);
	const int right = std::min(context.logicalWidth, frame.position.x + frame.size.width + horizontal);
	frame.position.x = std::max(0, frame.position.x - horizontal);
	frame.size.width = right - frame.position.x;
	frame.position.y = std::max(0, frame.position.y - top);
	frame.size.height += top;
	frame.size.height = std::min(frame.size.height, context.logicalHeight - frame.position.y);
	return frame;
}

inline int LogicalEdge(float physical, float origin, float scale, int limit)
{
	return std::clamp(static_cast<int>(std::lround((physical - origin) / scale)), 0, limit);
}

inline Rect InverseRect(PhysicalRect rect, const Context &context)
{
	const float center = context.outputOriginX + context.viewportWidth / 2;
	const float bottom = context.outputOriginY + context.viewportHeight;
	const int left = LogicalEdge(center + rect.x, context.outputOriginX, context.outputScaleX, context.logicalWidth);
	const int right = LogicalEdge(center + rect.x + rect.width, context.outputOriginX, context.outputScaleX, context.logicalWidth);
	const int top = LogicalEdge(bottom + rect.y, context.outputOriginY, context.outputScaleY, context.logicalHeight);
	const int end = LogicalEdge(bottom + rect.y + rect.height, context.outputOriginY, context.outputScaleY, context.logicalHeight);
	return { { left, top }, { std::max(1, right - left), std::max(1, end - top) } };
}


} // namespace detail
inline Geometry resolveGeometry(const Context &context, const std::array<Rect,13> &authored)
{
	Geometry result;
	result.context = context;
	const bool compact = !context.hasHudOverride && context.valid;
	if (!compact) {
		result.mode = context.hasHudOverride ? GeometryMode::ExplicitAuthoredV1 : GeometryMode::SmallAuthored;
		result.elements = authored;
		for (int i = 0; i < 8; ++i) result.beltSlots[i] = beltSlotRect(result.elements[2], i);
		result.frame = detail::AuthoredFrame(result);
		auto &metrics = result.metrics;
		metrics.uiScale = metrics.designScaleX = metrics.designScaleY = context.logicalHeight / 480.0F;
		metrics.infoGlyphScaleX = metrics.labelGlyphScaleX = metrics.keyGlyphScaleX = metrics.valueGlyphScaleX = metrics.designScaleX;
		metrics.infoGlyphScaleY = metrics.labelGlyphScaleY = metrics.keyGlyphScaleY = metrics.valueGlyphScaleY = metrics.designScaleY;
		metrics.infoPhysicalGlyphHeight = 12 * metrics.infoGlyphScaleY * context.outputScaleY;
		metrics.infoPhysicalLinePitch = metrics.infoPhysicalGlyphHeight;
		return result;
	}
	result.mode = GeometryMode::Compact;
	auto &metrics = result.metrics;
	const float u = std::min({ 1.0F, context.viewportHeight / 1080.0F, context.viewportWidth / 912.0F });
	const bool smallOutput = context.viewportHeight < 720;
	metrics.uiScale = u;
	metrics.designScaleX = u / context.outputScaleX;
	metrics.designScaleY = u / context.outputScaleY;
	metrics.infoPhysicalGlyphHeight = std::max(12.0F, 16 * u);
	metrics.infoPhysicalLinePitch = std::max(smallOutput ? 12.0F : 14.0F, 20 * u);
	metrics.infoPhysicalInset = std::max(smallOutput ? 2.0F : 4.0F, 6 * u);
	const float infoFont = metrics.infoPhysicalGlyphHeight / 12;
	const float labelFont = std::max(12.0F, 18 * u) / 12;
	metrics.infoGlyphScaleX = infoFont / context.outputScaleX;
	metrics.infoGlyphScaleY = infoFont / context.outputScaleY;
	metrics.labelGlyphScaleX = metrics.keyGlyphScaleX = metrics.valueGlyphScaleX = labelFont / context.outputScaleX;
	metrics.labelGlyphScaleY = metrics.keyGlyphScaleY = metrics.valueGlyphScaleY = labelFont / context.outputScaleY;
	metrics.infoLineHeight = static_cast<int>(std::ceil(metrics.infoPhysicalLinePitch / infoFont - 0.001F));
	// Source raster line boxes have integer pitch. Reserve their effective
	// physical pitch, including the 720..810 interval where ceil adds a pixel.
	metrics.infoPhysicalLinePitch = metrics.infoLineHeight * infoFont;
	const int sourceInset = static_cast<int>(std::floor(metrics.infoPhysicalInset / infoFont));
	std::array<detail::PhysicalRect, 13> physical = detail::CompactEntries;
	for (auto &rect : physical) { rect.x *= u; rect.y *= u; rect.width *= u; rect.height *= u; }
	// Five actual line boxes, rather than five nominal glyph heights, must fit.
	float infoHeight = std::max(physical[4].height, 5 * metrics.infoPhysicalLinePitch + 2 * metrics.infoPhysicalInset);
	const float infoBottom = physical[4].y + physical[4].height;
	physical[4].y = infoBottom - infoHeight;
	physical[4].height = infoHeight;
	// Inverting fractional SDL edges can lose one logical pixel. Keep the
	// existing bottom edge and grow upward only when that source buffer would
	// no longer contain all five line boxes plus both actual source insets.
	const int sourceMinimum = 5 * metrics.infoLineHeight + 2 * sourceInset;
	const int logicalMinimum = static_cast<int>(std::ceil(sourceMinimum * metrics.infoGlyphScaleY - 0.001F));
	const int inverseHeight = detail::InverseRect(physical[4], context).size.height;
	if (inverseHeight < logicalMinimum) {
		infoHeight += (logicalMinimum - inverseHeight) * context.outputScaleY;
		physical[4].y = infoBottom - infoHeight;
		physical[4].height = infoHeight;
	}
	const float lift = infoHeight - detail::CompactEntries[4].height * u;
	physical[2].y -= lift;
	// Keep the existing multiplayer controls in the same protected bottom band
	// as the belt when native 352px bodies are open at 640x480.
	if (smallOutput || context.logicalHeight <= 480) {
		physical[11].y = physical[12].y = physical[2].y;
	}
	physical[3].y = physical[4].y + (infoHeight - physical[3].height) / 2;
	for (size_t i = 0; i < physical.size(); ++i) result.elements[i] = detail::InverseRect(physical[i], context);
	for (int i = 0; i < 8; ++i) {
		detail::PhysicalRect slot = physical[2];
		slot.x += slot.width * i / 8;
		slot.width /= 8;
		result.beltSlots[i] = detail::InverseRect(slot, context);
	}
	float left = physical[2].x, top = physical[2].y;
	float right = left + physical[2].width, bottom = top + physical[2].height;
	for (size_t i : { size_t { 3 }, size_t { 4 }, size_t { 5 }, size_t { 6 }, size_t { 7 }, size_t { 8 }, size_t { 9 }, size_t { 10 } }) {
		left = std::min(left, physical[i].x); top = std::min(top, physical[i].y);
		right = std::max(right, physical[i].x + physical[i].width); bottom = std::max(bottom, physical[i].y + physical[i].height);
	}
	result.frame = detail::InverseRect({ left - 16 * u, top - 8 * u, right - left + 32 * u, bottom - top + 12 * u }, context);
	const int sourceHeight = std::max(1, static_cast<int>(result.elements[4].size.height / metrics.infoGlyphScaleY));
	metrics.infoInsetX = sourceInset;
	metrics.infoInsetY = std::min(sourceInset, std::max(0, (sourceHeight - 5 * metrics.infoLineHeight) / 2));
	metrics.infoPhysicalInset = metrics.infoInsetY * infoFont;
	return result;
}

// Authored fallback rectangles are host input: loading layout.ini is not core.
inline Geometry resolveGeometry(const Context &c) { return resolveGeometry(c,authoredDefaults(c)); }
inline Size elementMinimum(std::size_t id,const Context &c,Size physicalMinimum={0,0}) {
    constexpr std::array<Size,ElementCount> minimum{{{64,82},{64,82},{192,24},{32,32},{180,64},{50,20},{50,20},{50,20},
        {50,20},{50,20},{50,20},{32,24},{32,24},{160,9},{100,40},{100,24},{64,12},{160,72}}};
    auto size=minimum[std::min(id,minimum.size()-1)];
    if(id==4){const auto m=resolveGeometry(c).metrics;size.height=std::max(size.height,int(std::ceil(5*m.infoPhysicalLinePitch+2*m.infoPhysicalInset)));}
    size.width=std::max(size.width,physicalMinimum.width);size.height=std::max(size.height,physicalMinimum.height);
    return {std::min(std::max(1,c.logicalWidth),std::max(id==MinimapElement?160:1,int(std::ceil(size.width/std::max(0.001F,c.outputScaleX))))),
        std::min(std::max(1,c.logicalHeight),std::max(id==MinimapElement?72:1,int(std::ceil(size.height/std::max(0.001F,c.outputScaleY)))))};
}
inline Rect clampElement(std::size_t id,Rect r,const Context &c,Size minimum={0,0}) {
    const auto m=elementMinimum(id,c,minimum);const Size limit{std::max(1,c.logicalWidth),std::max(1,c.logicalHeight)};
    r.size.width=std::clamp(r.size.width,std::max(1,m.width),limit.width);r.size.height=std::clamp(r.size.height,std::max(1,m.height),limit.height);
    r.position.x=std::clamp(r.position.x,0,limit.width-r.size.width);r.position.y=std::clamp(r.position.y,0,limit.height-r.size.height);return r;
}
inline NormalizedRect storeOverride(std::size_t id,Rect r,const Context &c,Size minimum={0,0}) {
    r=clampElement(id,r,c,minimum);
    if(id==ClockElement||id==MinimapElement){const auto edge=[](int pixel,int extent){return std::clamp(int((std::int64_t(pixel)*NormalizedUnits+std::max(1,extent)-1)/std::max(1,extent)),0,NormalizedUnits);};
        const int left=edge(r.position.x,c.logicalWidth),top=edge(r.position.y,c.logicalHeight);
        return {left,top,std::max(1,edge(r.position.x+r.size.width,c.logicalWidth)-left),std::max(1,edge(r.position.y+r.size.height,c.logicalHeight)-top)};}
    const auto units=[](int pixel,int extent){return std::clamp(pixel*NormalizedUnits/std::max(1,extent),0,NormalizedUnits);};
    return {units(r.position.x,c.logicalWidth),units(r.position.y,c.logicalHeight),std::max(1,units(r.size.width,c.logicalWidth)),std::max(1,units(r.size.height,c.logicalHeight))};
}
inline Rect applyOverride(std::size_t id,Rect standard,const Context&c,const Overrides&values,Size minimum={0,0}) {
    if(id>=values.size()||!values[id])
        return standard;
    const auto &r=*values[id];
    const Point p{r.x*c.logicalWidth/NormalizedUnits,r.y*c.logicalHeight/NormalizedUnits};
    const Size size=id==ClockElement||id==MinimapElement?Size{(r.x+r.width)*c.logicalWidth/NormalizedUnits-p.x,(r.y+r.height)*c.logicalHeight/NormalizedUnits-p.y}
        :Size{r.width*c.logicalWidth/NormalizedUnits,r.height*c.logicalHeight/NormalizedUnits};
    return clampElement(id,{p,size},c,minimum);
}
inline std::uint8_t resizeEdges(Rect r,Point p,int grip=5) {
    if(!r.contains(p)||grip<=0)return ResizeNone;
    grip=std::min({grip,std::max(1,r.size.width/2),std::max(1,r.size.height/2)});std::uint8_t edges=ResizeNone;
    if(p.x<r.position.x+grip)edges|=ResizeLeft;else if(p.x>=r.position.x+r.size.width-grip)edges|=ResizeRight;
    if(p.y<r.position.y+grip)edges|=ResizeTop;else if(p.y>=r.position.y+r.size.height-grip)edges|=ResizeBottom;
    return edges;
}
inline Rect resizePreview(std::size_t id,Rect original,Point start,Point pointer,std::uint8_t edges,const Context&c,Size minimum={0,0}) {
    Rect r=original;const Delta delta=pointer-start;
    if(edges!=ResizeNone){const Size m=elementMinimum(id,c,minimum);int left=r.position.x,top=r.position.y,right=left+r.size.width,bottom=top+r.size.height;
        if(edges&ResizeLeft)left=std::clamp(left+delta.deltaX,0,right-m.width);else if(edges&ResizeRight)right=std::clamp(right+delta.deltaX,left+m.width,std::max(1,c.logicalWidth));
        if(edges&ResizeTop)top=std::clamp(top+delta.deltaY,0,bottom-m.height);else if(edges&ResizeBottom)bottom=std::clamp(bottom+delta.deltaY,top+m.height,std::max(1,c.logicalHeight));
        r={{left,top},{right-left,bottom-top}};
    }else r.position+=delta;
    return clampElement(id,r,c,minimum);
}

class ElementState {
public:
    const Overrides& overrides()const{return values_;}std::uint64_t generation()const{return generation_;}
    bool dragging()const{return gesture_.has_value();}
    void replace(Overrides values){cancel();values_=std::move(values);++generation_;}
    bool begin(std::size_t id,Rect r,Point pointer,std::uint8_t edges,const Context&c,Size minimum={0,0}) {
        if(gesture_){cancel();return false;}if(id>=ElementCount)return false;
        gesture_=Gesture{id,clampElement(id,r,c,minimum),pointer,std::uint8_t(edges&15),c,values_[id]};return true;
    }
    bool update(Point pointer,const Context&c,Eligibility eligible={},Size minimum={0,0}) {
        if(!gesture_)
            return false;
        if(!eligible.allows()||!sameTransform(gesture_->context,c)){cancel();return false;}
        values_[gesture_->id]=storeOverride(gesture_->id,resizePreview(gesture_->id,gesture_->original,gesture_->start,pointer,gesture_->edges,c,minimum),c,minimum);++generation_;return true;
    }
    // Revalidate the UP even if no MOVE followed a DPI/modal/visibility change.
    bool end(Point pointer,const Context&c,Eligibility eligible={},Size minimum={0,0}) {
        if(!update(pointer,c,eligible,minimum))
            return false;
        gesture_.reset();
        return true;
    }
    bool commit(const Context&c,Eligibility eligible={}) {
        if(!gesture_)return false;
        if(!eligible.allows()||!sameTransform(gesture_->context,c)){cancel();return false;}
        gesture_.reset();return true;
    }
    void cancel(){if(gesture_){values_[gesture_->id]=gesture_->previous;gesture_.reset();++generation_;}}
    bool reset(std::size_t id){if(id>=ElementCount)return false;cancel();values_[id].reset();++generation_;return true;}
    void resetAll(){cancel();values_={};++generation_;}
    bool customized()const{for(const auto&v:values_)if(v)return true;return false;}
    std::optional<std::size_t> dragElement()const{return gesture_?std::optional<std::size_t>(gesture_->id):std::nullopt;}
private:
    struct Gesture{std::size_t id;Rect original;Point start;std::uint8_t edges;Context context;std::optional<NormalizedRect> previous;};
    Overrides values_{};std::optional<Gesture> gesture_;std::uint64_t generation_=0;
};

inline Rect minimapDefaultBounds(int width,int height) {
    width=std::max(1,width);height=std::max(1,height);const int padding=std::min(8,std::max(0,width/32));
    const int w=std::min(std::max(1,width-2*padding),std::clamp(width/4,240,640));
    const int h=std::min(std::max(1,height-2*padding),w/2+28);return {{width-padding-w,padding},{w,h}};
}
inline Rect minimapViewport(Rect bounds) {
    const int inset=std::min(4,std::max(0,(bounds.size.width-1)/2)),header=std::min(24,std::max(0,bounds.size.height-1));
    return {{bounds.position.x+inset,bounds.position.y+header},{std::max(1,bounds.size.width-2*inset),std::max(1,bounds.size.height-header-inset)}};
}
inline int minimapCoverageWidth(Rect content){return std::max(content.size.width,2*content.size.height);}
inline Rect clockDefaultBounds(int width){return {{std::max(0,width-76),6},{76,16}};}
} // namespace diablo_hud
