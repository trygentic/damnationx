// Diablo3D / DevilutionX HUD extraction; modified for explicit host state.
// Inherited terms and authors are retained in LICENSE.md / NOTICE.md.
#pragma once
#include "windows.hpp"
#include <optional>
#include <vector>
namespace diablo_hud {
enum class OwnerKind { None,Modal,Clock,WindowBody,WindowHeader,Minimap,Element };
struct Owner { OwnerKind kind=OwnerKind::None;std::size_t id=0;bool owns()const{return kind!=OwnerKind::None;} };
struct Overlay { std::size_t id=0;Rect bounds{};bool visible=false; };
// Bounds are caller snapshots in one logical coordinate space, bottom-to-top.
inline Owner ResolvePhysicalOwner(Point point,const std::vector<WindowRecord>&windows,
    Overlay clock={},Overlay minimap={},const std::vector<Overlay>&elements={},bool modal=false,bool relativeCaptured=false) {
    if(modal)
        return {OwnerKind::Modal,0};
    if(relativeCaptured)
        return {};
    if(clock.visible&&clock.bounds.contains(point))return {OwnerKind::Clock,clock.id};
    for(auto it=windows.rbegin();it!=windows.rend();++it)if(it->visible&&it->visual.contains(point)) {
        const auto native=VisualToNative(point,it->native,it->visual);
        return {it->body.contains(native)?OwnerKind::WindowBody:OwnerKind::WindowHeader,it->id};
    }
    if(minimap.visible&&minimap.bounds.contains(point))return {OwnerKind::Minimap,minimap.id};
    for(auto it=elements.rbegin();it!=elements.rend();++it)if(it->visible&&it->bounds.contains(point))return {OwnerKind::Element,it->id};
    return {};
}
struct PointerState {Point pointer{};unsigned depth=0;std::optional<WindowRecord> captured;};
class PointerScope {
public:
    PointerScope(PointerState &state,const std::vector<WindowRecord>&windows,bool clockOwns=false,bool relativeCaptured=false)
        :state_(state),saved_(state.pointer),owner_(state.depth==0) {
        if(owner_){
            state_.captured.reset();
            if(!clockOwns&&!relativeCaptured)
                for(auto it=windows.rbegin();it!=windows.rend();++it)if(it->visible&&it->visual.contains(saved_)){state_.captured=*it;break;}
            if(state_.captured)
                state_.pointer=VisualToNative(saved_,state_.captured->native,state_.captured->visual);
        }
        ++state_.depth;
    }
    ~PointerScope(){--state_.depth;if(owner_){state_.pointer=saved_;state_.captured.reset();}}
    PointerScope(const PointerScope&)=delete;PointerScope&operator=(const PointerScope&)=delete;
private:PointerState&state_;Point saved_;bool owner_;
};
inline bool IsNativePointer(const PointerState&state){return state.depth!=0&&state.captured.has_value();}
inline std::optional<std::size_t> CapturedWindowAt(const PointerState&state,bool bodyOnly=false,bool ownerVisible=true) {
    if(!ownerVisible||!state.captured||!state.captured->visible)return std::nullopt;
    const auto &w=*state.captured;return (bodyOnly?w.body:w.native).contains(state.pointer)?std::optional<std::size_t>(w.id):std::nullopt;
}
inline std::uint32_t ButtonMask(unsigned button){return button>0&&button<=31?std::uint32_t(1)<<(button-1):0;}
class InputPairs {
public:
    void own(unsigned button){owned_|=ButtonMask(button);}bool release(unsigned button){const auto mask=ButtonMask(button);const bool was=(owned_&mask)!=0;owned_&=~mask;return was;}
    bool owns(unsigned button)const{return (owned_&ButtonMask(button))!=0;}std::uint32_t mask()const{return owned_;}
    void cancel(){owned_=0;}
private:std::uint32_t owned_=0;
};
} // namespace diablo_hud
