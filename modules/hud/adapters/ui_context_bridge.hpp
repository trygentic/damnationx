// Diablo3D HUD contribution by Douglas Pan; modified for explicit host values.
// Inherited Sustainable Use License v1.0 and notices remain applicable.
// Opt-in CPU contract only: no SDL3 or WebGPU presentation backend.
#pragma once
#include <diablo_hud/types.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace diablo_hud::damnationx {

struct ScreenPoint { Point value {}; };
struct UiPoint { Point value {}; };
struct OutputRect { float x = 0, y = 0, width = 0, height = 0; };

struct HostSnapshot {
    Size uiSize {}, screenSize {}, windowSize {}, drawableSize {};
    int uiScaleQuarters = 4;
    Point uiScreenOffset {}; // actual GetUiTransform offsets, not a size ratio
    OutputRect viewport {}; // actual drawable-pixel viewport, including letterbox
    std::uintptr_t hostIdentity = 0; // opaque owner token, not an SDL_Renderer*
    std::uint64_t presentationGeneration = 0, uiTransformGeneration = 0;
    std::uint64_t customizationGeneration = 0;
    bool hasHudOverride = false, integerScale = false, available = false;
};

struct UiContextBridge {
    Context context {};
    HostSnapshot host {};
};

// The caller adapts native Point/Rectangle types over a stable host snapshot.
// Callbacks may observe/counter calls, but must not change transforms/game state.
struct HostUiCallbacks {
    const void *state = nullptr;
    bool (*screenToUi)(const void *, ScreenPoint, UiPoint &) = nullptr;
    bool (*uiRectToScreen)(const void *, Rect, Rect &) = nullptr;
};

namespace detail {
inline bool ValidSize(Size size)
{
    // Target UI dimensions are uint16; this bounds arithmetic, not GPU capacity.
    return size.width > 0 && size.height > 0 && size.width <= 65535 && size.height <= 65535;
}

inline bool ValidViewport(OutputRect r, Size drawable)
{
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height)
        && r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0
        && double(r.x) + r.width <= drawable.width && double(r.y) + r.height <= drawable.height;
}

inline std::optional<Rect> ClipRect(Rect rect, Size bounds)
{
    if (rect.size.width <= 0 || rect.size.height <= 0)
        return std::nullopt;
    const auto left = std::max<std::int64_t>(0, rect.position.x);
    const auto top = std::max<std::int64_t>(0, rect.position.y);
    const auto right = std::min<std::int64_t>(bounds.width, std::int64_t(rect.position.x) + rect.size.width);
    const auto bottom = std::min<std::int64_t>(bounds.height, std::int64_t(rect.position.y) + rect.size.height);
    if (right <= left || bottom <= top)
        return std::nullopt;
    return Rect { { int(left), int(top) }, { int(right - left), int(bottom - top) } };
}
} // namespace detail

inline std::optional<UiContextBridge> BuildUiContextBridge(const HostSnapshot &host)
{
    if (!host.available || host.hostIdentity == 0 || host.presentationGeneration == 0 || host.uiTransformGeneration == 0
        || !detail::ValidSize(host.uiSize) || !detail::ValidSize(host.screenSize)
        || !detail::ValidSize(host.windowSize) || !detail::ValidSize(host.drawableSize)
        || host.uiScaleQuarters < 4 || host.uiScaleQuarters > 16
        || host.uiScreenOffset.x < 0 || host.uiScreenOffset.y < 0
        || host.uiScreenOffset.x >= host.screenSize.width || host.uiScreenOffset.y >= host.screenSize.height
        || !detail::ValidViewport(host.viewport, host.drawableSize))
        return std::nullopt;

    UiContextBridge result;
    result.host = host;
    Context &context = result.context;
    const float screenScaleX = host.viewport.width / float(host.screenSize.width);
    const float screenScaleY = host.viewport.height / float(host.screenSize.height);
    const float uiScale = float(host.uiScaleQuarters) / 4.0F;
    context.rendererIdentity = host.hostIdentity;
    context.logicalWidth = host.uiSize.width;
    context.logicalHeight = host.uiSize.height;
    context.outputWidth = host.drawableSize.width;
    context.outputHeight = host.drawableSize.height;
    context.rendererLogicalWidth = host.screenSize.width;
    context.rendererLogicalHeight = host.screenSize.height;
    context.rawViewportX = int(std::floor(host.viewport.x));
    context.rawViewportY = int(std::floor(host.viewport.y));
    context.rawViewportWidth = int(std::ceil(double(host.viewport.x) + host.viewport.width)) - context.rawViewportX;
    context.rawViewportHeight = int(std::ceil(double(host.viewport.y) + host.viewport.height)) - context.rawViewportY;
    // Nominal physical metrics only. Exact raster edges use uiRectToScreen.
    context.outputScaleX = uiScale * screenScaleX;
    context.outputScaleY = uiScale * screenScaleY;
    context.outputOriginX = host.viewport.x + float(host.uiScreenOffset.x) * screenScaleX;
    context.outputOriginY = host.viewport.y + float(host.uiScreenOffset.y) * screenScaleY;
    if (!std::isfinite(context.outputScaleX) || !std::isfinite(context.outputScaleY)
        || context.outputScaleX <= 0 || context.outputScaleY <= 0
        || !std::isfinite(context.outputOriginX) || !std::isfinite(context.outputOriginY))
        return std::nullopt;
    context.viewportWidth = host.viewport.width;
    context.viewportHeight = host.viewport.height;
    context.layoutGeneration = host.uiTransformGeneration;
    context.customizationGeneration = host.customizationGeneration;
    context.hasHudOverride = host.hasHudOverride;
    context.integerScale = host.integerScale;
    context.valid = true;
    return result;
}

