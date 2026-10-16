// Derived from the Diablo3D / DevilutionX HUD. Modified for explicit host
// state and standalone use. See LICENSE.md and NOTICE.md; notices are retained.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "types.hpp"

namespace diablo_hud {
namespace detail {

inline int WindowInt(std::int64_t value)
{
    return static_cast<int>(std::clamp(value,
        static_cast<std::int64_t>(std::numeric_limits<int>::min()),
        static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}

inline float PositiveWindowScale(float scale)
{
    return std::isfinite(scale) && scale > 0 ? scale : 1.0F;
}

inline int RoundedWindowExtent(std::int64_t extent, float scale)
{
    const float rounded = std::round(static_cast<float>(extent) * scale);
    if (!std::isfinite(rounded) || rounded >= static_cast<float>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    if (rounded <= static_cast<float>(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    return static_cast<int>(rounded);
}

} // namespace detail

/** Same selected-window scale as R4: zero means the viewport-dependent default.
 * Native compatibility presentation supplies enabled=false and retains scale 1.
 * The minimum is 1 even for viewports smaller than the native content. */
inline float UniformWindowScale(float requested, Size body, int header,
    const Context &context, bool enabled = true)
{
    if (!enabled || body.width <= 0 || body.height <= 0) return 1.0F;
    if (!std::isfinite(requested) || requested <= 0)
        requested = context.logicalHeight >= 700 ? 1.25F : 1.0F;
    const float fit = std::min(static_cast<float>(context.logicalWidth) / body.width,
        static_cast<float>(context.logicalHeight) / static_cast<float>(std::int64_t(body.height) + std::max(0, header)));
    return std::clamp(requested, 1.0F, std::max(1.0F, std::min(2.0F, fit)));
}

/** Clamp the unscaled native body origin against its complete displayed window.
 * Header and body use the same round operation as WindowBounds; no stored
 * preference is changed by a viewport clamp. */
inline Rect ClampWindowBody(Rect body, int header, float scale, const Context &context)
{
    if (body.size.width <= 0 || body.size.height <= 0) return body;
    scale = detail::PositiveWindowScale(scale);
    header = std::max(0, header);
    const int top = detail::RoundedWindowExtent(header, scale);
    const int width = detail::RoundedWindowExtent(body.size.width, scale);
    const int height = detail::RoundedWindowExtent(std::int64_t(body.size.height) + header, scale);
    const int rightLimit = detail::WindowInt(std::int64_t(context.logicalWidth) - width);
    const int bottomLimit = detail::WindowInt(std::int64_t(context.logicalHeight) - height + top);
    body.position.x = std::clamp(body.position.x, 0, std::max(0, rightLimit));
    body.position.y = std::clamp(body.position.y, top, std::max(top, bottomLimit));
    return body;
}

/** Full visual rectangle from the native body, preserving the R4 round policy. */
inline Rect WindowBounds(Rect body, int header, float scale)
{
    if (body.size.width <= 0 || body.size.height <= 0) return { body.position, {} };
    scale = detail::PositiveWindowScale(scale);
    header = std::max(0, header);
    return { { body.position.x, detail::WindowInt(std::int64_t(body.position.y)
                   - detail::RoundedWindowExtent(header, scale)) },
        { detail::RoundedWindowExtent(body.size.width, scale),
          detail::RoundedWindowExtent(std::int64_t(body.size.height) + header, scale) } };
}

/** R4 integer inverse mapping. The host chooses the physical topmost owner
 * before calling this; a nested native handler must keep that owner identity. */
inline Point VisualToNative(Point point, Rect native, Rect visual)
{
    if (native.size.width <= 0 || native.size.height <= 0
        || visual.size.width <= 0 || visual.size.height <= 0) return point;
    return { detail::WindowInt(std::int64_t(native.position.x)
                 + (std::int64_t(point.x) - visual.position.x) * native.size.width / visual.size.width),
        detail::WindowInt(std::int64_t(native.position.y)
            + (std::int64_t(point.y) - visual.position.y) * native.size.height / visual.size.height) };
}

/** First physical pixel whose inverse map selects this native pixel. Unsupported
 * or outside native points remain unchanged, as in the native focus helper. */
inline Point NativeToVisual(Point point, Rect native, Rect visual)
{
    if (!native.contains(point) || visual.size.width <= 0 || visual.size.height <= 0) return point;
    const auto edge = [](int pixel, int origin, int destination, int source) {
        return ((std::int64_t(pixel) - origin) * destination + source - 1) / source;
    };
    return { detail::WindowInt(std::int64_t(visual.position.x)
                 + edge(point.x, native.position.x, visual.size.width, native.size.width)),
        detail::WindowInt(std::int64_t(visual.position.y)
            + edge(point.y, native.position.y, visual.size.height, native.size.height)) };
}

/** Uniform corner resize projects pointer delta onto the original aspect-ratio
 * diagonal. Zero delta preserves scale, and contrary x/y motion stays continuous.
 * The host snapshots initial scale/pointer and applies commit/cancel separately. */
inline float ContinuousWindowScale(float initial, Delta delta, Size body, int header,
    const Context &context)
{
    if (body.width <= 0 || body.height <= 0) return 1.0F;
    if (!std::isfinite(initial)) initial = UniformWindowScale(0, body, header, context);
    const float width = static_cast<float>(body.width), height = static_cast<float>(body.height);
    const float change = (delta.deltaX * width + delta.deltaY * height)
        / (width * width + height * height);
    return UniformWindowScale(std::max(1.0F, initial + change), body, header, context);
}

/** One native and one displayed rectangle per identity. body is native-space;
 * records are supplied bottom-to-top by the host or WindowStack. */
struct WindowRecord {
    std::size_t id = 0;
    Rect native {}, visual {}, body {};
    bool visible = true;
};

/** Topmost visible rectangle wins. Its header blocks body-only lookup rather
 * than passing the point through to a body behind it. Clock/modal priority and
 * captured native pointer identity remain explicit host routing decisions. */
inline std::optional<std::size_t> FindWindowHit(const WindowRecord *ordered,
    std::size_t count, Point point, bool bodyOnly = false)
{
    if (ordered == nullptr) return std::nullopt;
    for (std::size_t i = count; i > 0; --i) {
        const auto &record = ordered[i - 1];
        if (!record.visible || !record.visual.contains(point)) continue;
        if (bodyOnly && !record.body.contains(VisualToNative(point, record.native, record.visual)))
            return std::nullopt;
        return record.id;
    }
    return std::nullopt;
}

/** Pointer/count permits native fixed arrays without per-hover allocation.
 * The caller keeps count records alive for the duration of the lookup. */
inline std::optional<std::size_t> FindWindowHit(const std::vector<WindowRecord> &ordered,
    Point point, bool bodyOnly = false)
{
    return FindWindowHit(ordered.data(), ordered.size(), point, bodyOnly);
}

/** R4 focus rotation: preserve relative ordering of every other identity. */
inline bool FocusWindows(std::vector<WindowRecord> &ordered, std::size_t id)
{
    const auto found = std::find_if(ordered.begin(), ordered.end(),
        [id](const WindowRecord &record) { return record.id == id && record.visible; });
    if (found == ordered.end()) return false;
    std::rotate(found, found + 1, ordered.end());
    return true;
}

/** Open/close and z-order are transient UI state. They do not own or modify
 * WindowPreferences, normalized layout records, files or native game flags.
 * The host supplies unique IDs and refreshes rectangles before hit/draw. */
struct WindowStack {
    std::vector<WindowRecord> records;

    bool isOpen(std::size_t id) const
    {
        const auto found = std::find_if(records.begin(), records.end(),
            [id](const WindowRecord &record) { return record.id == id; });
        return found != records.end() && found->visible;
    }
    bool close(std::size_t id)
    {
        const auto found = std::find_if(records.begin(), records.end(),
            [id](const WindowRecord &record) { return record.id == id; });
        if (found == records.end()) return false;
        found->visible = false;
        return true;
    }
    bool reopen(std::size_t id, bool focus = true)
    {
        const auto found = std::find_if(records.begin(), records.end(),
            [id](const WindowRecord &record) { return record.id == id; });
        if (found == records.end()) return false;
        found->visible = true;
        if (focus) FocusWindows(records, id);
        return true;
    }
    bool focus(std::size_t id) { return FocusWindows(records, id); }
    std::optional<std::size_t> hit(Point point, bool bodyOnly = false) const
    {
        return FindWindowHit(records, point, bodyOnly);
    }
};

/** Renderer resampling uses precisely the same integer inverse as input.
 * The Rect overload returns source-space coordinates; Size returns local pixels.
 * Callers visit points inside destination and retain their own clip/opacity. */
inline Point SurfaceSourcePoint(Point point, Rect source, Rect destination)
{
    return VisualToNative(point, source, destination);
}
inline Point SurfaceSourcePoint(Point point, Size source, Rect destination)
{
    return SurfaceSourcePoint(point, Rect { {}, source }, destination);
}

} // namespace diablo_hud
