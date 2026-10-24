// Derived from the Diablo3D / DevilutionX HUD. Modified for explicit host
// state and standalone use. See LICENSE.md and NOTICE.md; notices are retained.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "types.hpp"

namespace diablo_hud {

inline constexpr std::size_t LayoutMaxBytes = 8192;

/** R4 element codec: whole-file acceptance, unchanged names and normalized units.
 * Failure does not modify the caller's current Overrides. Empty valid layouts
 * contain the header only. File access and replacement/backup belong to the host. */
inline std::optional<Overrides> ParseElementLayout(std::string_view bytes)
{
    if (bytes.size() > LayoutMaxBytes) return std::nullopt;
    std::istringstream input { std::string(bytes) };
    std::string header;
    if (!std::getline(input, header)) return std::nullopt;
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (header != "D3DHUDLAYOUT1") return std::nullopt;
    Overrides candidate {};
    std::string line;
    std::size_t readBytes = header.size(), rows = 0;
    while (std::getline(input, line)) {
        readBytes += line.size() + 1;
        if (readBytes > LayoutMaxBytes || ++rows > ElementCount) return std::nullopt;
        std::istringstream row { line };
        std::string name, tail;
        NormalizedRect rect;
        if (!(row >> name >> rect.x >> rect.y >> rect.width >> rect.height) || row >> tail)
            return std::nullopt;
        const auto found = std::find(ElementNames.begin(), ElementNames.end(), name);
        if (found == ElementNames.end()) return std::nullopt;
        const std::size_t slot = static_cast<std::size_t>(found - ElementNames.begin());
        if (candidate[slot] || rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0
            || rect.x > NormalizedUnits || rect.y > NormalizedUnits
            || rect.width > NormalizedUnits || rect.height > NormalizedUnits
            || rect.x + rect.width > NormalizedUnits || rect.y + rect.height > NormalizedUnits)
            return std::nullopt;
        candidate[slot] = rect;
    }
    if (input.bad()) return std::nullopt;
    return candidate;
}

/** Serialize a host-validated snapshot in the same ID order as the R4 writer. */
inline std::string SerializeElementLayout(const Overrides &overrides)
{
    std::ostringstream serialized;
    serialized << "D3DHUDLAYOUT1\n";
    for (std::size_t i = 0; i < overrides.size(); ++i) {
        if (!overrides[i]) continue;
        const auto &rect = *overrides[i];
        serialized << ElementNames[i] << ' ' << rect.x << ' ' << rect.y << ' '
                   << rect.width << ' ' << rect.height << '\n';
    }
    return serialized.str();
}

/** Legacy R4 window rows have no header. Invalid rows are ignored and valid
 * duplicate positions replace earlier positions. An absent/invalid scale keeps
 * the previous scale for that slot, matching the native two-array loader.
 * Oversized input is declined before any rows are considered. Slot zero stays empty. */
inline std::optional<WindowPreferences> ParseWindowLayout(std::string_view bytes)
{
    if (bytes.size() > LayoutMaxBytes) return std::nullopt;
    WindowPreferences candidate {};
    std::istringstream input { std::string(bytes) };
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row { line };
        int slot, x, y;
        float scale = 0;
        if (!(row >> slot >> x >> y) || slot <= 0 || slot >= static_cast<int>(candidate.size())
            || x < 0 || x >= 32768 || y < 0 || y >= 32768) continue;
        auto &preference = candidate[static_cast<std::size_t>(slot)];
        const float previousScale = preference ? preference->scale : 0;
        preference = WindowPreference { { x, y }, previousScale };
        if (row >> scale && std::isfinite(scale) && scale > 0)
            preference->scale = std::clamp(scale, 1.0F, 2.0F);
    }
    return candidate;
}

/** Preserve the old position-plus-scale rows and native stream precision.
 * A scale-only edit must first supply its displayed body position in the host. */
inline std::string SerializeWindowLayout(const WindowPreferences &preferences)
{
    std::ostringstream output;
    for (std::size_t slot = 1; slot < preferences.size(); ++slot) {
        if (!preferences[slot]) continue;
        const auto &preference = *preferences[slot];
        output << slot << ' ' << preference.position.x << ' ' << preference.position.y
               << ' ' << preference.scale << '\n';
    }
    return output.str();
}

} // namespace diablo_hud
