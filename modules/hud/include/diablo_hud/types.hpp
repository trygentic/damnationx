// Derived from the Diablo3D / DevilutionX HUD. Modified for explicit host
// state and standalone use. See LICENSE.md and NOTICE.md; notices are retained.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>

namespace diablo_hud {
struct Point { int x=0,y=0; };
struct Size { int width=0,height=0; };
struct Delta { int deltaX=0,deltaY=0; };
struct Rect {
    Point position{}; Size size{};
    bool contains(Point p) const { return size.width>0 && size.height>0 && p.x>=position.x && p.y>=position.y
        && std::int64_t(p.x)<std::int64_t(position.x)+size.width && std::int64_t(p.y)<std::int64_t(position.y)+size.height; }
};
inline bool operator==(Point a,Point b){return a.x==b.x&&a.y==b.y;}
inline bool operator!=(Point a,Point b){return !(a==b);}
inline bool operator==(Size a,Size b){return a.width==b.width&&a.height==b.height;}
inline bool operator!=(Size a,Size b){return !(a==b);}
inline bool operator==(Rect a,Rect b){return a.position==b.position&&a.size==b.size;}
inline bool operator!=(Rect a,Rect b){return !(a==b);}
inline Point operator+(Point p,Delta d){return {p.x+d.deltaX,p.y+d.deltaY};}
inline Point operator-(Point p,Delta d){return {p.x-d.deltaX,p.y-d.deltaY};}
inline Delta operator-(Point a,Point b){return {a.x-b.x,a.y-b.y};}
inline Point &operator+=(Point &p,Delta d){p=p+d;return p;}
inline Delta &operator+=(Delta &p,Delta d){p.deltaX+=d.deltaX;p.deltaY+=d.deltaY;return p;}

// Numeric output transform. The host supplies identity, never an SDL object.
struct Context {
    std::uintptr_t rendererIdentity=0;
    int logicalWidth=1,logicalHeight=1,outputWidth=1,outputHeight=1;
    int rendererLogicalWidth=0,rendererLogicalHeight=0;
    int rawViewportX=0,rawViewportY=0,rawViewportWidth=1,rawViewportHeight=1;
    float outputScaleX=1,outputScaleY=1,outputOriginX=0,outputOriginY=0,viewportWidth=1,viewportHeight=1;
    std::uint64_t layoutGeneration=0,customizationGeneration=0;
    bool hasHudOverride=false,integerScale=false,valid=false;
};
inline bool operator==(const Context&a,const Context&b){return std::tie(a.rendererIdentity,a.logicalWidth,a.logicalHeight,a.outputWidth,a.outputHeight,
    a.rendererLogicalWidth,a.rendererLogicalHeight,a.rawViewportX,a.rawViewportY,a.rawViewportWidth,a.rawViewportHeight,
    a.outputScaleX,a.outputScaleY,a.outputOriginX,a.outputOriginY,a.viewportWidth,a.viewportHeight,a.layoutGeneration,a.customizationGeneration,
    a.hasHudOverride,a.integerScale,a.valid)==std::tie(b.rendererIdentity,b.logicalWidth,b.logicalHeight,b.outputWidth,b.outputHeight,
    b.rendererLogicalWidth,b.rendererLogicalHeight,b.rawViewportX,b.rawViewportY,b.rawViewportWidth,b.rawViewportHeight,
    b.outputScaleX,b.outputScaleY,b.outputOriginX,b.outputOriginY,b.viewportWidth,b.viewportHeight,b.layoutGeneration,b.customizationGeneration,
    b.hasHudOverride,b.integerScale,b.valid);}
inline bool sameTransform(Context a,Context b){a.layoutGeneration=b.layoutGeneration;a.customizationGeneration=b.customizationGeneration;return a==b;}
inline constexpr std::size_t ElementCount=18,ClockElement=16,MinimapElement=17;
inline constexpr int NormalizedUnits=10000;
inline constexpr std::array<const char*,ElementCount> ElementNames{{"Health","Mana","Belt","Spell","Info","Character","Inventory","Spells","Quests","Map","Menu","Chat","Friendly","Experience","Level","Durability","Clock","Minimap"}};
enum ResizeEdge : std::uint8_t { ResizeNone=0,ResizeLeft=1,ResizeRight=2,ResizeTop=4,ResizeBottom=8 };
struct NormalizedRect { int x=0,y=0,width=0,height=0; };
inline bool operator==(NormalizedRect a,NormalizedRect b){return std::tie(a.x,a.y,a.width,a.height)==std::tie(b.x,b.y,b.width,b.height);}
using Overrides=std::array<std::optional<NormalizedRect>,ElementCount>;
struct WindowPreference { Point position{};float scale=0; };
using WindowPreferences=std::array<std::optional<WindowPreference>,7>;
struct Eligibility { bool enabled=true,focused=true,modal=false,heldItem=false,visible=true; bool allows()const{return enabled&&focused&&!modal&&!heldItem&&visible;} };

enum class GeometryMode : std::uint8_t { SmallAuthored,ExplicitAuthoredV1,Compact };
struct Metrics {
    float designScaleX=1,designScaleY=1,infoGlyphScaleX=1,infoGlyphScaleY=1,labelGlyphScaleX=1,labelGlyphScaleY=1;
    float keyGlyphScaleX=1,keyGlyphScaleY=1,valueGlyphScaleX=1,valueGlyphScaleY=1;
    int infoLineHeight=12,infoInsetX=0,infoInsetY=0;
    float infoPhysicalGlyphHeight=12,infoPhysicalLinePitch=12,infoPhysicalInset=0,uiScale=1;
};
struct Geometry { GeometryMode mode=GeometryMode::SmallAuthored;std::array<Rect,13> elements{};std::array<Rect,8> beltSlots{};Rect frame{};Metrics metrics{};Context context{}; };
} // namespace diablo_hud
