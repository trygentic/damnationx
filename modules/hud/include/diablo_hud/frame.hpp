// Diablo3D / DevilutionX HUD contract; see LICENSE.md and NOTICE.md.
#pragma once
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
namespace diablo_hud {
// Owns sequencing only. Pixel snapshots, decoders, textures and host command
// handlers belong to a renderer/game adapter, never to this contract object.
enum class FramePhase { Empty,Capturing,PanelsSealed,CursorDrawn,LateOverlay,Prepared,Presented,Recovered,Invalid };
class FrameContract {
public:
    bool begin(std::uint64_t identity,int width,int height) {
        invalidate();if(width<=0||height<=0||std::uint64_t(width)*unsigned(height)>16U*1024U*1024U)return false;
        identity_=identity;phase_=FramePhase::Capturing;return true;
    }
    bool beginPanel(std::size_t id){if(phase_!=FramePhase::Capturing||open_)return false;open_=id;return true;}
    bool sealPanel(){if(!open_)return false;open_.reset();return true;}
    bool sealPanels(){if(phase_!=FramePhase::Capturing||open_)return false;phase_=FramePhase::PanelsSealed;return true;}
    bool cursorDrawn(){if(phase_!=FramePhase::PanelsSealed)return false;phase_=FramePhase::CursorDrawn;cursor_=true;return true;}
    bool lateOverlay(){if(phase_!=FramePhase::CursorDrawn&&phase_!=FramePhase::LateOverlay)return false;phase_=FramePhase::LateOverlay;return true;}
    bool prepare(std::uint64_t identity){if(identity!=identity_||open_||(phase_!=FramePhase::CursorDrawn&&phase_!=FramePhase::LateOverlay))return false;phase_=FramePhase::Prepared;return true;}
    bool present(std::uint64_t identity){if(identity!=identity_||phase_!=FramePhase::Prepared)return false;phase_=FramePhase::Presented;return true;}
    // Materialization is a transport fallback, not the end of composition.
    bool materialize(){if(phase_==FramePhase::Empty||phase_==FramePhase::Invalid||phase_==FramePhase::Recovered||phase_==FramePhase::Presented)return false;materialized_=true;return true;}
    bool materialized()const{return materialized_;}
    bool recover(){if(phase_==FramePhase::Empty||phase_==FramePhase::Invalid)return false;open_.reset();phase_=FramePhase::Recovered;return true;}
    void invalidate(){identity_=0;open_.reset();phase_=FramePhase::Empty;cursor_=false;materialized_=false;}
    FramePhase phase()const{return phase_;}bool preservesCursor()const{return cursor_;}std::uint64_t identity()const{return identity_;}
private:std::uint64_t identity_=0;std::optional<std::size_t>open_;FramePhase phase_=FramePhase::Empty;bool cursor_=false,materialized_=false;
};
} // namespace diablo_hud
