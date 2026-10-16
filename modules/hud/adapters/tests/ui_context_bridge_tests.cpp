// Prepared CPU tests for Douglas Pan's opt-in HUD bridge. Not executed here.
// Inherited Sustainable Use License v1.0 and notices remain applicable.
#include "../ui_context_bridge.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace diablo_hud;
namespace bridge = diablo_hud::damnationx;

namespace {
unsigned Checks = 0;
void Check(bool condition, const char *description)
{
    ++Checks;
    if (!condition)
        throw std::runtime_error(description);
}

struct MockHost {
    mutable int pointCalls = 0, rectCalls = 0;
    bool refusePoint = false, refuseRect = false, badPoint = false, badRect = false;
};

// Numeric q5 fixture of target UI rules; the bridge does not implement them.
int Edge(int ui) { return ui == 0 ? 0 : 1 + (ui * 5 + 3) / 4; }

bool ScreenToUi(const void *state, bridge::ScreenPoint screen, bridge::UiPoint &ui)
{
    const auto &host = *static_cast<const MockHost *>(state);
    ++host.pointCalls;
    if (host.refusePoint)
        return false;
    if (host.badPoint) {
        ui.value = { 1024, 576 };
        return true;
    }
    ui.value = { std::max(0, (screen.value.x - 1) * 4 / 5), std::max(0, (screen.value.y - 1) * 4 / 5) };
    return true;
}

bool UiRectToScreen(const void *state, Rect ui, Rect &screen)
{
    const auto &host = *static_cast<const MockHost *>(state);
    ++host.rectCalls;
    if (host.refuseRect)
        return false;
    if (host.badRect) {
        screen = { { 0, 0 }, { 0, 0 } };
        return true;
    }
    const int left = Edge(ui.position.x), top = Edge(ui.position.y);
    screen = { { left, top }, { Edge(ui.position.x + ui.size.width) - left, Edge(ui.position.y + ui.size.height) - top } };
    return true;
}

bridge::HostSnapshot Snapshot()
{
    bridge::HostSnapshot s;
    s.uiSize = { 1024, 576 };
    s.screenSize = { 1281, 721 };
    s.windowSize = s.screenSize;
    s.drawableSize = s.screenSize;
    s.uiScaleQuarters = 5;
    s.uiScreenOffset = { 1, 1 };
    s.viewport = { 0, 0, 1281, 721 };
    s.hostIdentity = 42; // no renderer pointer or SDL dependency
    s.presentationGeneration = 1;
    s.uiTransformGeneration = 1;
    s.available = true;
    return s;
}

void TestPointerAndEdges()
{
    auto s = Snapshot();
    const auto map = bridge::BuildUiContextBridge(s);
    Check(map.has_value(), "valid numeric snapshot");
    Check(map->context.rendererIdentity == 42 && map->context.valid, "opaque owner works without renderer");
    MockHost host;
    const bridge::HostUiCallbacks callbacks { &host, ScreenToUi, UiRectToScreen };
    const auto a = bridge::RouteScreenPointer(*map, s, callbacks, { { 1, 1 } }, true);
    const auto b = bridge::RouteScreenPointer(*map, s, callbacks, { { 2, 2 } }, true);
    Check(a.hudUi && b.hudUi && a.hudUi->value == Point { 0, 0 } && b.hudUi->value == Point { 0, 0 }, "q5 inverse golden");
    Check(a.worldScreen.value == Point { 1, 1 } && b.worldScreen.value == Point { 2, 2 }, "same UI cell retains distinct world pixels");
    Check(host.pointCalls == 2, "one host mapping per eligible pointer");
    const auto bar = bridge::RouteScreenPointer(*map, s, callbacks, { { 0, 0 } }, false);
    Check(!bar.hudUi && bar.worldScreen.value == Point { 0, 0 } && host.pointCalls == 2, "validity precedes integer truncation");
    const auto outside = bridge::RouteScreenPointer(*map, s, callbacks, { { -1, 0 } }, true);
    Check(!outside.hudUi && outside.worldScreen.value == Point { -1, 0 } && host.pointCalls == 2, "outside screen does not become HUD");
    const auto first = bridge::ProjectUiRect(*map, s, callbacks, { { 0, 0 }, { 1, 1 } });
    Check(first && first->screen == Rect { { 0, 0 }, { 3, 3 } }, "zero-edge coverage golden");
    const auto second = bridge::ProjectUiRect(*map, s, callbacks, { { 1, 1 }, { 1, 1 } });
    Check(second && second->screen == Rect { { 3, 3 }, { 1, 1 } }, "rectangle uses edge difference");
    const auto last = bridge::ProjectUiRect(*map, s, callbacks, { { 1023, 575 }, { 1, 1 } });
    Check(last && last->screen.position == Point { 1280, 720 } && last->screen.size == Size { 1, 1 }, "last UI pixel reaches odd screen edge");
    const auto clipped = bridge::ProjectUiRect(*map, s, callbacks, { { -10, -5 }, { 12, 8 } });
    Check(clipped && clipped->ui == Rect { { 0, 0 }, { 2, 3 } } && clipped->screen.size == Size { 4, 5 }, "clip retains host edges");
    const int calls = host.rectCalls;
    Check(!bridge::ProjectUiRect(*map, s, callbacks, { { 2000, 2000 }, { 10, 10 } }) && host.rectCalls == calls, "empty clip does not call host");
    host.refusePoint = true;
    Check(!bridge::RouteScreenPointer(*map, s, callbacks, { { 2, 2 } }, true).hudUi, "point callback refusal");
    host.refusePoint = false;
    host.badPoint = true;
    Check(!bridge::RouteScreenPointer(*map, s, callbacks, { { 2, 2 } }, true).hudUi, "invalid callback point refused");
    host.refuseRect = true;
    Check(!bridge::ProjectUiRect(*map, s, callbacks, { { 1, 1 }, { 1, 1 } }), "rect callback refusal");
    host.refuseRect = false;
    host.badRect = true;
    Check(!bridge::ProjectUiRect(*map, s, callbacks, { { 1, 1 }, { 1, 1 } }), "empty callback rectangle refused");
    Check(!bridge::RouteScreenPointer(*map, s, {}, { { 2, 2 } }, true).hudUi, "absent point callback");
    Check(!bridge::ProjectUiRect(*map, s, {}, { { 1, 1 }, { 1, 1 } }), "absent rectangle callback");
}

void TestLifecycleAndAdmission()
{
    const auto s = Snapshot();
    const auto map = *bridge::BuildUiContextBridge(s);
    MockHost host;
    const bridge::HostUiCallbacks callbacks { &host, ScreenToUi, UiRectToScreen };
    auto changed = s;
    ++changed.presentationGeneration;
    Check(!bridge::CanReuseCoordinateMap(map, *bridge::BuildUiContextBridge(changed)), "same-size device recreation invalidates");
    Check(!bridge::RouteScreenPointer(map, changed, callbacks, { { 2, 2 } }, true).hudUi && host.pointCalls == 0, "stale epoch stops callback");
    changed = s;
    ++changed.uiTransformGeneration;
    Check(!bridge::CanReuseCoordinateMap(map, *bridge::BuildUiContextBridge(changed)), "same-size UI epoch invalidates");
    changed = s;
    ++changed.windowSize.width;
    Check(!bridge::CanReuseCoordinateMap(map, *bridge::BuildUiContextBridge(changed)), "window units resize invalidates");
    changed = s;
    ++changed.drawableSize.width;
    Check(!bridge::CanReuseCoordinateMap(map, *bridge::BuildUiContextBridge(changed)), "drawable resize invalidates");
    changed = s;
    ++changed.hostIdentity;
    Check(!bridge::CanReuseCoordinateMap(map, *bridge::BuildUiContextBridge(changed)), "owner replacement invalidates");
    changed = s;
    ++changed.customizationGeneration;
    Check(bridge::CanReuseCoordinateMap(map, *bridge::BuildUiContextBridge(changed)), "customization alone is not a coordinate change");
    changed.hasHudOverride = true;
    const auto customized = *bridge::BuildUiContextBridge(changed);
    Check(customized.context.hasHudOverride, "core retains appearance metadata");
    Check(bridge::CanReuseCoordinateMap(map, customized), "appearance toggle is not a coordinate change");
    Check(!sameTransform(map.context, customized.context), "coordinate reuse does not bypass core transaction checks");
    Check(bridge::RouteScreenPointer(map, changed, callbacks, { { 2, 2 } }, true).hudUi.has_value(), "appearance toggle retains exact pointer mapping");
    Check(bridge::ProjectUiRect(map, changed, callbacks, { { 1, 1 }, { 1, 1 } }).has_value(), "appearance toggle retains exact rectangle mapping");
    changed = s;
    changed.available = false;
    Check(!bridge::BuildUiContextBridge(changed), "unavailable host");
    changed = s;
    changed.hostIdentity = 0;
    Check(!bridge::BuildUiContextBridge(changed), "missing identity");
    changed = s;
    changed.screenSize.width = 0;
    Check(!bridge::BuildUiContextBridge(changed), "zero screen size");
    changed = s;
    changed.drawableSize.width = 65536;
    Check(!bridge::BuildUiContextBridge(changed), "arithmetic dimension bound");
    changed = s;
    changed.uiScaleQuarters = 3;
    Check(!bridge::BuildUiContextBridge(changed), "invalid quarter scale");
    changed = s;
    changed.viewport.x = std::numeric_limits<float>::quiet_NaN();
    Check(!bridge::BuildUiContextBridge(changed), "NaN viewport");
    changed = s;
    changed.viewport.width = std::numeric_limits<float>::infinity();
    Check(!bridge::BuildUiContextBridge(changed), "infinite viewport");
    changed = s;
    changed.viewport.width = std::numeric_limits<float>::denorm_min();
    Check(!bridge::BuildUiContextBridge(changed), "derived X scale underflow refused");
    changed = s;
    changed.viewport.height = std::numeric_limits<float>::denorm_min();
    Check(!bridge::BuildUiContextBridge(changed), "derived Y scale underflow refused");
    changed = s;
    changed.viewport.x = 1;
    Check(!bridge::BuildUiContextBridge(changed), "viewport outside drawable");
}

void TestFractionalLetterbox()
{
    auto s = Snapshot();
    s.drawableSize = { 1281, 1000 };
    s.viewport = { 0, 139.5F, 1281, 721 };
    const auto map = *bridge::BuildUiContextBridge(s);
    Check(map.context.rawViewportY == 139 && map.context.rawViewportHeight == 722, "integer viewport preserves half-pixel origin");
    Check(map.context.outputOriginY == 140.5F, "nominal metrics retain UI remainder");
    MockHost host;
    const bridge::HostUiCallbacks callbacks { &host, ScreenToUi, UiRectToScreen };
    const auto rect = bridge::ProjectUiRect(map, s, callbacks, { { 0, 0 }, { 1, 1 } });
    Check(rect && rect->output.y == 139.5F && rect->output.height == 3, "coverage uses exact callback origin");
    s.drawableSize = { 2562, 2000 };
    s.viewport = { 0, 279, 2562, 1442 };
    const auto highDpi = *bridge::BuildUiContextBridge(s);
    const auto scaled = bridge::ProjectUiRect(highDpi, s, callbacks, { { 1, 1 }, { 1, 1 } });
    Check(scaled && scaled->output.x == 6 && scaled->output.y == 285 && scaled->output.width == 2, "drawable pixels separate from window units");
}
} // namespace

int main()
{
    try {
        TestPointerAndEdges();
        TestLifecycleAndAdmission();
        TestFractionalLetterbox();
        std::cout << Checks << " checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
