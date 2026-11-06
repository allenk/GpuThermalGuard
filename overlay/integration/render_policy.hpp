#pragma once
#include "tray/osd_compact.hpp"
#include "../ipc/osd_section.hpp"
#include <algorithm>
#include <cmath>

namespace gtg::overlay::integration {
inline float BoundRenderScale(float scale, bool collapsed) noexcept {
    // ChooseFootprint cannot exceed these bounds for any valid arrangement,
    // including all optional records. Cap before GDI+ allocates its bitmap.
    namespace compact = tray::compact;
    constexpr int width = compact::CollapsedWidth(compact::kRecordCount);
    constexpr int height = compact::kCollapsedSingleRowHeight +
                           (compact::kRecordCount - 1) * compact::kCollapsedRowHeightIncrement;
    constexpr int expanded_height = compact::ExpandedHeight(compact::kRecordCount);
    if (!std::isfinite(scale) || scale <= 0) return 0;
    return std::min({scale, static_cast<float>(ipc::kMaxWidth) / (collapsed ? width : 388),
                     static_cast<float>(ipc::kMaxHeight) / (collapsed ? height : expanded_height)});
}

inline constexpr std::uint64_t kRenderIntervalMs = 500;

// Whether the game's copy is drawn again on this tick: on its 500 ms cadence,
// or at once when the desktop OSD's size has changed since the last drawing.
// The placement follows the desktop window on every tick, so without the
// second rule the image drawn for the old shape is placed by the new shape's
// anchor for up to half a second: folding the expanded view into Tiny showed
// the tall image standing above Tiny's place before it shrank.
[[nodiscard]] constexpr bool RenderDue(const std::uint64_t last_render, const std::uint64_t now,
                                       const int drawn_width, const int drawn_height,
                                       const int width, const int height) noexcept {
    if (last_render == 0 || now < last_render || now - last_render >= kRenderIntervalMs)
        return true;
    return drawn_width != width || drawn_height != height;
}
}  // namespace gtg::overlay::integration