// Core sameTransform ignores layout/customization generations. This predicate
// also handles host lifecycle and transform epochs. Appearance alone is not a
// coordinate change; the core still applies its own transaction-context checks.
inline bool CanReuseCoordinateMap(const UiContextBridge &previous, const UiContextBridge &current)
{
    if (!(previous.context.valid && current.context.valid
        && previous.host.available && current.host.available
        && previous.host.presentationGeneration == current.host.presentationGeneration
        && previous.host.uiTransformGeneration == current.host.uiTransformGeneration
        && previous.host.uiScaleQuarters == current.host.uiScaleQuarters
        && previous.host.uiScreenOffset == current.host.uiScreenOffset
        && previous.host.windowSize == current.host.windowSize))
        return false;
    Context previousCoordinates = previous.context;
    Context currentCoordinates = current.context;
    previousCoordinates.hasHudOverride = false;
    currentCoordinates.hasHudOverride = false;
    return sameTransform(previousCoordinates, currentCoordinates);
}

struct PointerCoordinates {
    ScreenPoint worldScreen {}; // exact caller value, even when HUD declines it
    std::optional<UiPoint> hudUi;
};

inline PointerCoordinates RouteScreenPointer(const UiContextBridge &captured, const HostSnapshot &current,
    const HostUiCallbacks &callbacks, ScreenPoint screen, bool insideViewportBeforeTruncation)
{
    PointerCoordinates result { screen, std::nullopt };
    const auto now = BuildUiContextBridge(current);
    if (!now || !CanReuseCoordinateMap(captured, *now) || !insideViewportBeforeTruncation || !callbacks.screenToUi)
        return result;
    if (screen.value.x < 0 || screen.value.y < 0
        || screen.value.x >= current.screenSize.width || screen.value.y >= current.screenSize.height)
        return result;
    UiPoint ui;
    if (callbacks.screenToUi(callbacks.state, screen, ui)
        && ui.value.x >= 0 && ui.value.y >= 0 && ui.value.x < current.uiSize.width && ui.value.y < current.uiSize.height)
        result.hudUi = ui;
    return result;
}

struct ProjectedUiRect {
    Rect ui {}, screen {};
    OutputRect output {};
};

// Metadata only: pixel/coverage stamping and painter order remain host-owned.
inline std::optional<ProjectedUiRect> ProjectUiRect(const UiContextBridge &captured, const HostSnapshot &current,
    const HostUiCallbacks &callbacks, Rect uiRect)
{
    const auto now = BuildUiContextBridge(current);
    if (!now || !CanReuseCoordinateMap(captured, *now) || !callbacks.uiRectToScreen)
        return std::nullopt;
    const auto clippedUi = detail::ClipRect(uiRect, current.uiSize);
    if (!clippedUi)
        return std::nullopt;
    Rect screenRect;
    if (!callbacks.uiRectToScreen(callbacks.state, *clippedUi, screenRect))
        return std::nullopt;
    const auto clippedScreen = detail::ClipRect(screenRect, current.screenSize);
    if (!clippedScreen)
        return std::nullopt;
    const float scaleX = current.viewport.width / float(current.screenSize.width);
    const float scaleY = current.viewport.height / float(current.screenSize.height);
    const float left = current.viewport.x + float(clippedScreen->position.x) * scaleX;
    const float top = current.viewport.y + float(clippedScreen->position.y) * scaleY;
    const float right = current.viewport.x + float(clippedScreen->position.x + clippedScreen->size.width) * scaleX;
    const float bottom = current.viewport.y + float(clippedScreen->position.y + clippedScreen->size.height) * scaleY;
    return ProjectedUiRect { *clippedUi, *clippedScreen, { left, top, right - left, bottom - top } };
}

} // namespace diablo_hud::damnationx
